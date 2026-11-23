# Simple Key-Value Store (C)

A command-line key-value database using an append-only `data.db` log and a custom linked-list in-memory index.

## Requirements
- macOS with Xcode Command Line Tools (`xcode-select --install`)
- `make` and a C compiler (`clang` is included with the tools)

## Build and run

```sh
make
./kvstore
```

The program reads commands from standard input. Use `EXIT` or Ctrl-D to close it. By default it creates `data.db` in the current directory.

Use a separate database file:

```sh
./kvstore --db /path/to/data.db
```

## Commands

```text
SET key value
GET key
DEL key
EXISTS key
MSET key value [key value ...]
MGET key [key ...]
EXPIRE key seconds
TTL key
RANGE start end
BEGIN
COMMIT
ABORT
HSET hash field value
HGET hash field
HGETALL hash
LPUSH key value
RPUSH key value
LRANGE key start stop
INCR key
DECR key
FLUSHDB
EXIT
```

Values and keys are whitespace-free tokens. Output uses `(nil)` for missing scalar/hash values, `-2` for a missing key's TTL, and `-1` for a key with no expiration.

## Persistence model

Each mutating command is appended to the log and flushed immediately. On startup, the log is replayed to reconstruct the in-memory linked-list index. The latest scalar SET wins. Expiration is stored as an absolute Unix timestamp in the log.

Transactions buffer writes until COMMIT. ABORT reloads the last committed log state.

## Tests

```sh
make test
```

GitHub Actions builds the program and runs the test script on pushes and pull requests.

## Important implementation notes

This is an educational project, not a production database. The command format is whitespace-tokenized (no spaces inside keys or values). A transaction's commands are appended individually at COMMIT, so a power failure during the commit sequence can leave a partially committed transaction. The simple linked-list index is intentionally linear-time.
