#!/usr/bin/env python3
"""Scale / resource smoke test shaped like the formal performance workload:
10,000 rows x 128-dim vectors loaded in batches of 100, then TopK queries.
Reports wall time and peak RSS so we can see the memory budget is respected.
"""
import json
import os
import random
import resource
import shutil
import subprocess
import sys
import time

BIN = os.environ.get("MVD_BIN", "./build/minivectordb")
DATA = os.environ.get("MVD_DATA", "/tmp/mvd-data")
DIM = 128
ROWS = 10000
BATCH = 100
QUERIES = 200
K = 10


class Proc:
    def __init__(self, data_dir):
        self.p = subprocess.Popen([BIN, "--data-dir", data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                  start_new_session=True)
        line = self.p.stdout.readline()
        assert json.loads(line) == {"type": "ready"}, line
        self.seq = 0

    def sql(self, statement, mode="exact"):
        self.seq += 1
        self.p.stdin.write(json.dumps(
            {"id": self.seq, "op": "execute", "sql": statement, "search_mode": mode}) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def shutdown(self):
        self.seq += 1
        self.p.stdin.write(json.dumps({"id": self.seq, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        resp = json.loads(self.p.stdout.readline())
        self.p.stdin.close()
        self.p.wait(timeout=60)
        return resp


def vec(rng):
    return "[" + ",".join(f"{rng.uniform(-1, 1):.5f}" for _ in range(DIM)) + "]"


def main():
    for d in (DATA, DATA + "-b"):
        shutil.rmtree(d, ignore_errors=True)
    os.makedirs(DATA)
    rng = random.Random(1234)

    p = Proc(DATA)
    assert p.sql(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), "
                 f"content VARCHAR(64), embedding VECTOR({DIM}));").get("ok")

    t0 = time.time()
    for start in range(0, ROWS, BATCH):
        values = []
        for i in range(start, start + BATCH):
            values.append(f"({i}, 'g{i % 7}', 'doc {i}', '{vec(rng)}')")
        r = p.sql("INSERT INTO docs VALUES " + ",".join(values) + ";", mode="exact")
        if not r.get("ok"):
            print("insert failed:", r)
            return 1
    t_write = time.time() - t0

    # a few filtered and unfiltered TopK queries
    t0 = time.time()
    total = 0
    for q in range(QUERIES):
        probe = vec(rng)
        if q % 2 == 0:
            sql = (f"SELECT id, L2_DISTANCE(embedding, '{probe}') AS d FROM docs "
                   f"WHERE category = 'g{q % 7}' ORDER BY d LIMIT {K};")
        else:
            sql = (f"SELECT id, L2_DISTANCE(embedding, '{probe}') AS d FROM docs "
                   f"ORDER BY d LIMIT {K};")
        r = p.sql(sql, mode="exact")
        if not r.get("ok") or len(r.get("rows", [])) != K:
            print("query failed:", r, sql[:80])
            return 1
        total += len(r["rows"])
    t_query = time.time() - t0

    # restart and confirm persistence
    p.shutdown()
    p = Proc(DATA)
    r = p.sql("SELECT id FROM docs ORDER BY id LIMIT 1;")
    ok_rows = r.get("rows") == [[0]]
    cnt = p.sql("SELECT id FROM docs ORDER BY id DESC LIMIT 1;")
    ok_last = cnt.get("rows") == [[ROWS - 1]]
    p.shutdown()

    size = sum(os.path.getsize(os.path.join(DATA, f)) for f in os.listdir(DATA))
    peak_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss

    print(f"rows                : {ROWS} x {DIM}-dim")
    print(f"write time          : {t_write:.2f}s  ({ROWS / t_write:.0f} rows/s)")
    print(f"query time          : {t_query:.2f}s  ({QUERIES / t_query:.0f} q/s, {total} rows)")
    print(f"data dir size       : {size / 1048576:.1f} MiB")
    print(f"peak child RSS      : {peak_kb / 1024:.1f} MiB")
    print(f"persist after restart: first={ok_rows} last={ok_last}")
    return 0 if (ok_rows and ok_last) else 1


if __name__ == "__main__":
    sys.exit(main())
