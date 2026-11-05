# Operations runbook

Meridian is an engineering simulator with one serialized book. This page says how to run it, what its failures look like and what to do about them. It does not turn it into a venue: see [the security boundary](DESIGN.md#resource-and-security-boundary).

## Starting

```bash
exchange_server --journal exchange.journal --accounts accounts.conf
```

It prints one JSON line when ready, including the port, the replayed sequence and the `state_hash` of the replayed state.

| Option | Default | Use |
|---|---|---|
| `--host` | 127.0.0.1 | Any other address is refused without `--allow-remote`. |
| `--allow-remote` | off | Accepts a reachable address. The protocol has no TLS, so tokens cross the network in the clear: put an independently secured tunnel in front. Account files whose tokens are one repeated character, like the demo's, are still refused. |
| `--max-clients` | 32 | Connections served at once, 1..128. |
| `--max-account-sessions` | off | Connections one account may hold. A login past it gets error 3 and is closed. |
| `--max-origin-connections` | off | Connections one source address may hold, counted before login. Behind a proxy, every client shares the proxy's address. |
| `--metrics-account` | off | The only account allowed to read Metrics. Others get error 2 and keep their session. |
| `--timeout-ms` | 5000 | Frame, write and idle deadline, 100..30000. |
| `--durability` | sync | `buffered` can lose acknowledged requests in a crash. |

The journal is created with owner-only permissions on POSIX. Keep the account file private as well, since it holds the tokens.

## Health

Ping and Metrics do not take the book's lock, so they keep answering while a writer is stuck in `fsync`. Both need a logged-in session. That gives two different readings:

- **Ping fails:** the process is gone or its network is.
- **Ping answers, and `global_sequence` in Metrics stops advancing while Submit times out:** the writer is stuck, which usually means storage.

Metrics counters are process-lifetime: connections, rejected_connections, frames, auth_failures, disconnect_errors, responses, then global_sequence.

## When something goes wrong

| What you see | What it means | What to do |
|---|---|---|
| `fatal exchange failure: ...` on stderr, exit 1 | A journal write or an apply failed. The process stops rather than serve state it cannot make durable. | Fix the storage and restart. Replay rebuilds the state, and clients retry their last sequence with the same payload. |
| `accept resource pressure: N` on stderr | The process is out of file descriptors. It backs off, keeps serving existing clients and accepts again when descriptors return. | Raise the descriptor limit or lower `--max-clients`. Look for clients holding connections. |
| `journal checksum mismatch`, `journal sequence gap` or `invalid journal record encoding` at startup | The file is corrupt somewhere other than its last record. Nothing is repaired automatically. | Restore from backup. |
| `journal already has a writer` at startup | Another server holds this journal. | Stop it, or use another journal. |
| `journal account configuration changed` at startup | Balances or limits in the account file differ from those the journal was written under. | Restore the file. See [Changing configuration](#changing-configuration). |
| A torn final record | A crash during an append. Truncated on the next start, as designed. Nothing that was acknowledged in sync mode is lost. | Nothing. |

## Investigating

`exchange inspect` rebuilds history from the journal without a server. It writes nothing and takes no lock, so it is safe beside a running writer:

```bash
exchange inspect exchange.journal accounts.conf --account 1
```

One JSON line per outcome and per execution, then a summary with the record count and `state_hash`. This is the way to answer questions the running server cannot: it keeps only the last outcome per account, and its feed is a ring of 8,192 events. Executions include resting orders filled by another account's request.

A request that emits more events than the ring holds prints `executions_lost` rather than a silently shorter list.

## Backup and restore

The journal is append-only, so copying it at any moment gives a valid prefix, possibly with a torn last record. Back up the account file with it: a journal cannot be replayed under a different configuration.

```bash
cp exchange.journal accounts.conf /backup/
exchange inspect /backup/exchange.journal /backup/accounts.conf | tail -1
```

`inspect` checks every record's checksum and the accounting invariants, so that last line is the proof that the copy is usable.

To restore, stop the server, put both files back and start it. The `state_hash` on its `ready` line must equal the one from `inspect`.

**What this cannot detect:** a backup missing its newest valid records is a valid journal. If that matters, record `global_sequence` from Metrics somewhere else and compare.

## Changing configuration

- **Tokens** can be rotated: edit the file and restart. They are not part of what the journal records.
- **Balances and limits** cannot change under an existing journal, because replaying it under other numbers would produce a different history. The server refuses. Changing them means starting a new journal, which is a new ledger; there is no migration.

## Storage

Each fresh request adds 60 bytes, and nothing rotates or compacts the journal, so monitor the disk. Restart replays the whole file: a 20,000-record journal took about 76 ms on the measured machine, which does not predict a far larger one.

## Stopping

SIGINT and SIGTERM stop accepting, wake every connection and exit 0 with a `stopped` line carrying `state_hash`. On Windows, Ctrl+C does this, but `kill` from another process is unconditional. In either case a crash is safe: that is what the journal is for.
