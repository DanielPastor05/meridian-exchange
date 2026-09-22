import assert from 'node:assert/strict';
import {Client, T, values} from '../tools/wire.mjs';
import {directory, cleanup, start} from './network_helpers.mjs';
const executable = process.argv[2];
if (!executable) throw Error('server executable required');
const dir = directory();
const clients = [];
let server;
try {
  server = await start(executable, dir, ['--durability', 'sync']);
  for (let i = 0; i < 6; i++) {
    const client = await Client.connect(server.port);
    await client.login(1, '1'.repeat(32));
    clients.push(client);
  }
  const identical = await Promise.all(clients.map(client => client.submit(1, 1, 100, 1, 90, 1)));
  assert.equal(identical[0].status, 'accepted');
  assert.equal(identical[0].globalSequence, 1n);
  identical.forEach(result => assert.deepEqual(result, identical[0]));
  const conflicting = await Promise.all(clients.map((client, i) => client.submit(2, 1, 200 + i, 1, 91, 1)));
  assert.equal(conflicting.filter(result => result.status === 'accepted').length, 1);
  assert.equal(conflicting.filter(result => result.status === 'conflict').length, 5);
  assert(conflicting.every(result => result.globalSequence === 2n));
  // A lost reply after transmission is resolved using the same sequence/payload.
  const lost = clients[0].submit(3, 1, 300, 1, 92, 2).catch(() => null);
  clients[0].close();
  await lost;
  const recovery = await Client.connect(server.port);
  await recovery.login(1, '1'.repeat(32));
  clients.push(recovery);
  const recovered = await recovery.submit(3, 1, 300, 1, 92, 2);
  assert.equal(recovered.status, 'accepted');
  assert.equal(recovered.globalSequence, 3n);
  assert.deepEqual(await recovery.submit(3, 1, 300, 1, 92, 2), recovered);
  clients.forEach(client => client.close());
  await server.stop();
  server = await start(executable, dir);
  const restarted = await Client.connect(server.port);
  clients.push(restarted);
  await restarted.login(1, '1'.repeat(32));
  assert.deepEqual(await restarted.submit(3, 1, 300, 1, 92, 2), recovered);
  const account = values((await restarted.send(T.account)).payload);
  assert.equal(account[2], 3n);
  assert.equal(account[6], 90n + 91n + 2n * 92n);
  console.log('PASS same-account identical/conflicting concurrency, lost reply and exact restart retry');
} finally {
  clients.forEach(client => client.close());
  await server?.stop();
  cleanup(dir);
}
