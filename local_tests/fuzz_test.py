#!/usr/bin/env python3
"""Randomised differential test: drive MiniVectorDB with a random workload and
compare every answer against an independent Python model of the required
semantics.

Only SQL that the task *requires* is generated, so any mismatch is a bug in the
database (or in the model).  Restarts and SIGKILLs are interleaved so ordering,
tie-breaking, filter, transaction and durability behaviour are all exercised.

usage: mvd_fuzz.py [iterations] [seed ...]
"""
import copy
import json
import math
import os
import random
import shutil
import signal
import struct
import subprocess
import sys

BIN = os.environ.get("MVD_BIN", "./build/minivectordb")
DATA = os.environ.get("MVD_DATA", "/tmp/mvd-data")
TOL = 1e-5

DIM = 3
PROBES = ["[0,0,0]", "[1,0,0]", "[0.5,0.5,0.5]", "[-1,2,0]"]


def f32(x):
    return struct.unpack("f", struct.pack("f", float(x)))[0]


def dist(a, b):
    return math.sqrt(sum((f32(x) - f32(y)) ** 2 for x, y in zip(a, b)))


# --------------------------------------------------------------- DB driver

class Db:
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.seq = 0
        self.spawn()

    def spawn(self):
        self.p = subprocess.Popen([BIN, "--data-dir", self.data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                  start_new_session=True)
        line = self.p.stdout.readline()
        if json.loads(line) != {"type": "ready"}:
            raise RuntimeError(f"bad handshake: {line!r}")

    def sql(self, statement, mode="exact"):
        self.seq += 1
        self.p.stdin.write(json.dumps(
            {"id": self.seq, "op": "execute", "sql": statement, "search_mode": mode}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError(f"no response for {statement!r}")
        return json.loads(line)

    def restart(self):
        self.seq += 1
        self.p.stdin.write(json.dumps({"id": self.seq, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        resp = json.loads(self.p.stdout.readline())
        assert resp.get("ok") is True, resp
        self.p.stdin.close()
        self.p.wait(timeout=60)
        self.spawn()

    def kill(self):
        os.killpg(os.getpgid(self.p.pid), signal.SIGKILL)
        self.p.wait(timeout=30)
        self.spawn()


# ------------------------------------------------------------- the model

class Table:
    def __init__(self, name, has_vec):
        self.name = name
        self.has_vec = has_vec
        self.rows = {}  # pk -> [id, s, vec?]


class Model:
    """Mirrors the documented semantics: int64 keys, code-point string order,
    ties broken by ascending primary key, filtered TopK inside the filtered
    set, and one explicit transaction at a time."""

    def __init__(self):
        self.tables = {}
        self.saved = None

    # ---- transactions
    def begin(self):
        self.saved = copy.deepcopy({k: t.rows for k, t in self.tables.items()})

    def commit(self):
        self.saved = None

    def rollback(self):
        if self.saved is not None:
            for k, rows in self.saved.items():
                if k in self.tables:
                    self.tables[k].rows = rows
        self.saved = None

    def crash(self):
        """A kill keeps committed state only: the current rows already are the
        committed state, the saved snapshot is what a rollback would restore."""
        self.saved = None

    # ---- helpers
    def cmp(self, a, b):
        return (a > b) - (a < b)

    def matches(self, row, table, where):
        for col, op, val in where:
            if col == "id":
                lhs, rhs = row[0], val
            elif col == "s":
                lhs, rhs = row[1], val
            else:
                raise AssertionError("bad where column " + col)
            c = self.cmp(lhs, rhs)
            ok = {"=": c == 0, "<": c < 0, "<=": c <= 0, ">": c > 0, ">=": c >= 0}[op]
            if not ok:
                return False
        return True


# ------------------------------------------------------------ generators

class Workload:
    def __init__(self, rng):
        self.rng = rng
        self.model = Model()
        self.log = []

    def rand_s(self):
        alphabet = "abcXYZ09"
        return "".join(self.rng.choice(alphabet) for _ in range(self.rng.randint(1, 4)))

    def rand_vec(self):
        return [f32(round(self.rng.uniform(-2, 2), 3)) for _ in range(DIM)]

    def next_pk(self, table, taken=None):
        used = set(table.rows)
        if taken is not None:
            used |= taken
        for _ in range(200):
            cand = self.rng.randint(1, 60)
            if cand not in used:
                return cand
        return None

    # each op returns (sql_text, expected_response_or_None, verify_callable)
    def op_create(self):
        name = "t%d" % self.rng.randint(1, 3)
        if name in self.model.tables:
            return None
        has_vec = self.rng.random() < 0.6
        t = Table(name, has_vec)
        self.model.tables[name] = t
        cols = f"id BIGINT PRIMARY KEY, s VARCHAR(16)" + (f", v VECTOR({DIM})" if has_vec else "")
        return f"CREATE TABLE {name} ({cols});", {}, None

    def op_insert(self):
        names = [n for n, t in self.model.tables.items()]
        if not names:
            return None
        name = self.rng.choice(names)
        t = self.model.tables[name]
        if len(t.rows) > 40:
            return None
        n = self.rng.randint(1, 3)
        pks, vals, taken = [], [], set()
        for _ in range(n):
            pk = self.next_pk(t, taken)
            if pk is None:
                break
            taken.add(pk)
            s = self.rand_s()
            v = self.rand_vec() if t.has_vec else None
            pks.append(pk)
            vals.append((pk, s, v))
        if not vals:
            return None
        for pk, s, v in vals:
            t.rows[pk] = [pk, s, v] if t.has_vec else [pk, s]
        parts = []
        for pk, s, v in vals:
            if t.has_vec:
                parts.append(f"({pk},'{s}','{json.dumps(v)}')")
            else:
                parts.append(f"({pk},'{s}')")
        return f"INSERT INTO {name} VALUES {','.join(parts)};", {"affected_rows": len(vals)}, None

    def op_update(self):
        names = list(self.model.tables)
        if not names:
            return None
        name = self.rng.choice(names)
        t = self.model.tables[name]
        if not t.rows:
            return None
        where = self.rand_where(t)
        targets = [pk for pk, row in t.rows.items() if self.model.matches(row, t, where)]
        sets = []
        new_s = {} 
        for pk in targets:
            new_s[pk] = self.rand_s()
        if self.rng.random() < 0.5 and t.has_vec:
            for pk in targets:
                pass
        if not targets:
            return None
        # build SET clauses
        assign_s = self.rng.random() < 0.7
        assign_v = t.has_vec and self.rng.random() < 0.5
        if not assign_s and not assign_v:
            assign_s = True
        if assign_s:
            # use a single literal for all matched rows to keep the SQL simple
            sval = self.rand_s()
            sets.append(f"s='{sval}'")
            for pk in targets:
                t.rows[pk][1] = sval
        if assign_v:
            vval = self.rand_vec()
            sets.append(f"v='{json.dumps(vval)}'")
            for pk in targets:
                t.rows[pk][2] = vval
        clauses = self.where_sql(where)
        return (f"UPDATE {name} SET {','.join(sets)}{clauses};",
                {"affected_rows": len(targets)}, None)

    def op_delete(self):
        names = list(self.model.tables)
        if not names:
            return None
        name = self.rng.choice(names)
        t = self.model.tables[name]
        where = self.rand_where(t)
        targets = [pk for pk, row in t.rows.items() if self.model.matches(row, t, where)]
        if not targets:
            return None
        for pk in targets:
            del t.rows[pk]
        return (f"DELETE FROM {name}{self.where_sql(where)};",
                {"affected_rows": len(targets)}, None)

    def rand_where(self, t):
        conds = []
        mode = self.rng.choice(["none", "id", "s", "both"])
        if mode in ("id", "both"):
            op = self.rng.choice(["=", "<", "<=", ">", ">="])
            conds.append(("id", op, self.rng.randint(0, 62)))
        if mode in ("s", "both"):
            conds.append(("s", "=", self.rand_s()))
        return conds

    def where_sql(self, where):
        if not where:
            return ""
        parts = []
        for col, op, val in where:
            parts.append(f"{col} {op} " + (f"'{val}'" if col == "s" else str(val)))
        return " WHERE " + " AND ".join(parts)

    def op_select(self):
        names = list(self.model.tables)
        if not names:
            return None
        name = self.rng.choice(names)
        t = self.model.tables[name]
        where = self.rand_where(t)
        selected = [row for row in t.rows.values() if self.model.matches(row, t, where)]

        out_items = []  # (sql text, output column name, value function)
        use_dist = t.has_vec and bool(selected) and self.rng.random() < 0.6
        probe = None
        if use_dist:
            probe = json.loads(self.rng.choice(PROBES))
            out_items.append(("id", "id", lambda r: r[0]))
            out_items.append((f"L2_DISTANCE(v,'{json.dumps(probe)}') AS d", "d",
                              lambda r: dist(r[2], probe)))
        else:
            cols = ["id", "s"]
            self.rng.shuffle(cols)
            for c in cols[:self.rng.randint(1, 2)]:
                out_items.append((c, c, (lambda cc: (lambda r: r[0] if cc == "id" else r[1]))(c)))

        order = None
        if self.rng.random() < 0.75:
            choices = ["id", "s"] + (["d"] if use_dist else [])
            order = (self.rng.choice(choices), self.rng.choice(["ASC", "DESC"]))
        limit = self.rng.choice([None, None, 1, 2, 5])

        def order_value(row):
            col = order[0]
            if col == "id":
                return row[0]
            if col == "s":
                return row[1]
            return dist(row[2], probe)

        rows = list(selected)
        if order is not None:
            # Ascending by key with the primary key as the tie break; for DESC a
            # stable reverse sort keeps the pk ascending inside equal keys,
            # which is what the task requires.
            rows.sort(key=lambda r: (order_value(r), r[0]))
            if order[1] == "DESC":
                rows.sort(key=lambda r: order_value(r), reverse=True)
        if limit is not None:
            rows = rows[:limit]

        projected = lambda r: [f(r) for _, _, f in out_items]
        exp = {"columns": [nm for _, nm, _ in out_items]}
        if order is None and limit is not None:
            # No ORDER BY plus LIMIT: which rows come back is unspecified, so
            # only the count and membership are meaningful.
            exp["subset"] = True
            exp["unordered"] = True
            exp["row_count"] = len(rows)
            exp["candidates"] = [projected(r) for r in selected]
        else:
            exp["rows"] = [projected(r) for r in rows]
            if order is None:
                exp["unordered"] = True
        sql = "SELECT " + ", ".join(s for s, _, _ in out_items) + f" FROM {name}" \
              + self.where_sql(where)
        if order is not None:
            sql += f" ORDER BY {order[0]} {order[1]}"
        if limit is not None:
            sql += f" LIMIT {limit}"
        return sql + ";", exp, None

    def op_transaction(self, n_inner=3):
        names = list(self.model.tables)
        if not names:
            return None
        self.model.begin()
        acts = []
        for _ in range(self.rng.randint(1, n_inner)):
            fn = self.rng.choice([self.op_insert, self.op_update, self.op_delete])
            r = fn()
            if r is None:
                continue
            acts.append(r)
        rollback = self.rng.random() < 0.5
        if rollback:
            self.model.rollback()
            tail = "ROLLBACK;"
        else:
            self.model.commit()
            tail = "COMMIT;"
        return ("txn", ("BEGIN;", {}), acts, (tail, {}))


def close(a, b):
    if isinstance(a, float) or isinstance(b, float):
        return isinstance(a, (int, float)) and isinstance(b, (int, float)) \
            and math.isclose(a, b, rel_tol=TOL, abs_tol=TOL)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(close(x, y) for x, y in zip(a, b))
    return type(a) is type(b) and a == b


def compare(resp, expected, label, statements):
    if expected is None:
        return None
    if resp.get("ok") is not True:
        return f"{label}: statement failed {json.dumps(resp)[:200]} :: {statements}"
    for key in ("affected_rows", "columns"):
        if key in expected and not close(resp.get(key), expected[key]):
            return f"{label}: {key} {resp.get(key)!r} != {expected[key]!r} :: {statements}"
    if expected.get("subset"):
        got = resp.get("rows")
        if not isinstance(got, list):
            return f"{label}: missing rows :: {statements}"
        if len(got) != expected["row_count"]:
            return f"{label}: expected {expected['row_count']} rows, got {got!r} :: {statements}"
        pool = list(expected["candidates"])
        for row in got:
            idx = next((i for i, c in enumerate(pool) if close(c, row)), None)
            if idx is None:
                return f"{label}: row {row!r} is not a candidate :: {statements}"
            pool.pop(idx)
        return None
    if "rows" in expected:
        got, want = resp.get("rows"), expected["rows"]
        if not isinstance(got, list):
            return f"{label}: missing rows :: {statements}"
        if expected.get("unordered"):
            pool = list(got)
            for row in want:
                idx = next((i for i, c in enumerate(pool) if close(c, row)), None)
                if idx is None:
                    return f"{label}: missing row {row!r} in {got!r} :: {statements}"
                pool.pop(idx)
            if pool:
                return f"{label}: unexpected rows {pool[:4]!r} :: {statements}"
        elif not close(got, want):
            return f"{label}: rows {got!r} != {want!r} :: {statements}"
    return None


def verify_all(db, model, label):
    """Full state comparison, ordered by primary key."""
    for name, t in model.tables.items():
        cols = "id, s" + (", v" if t.has_vec else "")
        resp = db.sql(f"SELECT {cols} FROM {name} ORDER BY id;")
        want = []
        for pk in sorted(t.rows):
            row = t.rows[pk]
            want.append([row[0], row[1]] + ([row[2]] if t.has_vec else []))
        if resp.get("ok") is not True:
            return f"{label}: full scan of {name} failed {json.dumps(resp)[:200]}"
        got = resp.get("rows", [])
        if len(got) != len(want):
            return f"{label}: {name} has {len(got)} rows, model has {len(want)}"
        for g, w in zip(got, want):
            if not close(g, w):
                return f"{label}: {name} row {g!r} != model {w!r}"
    return None


def run_seed(seed, iterations=140, verbose=False):
    rng = random.Random(seed)
    data = f"{DATA}-{seed}"
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(data)
    db = Db(data)
    wl = Workload(rng)
    history = []
    try:
        for step in range(iterations):
            roll = rng.random()
            produced = None
            if roll < 0.12:
                produced = wl.op_create()
            elif roll < 0.34:
                produced = wl.op_insert()
            elif roll < 0.48:
                produced = wl.op_update()
            elif roll < 0.58:
                produced = wl.op_delete()
            elif roll < 0.72:
                produced = wl.op_transaction()
            else:
                produced = wl.op_select()
            if produced is None:
                continue

            if produced[0] == "txn":
                _, begin, acts, tail = produced
                history.append(begin[0])
                resp = db.sql(begin[0])
                problem = compare(resp, begin[1], f"seed{seed} step{step}", begin[0])
                if problem:
                    return problem + "\n   history: " + " | ".join(history[-25:])
                for sql, exp, _ in acts:
                    history.append(sql)
                    problem = compare(db.sql(sql), exp, f"seed{seed} step{step}", sql)
                    if problem:
                        return problem + "\n   history: " + " | ".join(history[-25:])
                history.append(tail[0])
                resp = db.sql(tail[0])
                problem = compare(resp, tail[1], f"seed{seed} step{step}", tail[0])
                if problem:
                    return problem + "\n   history: " + " | ".join(history[-25:])
            else:
                sql, exp, _ = produced
                history.append(sql)
                resp = db.sql(sql)
                problem = compare(resp, exp, f"seed{seed} step{step}", sql)
                if problem:
                    return problem + "\n   history: " + " | ".join(history[-25:])

            if step % 17 == 16 and step > 0:
                problem = verify_all(db, wl.model, f"seed{seed} step{step} (pre-restart)")
                if problem:
                    return problem + "\n   history: " + " | ".join(history[-25:])
                if rng.random() < 0.5:
                    db.kill()          # uncommitted work is already gone in the model
                    wl.model.crash()
                else:
                    db.restart()
                problem = verify_all(db, wl.model, f"seed{seed} step{step} (post-restart)")
                if problem:
                    return problem + "\n   history: " + " | ".join(history[-25:])

        problem = verify_all(db, wl.model, f"seed{seed} final")
        if problem:
            return problem
        return None
    finally:
        try:
            db.p.stdin.close()
            db.p.wait(timeout=10)
        except Exception:
            try:
                os.killpg(os.getpgid(db.p.pid), signal.SIGKILL)
            except Exception:
                pass
        shutil.rmtree(data, ignore_errors=True)


def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 140
    seeds = [int(x) for x in sys.argv[2:]] or list(range(1, 11))
    bad = 0
    for seed in seeds:
        problem = run_seed(seed, iterations)
        if problem is None:
            print(f"seed {seed}: OK")
        else:
            bad += 1
            print(f"seed {seed}: MISMATCH\n   {problem}")
    print()
    print(f"fuzz: {len(seeds) - bad}/{len(seeds)} seeds clean")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
