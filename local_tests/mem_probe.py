#!/usr/bin/env python3
"""Measure the database's real peak RSS (VmHWM) while loading and querying a
data set several times larger than the page cache."""
import json
import os
import shutil
import subprocess
import sys
import time

BIN = os.environ.get("MVD_BIN", "./build/minivectordb")
DATA = os.environ.get("MVD_DATA", "/tmp/mvd-data")
ROWS = int(sys.argv[1]) if len(sys.argv) > 1 else 400000
DIM = 128


def vmhwm(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) / 1024.0  # MiB
    except OSError:
        pass
    return 0.0


def main():
    shutil.rmtree(DATA, ignore_errors=True)
    os.makedirs(DATA)
    p = subprocess.Popen([BIN, "--data-dir", DATA], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         text=True, bufsize=1, start_new_session=True)
    assert json.loads(p.stdout.readline()) == {"type": "ready"}
    seq = 0

    def sql(q):
        nonlocal seq
        seq += 1
        p.stdin.write(json.dumps({"id": seq, "op": "execute", "sql": q,
                                  "search_mode": "exact"}) + "\n")
        p.stdin.flush()
        return json.loads(p.stdout.readline())

    vec = "[" + ",".join("0.5" for _ in range(DIM)) + "]"
    sql(f"CREATE TABLE big (id BIGINT PRIMARY KEY, s VARCHAR(16), v VECTOR({DIM}));")
    t0 = time.time()
    for start in range(0, ROWS, 500):
        vals = ",".join(f"({i},'s{i % 97}','{vec}')" for i in range(start, start + 500))
        r = sql("INSERT INTO big VALUES " + vals + ";")
        assert r.get("ok"), r
    t_load = time.time() - t0
    during = vmhwm(p.pid)

    r = sql(f"SELECT id, L2_DISTANCE(v,'{vec}') AS d FROM big ORDER BY d LIMIT 10;")
    assert r.get("ok") and len(r["rows"]) == 10, r
    r = sql("SELECT id FROM big WHERE s = 's5' ORDER BY id LIMIT 10;")
    assert r.get("ok") and len(r["rows"]) == 10, r
    after_query = vmhwm(p.pid)

    size = sum(os.path.getsize(os.path.join(DATA, f)) for f in os.listdir(DATA)) / 1048576
    print(f"rows            : {ROWS} x {DIM}-dim")
    print(f"load time       : {t_load:.1f}s")
    print(f"data dir        : {size:.0f} MiB")
    print(f"VmHWM after load: {during:.0f} MiB")
    print(f"VmHWM after query: {after_query:.0f} MiB")

    seq += 1
    p.stdin.write(json.dumps({"id": seq, "op": "shutdown"}) + "\n")
    p.stdin.flush()
    p.stdout.readline()
    p.stdin.close()
    p.wait(timeout=60)
    shutil.rmtree(DATA, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
