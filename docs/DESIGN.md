# Design and failure contract

## State ownership

Each connection has a bounded worker; a mutex serializes the matching book, account ledger, journal, retries and event ring. One request is processed at a time per connection. A response is written after releasing the state lock, so a slow socket cannot block all matching while waiting for TCP space. Other requests may commit before an earlier response reaches its client; global sequence records the actual mutation order.

The matching core is single-writer C++20 with ordered price maps, FIFO links in preallocated slots, a free-slot stack and an unordered ID index. Insert/cancel costs O(log L) price-level work plus expected O(1) ID lookup; matching consumes M makers with a conservative O(M log L) bound. Snapshot is O(N). Quote currently scans orders at the best levels; self-trade validation scans the makers a taker would reach. Containers and trade vectors still allocate. A failed allocation poisons the durable service and requires restart.

New GTC orders match best price, then FIFO; execution uses the maker's price. IDs are unique among active orders and reusable after retirement. A full book admits a crossing order: it either completes against a maker or frees a maker slot for its remainder. A noncrossing order at full capacity is rejected before mutation. There is no market order, amendment, auction or short selling.

## Risk and settlement

An account configuration line contains id, 32-character lowercase hex token, initial cash, initial inventory, maximum order quantity, maximum open notional, maximum long position. Cash is in tick-value units; no floating-point conversions occur. There are at most 128 accounts; total configured cash, inventory and position limits must fit signed int64. These aggregate bounds make settlement additions and best-level quantities safe in the account service.

Each order must fit the size limit and its price-times-quantity must fit signed int64. Admission conservatively checks current open notional plus the incoming order's entire limit notional. A buy reserves limit price times remaining quantity and pending buy position; a sell reserves remaining inventory. Fills transfer cash at maker price and units of inventory, then release exactly the executed maker reservation. A taker's unfilled remainder receives a new reservation; unused price improvement stays available. Cancels release the entire remaining reservation. Kill cancels all owned orders in ID order and halts new orders; Resume removes that halt. Risk rejects consume the fresh request sequence but have no trading effects.

If any maker the incoming order would execute belongs to the same account, the entire incoming order is rejected before any trade. Ownership is checked on cancellations. Runtime verification recomputes reservations from the book and checks cash/inventory conservation; a separate test ledger integrates executions independently.

## Journal v2

All journal fields are **little-endian**, unlike the network protocol. CRC-32/IEEE uses reflected polynomial 0xedb88320 and initial/final XOR 0xffffffff. The file holds input requests; outcomes, accounts, feed sequence and the last response cache are reconstructed by replay.

| Header field | Bytes |
|---|---:|
| magic MRDNJNL2 | 8 |
| version 2 | 4 |
| order capacity | 8 |
| account configuration fingerprint | 8 |
| CRC over previous 28 bytes | 4 |

| Record field | Bytes |
|---|---:|
| global sequence | 8 |
| account | 8 |
| per-account request sequence | 8 |
| kind, side | 1 + 1 |
| zero reserved bytes | 6 |
| order ID | 8 |
| signed price bit pattern | 8 |
| quantity | 8 |
| CRC over previous 56 bytes | 4 |

The header is 32 bytes; records are 60. Legacy CLI journals use configuration/account/request sequence zero and must not be mixed with server journals. Server configuration fingerprint is FNV-1a over sorted numeric financial account fields; tokens are excluded to allow rotation while stopped. It detects accidental configuration changes, not malicious tampering. CRC and diagnostic state hashes are likewise not cryptographic integrity mechanisms. Version 1 files are rejected; no automatic migration is supplied.

The writer holds an OS lock (Windows sharing denial or advisory POSIX flock). Only incomplete final records of 1..59 bytes are truncated; complete CRC corruption, internal sequence gaps or malformed encodings fail closed. Losing an entire valid suffix cannot be detected without an external high-water mark. The account configuration and journal must be backed up together. No automatic repair, replication or rolling upgrade is implemented.

Sync: append → fflush → fsync/_commit → apply → response. POSIX startup also fsyncs the containing directory. Buffered mode performs fflush without storage sync. After an append/apply failure, no more requests may execute until restart. Physical device and filesystem semantics bound power-loss durability; tests kill processes and do not cut power.

## Recovery boundaries

| External kill boundary | Required result |
|---|---|
| before_append | target absent; earlier acknowledgements survive |
| mid_append | incomplete tail discarded |
| after_write, before sync | target may be absent or replayed; retry must settle once |
| after_sync / after_apply / before_response | target replayed, exact retry outcome reconstructed |
| after_response | acknowledged outcome and accounting preserved |

The harness synchronizes with a test-only fault hook, then terminates the separate server process externally. The target crosses another account's resting sell, so it checks both cash/inventory settlement and reservation release after restart. The hook intentionally splits an append to expose a deterministic torn tail; normal append does not split. An ambiguous response is resolved by retrying its account sequence, not by generating a new order ID.

## Resource and security boundary

Threads, frame size, active order capacity, account count, event retention, event/book page size and socket deadlines are bounded. The OS owns its TCP queues. The journal is append-only and grows with fresh requests; operators must monitor disk space. Snapshot copies, response vectors and result buffers allocate within the configured workload bounds. Full risk/kill operations can hold the state lock longer than a simple matching command; this is not a hard real-time implementation.

Tokens are checked over all 32 bytes for known accounts, but there is no TLS, user provisioning, audit authorization model, secrets vault or abuse-resistant Internet perimeter. Use loopback or an independently secured trusted network. The public fixture tokens are never credentials for a real system.
