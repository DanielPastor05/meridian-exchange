// The server while something is going wrong around it: a writer that has stopped, a signal
// with clients still connected, and a process out of file descriptors.
import assert from 'node:assert/strict';
import {once} from 'node:events';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import {Client, T, values} from '../tools/wire.mjs';
import {cleanup, delay, directory, start} from './network_helpers.mjs';

const executable = process.argv[2];
if (!executable) throw Error('server executable required');

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
    server = await start(limited, dir, ['--max-clients', '128', '--timeout-ms', '60000']);

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

await aStuckWriterDoesNotSilenceHealth();
if (process.platform === 'win32') {
  // Windows has no SIGTERM to catch, since kill() there is unconditional, and no ulimit.
  console.log('SKIP signal and descriptor limit cases on Windows');
} else {
  await aSignalStopsItWithClientsConnected();
  await runningOutOfDescriptorsIsSurvivable();
}
