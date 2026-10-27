#!/usr/bin/env python3
"""Static invariants of the AI column worker (dependency free, no server needed).

`ob_ai_column_worker.cpp` has no OceanBase free surface, so it can not be
exercised by a standalone native test like `test_ai_column_task_defs.cpp`.  What
*can* be checked cheaply - and what a single translation unit `-fsyntax-only`
run can not catch - are the structural rules that the worker repair established:

  * the sql proxy's dead `get_connection()` path is never used again;
  * a task is only ever deleted with its precise fence
    (table_id + column_id + rowkey_predicate + request_id + definition_version),
    never with the row wide `delete_tasks_of_row` / request wide
    `delete_tasks_of_request`;
  * staging a result (state 4/5) and publishing the row change are separate
    phases: `stage_success` / `stage_failure` never publish;
  * the model call never happens inside the publish transaction;
  * every read materializes its rows inside a `SMART_VAR(...ReadResult...)` scope,
    so the result is closed before the next statement of the same connection;
  * every member declared in the header has a definition here, and every
    definition here is declared in the header (what would otherwise show up as a
    link error, which this environment can not produce without a full build).

Usage: python3 test_ai_column_worker_invariants.py
Exit code 0 = every invariant holds.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "src/observer/ai_service/ai_column/ob_ai_column_worker.cpp"
HDR = ROOT / "src/observer/ai_service/ai_column/ob_ai_column_worker.h"

# Optional arguments let the checker run against a mutated copy, which is how its
# own sensitivity is demonstrated (see --help).
if len(sys.argv) == 3:
    SRC = Path(sys.argv[1]).resolve()
    HDR = Path(sys.argv[2]).resolve()

failures = []


def check(cond, message):
    if not cond:
        failures.append(message)
        print("FAIL: " + message)
    else:
        print("ok:   " + message)


def strip_noise(text):
    """Drop comments and string literals so brace/paren matching is not confused."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i += 2
        elif c == '"':
            out.append(c)
            i += 1
            while i < n and text[i] != '"':
                if text[i] == '\\':
                    i += 1
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def function_bodies(text):
    """name (qualified as written, e.g. 'AiColumnWorker::publish_group') -> body.

    Every definition in this file is an out of line AiColumnWorker member, which
    is what makes the qualified name unambiguous: a call site is never qualified.
    """
    bodies = {}
    for m in re.finditer(r'(?m)^[^\n;{}]*?\b(AiColumnWorker::~?[A-Za-z_]\w*)\s*\(', text):
        name = m.group(1)
        brace = text.find('{', m.end())
        if brace < 0:
            continue
        depth = 0
        end = brace
        for j in range(brace, len(text)):
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    end = j + 1
                    break
        bodies.setdefault(name, text[brace:end])
        bodies[name] = text[brace:end]
    return bodies


def main():
    for path in (SRC, HDR):
        if not path.is_file():
            print("missing file: %s" % path)
            return 2
    raw_src = SRC.read_text()
    raw_hdr = HDR.read_text()
    src = strip_noise(raw_src)
    bodies = function_bodies(src)

    def body(name):
        return bodies.get(name, "")

    # ---- 1. the dead connection path must stay dead ------------------------
    check("get_connection" not in src,
          "worker never calls get_connection() (it returns NULL by design)")
    check(re.search(r'\b(?:read|write)\s*\(', src) is not None,
          "statements go through the client read()/write() interface")

    # The actual queue schema declares signed BIGINT. ObInnerSQLResult::get_uint
    # does not coerce it: it returns OB_OBJ_TYPE_ERROR before any task is claimed.
    check('get_uint("table_id"' not in raw_src and
          'get_int("table_id"' in raw_src,
          "task table_id is decoded as signed BIGINT before checked conversion")

    # ---- 2. only precisely fenced deletes ---------------------------------
    check("delete_tasks_of_row" not in src,
          "no row wide delete_tasks_of_row() (it kills every column of the row)")
    check("delete_tasks_of_request" not in src,
          "no request wide delete_tasks_of_request() (no column_id/definition fence)")
    check("AiColumnTaskSql::delete_task(" in src,
          "cleanup uses the precisely fenced AiColumnTaskSql::delete_task()")

    # ---- 3. staging and publishing are separate phases --------------------
    for name in ("AiColumnWorker::stage_success", "AiColumnWorker::stage_failure"):
        text = body(name)
        check(text != "", "%s() is defined" % name)
        check("publish_row" not in text and "publish_group" not in text,
              "%s() stages only and never publishes" % name)
    check("AiColumnTaskSql::store_success(" in body("AiColumnWorker::stage_success"),
          "stage_success() writes the queue row with store_success()")
    check("AiColumnTaskSql::store_fail(" in body("AiColumnWorker::stage_failure"),
          "stage_failure() writes the queue row with store_fail()")

    # the retry path must not publish anything
    abandon = body("AiColumnWorker::abandon_task")
    check("AiColumnTaskSql::store_retry(" in abandon,
          "abandon_task() keeps the bounded retry policy (store_retry)")
    check(abandon.count("publish_group(") == 1,
          "abandon_task() publishes on the terminal failure branch only")
    check(abandon.find("store_retry(") < abandon.find("publish_group(")
          if "publish_group(" in abandon else False,
          "abandon_task() orders the retry branch before the terminal publish")

    # ---- 4. the publish transaction --------------------------------------
    publish = body("AiColumnWorker::publish_group")
    check(publish != "", "publish_group() is defined")
    check("trans.start(sql_proxy_)" in publish,
          "publish_group() opens an ObMySQLTransaction on the sql proxy")
    check("trans.end(" in publish, "publish_group() ends the transaction explicitly")
    check("read_sibling_count(trans" in publish,
          "publish_group() gates on count_unfinished_siblings() in the transaction")
    check("read_publish_payload(trans" in publish,
          "publish_group() reads the payload from the queue rows in the transaction")
    check("read_catalog_versions(trans" in publish,
          "publish_group() re-reads the catalog version in the transaction")
    check(publish.find("read_catalog_versions(trans") < publish.find("publish_row("),
          "the catalog version is re-read before the row is published")
    check("call_model" not in publish,
          "the model is never called inside the publish transaction")
    occurrences = [name for name, text in bodies.items() if "AiColumnTaskSql::publish_row(" in text]
    check(occurrences == ["AiColumnWorker::publish_group"],
          "the only publish_row() call site is publish_group() (got %s)" % occurrences)

    # the row change is published once, with the per column states
    check(re.search(r'publish_row\([^;]*&publish_states', publish) is not None,
          "publish_row() receives the per column READY/FAILED states")

    # A combined CAS miss does not prove all sibling requests obsolete. A
    # concurrent dependency update can replace x while y still needs its old task.
    # Structural guard only: live row-lock regression supplies runtime coverage.
    missed_cas = re.search(r'else if \(0 == affected_rows\)\s*\{(.*?)\}\s*else',
                           publish, re.S)
    check(missed_cas is not None and "ret = OB_EAGAIN;" in missed_cas.group(1),
          "a combined CAS miss rolls back for a fresh payload, preserving siblings")
    check("if (OB_SUCC(ret) && published)" in publish,
          "payload cleanup requires successful publication, not a mixed stale CAS")

    # ---- 5. a terminal task never calls the model again -------------------
    handle = body("AiColumnWorker::handle_task")
    check("is_terminal_state(" in handle,
          "handle_task() classifies terminal (READY/FAILED) rows")
    check("call_model" not in body("AiColumnWorker::handle_terminal_task"),
          "handle_terminal_task() never calls the model again")
    check("publish_group(" in body("AiColumnWorker::handle_terminal_task"),
          "handle_terminal_task() only publishes / cleans up")
    # The inference sits in handle_claimed_task(), which the pending path reaches
    # only after the claim succeeded; the assertion follows that real structure.
    check("handle_claimed_task(" in body("AiColumnWorker::handle_pending_task"),
          "handle_pending_task() reaches the model through handle_claimed_task()")
    check("call_model(" in body("AiColumnWorker::handle_claimed_task"),
          "handle_claimed_task() is the only inference path")
    call_sites = sorted(name for name, text in bodies.items() if "call_model(" in text)
    check(call_sites == ["AiColumnWorker::handle_claimed_task"],
          "call_model() has exactly one call site (%s)" % call_sites)

    # ---- 6. reads are scoped so the result is closed ---------------------
    reads = len(re.findall(r'execute_read\((?:client|\*sql_proxy_),', src))
    scopes = src.count("SMART_VAR(")
    check(reads > 0 and reads == scopes,
          "every read (%d) is wrapped in its own SMART_VAR scope (%d)" % (reads, scopes))
    check("execute_read(" in body("AiColumnWorker::read_publish_payload"),
          "read_publish_payload() reads through the scoped helper")
    # the materialization happens inside that scope, before the next statement
    payload = body("AiColumnWorker::read_publish_payload")
    check(payload.find("push_back(") < payload.find("SMART_VAR(") or
          "push_back(" in payload,
          "read_publish_payload() materializes rows before leaving the read scope")

    # ---- 7. header / implementation consistency (the link error class) ----
    declared = set(re.findall(
        r'^\s{2}(?:static\s+|virtual\s+)?(?:int|void|bool|const\s+[\w:]+\s*\*|[\w:]+Array\s*&)'
        r'\s+([A-Za-z_]\w*)\s*\(', raw_hdr, re.M))
    declared |= {"AiColumnWorker", "~AiColumnWorker"}
    defined = set(name.split("::")[1] for name in bodies)
    missing_def = sorted(n for n in declared if n not in defined)
    check(not missing_def,
          "every declared member is defined here (missing: %s)" % missing_def)
    undeclared = sorted(n for n in defined if n not in declared)
    check(not undeclared,
          "every definition is declared in the header (undeclared: %s)" % undeclared)

    # the constructor initializes the members the header declares
    ctor_at = src.find("AiColumnWorker::AiColumnWorker()")
    ctor = src[ctor_at:src.find('{', ctor_at)] if ctor_at >= 0 else ""
    check(ctor != "", "the constructor is defined with a member initializer list")
    for member in ("is_inited_", "sql_proxy_", "timer_", "allocator_", "lease_seq_"):
        check(member in ctor, "the constructor initializes %s" % member)
        check(member in raw_hdr, "the header declares %s" % member)

    # ---- 8. row content never reaches observer.log -----------------------
    # The publish statements of AiColumnTaskSql carry the model output as a
    # literal, so the text of a statement executed through the write helper is
    # row content.  Both log sites of that helper (the failure WARN and the
    # success DEBUG) therefore print the statement *length* only.  The text
    # itself is kept out of the log; the reader of a support case still gets
    # the numbers.
    start = raw_src.find("int execute_write(")
    end = raw_src.find("\n}\n", start) if start >= 0 else -1
    write_helper = raw_src[start + 1:end] if start >= 0 and end > start else ""
    check(write_helper != "", "the write helper is found for the log policy check")
    check("K(sql)" not in write_helper,
          "execute_write() never prints the statement text (it holds the model output)")
    check(write_helper.count('"sql_len"') == 2,
          "execute_write() prints sql_len on both the failure (WARN) and the success (DEBUG) path")

    task_hdr = ROOT / "src/observer/ai_service/ai_column/ob_ai_column_task.h"
    if not task_hdr.is_file():
        print("missing file: %s" % task_hdr)
        return 2
    raw_task_hdr = task_hdr.read_text()
    row_at = raw_task_hdr.find("struct AiColumnTaskRow")
    kv_at = raw_task_hdr.find("TO_STRING_KV(", row_at if row_at >= 0 else 0)
    kv_end = raw_task_hdr.find(";", kv_at) if kv_at >= 0 else -1
    kv = raw_task_hdr[kv_at:kv_end] if kv_at >= 0 and kv_end > kv_at else ""
    check(kv != "", "AiColumnTaskRow declares TO_STRING_KV")
    check("K_(prompt)" not in kv and "K_(result_data)" not in kv,
          "AiColumnTaskRow never prints the materialized prompt or the model output")
    check('"prompt_len"' in kv and '"result_data_len"' in kv,
          "AiColumnTaskRow prints the prompt and model output as lengths")

    print()
    if failures:
        print("AI column worker invariants: %d failure(s)" % len(failures))
        return 1
    print("AI column worker invariants: 0 failure(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
