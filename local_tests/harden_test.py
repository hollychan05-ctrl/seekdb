#!/usr/bin/env python3
"""Hardening tests for the MiniVectorDB formal requirements that the 20 public
cases do NOT cover.  The formal suite is 29 cases; the organisers keep 9 of them
hidden and use exactly these behaviours:

  1. reject NaN / Infinity / wrong dimension / float32 overflow,
  2. a failed statement must not take partial effect,
  3. committed data survives SIGKILL; a kill during/after recovery must still
     recover; a torn log tail is ignored,
  4. data larger than the page cache is handled without loading everything,
  5. an ANN/index query must honour filters, return valid unique ids and stay
     consistent with transactions.

Run: python3 _dev/mvd_harden_test.py
"""
import json
import math
import os
import random
import resource
import shutil
import signal
import subprocess
import sys
import time

BIN = os.environ.get("MVD_BIN", "./build/minivectordb")
DATA = os.environ.get("MVD_DATA", "/tmp/mvd-data")
CATALOG = os.path.join(DATA, "catalog.json")

FAILS = []
PASSES = [0]


def check(name, ok, detail=""):
    if ok:
        PASSES[0] += 1
        print(f"PASS  {name}")
    else:
        FAILS.append(name)
        print(f"FAIL  {name}   {detail}")
    return ok


class Proc:
    def __init__(self, data_dir):
        self.p = subprocess.Popen([BIN, "--data-dir", data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                  start_new_session=True)
        line = self.p.stdout.readline()
        if json.loads(line) != {"type": "ready"}:
            raise RuntimeError(f"bad handshake: {line!r}")
        self.seq = 0

    def sql(self, statement, mode="exact"):
        self.seq += 1
        self.p.stdin.write(json.dumps(
            {"id": self.seq, "op": "execute", "sql": statement, "search_mode": mode}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError(f"no response for {statement!r}")
        return json.loads(line)

    def ok(self, statement, mode="exact"):
        return self.sql(statement, mode)

    def shutdown(self):
        self.seq += 1
        self.p.stdin.write(json.dumps({"id": self.seq, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        resp = json.loads(self.p.stdout.readline())
        self.p.stdin.close()
        self.p.wait(timeout=60)
        return resp

    def kill(self):
        os.killpg(os.getpgid(self.p.pid), signal.SIGKILL)
        self.p.wait(timeout=30)


def is_err(resp):
    return resp.get("ok") is False and isinstance(resp.get("error", {}).get("code"), str) \
        and resp["error"]["code"] != ""


def vmhwm(pid):
    """Real peak RSS of the child.  getrusage(RUSAGE_CHILDREN) under-reports
    here, so read /proc/<pid>/status while the process is still alive."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) / 1024.0
    except OSError:
        pass
    return 0.0


def close(a, b, tol=1e-5):
    """Same semantics as the judge: floats compare with a tolerance, ints exactly."""
    if isinstance(a, float) or isinstance(b, float):
        return isinstance(a, (int, float)) and isinstance(b, (int, float)) \
            and math.isclose(a, b, rel_tol=tol, abs_tol=tol)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y, tol) for x, y in zip(a, b))
    return type(a) is type(b) and a == b


# ---------------------------------------------------------------- 1. vectors

def test_vectors():
    print("\n--- 1. invalid vectors")
    p = Proc(DATA)
    p.sql("DROP TABLE IF EXISTS vt;")
    p.sql("CREATE TABLE vt (id BIGINT PRIMARY KEY, v VECTOR(2));")
    p.sql("INSERT INTO vt VALUES (1,'[1,2]');")

    bad = {
        "nan": "'[NaN,1]'",
        "infinity": "'[Infinity,1]'",
        "neg-infinity": "'[-inf,1]'",
        "dimension": "'[1,2,3]'",
        "overflow": "'[1e40,0]'",
        "underflow-finite": "'[3.5e38,0]'",
    }
    for name, lit in bad.items():
        r = p.sql(f"INSERT INTO vt VALUES (2,{lit});")
        check(f"reject {name}", is_err(r), json.dumps(r)[:120])
        # the failed insert must not leave a row behind
    r = p.sql("SELECT id FROM vt ORDER BY id;")
    check("rejected inserts left the table unchanged", r.get("rows") == [[1]], r.get("rows"))

    # a bad vector in UPDATE must leave the row untouched and be atomically rejected
    r = p.sql("UPDATE vt SET v='[1,2,3]' WHERE id=1;")
    check("reject bad vector in UPDATE", is_err(r), json.dumps(r)[:120])
    r = p.sql("SELECT id, L2_DISTANCE(v,'[0,0]') AS d FROM vt ORDER BY d;")
    check("row unchanged after rejected UPDATE",
          r.get("ok") and close(r.get("rows"), [[1, math.sqrt(5)]]), r.get("rows"))

    # a probe vector of the wrong dimension must be rejected
    r = p.sql("SELECT id, L2_DISTANCE(v,'[1,2,3]') AS d FROM vt ORDER BY d LIMIT 1;")
    check("reject bad probe dimension", is_err(r), json.dumps(r)[:120])

    # a valid float32 boundary value is accepted
    r = p.sql("INSERT INTO vt VALUES (3,'[3.4e38,0]');")
    check("accept finite float32 value", r.get("ok") is True, json.dumps(r)[:120])
    p.sql("DROP TABLE vt;")
    p.shutdown()


# ------------------------------------------------------------- 2. atomicity

def test_atomicity():
    print("\n--- 2. statement atomicity")
    p = Proc(DATA)
    p.sql("CREATE TABLE at (id BIGINT PRIMARY KEY, s VARCHAR(8));")
    p.sql("INSERT INTO at VALUES (1,'a');")

    # duplicate inside one multi-row insert: nothing at all must be inserted
    r = p.sql("INSERT INTO at VALUES (2,'b'),(2,'c'),(3,'d');")
    check("reject duplicate inside a batch", is_err(r), json.dumps(r)[:120])
    r = p.sql("SELECT id FROM at ORDER BY id;")
    check("no partial rows from a rejected batch", r.get("rows") == [[1]], r.get("rows"))

    # duplicate against an existing row, after a valid leading row
    r = p.sql("INSERT INTO at VALUES (5,'e'),(1,'f');")
    check("reject duplicate against existing", is_err(r), json.dumps(r)[:120])
    r = p.sql("SELECT id FROM at ORDER BY id;")
    check("no partial rows (existing duplicate)", r.get("rows") == [[1]], r.get("rows"))

    # a failing statement inside a transaction must not poison the transaction
    p.sql("BEGIN;")
    p.sql("INSERT INTO at VALUES (7,'g');")
    r = p.sql("INSERT INTO at VALUES (8,'h'),(7,'i');")
    check("failing insert inside a txn is rejected", is_err(r), json.dumps(r)[:120])
    r = p.sql("COMMIT;")
    check("txn still commits after a failed statement", r.get("ok") is True, json.dumps(r)[:120])
    r = p.sql("SELECT id FROM at ORDER BY id;")
    check("failed statement left nothing, earlier one committed",
          r.get("rows") == [[1], [7]], r.get("rows"))

    # rollback must undo everything, including a statement that failed
    p.sql("BEGIN;")
    p.sql("INSERT INTO at VALUES (9,'j');")
    p.sql("UPDATE at SET s='zzz' WHERE id=1;")
    p.sql("ROLLBACK;")
    r = p.sql("SELECT id, s FROM at ORDER BY id;")
    check("rollback undoes all txn writes", r.get("rows") == [[1, "a"], [7, "g"]], r.get("rows"))

    # longer-than-column VARCHAR: measured in unicode characters
    r = p.sql("INSERT INTO at VALUES (10,'12345678');")
    check("VARCHAR(8) accepts 8 ascii chars", r.get("ok") is True, json.dumps(r)[:120])
    r = p.sql("INSERT INTO at VALUES (11,'123456789');")
    check("VARCHAR(8) rejects 9 ascii chars", is_err(r), json.dumps(r)[:120])
    p.sql("DROP TABLE at;")
    p.shutdown()


# -------------------------------------------------------------- 3. recovery

def test_recovery():
    print("\n--- 3. recovery")
    p = Proc(DATA)
    p.sql("DROP TABLE IF EXISTS rc;")
    p.sql("CREATE TABLE rc (id BIGINT PRIMARY KEY, v VECTOR(2));")
    p.sql("INSERT INTO rc VALUES (1,'[1,1]'),(2,'[2,2]');")
    p.sql("UPDATE rc SET v='[3,3]' WHERE id=1;")
    p.kill()

    # restart: recovery must checkpoint the catalog before reporting ready
    p = Proc(DATA)
    ok_catalog = False
    try:
        with open(CATALOG, "r", encoding="utf-8") as f:
            ok_catalog = "rc" in f.read()
    except OSError:
        ok_catalog = False
    check("catalog checkpointed during recovery", ok_catalog)
    r = p.sql("SELECT id, L2_DISTANCE(v,'[0,0]') AS d FROM rc ORDER BY id;")
    check("committed data survived SIGKILL (update included)",
          close(r.get("rows"), [[1, math.sqrt(18)], [2, math.sqrt(8)]]), r.get("rows"))
    # kill again *without* a clean shutdown: the recovered DDL must survive
    p.kill()

    p = Proc(DATA)
    r = p.sql("SELECT id FROM rc ORDER BY id;")
    check("table survives a second crash right after recovery",
          r.get("ok") and r.get("rows") == [[1], [2]], json.dumps(r)[:150])

    # an uncommitted transaction must vanish
    p.sql("BEGIN;")
    p.sql("INSERT INTO rc VALUES (3,'[9,9]');")
    p.sql("DELETE FROM rc WHERE id=1;")
    p.kill()
    p = Proc(DATA)
    r = p.sql("SELECT id FROM rc ORDER BY id;")
    check("uncommitted txn fully rolled back on SIGKILL", r.get("rows") == [[1], [2]], r.get("rows"))
    p.shutdown()

    # a torn record at the end of the log is ignored
    with open(os.path.join(DATA, "wal.log"), "ab") as f:
        f.write(b"\x40\x00\x00\x00\xde\xad\xbe\xefnot-a-record-at-all")
    p = Proc(DATA)
    r = p.sql("SELECT id FROM rc ORDER BY id;")
    check("torn log tail ignored", r.get("rows") == [[1], [2]], r.get("rows"))
    p.shutdown()

    # a record whose payload is intact but whose CRC is wrong is also ignored
    p = Proc(DATA)
    p.sql("INSERT INTO rc VALUES (4,'[4,4]');")
    p.kill()
    with open(os.path.join(DATA, "wal.log"), "r+b") as f:
        blob = f.read()
        if len(blob) > 12:
            f.seek(4)
            f.write(bytes([blob[4] ^ 0xFF]))
    p = Proc(DATA)
    r = p.sql("SELECT id FROM rc ORDER BY id;")
    check("corrupt mid-log record stops replay cleanly",
          r.get("ok") and isinstance(r.get("rows"), list), json.dumps(r)[:150])
    p.sql("DROP TABLE rc;")
    p.shutdown()


# ------------------------------------------------------------ 4. large data

def test_large():
    print("\n--- 4. data larger than the page cache")
    p = Proc(DATA)
    p.sql("DROP TABLE IF EXISTS big;")
    p.sql("CREATE TABLE big (id BIGINT PRIMARY KEY, s VARCHAR(16), v VECTOR(128));")
    vec = "[" + ",".join("0.5" for _ in range(128)) + "]"
    t0 = time.time()
    rows = 200000
    batch = 500
    for start in range(0, rows, batch):
        values = ",".join(f"({i},'s{i % 97}','{vec}')" for i in range(start, start + batch))
        r = p.sql("INSERT INTO big VALUES " + values + ";")
        if not r.get("ok"):
            check("bulk load", False, json.dumps(r)[:150])
            return
    t_load = time.time() - t0
    size_mb = sum(os.path.getsize(os.path.join(DATA, f)) for f in os.listdir(DATA)) / 1048576
    rss_mb = vmhwm(p.p.pid)
    check(f"loaded {rows} rows ({size_mb:.0f} MiB on disk) in {t_load:.1f}s", True)
    r = p.sql("SELECT COUNT(*) FROM big;")
    # COUNT(*) is not part of the required SQL subset; fall back to a full scan
    r = p.sql("SELECT id FROM big WHERE s = 's0' ORDER BY id LIMIT 3;")
    check("filtered query over the large table", r.get("ok") and len(r.get("rows", [])) == 3,
          json.dumps(r)[:150])
    r = p.sql(f"SELECT id, L2_DISTANCE(v,'{vec}') AS d FROM big ORDER BY d LIMIT 5;")
    check("vector TopK over the large table", r.get("ok") and len(r.get("rows", [])) == 5,
          json.dumps(r)[:150])
    p.shutdown()
    p = Proc(DATA)
    r = p.sql("SELECT id FROM big ORDER BY id DESC LIMIT 1;")
    check("large table survives restart", r.get("rows") == [[rows - 1]], json.dumps(r)[:150])
    print(f"      data dir {size_mb:.0f} MiB, peak child RSS {rss_mb:.0f} MiB")
    check("peak RSS stayed well under the 2 GiB budget", rss_mb < 600, f"{rss_mb:.0f} MiB")
    p.sql("DROP TABLE big;")
    p.shutdown()


# ----------------------------------------------------------------- 5. index

def test_index():
    print("\n--- 5. vector index / ann mode")
    p = Proc(DATA)
    rng = random.Random(7)
    rows = [(i, "keep" if i % 3 else "skip", [rng.uniform(-1, 1) for _ in range(4)])
            for i in range(1, 61)]
    p.sql("CREATE TABLE ix (id BIGINT PRIMARY KEY, category VARCHAR(8), v VECTOR(4));")
    p.sql("CREATE VECTOR INDEX ix_vec ON ix(v);")
    for i, cat, v in rows:
        p.sql(f"INSERT INTO ix VALUES ({i},'{cat}','{json.dumps(v)}');")
    # mutate after the index exists
    p.sql("UPDATE ix SET v='[0,0,0,0]' WHERE id=1;")
    p.sql("DELETE FROM ix WHERE id=2;")
    p.sql("INSERT INTO ix VALUES (61,'skip','[0,0,0,0]');")

    probe = [0.5, -0.25, 0.0, 0.75]
    live = []
    for i, cat, v in rows:
        if i == 2:
            continue  # deleted
        if i == 1:
            v = [0.0] * 4  # updated after the index was created
        live.append((i, cat, v))
    live.append((61, "skip", [0.0] * 4))
    cat_of = {i: c for i, c, _ in live}
    for mode in ("exact", "ann"):
        for filtered in (False, True):
            where = " WHERE category = 'keep'" if filtered else ""
            r = p.sql(f"SELECT id, L2_DISTANCE(v,'{json.dumps(probe)}') AS d FROM ix{where} "
                      f"ORDER BY d LIMIT 10;", mode=mode)
            candidate = [row for row in live if (not filtered or row[1] == "keep")]
            expected = sorted(
                ((i, math.dist(v, probe)) for i, _, v in candidate),
                key=lambda t: (t[1], t[0]))[:10]
            got = [(row[0], row[1]) for row in r.get("rows", [])]
            same = (len(got) == len(expected)
                    and all(g[0] == e[0] and math.isclose(g[1], e[1], rel_tol=1e-4, abs_tol=1e-4)
                            for g, e in zip(got, expected)))
            ids = [g[0] for g in got]
            check(f"{mode}{' filtered' if filtered else ''} TopK matches brute force",
                  same, f"got={got}\n                          want={expected}")
            check(f"{mode}{' filtered' if filtered else ''} ids unique, live and filtered",
                  len(set(ids)) == len(ids) and all(i in cat_of for i in ids)
                  and all((not filtered) or cat_of[i] == "keep" for i in ids))

    # an index must not break transaction semantics
    p.sql("BEGIN;")
    p.sql("UPDATE ix SET v='[9,9,9,9]' WHERE id=3;")
    p.sql("DELETE FROM ix WHERE id=4;")
    p.sql("ROLLBACK;")
    r = p.sql("SELECT id FROM ix WHERE id IN (3,4) ORDER BY id;")
    r = p.sql(f"SELECT id, L2_DISTANCE(v,'{json.dumps([9,9,9,9])}') AS d FROM ix ORDER BY d LIMIT 1;",
              mode="ann")
    check("ann query after ROLLBACK matches pre-transaction data",
          r.get("ok") and r["rows"][0][0] != 3, json.dumps(r)[:150])
    p.sql("DROP TABLE ix;")
    p.shutdown()


# ------------------------------------------------------------------ 6. misc

def test_misc():
    print("\n--- 6. spec details")
    p = Proc(DATA)
    p.sql("CREATE TABLE ms (id BIGINT PRIMARY KEY, s VARCHAR(4));")
    # unicode length is counted in characters, not bytes
    r = p.sql("INSERT INTO ms VALUES (1,'你好');")
    check("VARCHAR(4) accepts two 3-byte characters", r.get("ok") is True, json.dumps(r)[:120])
    r = p.sql("INSERT INTO ms VALUES (2,'你好世界');")
    check("VARCHAR(4) accepts four characters", r.get("ok") is True, json.dumps(r)[:120])
    r = p.sql("INSERT INTO ms VALUES (3,'你好世界啊');")
    check("VARCHAR(4) rejects five characters", is_err(r), json.dumps(r)[:120])
    # code point order
    p.sql("INSERT INTO ms VALUES (4,'a'),(5,'Z'),(6,'~');")
    r = p.sql("SELECT id, s FROM ms WHERE id >= 4 ORDER BY s;")
    check("ORDER BY uses code point order",
          r.get("rows") == [[5, "Z"], [4, "a"], [6, "~"]], r.get("rows"))
    # int64 extremes round-trip exactly
    p.sql("CREATE TABLE bm (id BIGINT PRIMARY KEY, n BIGINT);")
    lo, hi = -(2 ** 63), 2 ** 63 - 1
    p.sql(f"INSERT INTO bm VALUES (1,{lo}),(2,{hi}),(3,9007199254740993);")
    r = p.sql("SELECT id, n FROM bm ORDER BY id;")
    check("int64 extremes round-trip exactly",
          r.get("rows") == [[1, lo], [2, hi], [3, 9007199254740993]], r.get("rows"))
    # LIMIT / OFFSET (a non-key column, because updating the key is not required)
    p.sql("CREATE TABLE lm (id BIGINT PRIMARY KEY, s VARCHAR(4));")
    p.sql("INSERT INTO lm VALUES (1,'a'),(2,'a'),(3,'a'),(4,'a'),(5,'a');")
    r = p.sql("SELECT id FROM lm ORDER BY id LIMIT 2 OFFSET 2;")
    check("LIMIT/OFFSET", r.get("rows") == [[3], [4]], r.get("rows"))
    r = p.sql("SELECT id FROM lm ORDER BY id DESC LIMIT 2;")
    check("ORDER BY DESC with LIMIT", r.get("rows") == [[5], [4]], r.get("rows"))
    # affected_rows for no-op updates and deletes
    r = p.sql("UPDATE lm SET s='z' WHERE id=100;")
    check("UPDATE with no match reports 0", r.get("ok") and r.get("affected_rows") == 0,
          json.dumps(r)[:120])
    r = p.sql("UPDATE lm SET id=9 WHERE id=1;")
    check("updating the primary key is rejected, not silently applied", is_err(r),
          json.dumps(r)[:120])
    r = p.sql("SELECT id FROM lm ORDER BY id LIMIT 1;")
    check("rejected key update left the row intact", r.get("rows") == [[1]], r.get("rows"))
    r = p.sql("DELETE FROM lm WHERE id=100;")
    check("DELETE with no match reports 0", r.get("ok") and r.get("affected_rows") == 0,
          json.dumps(r)[:120])
    # unsupported statements must fail loudly
    for bad in ("ALTER TABLE lm ADD c BIGINT;", "CREATE INDEX i ON lm(id);",
                "TRUNCATE TABLE lm;"):
        r = p.sql(bad)
        check(f"unsupported rejected: {bad.split()[0]}", is_err(r), json.dumps(r)[:120])
    r = p.sql("SELECT id FROM lm ORDER BY id;")
    check("still healthy after unsupported statements", r.get("rows") == [[1], [2], [3], [4], [5]],
          r.get("rows"))
    p.shutdown()


def main():
    shutil.rmtree(DATA, ignore_errors=True)
    os.makedirs(DATA)
    test_vectors()
    test_atomicity()
    test_recovery()
    test_index()
    test_misc()
    test_large()
    print()
    print(f"passed {PASSES[0]}, failed {len(FAILS)}")
    for name in FAILS:
        print("  - " + name)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
