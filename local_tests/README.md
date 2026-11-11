# Local validation scripts

These cover what the shipped `protocol_check/` public cases do **not**: the
formal task keeps 9 of its 29 cases hidden and uses exactly the behaviours
listed in `../mysqltest/README.md` ("正式验收要求仍然公开").

Prerequisites: Python 3.9+, the built binary, and an empty data directory.

```bash
cmake -S minivectordb -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
export MVD_BIN="$PWD/build/minivectordb"

python3 local_tests/crash_test.py     # SIGKILL: committed survives, uncommitted rolls back, torn log
python3 local_tests/harden_test.py    # invalid vectors, atomicity, recovery, large data, ann/index
python3 local_tests/scale_test.py     # 10k x 128-dim load + TopK timing and peak RSS
```

`MVD_DATA` overrides the data directory (default `/tmp/mvd-data`).

## What `harden_test.py` asserts

| Section | Assertions |
| --- | --- |
| invalid vectors | NaN, `Infinity`, `-inf`, wrong dimension and float32 overflow are each rejected; the rejected statement leaves the table unchanged; a finite boundary value (`3.4e38`) is accepted; a bad probe vector in `L2_DISTANCE` is rejected |
| statement atomicity | a duplicate primary key inside a batch or against an existing row inserts nothing at all; a failing statement inside a transaction does not poison the transaction; `ROLLBACK` undoes every write; `VARCHAR(n)` accepts n characters and rejects n+1 |
| recovery | the catalog is checkpointed during recovery (so a second kill cannot lose the DDL); committed rows including updates survive SIGKILL; an uncommitted transaction vanishes; a torn or CRC-corrupt log tail is ignored |
| large data | 200,000 rows x 128-dim (~100 MiB of vectors) load, filter, TopK and restart, with peak RSS far below the 2 GiB budget |
| ann / index | after `CREATE VECTOR INDEX` with interleaved insert/update/delete, both `exact` and `ann`, filtered and unfiltered, return exactly the brute-force TopK with unique, live, filter-respecting ids; a rolled-back transaction is not visible to an `ann` query |
| spec details | unicode character counting, code point ordering, int64 round-trip including `INT64_MIN`, `LIMIT`/`OFFSET`, `ORDER BY ... DESC`, zero-match `UPDATE`/`DELETE`, and loud failures for unsupported statements |
