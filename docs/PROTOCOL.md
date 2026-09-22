# MDX1 binary protocol

TCP/IPv4. All wire integers are unsigned big-endian unless noted. A connection first sends Hello and is then bound to that account. Responses follow request order on that connection. Concurrent connections are serialized by acquisition of the state mutex; there is no cross-connection arrival-time guarantee.

## Framing

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | ASCII MDX1 |
| 4 | 2 | version = 1 |
| 6 | 2 | type |
| 8 | 4 | payload bytes, at most 65,536 |
| 12 | 4 | reserved = 0 |

Normal response type is request type OR 0x8000. Error type 65535 carries one u64: 1 unsupported message; 2 authentication required/rejected. Invalid framing, invalid lengths/enums or a deadline close the connection. Such protocol failures do not consume a request sequence. Business rejections are ordinary Submit outcomes.

The frame deadline covers header plus payload, without resetting per byte. Writes have a separate deadline. Idle connections also expire, so send Ping to retain an otherwise idle session. Defaults: 5 seconds, 32 clients, 65,536 active orders. Server options allow 100..30,000 ms, 1..128 clients and 1..1,000,000 orders. A client above the connection cap is closed.

## Payloads

Every listed field is u64 (8 bytes), except the 32 ASCII bytes of the Hello token. There is no padding or C++ struct serialization. Signed price uses the two's-complement int64 bit pattern; accepted prices are positive.

| Type | Request | Response |
|---|---|---|
| 1 Hello | account, token[32] | account, last_request, global_sequence, event_sequence, Balance, Quote |
| 2 Submit | request_sequence, kind, order_id, side, price, quantity | Outcome |
| 3 Account | empty | same fields as Hello, current values |
| 4 Events | after_event_sequence, limit (1..256) | gap, latest, Quote, count, Event[count] |
| 5 Book | offset, version, limit (1..256) | changed, version, total, offset, count, Order[count] |
| 6 Ping | empty | empty |
| 7 Metrics | empty | connections, rejected_connections, frames, auth_failures, disconnect_errors, responses, global_sequence |

Balance: cash, inventory, reserved_cash, reserved_inventory, buy_quantity, open_notional, halted (0/1).

Quote: bid_price, ask_price, bid_quantity, ask_quantity. An empty side has price/quantity zero. Quantities aggregate the best price level, not the entire book.

Order: id, side, price, remaining_quantity.

Outcome (19 integers): global_sequence, request_sequence, code, remaining, filled, executions, cancelled, event_sequence, Balance, Quote.

Event (11 integers): event_sequence, global_sequence, type, maker_id, taker_id, execution_price, execution_quantity, Quote. Type 1 is a trade (Quote fields zero); type 2 is top-of-book (trade fields zero). Every fresh request emits a final type-2 event, including business rejections. Trades precede that event. The feed is public within the authenticated demo accounts; it is not private execution reporting.

## Submit rules and codes

Kinds: New=1, Cancel=2, Kill=3, Resume=4. Side: Buy=1, Sell=2. New uses positive id/price/quantity. Cancel uses id, side=1, price=quantity=0. Kill/Resume require id=price=quantity=0 and side=1. Business validation rejects noncanonical values without mutating orders.

| Code | Meaning | Code | Meaning |
|---:|---|---:|---|
| 0 | accepted | 11 | halted |
| 1 | cancelled | 12 | order_limit |
| 2 | invalid | 13 | funds |
| 3 | duplicate_order | 14 | inventory |
| 4 | unknown_order | 15 | exposure |
| 5 | capacity | 16 | position |
| 6 | stale | 17 | overflow |
| 7 | gap | 18 | self_trade |
| 8 | conflict | 19 | killed |
| 9 | unknown_account | 20 | resumed |
| 10 | not_owner | | |

UnknownAccount is a state-machine result; the gateway rejects an unknown account during authentication before Submit. Global_sequence is the order's logged position for a fresh/cached outcome; sequence-error outcomes report the current global sequence.

## Retry contract

Per-account sequence starts at 1. The next sequence is journaled even when risk or matching rejects it. Read-only queries do not advance it. An identical request at the last sequence returns the **original** Outcome, including its historical balance/quote. Use Account for current balances. A changed payload at that sequence returns Conflict; an older/zero sequence returns Stale; a future skipped sequence returns Gap. These results are not journaled and do not mutate state.

Only the last exact outcome per account is cached, reconstructed by deterministic replay. To recover an uncertain response, reconnect and resend the same sequence and payload before issuing the next request on that account. Pipelining is permitted, but older outcomes cannot be retrieved after a later request commits. The load tool deliberately pipelines without retries; an error invalidates that experiment's success count.

## Feed recovery and book pagination

Poll Events starting after the last event actually consumed, then advance to the last returned event's sequence, **not** latest when the page is incomplete. The ring retains 8,192 events. A stale or future cursor returns gap=1, no events, and a Quote snapshot at latest. Replace your top-of-book state with that quote and resume after latest. This recovers current top-of-book, not missing historical trades or full depth.

Book provides full resting depth. Start with offset=0, version=0; subsequent pages use the returned version and advance offset by count. The returned version identifies an immutable snapshot, so later mutations do not invalidate its remaining pages. Snapshots expire five seconds after capture or earlier under the global 64 MiB / 32-version retention budget. If the requested snapshot has expired or been evicted, changed=1 and count=0; restart pagination. The first request for a new version copies the book once under the state owner; continuation pages copy only their returned orders. Captures should still be rate-limited in a hostile environment. There is no pushed multicast or full-depth incremental feed.
