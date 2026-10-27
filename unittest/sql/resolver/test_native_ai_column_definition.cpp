// Real parser/resolver/schema contract tests; no mocks or production substitution.
#include <cstdio>
#include <cstring>
#include "lib/allocator/ob_allocator.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "share/schema/ob_table_schema.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::share::schema;
static int checks = 0, failures = 0;
static void expect(bool ok, const char *message)
{
  ++checks;
  if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
static const ParseNode *find_ai(const ParseNode *n)
{
  if (n == nullptr || n->type_ == T_AI_COLUMN) return n;
  for (int i = 0; i < n->num_child_; ++i) if (const ParseNode *p = find_ai(n->children_[i])) return p;
  return nullptr;
}
static void init_column(ObColumnSchemaV2 &column, const char *name, uint64_t id, ObObjType type)
{
  expect(column.set_column_name(name) == OB_SUCCESS, "column name setup");
  column.set_column_id(id);
  column.set_data_type(type);
  column.set_charset_type(CHARSET_BINARY);
  column.set_collation_type(CS_TYPE_BINARY);
  if (type == ObCollectionSQLType) {
    ObSEArray<ObString, 1> ext;
    expect(ext.push_back(ObString::make_string("VECTOR(4)")) == OB_SUCCESS, "vector setup");
    expect(column.set_extended_type_info(ext) == OB_SUCCESS, "vector extended type setup");
  }
}
static int resolve(const char *expression, const char *options, ObObjType type,
                   const char *name, ObIAllocator &alloc, ObColumnSchemaV2 &column,
                   ObAIColumnDefinition &def)
{
  char sql[4096];
  const char *sql_type = type == ObCollectionSQLType ? "VECTOR(4)" : type == ObIntType ? "BIGINT" : "JSON";
  std::snprintf(sql, sizeof(sql), "CREATE TABLE t (content VARCHAR(200), %s %s AI COLUMN (%s) %s)",
      name, sql_type, expression, options);
  ObParser parser(alloc, 0);
  ParseResult parsed{};
  int ret = parser.parse(ObString::make_string(sql), parsed);
  expect(ret == OB_SUCCESS, "test input parses before resolver validation");
  const ParseNode *node = ret == OB_SUCCESS ? find_ai(parsed.result_tree_) : nullptr;
  if (node != nullptr) ret = ObAIColumnResolver::resolve_ai_column_attribute(node, column, alloc, ObString(), def);
  else if (ret == OB_SUCCESS) ret = OB_ERR_UNEXPECTED;
  std::printf("CASE expr=%s options=%s ret=%d retries=%lld sync=%.*s spec=%.*s deps=%lld\n",
      expression, options, ret, (long long)def.max_retries_, def.sync_mode_.length(), def.sync_mode_.ptr(),
      def.result_spec_.length(), def.result_spec_.ptr(), (long long)def.dep_column_names_.count());
  return ret;
}
static void test_definitions()
{
  struct Case { const char *expr; const char *options; ObObjType type; int expected; };
  // expected: 1 success, -1 rejection, 0 observation only (no specified duplicate policy or arbitrary numeric cap).
  const Case cases[] = {
    {"AI_COMPLETE('m', content)", "", ObJsonType, 1},
    {"AI_COMPLETE('m', content)", "WITH(sync_mode='async', max_retries=2)", ObIntType, 1},
    {"AI_EMBED('m', content)", "", ObCollectionSQLType, 1},
    {"AI_EMBED('m', content,4)", "", ObCollectionSQLType, 1},
    {"AI_COMPLETE('m', content)", "WITH(max_retries=0)", ObJsonType, 1},
    {"AI_COMPLETE('m', content)", "WITH(max_retries=-1)", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(max_retries=1.5)", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(max_retries='nonsense')", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(max_retries='2')", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(max_retries=9223372036854775807)", ObJsonType, 0},
    {"AI_COMPLETE('m', content)", "WITH(max_retries=9223372036854775808)", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(max_retries=18446744073709551616)", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(sync_mode=1)", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(sync_mode='sync')", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(sync_mode='invalid')", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(unknown_option=2)", ObJsonType, -1},
    {"AI_COMPLETE('m', content)", "WITH(max_retries=1,max_retries=2)", ObJsonType, 0},
    {"AI_COMPLETE('m', content)", "WITH(sync_mode='async',sync_mode='async')", ObJsonType, 0},
    {"CONCAT('m', content)", "", ObJsonType, -1},
    {"AI_COMPLETE()", "", ObJsonType, -1},
    {"AI_COMPLETE('m')", "", ObJsonType, -1},
    {"AI_COMPLETE('m',content,1,2)", "", ObJsonType, -1},
    {"AI_COMPLETE(7,content)", "", ObJsonType, -1},
    {"AI_COMPLETE('m',content,42)", "", ObJsonType, 0},
    {"AI_EMBED('m',content,3)", "", ObCollectionSQLType, -1},
    {"AI_EMBED('m',content,4.0)", "", ObCollectionSQLType, -1},
    {"AI_EMBED('m',content,0)", "", ObCollectionSQLType, -1},
    {"AI_EMBED('m',content,-1)", "", ObCollectionSQLType, -1},
    {"AI_EMBED('m',content)", "", ObJsonType, -1}
  };
  for (const Case &c : cases) {
    ObArenaAllocator alloc("AiDefTest");
    ObColumnSchemaV2 column;
    init_column(column, "tags", 17, c.type);
    ObAIColumnDefinition def;
    const int ret = resolve(c.expr, c.options, c.type, "tags", alloc, column, def);
    if (c.expected == 1) expect(ret == OB_SUCCESS, "valid AI definition is accepted");
    else if (c.expected == -1) expect(ret != OB_SUCCESS, "invalid AI definition is rejected");
    if (ret == OB_SUCCESS && c.expected == 1) expect(ObAIColumnResolver::is_ai_column(column), "valid definition sets schema flag");
  }
}
static void test_dependencies()
{
  ObArenaAllocator alloc("AiDepTest");
  ObTableSchema table;
  table.set_table_id(50001);
  table.set_table_type(USER_TABLE);
  ObColumnSchemaV2 content, first, second;
  init_column(content, "content", 16, ObIntType);
  init_column(first, "first_ai", 17, ObJsonType);
  init_column(second, "second_ai", 18, ObJsonType);
  ObAIColumnDefinition a, b;
  expect(resolve("AI_COMPLETE('m', second_ai)", "", ObJsonType, "first_ai", alloc, first, a) == OB_SUCCESS,
      "parse first cyclic definition");
  expect(resolve("AI_COMPLETE('m', first_ai)", "", ObJsonType, "second_ai", alloc, second, b) == OB_SUCCESS,
      "parse second cyclic definition");
  expect(table.add_column(content) == OB_SUCCESS && table.add_column(first) == OB_SUCCESS &&
      table.add_column(second) == OB_SUCCESS, "add dependency test columns");
  const int ar = ObAIColumnResolver::check_ai_column_dependencies(table, a);
  const int br = ObAIColumnResolver::check_ai_column_dependencies(table, b);
  std::printf("DEPENDENCY cycle first=%d second=%d\n", ar, br);
  expect(ar == OB_NOT_SUPPORTED && br == OB_NOT_SUPPORTED, "both AI-to-AI references rejected");
  a.dep_column_names_.reset();
  expect(a.dep_column_names_.push_back(ObString::make_string("content")) == OB_SUCCESS, "ordinary dependency setup");
  expect(ObAIColumnResolver::check_ai_column_dependencies(table, a) == OB_SUCCESS, "ordinary dependency accepted");
}
static void test_hidden_collisions()
{
  const char *names[] = {"__ai_state_tags", "__ai_req_tags", "__ai_result_spec_tags"};
  for (const char *name : names) {
    ObArenaAllocator alloc("AiCollision");
    ObTableSchema table;
    table.set_table_id(50001);
    table.set_table_type(USER_TABLE);
    ObColumnSchemaV2 content, conflict, ai;
    init_column(content, "content", 16, ObIntType);
    init_column(conflict, name, 17, ObIntType);
    init_column(ai, "tags", 18, ObJsonType);
    ObAIColumnDefinition def;
    expect(resolve("AI_COMPLETE('m',content)", "", ObJsonType, "tags", alloc, ai, def) == OB_SUCCESS,
        "collision test AI definition resolves");
    expect(table.add_column(content) == OB_SUCCESS && table.add_column(conflict) == OB_SUCCESS &&
        table.add_column(ai) == OB_SUCCESS, "add user collision and AI columns");
    const int ret = ObAIColumnResolver::append_hidden_columns(table, alloc);
    const ObColumnSchemaV2 *stored = table.get_column_schema(ObString::make_string(name));
    std::printf("COLLISION %s ret=%d count=%lld existing_hidden=%d existing_id=%llu\n", name, ret,
        (long long)table.get_column_count(), stored == nullptr ? -1 : stored->is_hidden(),
        stored == nullptr ? 0ULL : (unsigned long long)stored->get_column_id());
    expect(ret != OB_SUCCESS, "user-owned column cannot be silently reused as AI runtime hidden column");
    expect(stored != nullptr && !stored->is_hidden() && stored->get_column_id() == 17,
        "conflicting user column remains unmodified");
  }
}
static void test_hidden_result_spec_accuracy()
{
  ObArenaAllocator alloc("AiAccuracy");
  ObTableSchema table;
  table.set_table_id(50001);
  table.set_table_type(USER_TABLE);
  ObColumnSchemaV2 content, ai;
  init_column(content, "content", 16, ObIntType);
  init_column(ai, "tags", 17, ObJsonType);
  ObAIColumnDefinition def;
  expect(resolve("AI_COMPLETE('m',content)", "", ObJsonType, "tags", alloc, ai, def) == OB_SUCCESS,
      "accuracy test AI definition resolves");
  expect(table.add_column(content) == OB_SUCCESS && table.add_column(ai) == OB_SUCCESS,
      "add accuracy test columns");
  expect(ObAIColumnResolver::append_hidden_columns(table, alloc) == OB_SUCCESS,
      "append accuracy test hidden columns");
  const ObColumnSchemaV2 *spec = table.get_column_schema(def.result_spec_column_name_);
  expect(spec != nullptr, "result spec hidden column exists");
  if (spec != nullptr) {
    const int64_t expected = ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType].get_length();
    std::printf("ACCURACY result_spec length=%lld expected=%lld\n",
        (long long)spec->get_data_length(), (long long)expected);
    expect(spec->get_data_type() == ObLongTextType, "result spec remains LONGTEXT");
    expect(spec->get_data_length() == expected,
        "hidden LONGTEXT uses normal DDL default accuracy, not negative unset length");
    ObObjType type = spec->get_data_type();
    int32_t length = spec->get_data_length();
    const int ret = ObDDLResolver::check_text_length(spec->get_charset_type(),
        spec->get_collation_type(), spec->get_column_name(), type, length, true);
    std::printf("ACCURACY real DDL text validation ret=%d normalized_length=%d\n", ret, length);
    expect(ret == OB_SUCCESS, "generated hidden LONGTEXT passes real ALTER text length validation");
  }
}
int main()
{
  test_hidden_result_spec_accuracy();
  test_definitions();
  test_dependencies();
  test_hidden_collisions();
  std::printf("native AI definition: checks=%d failures=%d\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
