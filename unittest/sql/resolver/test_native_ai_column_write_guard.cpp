// Links the real write guard, schema predicates and session state. No mock or
// replacement production implementation.
#include <cstdio>
#include "lib/allocator/ob_allocator.h"
#include "share/schema/ob_table_schema.h"
#include "sql/resolver/dml/ai_column_write_guard.h"
using namespace oceanbase::common;
using namespace oceanbase::share::schema;
using namespace oceanbase::sql;

static int checks = 0;
static int failures = 0;
static void expect(bool ok, const char *message)
{
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

static void init_column(ObColumnSchemaV2 &column, const char *name, uint64_t id)
{
  expect(column.set_column_name(name) == OB_SUCCESS, "set column name");
  column.set_column_id(id);
  column.set_data_type(ObIntType);
  column.set_nullable(true);
  column.set_charset_type(CHARSET_BINARY);
  column.set_collation_type(CS_TYPE_BINARY);
}

// The guard is the only place that decides whether a write is the AI column
// runtime itself, so its truth table is the contract: the DML hook of
// ObTableModifyOp and a real inner session are internal, a user session that was
// marked inner for a nested statement (a stored procedure, a SPI connection) is
// not.
static void test_internal_write_decision()
{
  expect(!AiColumnWriteGuard::is_internal_write_flags(false, false, false),
         "a business session is not an internal writer");
  expect(!AiColumnWriteGuard::is_internal_write_flags(true, false, false),
         "a nested statement on the user session is not an internal writer");
  expect(AiColumnWriteGuard::is_internal_write_flags(true, true, false),
         "a real inner session is an internal writer");
  expect(!AiColumnWriteGuard::is_internal_write_flags(false, true, false),
         "an inner flag without the inner session is not an internal writer");
  expect(AiColumnWriteGuard::is_internal_write_flags(false, false, true),
         "the ai column hook is an internal writer");
  expect(AiColumnWriteGuard::is_internal_write_flags(true, false, true),
         "the ai column hook on a nested user session is an internal writer");
  expect(AiColumnWriteGuard::is_internal_write_flags(true, true, true),
         "the ai column hook on an inner session is an internal writer");
  expect(!AiColumnWriteGuard::is_internal_write(NULL, false),
         "a missing session is not an internal writer");
  expect(AiColumnWriteGuard::is_internal_write(NULL, true),
         "an active ai column hook is an internal writer without a session");

  ObSQLSessionInfo session;
  expect(!AiColumnWriteGuard::is_internal_write(&session, false),
         "a fresh session is not an internal writer");
  session.set_inner_session();
  expect(session.is_inner(), "set_inner_session marks the session inner");
  expect(!AiColumnWriteGuard::is_internal_write(&session, false),
         "an inner session that reuses the user session is not an internal writer");
  session.set_real_inner_session(true);
  expect(AiColumnWriteGuard::is_internal_write(&session, false),
         "a real inner session is an internal writer");
  session.set_real_inner_session(false);
  expect(!AiColumnWriteGuard::is_internal_write(&session, false),
         "dropping the real inner flag restores the business decision");
}

// An AI column is identified by the persisted column flag. The comment prefix is
// only syntax: a user COMMENT that fakes it must not protect a column.
static void test_protected_column()
{
  ObArenaAllocator allocator("AiWriteGuard");
  ObColumnSchemaV2 ordinary;
  init_column(ordinary, "content", OB_APP_MIN_COLUMN_ID);
  expect(ordinary.set_comment("an ordinary column") == OB_SUCCESS, "set an ordinary comment");
  expect(!AiColumnWriteGuard::is_protected_column(ordinary), "an ordinary column is writable");

  ObColumnSchemaV2 forged;
  init_column(forged, "fake_ai", OB_APP_MIN_COLUMN_ID + 1);
  expect(forged.set_comment("__ai_column__{\"ai_function_name\":\"AI_COMPLETE\"}") == OB_SUCCESS,
         "set a comment that fakes the ai column syntax");
  expect(comment_has_ai_column_meta(forged.get_comment_str()),
         "the fake comment looks like ai column metadata");
  expect(!is_ai_column_column(forged), "the fake comment sets no ai column flag");
  expect(!AiColumnWriteGuard::is_protected_column(forged),
         "a forged comment does not make a column an ai column");

  ObColumnSchemaV2 ai_column;
  init_column(ai_column, "tags", OB_APP_MIN_COLUMN_ID + 2);
  expect(ai_column.set_comment("the user comment of an ai column") == OB_SUCCESS,
         "set a user comment on the ai column");
  ai_column.add_column_flag(AI_COLUMN_FLAG);
  expect(AiColumnWriteGuard::is_protected_column(ai_column),
         "an ai column is protected by its flag alone");

  ObColumnSchemaV2 both;
  init_column(both, "summary", OB_APP_MIN_COLUMN_ID + 3);
  expect(both.set_comment("__ai_column__{\"ai_function_name\":\"AI_COMPLETE\"}") == OB_SUCCESS,
         "set the ai column metadata comment");
  both.add_column_flag(AI_COLUMN_FLAG);
  expect(has_ai_column_definition(both),
         "an ai column with its metadata comment has a definition");
  expect(AiColumnWriteGuard::is_protected_column(both), "an ai column with metadata is protected");

  UNUSED(allocator);
}

// The clause names the rejected statement in the user error, so every clause has
// its own subject.
static void test_write_subject()
{
  const char *insert = AiColumnWriteGuard::write_subject(AiColumnWriteGuard::INSERT_CLAUSE);
  const char *odku = AiColumnWriteGuard::write_subject(
      AiColumnWriteGuard::ON_DUPLICATE_KEY_UPDATE_CLAUSE);
  const char *update = AiColumnWriteGuard::write_subject(AiColumnWriteGuard::UPDATE_CLAUSE);
  expect(NULL != insert && NULL != odku && NULL != update, "every clause has a subject");
  expect(0 != std::strcmp(insert, odku), "insert and on duplicate key update differ");
  expect(0 != std::strcmp(insert, update), "insert and update differ");
  expect(0 != std::strcmp(odku, update), "on duplicate key update and update differ");
  std::printf("insert subject: %s\n", insert);
  std::printf("odku subject: %s\n", odku);
  std::printf("update subject: %s\n", update);
}

int main()
{
  test_internal_write_decision();
  test_protected_column();
  test_write_subject();
  std::printf("%d checks, %d failures\n", checks, failures);
  return 0 == failures ? 0 : 1;
}
