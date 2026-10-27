// Links real schema/resolver code. No mock or replacement production implementation.
#include <cstdio>
#include <set>
#include "lib/allocator/ob_allocator.h"
#include "share/schema/ob_table_schema.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
using namespace oceanbase::common;
using namespace oceanbase::share::schema;
using namespace oceanbase::sql;
static int checks = 0;
static int failures = 0;
static void expect(bool ok, const char *message)
{
  ++checks;
  if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
static void init_table(ObTableSchema &table)
{
  table.set_table_id(50001);
  table.set_table_type(USER_TABLE);
  expect(table.set_table_name("ai_schema_probe") == OB_SUCCESS, "set table name");
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
static void dump(const ObTableSchema &table, const char *label, int ret)
{
  std::printf("%s ret=%d count=%lld max_id=%llu table_id=%llu\n", label, ret,
      (long long)table.get_column_count(), (unsigned long long)table.get_max_used_column_id(),
      (unsigned long long)table.get_table_id());
  for (int64_t i = 0; i < table.get_column_count(); ++i) {
    const ObColumnSchemaV2 *c = table.get_column_schema_by_idx(i);
    std::printf("  %.*s id=%llu table=%llu hidden=%d prev=%llu next=%llu type=%d len=%d\n",
        c->get_column_name_str().length(), c->get_column_name_str().ptr(),
        (unsigned long long)c->get_column_id(), (unsigned long long)c->get_table_id(), c->is_hidden(),
        (unsigned long long)c->get_prev_column_id(), (unsigned long long)c->get_next_column_id(),
        (int)c->get_data_type(), (int)c->get_data_length());
  }
}
static void test_schema_add_contract()
{
  ObTableSchema table;
  init_table(table);
  ObColumnSchemaV2 column;
  init_column(column, "explicit_id_only", OB_APP_MIN_COLUMN_ID + 9);
  expect(column.get_table_id() == OB_INVALID_ID, "test input leaves table_id unassigned");
  const int ret = table.add_column(column);
  dump(table, "add explicit column_id only", ret);
  expect(ret == OB_SUCCESS, "add_column succeeds with explicitly assigned column_id");
  const ObColumnSchemaV2 *stored = table.get_column_schema(column.get_column_id());
  expect(stored != nullptr && stored->get_table_id() == table.get_table_id(), "add_column fills table_id");
  expect(table.get_max_used_column_id() == column.get_column_id(), "add_column advances max_used_column_id");
}
static void test_hidden_columns(int ai_count)
{
  ObArenaAllocator allocator("AiSchemaTest");
  ObTableSchema table;
  init_table(table);
  ObColumnSchemaV2 ordinary;
  init_column(ordinary, "content", OB_APP_MIN_COLUMN_ID);
  expect(table.add_column(ordinary) == OB_SUCCESS, "add ordinary column");
  for (int i = 0; i < ai_count; ++i) {
    const char *name = i == 0 ? "tags" : "summary";
    ObColumnSchemaV2 ai;
    init_column(ai, name, OB_APP_MIN_COLUMN_ID + i + 1);
    ObAIColumnDefinition def;
    def.ai_function_name_ = ObString::make_string("AI_COMPLETE");
    def.model_name_ = ObString::make_string("native_model");
    def.prompt_expr_ = ObString::make_string("concat('x;=:', content)");
    def.normalized_expr_ = def.prompt_expr_;
    def.result_spec_ = ObString::make_string("int:64");
    def.sync_mode_ = ObString::make_string("async");
    def.max_retries_ = 2;
    expect(def.dep_column_names_.push_back(ObString::make_string("content")) == OB_SUCCESS, "add dependency");
    const char *prefixes[] = {ObAIColumnResolver::AI_STATE_COLUMN_PREFIX,
        ObAIColumnResolver::AI_REQUEST_COLUMN_PREFIX, ObAIColumnResolver::AI_RESULT_SPEC_COLUMN_PREFIX};
    ObString *names[] = {&def.state_column_name_, &def.request_column_name_, &def.result_spec_column_name_};
    for (int k = 0; k < 3; ++k) expect(ObAIColumnResolver::build_hidden_column_name(
        allocator, prefixes[k], ObString::make_string(name), *names[k]) == OB_SUCCESS, "build hidden name");
    expect(ObAIColumnResolver::set_definition(allocator, def, ObString::make_string("user;comment"), ai) == OB_SUCCESS,
        "encode AI definition on result column");
    ObAIColumnDefinition decoded;
    ObString comment;
    expect(ObAIColumnResolver::decode_definition(allocator, ai.get_comment_str(), decoded, comment) == OB_SUCCESS,
        "decode real AI metadata");
    expect(decoded.prompt_expr_ == def.prompt_expr_ && comment == ObString::make_string("user;comment"),
        "codec preserves prompt and comment separators");
    expect(decoded.state_column_name_ == def.state_column_name_ && decoded.request_column_name_ == def.request_column_name_ &&
        decoded.result_spec_column_name_ == def.result_spec_column_name_, "codec preserves hidden names");
    expect(table.add_column(ai) == OB_SUCCESS, "add AI result column");
  }
  // ALTER can retain a high-water mark after dropping a higher-numbered column.
  if (ai_count == 2) table.set_max_used_column_id(200);
  const uint64_t old_max = table.get_max_used_column_id();
  const int ret = ObAIColumnResolver::append_hidden_columns(table, allocator);
  dump(table, ai_count == 1 ? "append one AI" : "append two AI", ret);
  expect(ret == OB_SUCCESS, "append_hidden_columns succeeds");
  expect(table.get_column_count() == 1 + ai_count * 4, "three hidden columns per AI result");
  std::set<uint64_t> ids;
  int hidden_count = 0;
  for (int64_t i = 0; i < table.get_column_count(); ++i) {
    const ObColumnSchemaV2 *c = table.get_column_schema_by_idx(i);
    expect(c->get_column_id() != OB_INVALID_ID && c->get_column_id() >= OB_APP_MIN_COLUMN_ID,
        "every column ID is valid");
    expect(ids.insert(c->get_column_id()).second, "column IDs are unique");
    expect(c->get_table_id() == table.get_table_id(), "all columns carry table ID");
    if (c->is_hidden()) { ++hidden_count; expect(c->get_column_id() > old_max, "hidden IDs follow user IDs"); }
  }
  expect(hidden_count == ai_count * 3, "all runtime columns are hidden");
  expect(table.get_max_used_column_id() == old_max + ai_count * 3, "max ID advances over all hidden columns");
  if (ret == OB_SUCCESS) {
    for (int i = 0; i < ai_count; ++i) {
      const char *prefixes[] = {"__ai_state_", "__ai_req_", "__ai_result_spec_"};
      const ObObjType types[] = {ObIntType, ObVarcharType, ObLongTextType};
      for (int k = 0; k < 3; ++k) {
        char name[80];
        std::snprintf(name, sizeof(name), "%s%s", prefixes[k], i == 0 ? "tags" : "summary");
        const ObColumnSchemaV2 *c = table.get_column_schema(ObString::make_string(name));
        expect(c != nullptr && c->is_hidden() && c->is_nullable() && c->get_data_type() == types[k],
            "runtime hidden name, visibility, nullability, and type match definition");
        if (c != nullptr && k == 1) expect(c->get_data_length() == 128, "request watermark has length 128");
      }
    }
    for (int64_t i = 0; i < table.get_column_count(); ++i) {
      const ObColumnSchemaV2 *c = table.get_column_schema_by_idx(i);
      const uint64_t prev = i == 0 ? 0 : table.get_column_schema_by_idx(i - 1)->get_column_id();
      const uint64_t next = i + 1 == table.get_column_count() ? 0 : table.get_column_schema_by_idx(i + 1)->get_column_id();
      expect(c->get_prev_column_id() == prev && c->get_next_column_id() == next,
          "column prev/next links remain well-formed");
    }
    const int64_t old_count = table.get_column_count();
    const uint64_t max_id = table.get_max_used_column_id();
    expect(ObAIColumnResolver::append_hidden_columns(table, allocator) == OB_SUCCESS, "second append is successful");
    expect(table.get_column_count() == old_count && table.get_max_used_column_id() == max_id,
        "append is idempotent and consumes no extra IDs");
  }
}
static void test_task_optional_columns()
{
  ObTableSchema table;
  expect(oceanbase::share::ObInnerTableSchema::all_ai_column_task_schema(table) == OB_SUCCESS,
      "generated task schema constructs successfully");
  for (const char *name : {"lease_owner", "lease_expire_ts", "result_data", "error_msg"}) {
    const ObColumnSchemaV2 *column = table.get_column_schema(ObString::make_string(name));
    expect(column != nullptr && column->is_nullable(),
        "pending tasks permit absent lease, staged result and error");
  }
  for (const char *name : {"table_id", "column_id", "rowkey_predicate", "request_id", "state"}) {
    const ObColumnSchemaV2 *column = table.get_column_schema(ObString::make_string(name));
    expect(column != nullptr && !column->is_nullable(),
        "task identity and state remain mandatory");
  }
}
int main()
{
  test_task_optional_columns();
  test_schema_add_contract();
  test_hidden_columns(1);
  test_hidden_columns(2);
  std::printf("native AI hidden schema: checks=%d failures=%d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
