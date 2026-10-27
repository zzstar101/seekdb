#!/usr/bin/env python3
"""Differential correctness checks for the FTS count-only / no-score paths.

The scoring optimisations in this branch narrow what the text-retrieval scan
projects: when the query needs no relevance score, the plan stops reading
token_cnt/doc_length and the scan no longer materialises row images. A mistake
there shows up either as a hard error (ERROR 4016 in an older attempt) or as a
silently wrong row count.

Rather than re-implement mysqltest, this script asks the same question twice:
once through the fast path, once through the path that still computes scores.
Both answers must agree. The shapes that previously broke (projection without a
score column, MATCH inside aggregates and predicates) are exercised explicitly.

Usage: replay_fts_regression.py [--host 127.0.0.1] [--port 2881]
                               [--user root] [--database fts_replay]
Exit code 0 == every check passed.
"""

import argparse
import sys

import pymysql

TERMS = [
    "alpha",
    "bravo",
    "charlie",
    "delta",
    "echo",
    "alpha bravo",
    "alpha alpha",
    "foxtrot",
    "alpha bravo charlie delta echo",
    "missingterm",
]

DDL = [
    "DROP DATABASE IF EXISTS {db}",
    "CREATE DATABASE {db}",
    "USE {db}",
    "SET NAMES utf8mb4",
    """CREATE TABLE docs (
         id INT PRIMARY KEY,
         body VARCHAR(200),
         FULLTEXT INDEX body_beng(body) WITH PARSER beng)""",
    """CREATE TABLE score_oracle (
         term VARCHAR(40), id INT, score DOUBLE, PRIMARY KEY(term, id))""",
]

# 1..600 rows, with independently known id sets per term: the boundaries at
# 255/256/257/512 are the batch edges the count-only iterator walks over.
INSERT_ROWS = """
INSERT INTO docs
SELECT n, CONCAT(IF(n <= 255, 'alpha alpha ', ''),
                 IF(n <= 256, 'bravo ', ''),
                 IF(n <= 257, 'charlie ', ''),
                 IF(n <= 512, 'delta ', ''),
                 'echo ', IF(MOD(n, 2) = 1, 'oddset', 'evenset'))
FROM (SELECT 1 + a.n + 10 * b.n + 100 * c.n AS n
      FROM digits a CROSS JOIN digits b CROSS JOIN digits c) numbers
WHERE n <= 600
"""


class Checker:
    def __init__(self):
        self.failures = []
        self.checked = 0

    def fail(self, name, detail):
        self.failures.append((name, detail))
        print("FAIL %s: %s" % (name, detail))

    def ok(self, name, detail=""):
        self.checked += 1
        print("PASS %s%s" % (name, (" (" + detail + ")") if detail else ""))


def query_one(cur, sql, args=None):
    cur.execute(sql, args or ())
    row = cur.fetchone()
    return None if row is None else row[0]


def check_counts(cur, chk):
    """Every term must count the same with and without relevance scoring."""
    for term in TERMS:
        count_only = query_one(
            cur, "SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(%s)", (term,)
        )
        scored = query_one(
            cur,
            "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST(%s) "
            "ORDER BY MATCH(body) AGAINST(%s) DESC) t",
            (term, term),
        )
        if count_only != scored:
            chk.fail("count matches scored path for %r" % term,
                     "count-only=%r scored=%r" % (count_only, scored))
        else:
            chk.ok("count matches scored path for %r" % term, "%d rows" % count_only)


def check_shapes(cur, chk):
    """The projection shapes that must not error out on the score-free path."""
    term = "alpha"
    try:
        cur.execute("SELECT id FROM docs WHERE MATCH(body) AGAINST(%s) LIMIT 5", (term,))
        rows = cur.fetchall()
        chk.ok("id-only projection", "%d rows" % len(rows))
    except Exception as exc:  # noqa: BLE001 - any error is the finding
        chk.fail("id-only projection", repr(exc))

    try:
        value = query_one(cur, "SELECT GROUP_CONCAT(id) FROM docs WHERE MATCH(body) AGAINST(%s)", (term,))
        ids = [] if not value else str(value).split(",")
        expected = query_one(cur, "SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(%s)", (term,))
        if len(ids) != expected:
            chk.fail("group_concat over matches", "%d ids vs %r matches" % (len(ids), expected))
        else:
            chk.ok("group_concat over matches", "%d ids" % len(ids))
    except Exception as exc:  # noqa: BLE001
        chk.fail("group_concat over matches", repr(exc))

    for fn in ("MIN", "MAX", "AVG"):
        try:
            value = query_one(
                cur,
                "SELECT %s(MATCH(body) AGAINST(%s)) FROM docs WHERE MATCH(body) AGAINST(%s)" % (fn, "%s", "%s"),
                (term, term),
            )
            if fn == "AVG" or (isinstance(value, (int, float)) and value > 0):
                chk.ok("%s over match score" % fn, repr(value))
            else:
                chk.fail("%s over match score" % fn, "expected a positive number, got %r" % (value,))
        except Exception as exc:  # noqa: BLE001
            chk.fail("%s over match score" % fn, repr(exc))

    try:
        value = query_one(
            cur,
            "SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(%s) > 0",
            (term,),
        )
        chk.ok("match score in a predicate", "%r rows" % value)
    except Exception as exc:  # noqa: BLE001
        chk.fail("match score in a predicate", repr(exc))

    # Shapes that must stay on the scored path: a residual predicate, a join,
    # a subquery, HAVING and COUNT(DISTINCT) all disqualify count-only.
    shapes = [
        ("residual predicate",
         "SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(%s) AND id > 100", (term,)),
        ("join",
         "SELECT COUNT(*) FROM docs d JOIN docs e ON d.id = e.id "
         "WHERE MATCH(d.body) AGAINST(%s)", (term,)),
        ("subquery",
         "SELECT COUNT(*) FROM (SELECT id FROM docs WHERE MATCH(body) AGAINST(%s)) t", (term,)),
        ("having",
         "SELECT id FROM docs WHERE MATCH(body) AGAINST(%s) GROUP BY id HAVING id > 0", (term,)),
        ("count distinct",
         "SELECT COUNT(DISTINCT id) FROM docs WHERE MATCH(body) AGAINST(%s)", (term,)),
    ]
    baseline = query_one(cur, "SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(%s)", (term,))
    for name, sql, args in shapes:
        try:
            cur.execute(sql, args)
            rows = cur.fetchall()
            if name in ("having",):
                if len(rows) != baseline:
                    chk.fail("%s keeps all matches" % name, "%d vs %d" % (len(rows), baseline))
                else:
                    chk.ok("%s keeps all matches" % name, "%d rows" % len(rows))
            else:
                got = rows[0][0]
                if got != baseline:
                    chk.fail("%s returns the same matches" % name, "%r vs %r" % (got, baseline))
                else:
                    chk.ok("%s returns the same matches" % name, "%r" % (got,))
        except Exception as exc:  # noqa: BLE001
            chk.fail("%s returns the same matches" % name, repr(exc))


def check_scores(cur, chk):
    """A scored query must still produce a positive, ordered score."""
    term = "alpha"
    try:
        cur.execute(
            "SELECT id, MATCH(body) AGAINST(%s) AS s FROM docs WHERE MATCH(body) AGAINST(%s) "
            "ORDER BY s DESC LIMIT 5",
            (term, term),
        )
        rows = cur.fetchall()
        if not rows:
            chk.fail("scored query returns rows", "no rows for %r" % term)
        elif not all(r[1] is not None and float(r[1]) > 0 for r in rows):
            chk.fail("scored query returns positive scores", repr(rows[:3]))
        else:
            scores = [float(r[1]) for r in rows]
            if scores != sorted(scores, reverse=True):
                chk.fail("scores are sorted descending", repr(scores))
            else:
                chk.ok("scored query returns positive, sorted scores", repr(scores[:3]))
    except Exception as exc:  # noqa: BLE001
        chk.fail("scored query returns positive, sorted scores", repr(exc))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--password", default="")
    parser.add_argument("--database", default="fts_replay")
    args = parser.parse_args()

    conn = pymysql.connect(
        host=args.host,
        port=args.port,
        user=args.user,
        password=args.password,
        charset="utf8mb4",
        autocommit=True,
        connect_timeout=10,
        read_timeout=300,
        write_timeout=300,
    )
    chk = Checker()
    try:
        with conn.cursor() as cur:
            for stmt in DDL:
                cur.execute(stmt.format(db=args.database))
            cur.execute("USE %s" % args.database)
            cur.execute("CREATE TABLE digits (n INT PRIMARY KEY)")
            cur.execute("INSERT INTO digits VALUES (0),(1),(2),(3),(4),(5),(6),(7),(8),(9)")
            cur.execute(INSERT_ROWS)
            inserted = query_one(cur, "SELECT COUNT(*) FROM docs")
            if inserted != 600:
                chk.fail("document rows loaded", "%r" % (inserted,))
            else:
                chk.ok("document rows loaded", "%d" % inserted)

            check_counts(cur, chk)
            check_shapes(cur, chk)
            check_scores(cur, chk)
    finally:
        conn.close()

    print("\n%d checks, %d failures" % (chk.checked + len(chk.failures), len(chk.failures)))
    if chk.failures:
        for name, detail in chk.failures:
            print("  - %s: %s" % (name, detail))
        return 1
    print("all FTS regression checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
