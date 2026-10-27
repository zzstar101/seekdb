#!/usr/bin/env python3
"""Isolation invariants of the AI column service (dependency free, no server needed).

`ob_ai_column_service.cpp` is bound into the server as `sql::IAiColumnDmlHook` and
talks to the schema service and the sql proxy, so it can not be exercised by a
standalone native test like `test_ai_column_task_defs.cpp`; the behavioural end to
end coverage lives in tools/deploy/mysql_test/test_suite/ai_column and needs a
running observer.  What *can* be checked here, cheaply and on every change, are the
rules this isolation repair established - the rules a `-fsyntax-only` run can not
see and a code review forgets:

  1. the trusted schema snapshot decides whether the catalog is read at all: a
     table whose columns carry no persisted AI_COLUMN_FLAG is not an AI column
     table and __all_ai_column is never read for it (it used to be one catalog
     statement per table of every DML statement);
  2. no process wide table definition cache and no lock: the definitions of a
     table are loaded into an arena owned by the calling stack frame, so there is
     no entry that grows without bound, no lock held across a database read and no
     cached arena that has to be replaced when the schema version moves;
  3. every output of the service (definitions, primary key column names, task
     statements) is deep copied into the allocator of the caller, so nothing the
     service hands out points into a temporary that dies with the call;
  4. the behaviour of the three other public entry points is unchanged: strict
     read still rejects on unfinished tasks, an update still only recomputes the AI
     columns that depend on the changed columns, the delete / enqueue statements
     are still built by the same SQL builder.

Usage: python3 test_ai_column_service_isolation.py
       python3 test_ai_column_service_isolation.py <service.cpp> <service.h>
Exit code 0 = every invariant holds.

The two argument form is what demonstrates the sensitivity of this checker: run it
against the pre-repair sources (git show HEAD:...) and it fails, run it against the
tree and it passes.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "src/observer/ai_service/ai_column/ob_ai_column_service.cpp"
HDR = ROOT / "src/observer/ai_service/ai_column/ob_ai_column_service.h"

if len(sys.argv) == 3:
    SRC = Path(sys.argv[1]).resolve()
    HDR = Path(sys.argv[2]).resolve()

# The four public entry points of the service and the allocator each one must
# hand to the loader: this is the cross product the caller side relies on.
ENTRY_POINTS = (
    "AiColumnService::get_table_ai_columns",
    "AiColumnService::build_row_change_sql",
    "AiColumnService::check_strict_read",
    "AiColumnService::get_ai_column_defs",
)

failures = []


def check(cond, message):
    if not cond:
        failures.append(message)
        print("FAIL: " + message)
    else:
        print("ok:   " + message)


def strip_noise(text):
    """Drop comments and string/char literals so brace matching is not confused."""
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
        elif c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            while i < n and text[i] != quote:
                if text[i] == '\\':
                    i += 1
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


CONTROL = {
    "if", "for", "while", "switch", "return", "else", "do", "sizeof", "catch",
    "SMART_VAR", "OB_SUCC", "OB_FAIL", "OB_ISNULL", "OB_NOT_NULL", "UNUSED",
}


def function_bodies(text):
    """Last component of the defined name (e.g. 'load_table_defs') -> body text.

    Handles both `AiColumnService::member(...)` definitions and definitions of the
    functions of the anonymous namespace (`copy_sql(...)`).  Names inside the file
    are unique enough: a call site is never the first thing on a line and is never
    followed by a `{` at brace depth 0 of the definition.
    """
    bodies = {}
    for m in re.finditer(r'(?m)^[^\n;{}()]*?\b((?:[A-Za-z_]\w*::)*~?[A-Za-z_]\w*)\s*\(', text):
        raw = m.group(1)
        name = raw.split("::")[-1]
        if name in CONTROL:
            continue
        # Walk to the ')' that closes this argument list.  Requiring the match to
        # end there keeps a multi-line call argument (e.g. `ObTimeUtility::
        # current_time(),` on its own line) from being mistaken for a definition.
        depth = 1
        close = -1
        j = m.end()
        while j < len(text):
            if text[j] == '(':
                depth += 1
            elif text[j] == ')':
                depth -= 1
                if depth == 0:
                    close = j
                    break
            j += 1
        if close < 0:
            continue
        tail = text[close + 1:]
        stripped = tail.lstrip()
        if not (stripped.startswith('{') or stripped.startswith(':')):
            continue
        brace = close + 1 + (len(tail) - len(stripped))
        brace = text.find('{', brace)
        if brace < 0:
            continue
        between = text[close + 1:brace]
        if ';' in between:
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
        if end == brace:
            continue
        qualified = raw
        bodies.setdefault(name, text[brace:end])
        bodies[name] = text[brace:end]
        bodies.setdefault(qualified, text[brace:end])
    return bodies


def count(text, needle):
    return text.count(needle)


def main():
    for path in (SRC, HDR):
        if not path.is_file():
            print("missing file: %s" % path)
            return 2
    raw_src = SRC.read_text()
    raw_hdr = HDR.read_text()
    src = strip_noise(raw_src)
    hdr = strip_noise(raw_hdr)
    bodies = function_bodies(src)

    def body(name):
        return bodies.get(name, "")

    # ---- 0. the repaired shape exists at all -------------------------------
    check("table_has_ai_column" in hdr,
          "the header declares the trusted schema gate table_has_ai_column()")
    check(body("table_has_ai_column") != "",
          "the trusted schema gate table_has_ai_column() is defined")
    check(body("load_table_defs") != "",
          "the per call loader load_table_defs() is defined")
    for name in ENTRY_POINTS:
        check(body(name) != "", "%s() is defined" % name)

    # ---- 1. a non AI table never reads the catalog -------------------------
    gate = body("table_has_ai_column")
    check("is_ai_column_column" in gate,
          "the gate asks the persisted AI_COLUMN_FLAG (is_ai_column_column), "
          "not the comment text")
    check("select_catalog" not in gate,
          "the gate does not consult the catalog to answer its own question")
    check("sql_proxy_" not in gate,
          "the gate performs no database access")
    check("get_column_count" in gate and "get_column_schema_by_idx" in gate,
          "the gate scans the columns of the trusted schema snapshot")
    check(re.search(r'static\s+bool\s+table_has_ai_column\s*\(\s*const\s+share::schema::'
                    r'ObTableSchema\s*&', raw_hdr) is not None,
          "the gate is a pure static predicate over a const ObTableSchema")
    check("comment_has_ai_column_meta" not in src,
          "the gate is the persisted column flag, not the comment sniff "
          "comment_has_ai_column_meta()")

    catalog_sites = [name for name, text in bodies.items()
                     if "AiColumnTaskSql::select_catalog(" in text]
    check(sorted(set(n.split("::")[-1] for n in catalog_sites)) == ["load_table_defs"],
          "the only catalog read of the service is inside load_table_defs (got %s)"
          % sorted(set(catalog_sites)))
    loader = body("load_table_defs")
    check("select_catalog" in loader, "the loader reads the catalog")
    check(loader.find("table_has_ai_column(") >= 0 and
          loader.find("table_has_ai_column(") < loader.find("select_catalog("),
          "the trusted gate is tested before the catalog is read")
    check("load_entry" not in src and "get_or_load_entry" not in src and
          "get_or_load_entry" not in hdr,
          "the old shared cache loader get_or_load_entry() is gone")

    # every entry point reaches the catalog only through the gated loader
    for name in ENTRY_POINTS:
        text = body(name)
        check(count(text, "load_table_defs(") == 1,
              "%s() loads the definitions through load_table_defs() exactly once"
              % name)
        check("select_catalog" not in text,
              "%s() never builds a catalog statement itself" % name)
        check("read_catalog_row" not in text,
              "%s() never reads a catalog row itself" % name)

    # the loader is the only function that materializes catalog rows
    row_readers = [name for name, text in bodies.items() if "read_catalog_row(" in text]
    check(sorted(set(n.split("::")[-1] for n in row_readers)) == ["load_table_defs"],
          "read_catalog_row() is called by load_table_defs() only (got %s)"
          % sorted(set(row_readers)))

    # ---- 2. no unbounded cache, no lock ------------------------------------
    for needle in ("entries_", "ObMutex", "ObMutexGuard", "mutex_", "MutexGuard"):
        check(needle not in src, "the implementation has no %s" % needle)
        check(needle not in hdr, "the header has no %s" % needle)
    check("lib/lock/ob_mutex.h" not in raw_hdr,
          "the header no longer pulls in the mutex type")
    # no heap owned, schema version stamped entry
    for needle in ("ob_malloc", "OB_NEW(", "ob_free", "TableDefEntry *"):
        check(needle not in src, "the implementation does not own an entry (%s)" % needle)
        check(needle not in hdr, "the header does not own an entry (%s)" % needle)
    check("schema_version_" not in hdr,
          "a loaded entry is not pinned to a schema version (it is per call)")
    check("arena_" not in hdr and "arena_" not in src,
          "no entry owns an arena any more")

    # the remaining data members are exactly the runtime plumbing
    last_private = hdr.rfind("private:")
    members = set(re.findall(r'(?m)^  (?:const\s+)?(?:[\w:]+(?:::\w+)?[\s*&]+)+\**(\w+);\s*$',
                             hdr[last_private:]))
    check(members == {"is_inited_", "is_started_", "sql_proxy_", "worker_",
                      "request_seq_"},
          "the service holds no cached definitions (members: %s)" % sorted(members))

    # ---- 3. the caller owns the arena, every output is a deep copy ---------
    check("TableDefEntry entry;" in src,
          "an entry is a plain stack value")
    check(count(src, "TableDefEntry entry;") == len(ENTRY_POINTS),
          "all %d entry points keep their own entry on the stack (%d found)"
          % (len(ENTRY_POINTS), count(src, "TableDefEntry entry;")))
    check(count(src, "ObArenaAllocator defs_allocator;") == len(ENTRY_POINTS),
          "all %d entry points keep their own definition arena on the stack (%d found)"
          % (len(ENTRY_POINTS), count(src, "ObArenaAllocator defs_allocator;")))
    check(count(src, "load_table_defs(defs_allocator,") == len(ENTRY_POINTS),
          "all %d entry points load into their own arena (%d found)"
          % (len(ENTRY_POINTS), count(src, "load_table_defs(defs_allocator,")))
    check(re.search(r'load_table_defs\(\s*common::ObIAllocator\s*&', src) is not None,
          "load_table_defs() receives the allocator of the caller")
    check(re.search(r'read_catalog_row\(\s*common::ObIAllocator\s*&', src) is not None,
          "read_catalog_row() receives the allocator of the caller")
    for name in ENTRY_POINTS:
        text = body(name)
        check("ObArenaAllocator defs_allocator;" in text,
              "%s() allocates the definitions from its own arena" % name)
    check("TableDefEntry *&" not in hdr,
          "no loader hands out a pointer into shared state")

    # definitions are deep copied into the caller allocator
    defs_body = body("AiColumnService::get_ai_column_defs")
    check("defs.reuse()" in defs_body,
          "get_ai_column_defs() reuses the output array")
    check("copy_def(allocator," in defs_body,
          "get_ai_column_defs() deep copies each definition into the caller allocator")
    check("copy_def(allocator, entry.defs_.at(i), def)" in defs_body,
          "the copy source is the per call entry, the target is the caller allocator")
    check("= entry.defs_.at(i)" not in defs_body and
          "defs_.at(i);" not in defs_body,
          "get_ai_column_defs() never shallow copies a definition")

    # primary key column names are deep copied into the caller allocator
    pk_body = body("AiColumnService::get_table_ai_columns")
    check("ob_write_string(allocator, column_schema->get_column_name_str()" in pk_body,
          "get_table_ai_columns() deep copies the primary key column names")
    check("key.column_name_ = database" not in pk_body and
          "column_name_ = column_schema" not in pk_body,
          "get_table_ai_columns() never points a key at the schema snapshot")

    # task statements are deep copied into the caller allocator
    stmt_body = body("AiColumnService::build_row_change_sql")
    check("copy_sql" not in stmt_body,
          "build_row_change_sql() builds statements through build_change_sql()")
    check("stmts.push_back(" not in stmt_body,
          "build_row_change_sql() never pushes an uncopied statement")
    check("copy_sql(allocator," in body("build_change_sql"),
          "build_change_sql() deep copies every statement into the caller allocator")
    push_sites = [name for name, text in bodies.items() if "stmts.push_back(" in text]
    check(len(set(push_sites)) == 1,
          "only the copying helper stores a statement (got %s)" % sorted(set(push_sites)))

    # ---- 4. the behaviour of the four entry points is preserved ------------
    strict = body("AiColumnService::check_strict_read")
    check("OB_AI_COLUMN_UNFINISHED_TASK" in strict,
          "strict read still rejects with the unfinished task error")
    check("LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASK" in strict,
          "the unfinished task rejection still reaches the client as a user error")
    check("count_not_ready_rows(" in strict,
          "strict read still counts the rows whose result is not ready")
    check("read_count(" in strict,
          "strict read still reads the count inside the call (no escaped statement)")

    changes = body("AiColumnService::build_change_sql")
    for needle in ("invalidate_row(", "enqueue_task(", "delete_tasks_of_row(",
                   "depends_on_updated_columns("):
        check(needle in changes,
              "build_change_sql() still issues %s" % needle)

    loader_text = body("load_table_defs")
    for needle in ("resolve_hidden_column_names", "extract_prompt_expr",
                   "build_quoted_table_name"):
        check(needle in body("read_catalog_row"),
              "read_catalog_row() still resolves %s" % needle)

    # ---- 5. header / implementation consistency (the link error class) -----
    # member declarations sit at exactly two spaces of indentation; deeper
    # indentation is either a nested struct field or an argument continuation.
    # The comment stripped text is used so that a word of a doc comment can not
    # be mistaken for a declaration.
    declared = set(re.findall(
        r'(?m)^ {2}(?! )(?:static\s+|virtual\s+)?[^\n;{}]*?\b([A-Za-z_]\w*)\s*\(',
        hdr))
    declared |= {"AiColumnService", "~AiColumnService"}
    defined = set(name.split("::")[-1] for name in bodies)
    missing_def = sorted(n for n in declared if n not in defined)
    check(not missing_def,
          "every declared member is defined here (missing: %s)" % missing_def)
    qualified = set(name for name in bodies if "::" in name)
    undeclared = sorted(n for n in qualified
                        if n.split("::")[-1] not in declared)
    check(not undeclared,
          "every definition is declared in the header (undeclared: %s)" % undeclared)

    ctor_at = src.find("AiColumnService::AiColumnService()")
    ctor = src[ctor_at:src.find('{', ctor_at)] if ctor_at >= 0 else ""
    check(ctor != "", "the constructor is defined with a member initializer list")
    for member in ("is_inited_", "is_started_", "sql_proxy_", "worker_", "request_seq_"):
        check(member in ctor, "the constructor initializes %s" % member)

    # the entries of a call are values on that frame, never a service member:
    # this is what the member-set check above pins down, so nothing else here.

    print()
    if failures:
        print("AI column service isolation: %d failure(s)" % len(failures))
        return 1
    print("AI column service isolation: 0 failure(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
