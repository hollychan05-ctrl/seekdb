#!/usr/bin/env python3
"""Forced-termination recovery checks for MiniVectorDB.

The public protocol checker only exercises clean shutdown; the formal task also
kills the process with SIGKILL.  This script covers:
  1. committed data survives SIGKILL,
  2. an in-flight (uncommitted) transaction leaves no partial effect,
  3. a torn tail in wal.log does not break recovery.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import time

BIN = os.environ.get("MVD_BIN", "./build/minivectordb")
DATA = os.environ.get("MVD_DATA", "/tmp/mvd-data")


class Proc:
    def __init__(self, data_dir):
        # start_new_session: killing the child's process group must not take
        # the harness (or this script) down with it.
        self.p = subprocess.Popen([BIN, "--data-dir", data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                  start_new_session=True)
        line = self.p.stdout.readline()
        assert json.loads(line) == {"type": "ready"}, f"bad handshake: {line!r}"
        self.seq = 0

    def sql(self, statement, mode="exact"):
        self.seq += 1
        req = {"id": self.seq, "op": "execute", "sql": statement, "search_mode": mode}
        self.p.stdin.write(json.dumps(req) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        assert line, f"no response for: {statement}"
        return json.loads(line)

    def shutdown(self):
        self.seq += 1
        self.p.stdin.write(json.dumps({"id": self.seq, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        resp = json.loads(self.p.stdout.readline())
        self.p.stdin.close()
        self.p.wait(timeout=20)
        return resp

    def kill(self):
        os.killpg(os.getpgid(self.p.pid), signal.SIGKILL)
        self.p.wait(timeout=20)


def check(name, ok):
    print(("PASS  " if ok else "FAIL  ") + name)
    return ok


def main():
    shutil.rmtree(DATA, ignore_errors=True)
    os.makedirs(DATA)
    failures = 0

    # ---- 1. committed data survives SIGKILL
    p = Proc(DATA)
    p.sql("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(32), v VECTOR(2));")
    p.sql("INSERT INTO t VALUES (1,'a','[1,2]'),(2,'b','[3,4]');")
    p.sql("UPDATE t SET s='bb' WHERE id=2;")
    p.sql("DELETE FROM t WHERE id=1;")
    p.kill()
    p = Proc(DATA)
    r = p.sql("SELECT id, s FROM t ORDER BY id;")
    failures += not check("committed rows survive SIGKILL",
                          r.get("ok") and r.get("rows") == [[2, "bb"]])
    r = p.sql("SELECT id, L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d;")
    failures += not check("vector still readable after SIGKILL",
                          r.get("ok") and r.get("rows") == [[2, 5.0]])
    p.shutdown()

    # ---- 2. uncommitted transaction must be fully rolled back on SIGKILL
    p = Proc(DATA)
    p.sql("BEGIN;")
    p.sql("INSERT INTO t VALUES (3,'c','[1,1]');")
    p.sql("DELETE FROM t WHERE id=2;")
    p.kill()
    p = Proc(DATA)
    r = p.sql("SELECT id FROM t ORDER BY id;")
    failures += not check("uncommitted txn has no partial effect",
                          r.get("ok") and r.get("rows") == [[2]])
    p.shutdown()

    # ---- 3. torn tail in wal.log must be ignored
    p = Proc(DATA)
    p.sql("INSERT INTO t VALUES (7,'g','[7,7]');")
    p.kill()
    with open(os.path.join(DATA, "wal.log"), "ab") as f:
        f.write(b"\x40\x00\x00\x00\xde\xad\xbe\xefgarbage-not-a-record")
    p = Proc(DATA)
    r = p.sql("SELECT id FROM t ORDER BY id;")
    failures += not check("torn wal tail does not break recovery",
                          r.get("ok") and r.get("rows") == [[2], [7]])
    p.shutdown()

    # ---- 4. rollback then commit in one session
    p = Proc(DATA)
    p.sql("BEGIN;")
    p.sql("INSERT INTO t VALUES (8,'h','[8,8]');")
    p.sql("ROLLBACK;")
    p.sql("BEGIN;")
    p.sql("INSERT INTO t VALUES (9,'i','[9,9]');")
    p.sql("COMMIT;")
    r = p.sql("SELECT id FROM t ORDER BY id;")
    failures += not check("rollback then commit",
                          r.get("ok") and r.get("rows") == [[2], [7], [9]])
    p.shutdown()

    print()
    print("crash tests: " + ("ALL PASS" if failures == 0 else f"{failures} FAILURE(S)"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
