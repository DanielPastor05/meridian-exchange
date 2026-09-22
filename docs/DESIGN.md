# Design and failure contract

## Scope

One instrument, one writer, limit orders with price-time priority. No account
state or network transport. Commands are the only source of book mutations.
Prices are signed 64-bit positive integer ticks; IDs and quantities are unsigned
64-bit. Quantity is reduced using subtraction; the core does not multiply price
by quantity or sum quantities across the book, avoiding hidden notional overflow.

## Book layout

Bids and asks each use an ordered map from price to a FIFO head/tail pair. The
FIFO links are indices into a fixed-size vector of nodes. Recycling a slot adds
the new order at the tail; the slot number never determines time priority.

A hash map maps active IDs to slots. Cancellation finds the order, looks up its
price level, unlinks it and returns the slot to the free stack. Removing the last
order at a level removes the level itself. The book never remains crossed after
an accepted command.

For L price levels and M makers matched:

- Resting insertion: O(log L), plus expected O(1) ID-index work.
- Cancellation: expected O(1) ID lookup plus O(log L) level lookup.
- Matching: best-level access and FIFO consumption, with tree maintenance as
  levels are erased; O(M log L) is a conservative upper bound.
- Snapshot: O(number of resting orders), ordered bids descending then asks
  ascending, FIFO within a level. It is outside timed matching work.

The reference stores arrival-ordered orders in a vector and scans all candidates
for each execution. It shares the value types and contract but none of the
optimized data structures or validation implementation.

## Admission and identity

Validation, duplicate-ID detection and capacity admission precede matching. An
incoming order must find a free slot, even when it might execute completely. This
avoids performing trades before discovering that a remainder cannot be stored.
The policy intentionally sacrifices admission opportunities at full capacity.

IDs identify live orders, not requests. Reusing a retired ID creates a new order.
Consumers requiring retry deduplication need a separate session/request sequence
protocol, including persistence of its outcomes.

## Journal format v1

All multibyte fields are little-endian, encoded explicitly; no C++ object layouts
are serialized. Prices retain their signed 64-bit bit pattern. CRC is CRC-32/IEEE
(reflected polynomial 0xedb88320, initial/final XOR 0xffffffff).

| Header field | Bytes |
|---|---:|
| Magic `MRDNJNL1` | 8 |
| Format version, 1 | 4 |
| Order capacity | 8 |
| CRC of preceding 20 bytes | 4 |

| Command record field | Bytes |
|---|---:|
| Sequence, starting at 1 | 8 |
| Kind: new=1, cancel=2 | 1 |
| Side: buy=1, sell=2 | 1 |
| Reserved, zero | 6 |
| Order ID | 8 |
| Signed price ticks | 8 |
| Quantity | 8 |
| CRC of preceding 40 bytes | 4 |

Cancel commands use side=buy, price=0, quantity=0 as their canonical encoding.
Parsed engine-invalid values such as price=0 are still journaled and replay as
rejections. Unknown enum encodings are storage errors. Engine semantics and the
journal version must evolve together: changing matching/admission policy requires
a format/engine-version migration before reusing old journals.

An incomplete final record has 1..43 bytes. A read-only scan reports it. A writer
opening the file validates every complete record, truncates only that tail and
resumes at the next sequence. CRC mismatch in a complete record never triggers
automatic truncation. Missing records at the end cannot be detected without an
independent persisted high-water mark; sequence checks detect gaps inside a file.

The writer holds an OS-level exclusion lock (POSIX flock or Windows sharing
denial). On POSIX it is advisory: every writer must cooperate. A read-only replay
requires a stopped writer or an immutable copy. There is no live snapshot protocol.
Sync-mode startup also synchronizes the parent directory on POSIX, so a new
journal's name is covered. On Windows the file is committed through the CRT;
filesystem metadata and device behavior bound the power-loss guarantee. The local
tests exercise file operations and truncation, not physical power failure.

## Failure timeline

| Failure point | Recovery meaning |
|---|---|
| Before journal append | Command is absent |
| During an incomplete final append | Tail is reported/discarded on recovery |
| Complete append, before response | Command may be committed; replay applies it |
| After a sync-mode response | File sync completed before the response |
| After a buffered-mode response | OS has the write; power-loss durability is not promised |
| Full-record checksum mismatch | Stop and report corruption |

The file is the source of truth. Disk failures and allocation exceptions stop
the session; callers must not catch an engine exception and continue using a
potentially partially modified in-memory book. The CLI exits and a restart
reconstructs state from the journal.

The state fingerprint is stable FNV-1a over the sequence and canonical order
snapshot, using eight little-endian bytes per field. It is a debugging aid, not a
substitute for comparing actual executions/state or a cryptographic guarantee.

## Performance boundary

`Engine::apply` is the timed boundary. It includes matching, container work and
trade-result writes. Engine construction, workload generation, journal I/O,
parsing and snapshots are excluded. The benchmark reserves its trade buffer for
the known synthetic workload, while normal callers may allocate to grow theirs.

There is no CPU pinning or scheduling isolation in the portable harness. Tail
measurements can include OS scheduling and clock overhead. Closed-loop input
does not reveal queueing latency under overload. A network/open-loop benchmark
is a separate milestone, not a claim supported by these measurements.
