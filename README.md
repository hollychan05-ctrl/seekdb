# MiniVectorDB (2026-competition-round2)

A single-process, single-connection vector database for the round-2 tiebreak
task.  It speaks the JSON-Lines protocol over stdin/stdout described in
[`STDIO_PROTOCOL.md`](STDIO_PROTOCOL.md): it prints `{"type":"ready"}` after
recovering `/data`, then answers one request per line.

## Build and run

```bash
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

Or without Docker:

```bash
cmake -S minivectordb -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/minivectordb --data-dir /tmp/mvd
```

## Self-test

The public package ships with a protocol checker.  From this directory:

```bash
export VECTORDB_IMAGE=minivectordb:latest
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --handshake-only --output "protocol-results/handshake-$(date +%Y%m%d-%H%M%S)"
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --output "protocol-results/public-$(date +%Y%m%d-%H%M%S)"
```

## Supported SQL

| Statement | Notes |
| --- | --- |
| `CREATE TABLE t (...)` | `BIGINT`, `VARCHAR(n)`, `VECTOR(n)`; exactly one `BIGINT PRIMARY KEY`, at most one vector column |
| `CREATE VECTOR INDEX i ON t(v)` | accepted; retrieval stays exact, which also satisfies the recall requirement |
| `DROP TABLE [IF EXISTS] t` | |
| `INSERT INTO t [(cols)] VALUES (...), (...)` | multi-row, statement-atomic |
| `SELECT items FROM t [WHERE ...] [ORDER BY ...] [LIMIT n [OFFSET m]]` | `*`, plain columns, `L2_DISTANCE(col,'[...]') AS d` |
| `UPDATE t SET c=v [, ...] [WHERE ...]` | primary key cannot be updated |
| `DELETE FROM t [WHERE ...]` | |
| `BEGIN` / `COMMIT` / `ROLLBACK` | one explicit transaction at a time |

Anything else (for example `ALTER TABLE`) returns `unsupported_sql`.  Ties in
`ORDER BY` are broken by ascending primary key.  Filtered Top-K is computed
inside the filtered set, never by taking K rows first.

## Storage and durability

Data lives in `/data`:

| File | Purpose |
| --- | --- |
| `catalog.json` | checkpoint of the table definitions |
| `t<oid>.dat` | slotted-page heap file, one per table (8 KiB pages) |
| `wal.log` | redo log of committed page images plus a commit marker |

A transaction keeps the pages it touched pinned in the page cache, so an
uncommitted page never reaches the heap file; `ROLLBACK` simply drops those
pages.  `COMMIT` appends the changed page images and a commit marker to
`wal.log`, fsyncs, and only then lets the pages be written back.  Recovery
replays committed page images (idempotent) and then checkpoints.  A clean
shutdown flushes the heaps, rewrites `catalog.json` and truncates the log.

The page cache is bounded (96 MiB) and evicts least-recently-used pages, so
scans and recovery do not depend on the whole data set fitting in memory.

## Known limits

- Retrieval is an exact scan; `CREATE VECTOR INDEX` is recorded but no
  approximate index is maintained (exact results also satisfy `ann` recall).
- No `NULL`, `JOIN`, aggregation, sub-queries, `ALTER TABLE`, or foreign keys.
- Deleted rows leave holes; space is reused for same-page appends only.
