import net from 'node:net';

export const T = {hello: 1, submit: 2, account: 3, events: 4, book: 5, ping: 6, metrics: 7, error: 65535};
export const codes = ['accepted', 'cancelled', 'invalid', 'duplicate_order', 'unknown_order', 'capacity', 'stale', 'gap', 'conflict',
  'unknown_account', 'not_owner', 'halted', 'order_limit', 'funds', 'inventory', 'exposure', 'position', 'overflow', 'self_trade', 'killed', 'resumed'];
const MAX_U64 = (1n << 64n) - 1n;
const MAX_I64 = (1n << 63n) - 1n;
const MAGIC = Buffer.from('MDX1');

function integer(value, minimum, maximum, label) {
  if (typeof value === 'number' && !Number.isSafeInteger(value)) throw Error(label + ' must be a safe integer or decimal string');
  if (typeof value !== 'number' && typeof value !== 'bigint' && (typeof value !== 'string' || !/^-?[0-9]+$/.test(value)))
    throw Error(label + ' must be an integer');
  const result = BigInt(value);
  if (result < minimum || result > maximum) throw Error(label + ' is out of range');
  return result;
}
export const u64 = value => integer(value, 0n, MAX_U64, 'uint64');
export const i64 = value => integer(value, -MAX_I64 - 1n, MAX_I64, 'int64');

export function ints(...input) {
  const payload = Buffer.alloc(input.length * 8);
  input.forEach((value, i) => payload.writeBigUInt64BE(u64(value), i * 8));
  return payload;
}
export function values(payload) {
  if (payload.length % 8) throw Error('unaligned response');
  return Array.from({length: payload.length / 8}, (_, i) => payload.readBigUInt64BE(i * 8));
}
export function frame(type, payload = Buffer.alloc(0)) {
  if (!Number.isInteger(type) || type < 0 || type > 65535) throw Error('invalid frame type');
  if (payload.length > 65536) throw Error('frame exceeds 64 KiB');
  const header = Buffer.alloc(16);
  MAGIC.copy(header);
  header.writeUInt16BE(1, 4);
  header.writeUInt16BE(type, 6);
  header.writeUInt32BE(payload.length, 8);
  return Buffer.concat([header, payload]);
}
export function hello(account, token) {
  if (!/^[0-9a-f]{32}$/.test(token)) throw Error('token must be 32 lowercase hex characters');
  return Buffer.concat([ints(account), Buffer.from(token)]);
}
export function request(sequence, kind, id = 0, side = 1, price = 0, quantity = 0) {
  const k = integer(kind, 1n, 4n, 'kind');
  const s = integer(side, 1n, 2n, 'side');
  return ints(sequence, k, id, s, BigInt.asUintN(64, i64(price)), quantity);
}
function check(condition, message) { if (!condition) throw Error(message); }
function quote(v, at) {
  check(v[at] <= MAX_I64 && v[at + 1] <= MAX_I64, 'invalid quote price');
  check((v[at] === 0n) === (v[at + 2] === 0n) && (v[at + 1] === 0n) === (v[at + 3] === 0n), 'invalid quote quantity');
  check(v[at] === 0n || v[at + 1] === 0n || v[at] < v[at + 1], 'crossed quote');
}
function balance(v, at) {
  check(v[at + 6] <= 1n, 'invalid halted flag');
  check(v[at + 2] <= v[at] && v[at + 3] <= v[at + 1], 'invalid reserved balance');
}
export function decodeOutcome(payload) {
  const v = values(payload);
  check(v.length === 19, 'invalid outcome length');
  check(v[2] < BigInt(codes.length), 'unknown outcome code');
  balance(v, 8);
  quote(v, 15);
  const names = ['globalSequence', 'requestSequence', 'code', 'remaining', 'filled', 'executions', 'cancelled', 'eventSequence',
    'cash', 'inventory', 'reservedCash', 'reservedInventory', 'buyQuantity', 'openNotional', 'halted', 'bid', 'ask', 'bidQuantity', 'askQuantity'];
  const outcome = Object.fromEntries(names.map((name, i) => [name, v[i]]));
  outcome.status = codes[Number(outcome.code)];
  return outcome;
}
function validateResponse(type, payload, pending) {
  const v = values(payload);
  if (type === T.error) {
    check(v.length === 1 && v[0] >= 1n && v[0] <= 2n, 'invalid error response');
    return;
  }
  check(type === (pending.type | 0x8000), 'response type does not match request');
  switch (pending.type) {
    case T.hello:
    case T.account:
      check(v.length === 15, 'invalid account response length');
      if (pending.account !== undefined) check(v[0] === pending.account, 'response account does not match request');
      balance(v, 4);
      quote(v, 11);
      break;
    case T.submit:
      decodeOutcome(payload);
      check(v[1] === pending.sequence, 'response sequence does not match request');
      break;
    case T.ping:
      check(v.length === 0, 'invalid ping response');
      break;
    case T.metrics:
      check(v.length === 7, 'invalid metrics response');
      break;
    case T.book: {
      check(v.length >= 5 && v[0] <= 1n && v[4] <= 256n && v.length === 5 + Number(v[4]) * 4, 'invalid book response');
      check(v[0] === 0n || v[4] === 0n, 'changed book contains orders');
      if (v[0] === 0n) check(v[3] + v[4] <= v[2], 'invalid book offset');
      for (let i = 5; i < v.length; i += 4)
        check(v[i] > 0n && (v[i + 1] === 1n || v[i + 1] === 2n) && v[i + 2] > 0n && v[i + 2] <= MAX_I64 && v[i + 3] > 0n, 'invalid book order');
      break;
    }
    case T.events: {
      check(v.length >= 7 && v[0] <= 1n && v[6] <= 256n && v.length === 7 + Number(v[6]) * 11, 'invalid events response');
      check(v[0] === 0n || v[6] === 0n, 'gap response contains events');
      quote(v, 2);
      let previous = pending.after ?? 0n;
      for (let i = 7; i < v.length; i += 11) {
        check(v[i] > previous && v[i] <= v[1], 'invalid event sequence');
        check(v[i + 2] === 1n || v[i + 2] === 2n, 'invalid event type');
        if (v[i + 2] === 1n) check(v[i + 3] > 0n && v[i + 4] > 0n && v[i + 5] > 0n && v[i + 5] <= MAX_I64 && v[i + 6] > 0n, 'invalid trade');
        quote(v, i + 7);
        previous = v[i];
      }
      break;
    }
    default: throw Error('unknown response type');
  }
}
export function json(value) { return JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v.toString() : v, 2); }

export class Client {
  constructor(socket, timeout = 5000) {
    this.socket = socket;
    this.timeout = timeout;
    this.pending = [];
    this.buffer = Buffer.alloc(0);
    this.failure = null;
    this.account = undefined;
    socket.setNoDelay(true);
    socket.on('data', chunk => {
      try {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        while (this.buffer.length >= 16) {
          check(this.buffer.subarray(0, 4).equals(MAGIC) && this.buffer.readUInt16BE(4) === 1 && this.buffer.readUInt32BE(12) === 0, 'invalid response header');
          const size = this.buffer.readUInt32BE(8);
          check(size <= 65536, 'oversized response');
          if (this.buffer.length < 16 + size) break;
          const type = this.buffer.readUInt16BE(6);
          const payload = Buffer.from(this.buffer.subarray(16, 16 + size));
          const pending = this.pending[0];
          check(pending !== undefined, 'unsolicited response');
          // Validate before removing the promise: failure must reject it too.
          validateResponse(type, payload, pending);
          this.buffer = this.buffer.subarray(16 + size);
          this.pending.shift();
          clearTimeout(pending.timer);
          pending.resolve({type, payload});
        }
      } catch (error) { this.fail(error); }
    });
    socket.on('error', error => this.fail(error));
    socket.on('close', () => this.fail(Error('connection closed')));
  }
  static async connect(port, host = '127.0.0.1', timeout = 5000) {
    const socket = net.createConnection({port, host});
    const client = new Client(socket, timeout);
    await new Promise((resolve, reject) => {
      const finish = error => {
        clearTimeout(timer);
        socket.off('connect', onConnect);
        socket.off('error', onError);
        socket.off('close', onClose);
        if (error) { socket.destroy(); reject(error); } else resolve();
      };
      const onConnect = () => finish();
      const onError = error => finish(error);
      const onClose = () => finish(Error('connection closed before connect'));
      const timer = setTimeout(() => finish(Error('connect deadline')), timeout);
      socket.once('connect', onConnect);
      socket.once('error', onError);
      socket.once('close', onClose);
    });
    return client;
  }
  fail(error) {
    if (!this.failure) this.failure = error;
    for (const pending of this.pending) { clearTimeout(pending.timer); pending.reject(error); }
    this.pending = [];
    this.socket.destroy();
  }
  send(type, payload = Buffer.alloc(0), fragment = 0) {
    if (this.failure) return Promise.reject(this.failure);
    if (this.pending.length >= 4096) return Promise.reject(Error('client pending limit'));
    const bytes = frame(type, payload);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.fail(Error('response deadline')), this.timeout);
      const pending = {resolve, reject, timer, type, account: this.account};
      if (type === T.hello && payload.length >= 8) pending.account = payload.readBigUInt64BE();
      if (type === T.submit && payload.length >= 8) pending.sequence = payload.readBigUInt64BE();
      if (type === T.events && payload.length >= 8) pending.after = payload.readBigUInt64BE();
      this.pending.push(pending);
      try {
        if (fragment > 0) {
          for (let i = 0; i < bytes.length; i += fragment) this.socket.write(bytes.subarray(i, i + fragment));
        } else this.socket.write(bytes);
      } catch (error) { this.fail(error); }
    });
  }
  async login(account, token, fragment = 0) {
    const reply = await this.send(T.hello, hello(account, token), fragment);
    if (reply.type === T.error) throw Error('authentication rejected');
    this.account = u64(account);
    return values(reply.payload);
  }
  async submit(...args) {
    const reply = await this.send(T.submit, request(...args));
    if (reply.type === T.error) throw Error('protocol rejected');
    return decodeOutcome(reply.payload);
  }
  close() { this.socket.destroy(); }
}
