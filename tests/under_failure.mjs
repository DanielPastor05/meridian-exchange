// The server while something is going wrong around it: a writer that has stopped, a signal
// with clients still connected, and a process out of file descriptors.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {once} from 'node:events';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import {Client, T, values} from '../tools/wire.mjs';
import {cleanup, delay, directory, start} from './network_helpers.mjs';

const [executable, tool] = process.argv.slice(2);
if (!executable || !tool) throw Error('server and exchange executables required');
const journal = dir => path.join(dir, 'server.journal');

const TOKEN_1 = '1'.repeat(32);
const TOKEN_2 = '2'.repeat(32);

async function rawConnection(port) {
  const socket = net.connect(port, '127.0.0.1');
  socket.on('error', () => {});
  await once(socket, 'connect');
  return socket;
}

async function until(condition, what, timeout = 15000) {
  const end = Date.now() + timeout;
  for (;;) {
    if (await condition()) return;
    if (Date.now() > end) throw Error('timed out waiting for ' + what);
    await delay(50);
  }
}

// A writer stuck before its journal append holds the book's lock for as long as it is stuck,
// as a slow fsync would. Liveness and metrics are the two things that must not queue behind it.
async function aStuckWriterDoesNotSilenceHealth() {
  const dir = directory();
  const clients = [];
  let server;
  try {
    server = await start(executable, dir, ['--fault-stage', 'before_append', '--fault-account', '1', '--fault-request', '1']);
    const writer = await Client.connect(server.port);
    clients.push(writer);
    await writer.login(1, TOKEN_1);
    const observer = await Client.connect(server.port);
    clients.push(observer);
    await observer.login(2, TOKEN_2);

    const stuck = writer.submit(1, 1, 10, 1, 90, 1).catch(() => null);
    await server.waitFor('fault_ready');

    assert.equal((await observer.send(T.ping)).type, T.ping | 0x8000);
    const metrics = values((await observer.send(T.metrics)).payload);
    assert.equal(metrics.length, 7);
    assert.equal(metrics[6], 0n, 'metrics reported a sequence the journal never committed');

    await server.stop();
    await stuck;
    console.log('PASS ping and metrics answer while the writer is stuck');
  } finally {
    clients.forEach(client => client.close());
    await server?.stop();
    cleanup(dir);
  }
}

// One logged-in client and one halfway through a frame header. A worker that only noticed the
// stop at its next frame, or that had to wait out its socket timeout, would miss the deadline
// in server.stop, which also requires exit code 0.
async function aSignalStopsItWithClientsConnected() {
  const dir = directory();
  const clients = [];
  const sockets = [];
  let server;
  try {
    server = await start(executable, dir, ['--timeout-ms', '30000']);
    const idle = await Client.connect(server.port);
    clients.push(idle);
    await idle.login(1, TOKEN_1);
    const halfway = await rawConnection(server.port);
    sockets.push(halfway);
    halfway.write(Buffer.from('MDX1'));
    await delay(100);

    const result = await server.stop('SIGTERM');
    assert.equal(result.code, 0);
    await server.waitFor('stopped', 1000);
    console.log('PASS SIGTERM stops the server cleanly with clients connected');
  } finally {
    clients.forEach(client => client.close());
    sockets.forEach(socket => socket.destroy());
    await server?.stop();
    cleanup(dir);
  }
}

// Really out of descriptors rather than a classified error code: a process limit, and more
// connections than it allows. The server has to survive it, keep serving the clients it has,
// and accept again once descriptors come back.
async function runningOutOfDescriptorsIsSurvivable() {
  const dir = directory();
  const clients = [];
  const sockets = [];
  let server;
  try {
    const limited = path.join(dir, 'limited-server.sh');
    fs.writeFileSync(limited, `#!/bin/sh\nulimit -n 64\nexec "${path.resolve(executable)}" "$@"\n`, {mode: 0o755});
    server = await start(limited, dir, ['--max-clients', '128', '--timeout-ms', '30000']);

    const established = await Client.connect(server.port);
    clients.push(established);
    await established.login(1, TOKEN_1);

    for (let i = 0; i < 100; i++) sockets.push(await rawConnection(server.port));
    await until(() => server.stderr.includes('accept resource pressure'), 'the limit to be reached');

    assert.equal((await established.send(T.ping)).type, T.ping | 0x8000);

    sockets.forEach(socket => socket.destroy());
    await until(async () => {
      let client;
      try {
        client = await Client.connect(server.port, '127.0.0.1', 1000);
        await client.login(2, TOKEN_2);
        return (await client.send(T.ping)).type === (T.ping | 0x8000);
      } catch {
        return false;
      } finally {
        client?.close();
      }
    }, 'a new client to be accepted');

    assert.equal((await server.stop('SIGTERM')).code, 0);
    console.log('PASS running out of file descriptors is survivable and recovers');
  } finally {
    clients.forEach(client => client.close());
    sockets.forEach(socket => socket.destroy());
    await server?.stop();
    cleanup(dir);
  }
}

// Anything but loopback has to be asked for, and never with the demo's tokens.
async function aReachableAddressHasToBeMeant() {
  const dir = directory();
  let server;
  try {
    await assert.rejects(start(executable, dir, ['--host', '0.0.0.0']), /allow-remote/);
    await assert.rejects(start(executable, dir, ['--host', '0.0.0.0', '--allow-remote']), /fixture tokens/);

    const token = 'a1b2c3d4'.repeat(4);
    const strong = path.join(dir, 'strong.conf');
    fs.writeFileSync(strong, `1 ${token} 1000000000 100000 10000 1000000000 1000000\n`, {mode: 0o600});
    server = await start(executable, dir, ['--host', '0.0.0.0', '--allow-remote', '--accounts', strong]);
    const client = await Client.connect(server.port);
    await client.login(1, token);
    client.close();
    console.log('PASS a reachable address needs --allow-remote and real tokens');
  } finally {
    await server?.stop();
    cleanup(dir);
  }
}

async function quotasAreEnforcedAndGivenBack() {
  const clients = [];
  const connectTo = server => async (account, token) => {
    const client = await Client.connect(server.port);
    clients.push(client);
    await client.login(account, token);
    return client;
  };
  const eventually = (connect, account, token, what) => until(async () => {
    try {
      await connect(account, token);
      return true;
    } catch {
      return false;
    }
  }, what);

  let dir = directory();
  let server;
  try {
    server = await start(executable, dir, ['--max-account-sessions', '2', '--metrics-account', '2']);
    const connect = connectTo(server);
    const first = await connect(1, TOKEN_1);
    await connect(1, TOKEN_1);
    await assert.rejects(connect(1, TOKEN_1), /authentication rejected/);

    // Another account is not charged for that one, and metrics are its alone.
    const operator = await connect(2, TOKEN_2);
    assert.equal((await operator.send(T.metrics)).type, T.metrics | 0x8000);
    assert.equal((await first.send(T.metrics)).type, T.error, 'metrics answered the wrong account');
    assert.equal((await first.send(T.ping)).type, T.ping | 0x8000, 'ping is for everyone');

    first.close();
    await eventually(connect, 1, TOKEN_1, 'a closed session to be given back');
    await server.stop();
    clients.splice(0).forEach(client => client.close());
    cleanup(dir);

    dir = directory();
    server = await start(executable, dir, ['--max-origin-connections', '2']);
    const fromOrigin = connectTo(server);
    const holder = await fromOrigin(1, TOKEN_1);
    await fromOrigin(2, TOKEN_2);
    await assert.rejects(fromOrigin(3, '3'.repeat(32)));

    holder.close();
    await eventually(fromOrigin, 3, '3'.repeat(32), 'a closed connection to be given back');
    console.log('PASS account and origin quotas, metrics authorization, and giving them back');
  } finally {
    clients.forEach(client => client.close());
    await server?.stop();
    cleanup(dir);
  }
}

const parseLines = text => text.trim().split('\n').map(line => JSON.parse(line));

// The journal alone answers what the server no longer can, and what it rebuilds has to be
// the same state the server held: ownership, retry records and retained events included.
async function theJournalAloneRebuildsTheHistory() {
  const dir = directory();
  const clients = [];
  let server;
  try {
    const inspect = (...extra) => parseLines(execFileSync(tool, ['inspect', journal(dir), path.join(dir, 'accounts.conf'), ...extra], {encoding: 'utf8', stdio: 'pipe'}));
    server = await start(executable, dir);
    const buyer = await Client.connect(server.port);
    clients.push(buyer);
    await buyer.login(1, TOKEN_1);
    const seller = await Client.connect(server.port);
    clients.push(seller);
    await seller.login(2, TOKEN_2);

    const resting = await buyer.submit(1, 1, 10, 1, 90, 5);
    assert.equal(resting.status, 'accepted');
    const crossing = await seller.submit(1, 1, 20, 2, 90, 3);
    assert.equal(crossing.filled, 3n);
    // Now the first outcome is no longer the last, and the server has forgotten it.
    await buyer.submit(2, 1, 11, 1, 80, 1);
    assert.equal((await buyer.submit(1, 1, 10, 1, 90, 5)).status, 'stale');

    // Beside the running writer, without disturbing it.
    const live = inspect('--account', '1');
    assert.deepEqual(live.filter(line => line.type === 'outcome').map(line => [line.request_sequence, line.order, line.status]),
      [['1', '10', 'accepted'], ['2', '11', 'accepted']]);
    assert.deepEqual(live.filter(line => line.type === 'execution').map(line => [line.maker_account, line.taker_account, line.price, line.quantity]),
      [['1', '2', '90', '3']], 'a resting order filled by someone else is this account\'s execution too');
    assert.equal((await buyer.send(T.ping)).type, T.ping | 0x8000);

    assert.equal(inspect('--account', '3').filter(line => line.type === 'execution').length, 0);

    const before = fs.readFileSync(journal(dir));
    const summary = inspect().at(-1);
    assert.equal(summary.type, 'inspect');
    assert.equal(summary.records, 3);
    assert.deepEqual(fs.readFileSync(journal(dir)), before, 'inspecting changed the journal');

    clients.forEach(client => client.close());
    await server.stop(process.platform === 'win32' ? 'SIGKILL' : 'SIGTERM');
    if (process.platform !== 'win32') {
      const stopped = await server.waitFor('stopped', 1000);
      assert.equal(summary.state_hash, stopped.state_hash, 'the journal rebuilt a different state than the server held');
    }

    server = await start(executable, dir);
    assert.equal(server.ready.state_hash, summary.state_hash, 'a restart rebuilt a different state than inspect');

    // Changing an account's limits changes what the journal means, and has to be refused.
    await server.stop();
    const accounts = path.join(dir, 'accounts.conf');
    const original = fs.readFileSync(accounts, 'utf8');
    fs.writeFileSync(accounts, original.replace('1000000000', '999'));
    assert.throws(() => inspect(), /different account configuration/);
    fs.writeFileSync(accounts, original.replace('1'.repeat(32), 'f'.repeat(32)));
    assert.equal(inspect().at(-1).state_hash, summary.state_hash, 'rotating a token changed the replayed state');
    console.log('PASS the journal alone rebuilds outcomes, executions and the exact state');
  } finally {
    clients.forEach(client => client.close());
    await server?.stop();
    cleanup(dir);
  }
}

await aStuckWriterDoesNotSilenceHealth();
await aReachableAddressHasToBeMeant();
await quotasAreEnforcedAndGivenBack();
await theJournalAloneRebuildsTheHistory();
if (process.platform === 'win32') {
  // Windows has no SIGTERM to catch, since kill() there is unconditional, and no ulimit.
  console.log('SKIP signal and descriptor limit cases on Windows');
} else {
  await aSignalStopsItWithClientsConnected();
  await runningOutOfDescriptorsIsSurvivable();
}
