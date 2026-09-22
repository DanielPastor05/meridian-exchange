import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {Client, T, ints, values} from '../tools/wire.mjs';
import {directory, cleanup, start} from './network_helpers.mjs';

// Independent representation and arbitrary-precision arithmetic. Reservations
// are derived from a vector of live orders, never from server events/results.
class Model {
  orders = [];
  global = 0n;
  events = 0n;
  accounts = new Map([1, 2, 3].map(id => [id, {cash: 1000n, inventory: 30n, last: 0n, halted: false}]));
  balance(id) {
    const account = this.accounts.get(id);
    const own = this.orders.filter(order => order.owner === id);
    return {
      cash: account.cash, inventory: account.inventory,
      reservedCash: own.filter(order => order.side === 1).reduce((sum, order) => sum + order.price * order.quantity, 0n),
      reservedInventory: own.filter(order => order.side === 2).reduce((sum, order) => sum + order.quantity, 0n),
      buyQuantity: own.filter(order => order.side === 1).reduce((sum, order) => sum + order.quantity, 0n),
      openNotional: own.reduce((sum, order) => sum + order.price * order.quantity, 0n),
      halted: account.halted ? 1n : 0n,
    };
  }
  quote() {
    const bids = this.orders.filter(order => order.side === 1);
    const asks = this.orders.filter(order => order.side === 2);
    const bid = bids.reduce((best, order) => order.price > best ? order.price : best, 0n);
    const ask = asks.reduce((best, order) => best === 0n || order.price < best ? order.price : best, 0n);
    return {bid, ask,
      bidQuantity: bids.filter(order => order.price === bid).reduce((sum, order) => sum + order.quantity, 0n),
      askQuantity: asks.filter(order => order.price === ask).reduce((sum, order) => sum + order.quantity, 0n)};
  }
  apply(owner, command) {
    const account = this.accounts.get(owner);
    const [sequence, kind, id, side, price, quantity] = command;
    assert.equal(sequence, account.last + 1n);
    const b = this.balance(owner);
    let code = 2n, remaining = 0n, filled = 0n, executions = 0n, cancelled = 0n;
    if (kind === 1) {
      const candidates = this.orders.filter(order => order.side !== side && (side === 1 ? order.price <= price : order.price >= price));
      // stable sorting preserves vector insertion order at equal prices.
      candidates.sort((a, other) => a.price === other.price ? 0 : (side === 1 ? a.price < other.price : a.price > other.price) ? -1 : 1);
      let toFill = quantity;
      const matches = [];
      for (const maker of candidates) {
        if (toFill === 0n) break;
        const amount = maker.quantity < toFill ? maker.quantity : toFill;
        matches.push({maker, amount});
        toFill -= amount;
      }
      const cost = price * quantity;
      if (id === 0n || price <= 0n || quantity === 0n) code = 2n;
      else if (this.orders.some(order => order.id === id)) code = 3n;
      else if (account.halted) code = 11n;
      else if (quantity > 20n) code = 12n;
      else if (cost > (1n << 63n) - 1n) code = 17n;
      else if (cost + b.openNotional > 1500n) code = 15n;
      else if (side === 1 && cost + b.reservedCash > b.cash) code = 13n;
      else if (side === 1 && quantity + b.inventory + b.buyQuantity > 50n) code = 16n;
      else if (side === 2 && quantity + b.reservedInventory > b.inventory) code = 14n;
      else if (matches.some(match => match.maker.owner === owner)) code = 18n;
      else if (this.orders.length === 16 && matches.length === 0) code = 5n;
      else {
        code = 0n;
        for (const {maker, amount} of matches) {
          const buyer = this.accounts.get(side === 1 ? owner : maker.owner);
          const seller = this.accounts.get(side === 2 ? owner : maker.owner);
          buyer.cash -= amount * maker.price;
          seller.cash += amount * maker.price;
          buyer.inventory += amount;
          seller.inventory -= amount;
          maker.quantity -= amount;
          filled += amount;
          executions++;
        }
        this.orders = this.orders.filter(order => order.quantity !== 0n);
        remaining = quantity - filled;
        if (remaining > 0n) this.orders.push({owner, id, side, price, quantity: remaining});
      }
    } else if (kind === 2) {
      const existing = this.orders.find(order => order.id === id);
      if (id === 0n) code = 2n;
      else if (!existing) code = 4n;
      else if (existing.owner !== owner) code = 10n;
      else { this.orders = this.orders.filter(order => order !== existing); code = 1n; cancelled = 1n; }
    } else {
      account.halted = kind === 3;
      code = kind === 3 ? 19n : 20n;
      if (account.halted) {
        cancelled = BigInt(this.orders.filter(order => order.owner === owner).length);
        this.orders = this.orders.filter(order => order.owner !== owner);
      }
    }
    account.last = sequence;
    this.global++;
    this.events += executions + 1n;
    return {globalSequence: this.global, requestSequence: sequence, code, remaining, filled, executions, cancelled,
      eventSequence: this.events, ...this.balance(owner), ...this.quote()};
  }
}

const executable = process.argv[2];
if (!executable) throw Error('server executable required');
for (const initialSeed of [0x20260922, 0xFACE1234]) {
  const dir = directory();
  const clients = [];
  let server;
  try {
    fs.writeFileSync(path.join(dir, 'accounts.conf'), [1, 2, 3].map(id => id + ' ' + String(id).repeat(32) + ' 1000 30 20 1500 50').join('\n') + '\n', {mode: 0o600});
    server = await start(executable, dir, ['--durability', 'buffered', '--capacity', '16', '--timeout-ms', '30000']);
    for (const id of [1, 2, 3]) {
      const client = await Client.connect(server.port, '127.0.0.1', 30000);
      await client.login(id, String(id).repeat(32));
      clients.push(client);
    }
    const model = new Model();
    let seed = initialSeed;
    const random = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return seed >>> 0; };
    let nextId = 1n;
    const seen = new Set();
    for (let step = 0; step < 2000; step++) {
      const owner = 1 + random() % 3;
      const sequence = model.accounts.get(owner).last + 1n;
      const choice = random() % 100;
      let command;
      if (choice < 65) {
        const duplicate = choice < 8 && model.orders.length > 0;
        const id = duplicate ? model.orders[random() % model.orders.length].id : nextId++;
        const price = choice === 63 ? (1n << 63n) - 1n : choice === 64 ? -1n : BigInt(20 + random() % 61);
        command = [sequence, 1, id, 1 + random() % 2, price, BigInt(random() % 25)];
      } else if (choice < 90) {
        const id = model.orders.length && choice < 87 ? model.orders[random() % model.orders.length].id : nextId + 100n;
        command = [sequence, 2, id, 1, 0n, 0n];
      } else command = [sequence, choice < 93 ? 3 : 4, 0n, 1, 0n, 0n];
      const expected = model.apply(owner, command);
      const actual = await clients[owner - 1].submit(...command);
      const {status, ...fields} = actual;
      seen.add(status);
      assert.deepEqual(fields, expected, 'independent model diverged at seed=' + initialSeed + ', step=' + step);
      if (step % 25 === 0) {
        for (const id of [1, 2, 3]) {
          const state = values((await clients[id - 1].send(T.account)).payload);
          assert.deepEqual(state.slice(4, 11), Object.values(model.balance(id)));
        }
        const page = values((await clients[0].send(T.book, ints(0, 0, 256))).payload);
        const expectedBook = [...model.orders].sort((a, b) => a.side !== b.side ? a.side - b.side : a.price === b.price ? 0 : (a.side === 1 ? a.price > b.price : a.price < b.price) ? -1 : 1);
        assert.deepEqual(page.slice(5), expectedBook.flatMap(order => [order.id, BigInt(order.side), order.price, order.quantity]));
      }
    }
    for (const status of ['accepted', 'cancelled', 'not_owner', 'funds', 'inventory', 'exposure', 'position', 'self_trade', 'halted', 'overflow'])
      assert(seen.has(status), 'seed failed to cover ' + status);
    console.log('PASS 2000 independent BigInt risk/matching/accounting transitions, seed=' + initialSeed);
  } finally {
    clients.forEach(client => client.close());
    await server?.stop();
    cleanup(dir);
  }
}
