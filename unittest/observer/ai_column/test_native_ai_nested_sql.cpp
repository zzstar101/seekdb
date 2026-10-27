#include <cstdio>
#include "lib/allocator/page_arena.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/ob_sql_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
int main()
{
  ObArenaAllocator allocator;
  ObExecContext outer(allocator), inner(allocator);
  ObSQLSessionInfo session;
  ObSqlCtx outer_sql;
  outer_sql.stmt_type_ = stmt::T_INSERT;
  outer.set_sql_ctx(&outer_sql);
  LinkExecCtxGuard outer_link(session, outer);
  LinkExecCtxGuard link(session, inner);
  int failures = 0, checks = 0;
  auto check = [&](bool value, const char *message) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
  };
  check(!ObSQLUtils::is_nested_sql(&inner), "unguarded child is not an AI nested statement");
  {
    AiColumnHookGuard guard;
    check(ObSQLUtils::is_nested_sql(&inner), "AI internal SQL shares parent statement transaction");
    check(!ObSQLUtils::is_nested_sql(&outer), "AI guard does not turn root into nested SQL");
    check(!ObSQLUtils::is_nested_sql(nullptr), "null context remains non-nested");
    outer_sql.stmt_type_ = stmt::T_ALTER_TABLE;
    check(!ObSQLUtils::is_nested_sql(&inner), "RETRY command has no active parent DML statement");
    outer_sql.stmt_type_ = stmt::T_INSERT;
    {
      AiColumnHookGuard recursive_guard;
      check(ObSQLUtils::is_nested_sql(&inner), "recursive guard preserves transaction nesting");
    }
    check(ObSQLUtils::is_nested_sql(&inner), "inner guard destruction preserves outer guard");
  }
  check(!ObSQLUtils::is_nested_sql(&inner), "guard scope does not leak into ordinary SQL");
  outer.get_das_ctx().is_fk_cascading_ = true;
  check(ObSQLUtils::is_nested_sql(&inner), "existing foreign-key nesting unchanged");
  std::printf("AI nested SQL: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
