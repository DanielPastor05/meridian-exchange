import assert from 'node:assert/strict';
import {EventEmitter} from 'node:events';
import {test} from 'node:test';
import {Client, T, ints, i64, u64, request, frame, values} from '../tools/wire.mjs';

class Socket extends EventEmitter {
  sent = [];
  destroyed = false;
  setNoDelay() {}
  write(bytes) { this.sent.push(Buffer.from(bytes)); return true; }
  destroy() { this.destroyed = true; }
}
function pair() { const socket = new Socket(); return {socket, client: new Client(socket, 100)}; }
const validAccount = () => ints(1, 0, 0, 0, 1000, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0);
const validOutcome = sequence => ints(1, sequence, 0, 0, 0, 0, 0, 1, 1000, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0);

test('unsigned/signed boundaries reject truncation and unsafe coercion', () => {
  for (const value of [-1, '-1', 1n << 64n, '18446744073709551617', 9007199254740992, NaN, Infinity, 1.5, '', ' ', '0x10', true, null, {}])
    assert.throws(() => u64(value));
  assert.equal(u64('18446744073709551615'), (1n << 64n) - 1n);
  assert.equal(i64('-9223372036854775808'), -(1n << 63n));
  assert.throws(() => i64(1n << 63n));
  assert.throws(() => i64(-(1n << 63n) - 1n));
  assert.throws(() => request('18446744073709551617', 1, 1, 1, 100, 1));
  assert.throws(() => request(1, 1, '18446744073709551617', 1, 100, 1));
  assert.throws(() => request(1, 1, 1, 1, 100, '18446744073709551617'));
  assert.throws(() => request(1, 999, 1, 1, 100, 1));
  assert.equal(values(request(1, 1, 1, 1, -1, 1))[4], (1n << 64n) - 1n);
});

test('login rejects wrong type, account, length and high-bit magic', async () => {
  const corrupt = frame(T.hello | 0x8000, validAccount());
  for (let i = 0; i < 4; i++) corrupt[i] |= 128;
  const wrongAccount = validAccount(); wrongAccount.writeBigUInt64BE(2n);
  for (const response of [frame(T.ping | 0x8000), frame(T.hello | 0x8000), frame(T.hello | 0x8000, wrongAccount), corrupt]) {
    const {socket, client} = pair();
    const result = client.login(1, '1'.repeat(32));
    socket.emit('data', response);
    await assert.rejects(result);
    assert.equal(socket.destroyed, true);
    assert.equal(client.pending.length, 0);
  }
});

test('submit rejects wrong correlation and unknown codes before resolving pending', async () => {
  const unknown = validOutcome(1); unknown.writeBigUInt64BE(999n, 16);
  const invalidFlag = validOutcome(1); invalidFlag.writeBigUInt64BE(2n, 14 * 8);
  for (const response of [frame(T.submit | 0x8000, validOutcome(999)), frame(T.submit | 0x8000, unknown), frame(T.submit | 0x8000, invalidFlag)]) {
    const {socket, client} = pair();
    const result = client.submit(1, 1, 10, 1, 100, 1);
    socket.emit('data', response);
    await assert.rejects(result);
    assert.equal(client.pending.length, 0);
  }
});

test('valid fragmented responses work at every boundary', async () => {
  const bytes = frame(T.hello | 0x8000, validAccount());
  for (let split = 0; split <= bytes.length; split++) {
    const {socket, client} = pair();
    const result = client.login(1, '1'.repeat(32));
    socket.emit('data', bytes.subarray(0, split));
    socket.emit('data', bytes.subarray(split));
    assert.equal((await result)[0], 1n);
    client.close();
  }
});

test('coalesced ordered replies and valid error frames', async () => {
  const {socket, client} = pair();
  const first = client.submit(1, 1, 10, 1, 100, 1);
  const second = client.submit(2, 2, 10);
  socket.emit('data', Buffer.concat([frame(T.submit | 0x8000, validOutcome(1)), frame(T.submit | 0x8000, validOutcome(2))]));
  assert.equal((await first).requestSequence, 1n);
  assert.equal((await second).requestSequence, 2n);
  const badLogin = client.login(1, 'f'.repeat(32));
  socket.emit('data', frame(T.error, ints(2)));
  await assert.rejects(badLogin, /authentication rejected/);
  client.close();
});

test('framing errors reject all in-flight requests', async () => {
  for (const alter of [bytes => bytes.writeUInt16BE(2, 4), bytes => bytes.writeUInt32BE(1, 12), bytes => bytes.writeUInt32BE(65537, 8)]) {
    const {socket, client} = pair();
    const first = client.send(T.ping);
    const second = client.send(T.ping);
    const response = frame(T.ping | 0x8000); alter(response);
    socket.emit('data', response);
    await assert.rejects(first);
    await assert.rejects(second);
    assert.equal(client.pending.length, 0);
  }
});
