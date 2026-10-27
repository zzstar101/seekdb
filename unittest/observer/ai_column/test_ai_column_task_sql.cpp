// Native SQL-builder regressions. Run with run_sql_builder_test.py after a release build.
#include <cstdio>
#include <string>
#include "observer/ai_service/ai_column/ob_ai_column_task.h"
#include "sql/parser/ob_parser.h"

static ParseNode *find_node(ParseNode *node, int type)
{
  if (node == nullptr || node->type_ == type) return node;
  for (int i = 0; i < node->num_child_; ++i) {
    if (ParseNode *found = find_node(node->children_[i], type)) return found;
  }
  return nullptr;
}

using namespace oceanbase::common;
using oceanbase::observer::ai_column::AiColumnTaskRow;
using oceanbase::observer::ai_column::AiColumnTaskSql;

// The task row is printed by every LOG_* site of the runtime, so its
// TO_STRING_KV is the single place that decides what reaches observer.log: the
// identifiers and the state, plus the *lengths* of the materialized prompt and
// of the model output, but never their text (both are row content).
static void test_task_row_log_format(int &failures)
{
  auto check = [&failures](bool condition, const char *name) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
  };
  AiColumnTaskRow row;
  row.table_id_ = 10001;
  row.column_id_ = 16;
  row.prompt_ = ObString::make_string("Classify: customer row payload");
  row.result_data_ = ObString::make_string("mock:model-output");
  char buf[8192];
  const int64_t len = row.to_string(buf, static_cast<int64_t>(sizeof(buf)));
  const std::string text(buf, len > 0 ? static_cast<size_t>(len) : 0);
  check(text.find("customer row payload") == std::string::npos,
        "the materialized prompt is never printed");
  check(text.find("mock:model-output") == std::string::npos,
        "the model output is never printed");
  check(text.find("prompt_len") != std::string::npos &&
        text.find("result_data_len") != std::string::npos,
        "the prompt and the model output are printed as lengths");
  check(text.find("10001") != std::string::npos && text.find("16") != std::string::npos,
        "the identifiers are still printed for support");
}

int main()
{
  int failures = 0;
  auto check = [&failures](bool condition, const char *name) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
  };
  test_task_row_log_format(failures);
  ObSqlString sql;
  const char identifier[] = {'a', '`', 'b'};
  check(OB_SUCCESS == AiColumnTaskSql::append_backquoted(sql, ObString(3, identifier)),
        "bounded identifier accepted");
  check(std::string(sql.ptr(), sql.length()) == "`a``b`", "embedded backtick is doubled");

  sql.reset();
  const char bytes[] = {'a', '\'', '\\', '\0', '\n'};
  check(OB_SUCCESS == AiColumnTaskSql::append_literal(sql, ObString(sizeof(bytes), bytes)),
        "binary-safe literal accepted");
  check(std::string(sql.ptr(), sql.length()) == "CONVERT(X'61275C000A' USING utf8mb4)",
        "literal is independent of NO_BACKSLASH_ESCAPES and preserves NUL");

  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::count_unfinished_siblings(
      10001, ObString::make_string("`id`=1"), ObString::make_string("request"), sql),
      "group gate SQL builds");
  check(std::string(sql.ptr(), sql.length()).find("state NOT IN (4, 5)") != std::string::npos,
        "both READY and FAILED siblings are terminal");

  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::select_publish_payload(
      10001, ObString::make_string("`id`=1"), ObString::make_string("request"), sql),
      "group payload SQL builds");
  const std::string payload(sql.ptr(), sql.length());
  check(payload.find("state IN (4, 5)") != std::string::npos,
        "publication includes failed siblings as well as successful siblings");
  check(payload.find("column_id, state, definition_version, result_data") != std::string::npos,
        "publication carries each sibling state and definition fence");
  oceanbase::observer::ai_column::AiColumnDef def;
  def.table_id_ = 10001;
  def.column_id_ = 16;
  def.state_column_id_ = 17;
  def.request_column_id_ = 18;
  def.table_name_ = ObString::make_string("`db`.`tbl`");
  def.column_name_ = ObString::make_string("a`b");
  def.state_column_name_ = ObString::make_string("s`t");
  def.request_column_name_ = ObString::make_string("r`q");
  def.model_name_ = ObString::make_string("model");
  def.prompt_expr_ = ObString::make_string("content");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::enqueue_task(def, ObString::make_string("`id`=1"),
                                                   ObString::make_string("request"), sql),
        "enqueue SQL builds");
  ObArenaAllocator parse_allocator("AiTaskParse");
  oceanbase::sql::ObParser parser(parse_allocator, 0);
  ParseResult parsed{};
  check(OB_SUCCESS == parser.parse(ObString(sql.length(), sql.ptr()), parsed),
        "enqueue SQL parses with the real SQL parser");
  ParseNode *columns = find_node(parsed.result_tree_, T_COLUMN_LIST);
  ParseNode *projection = find_node(parsed.result_tree_, T_PROJECT_LIST);
  check(columns != nullptr && projection != nullptr && columns->num_child_ == projection->num_child_,
        "every queued task column has a SELECT value, including both timestamps");
  const std::string enqueue(sql.ptr(), sql.length());
  check(enqueue.find("REPLACE INTO oceanbase.__all_ai_column_task ") == 0,
        "enqueue targets the system queue independently of the business database");
  oceanbase::observer::ai_column::AiColumnDefArray defs;
  check(OB_SUCCESS == defs.push_back(def), "definition allocated");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::invalidate_row(defs, ObString::make_string("`id`=1"),
                                                    ObString::make_string("request"), sql),
        "invalidation SQL builds");
  const std::string invalidation(sql.ptr(), sql.length());
  check(invalidation.find("`a``b` = NULL") != std::string::npos &&
        invalidation.find("`s``t` = 1") != std::string::npos &&
        invalidation.find("`r``q` = ") != std::string::npos,
        "invalidation quotes all result and hidden identifiers");
  ObSEArray<ObString, 4> values;
  check(OB_SUCCESS == values.push_back(ObString::make_string("result")), "result allocated");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::publish_row(defs, ObString::make_string("`id`=1"),
                                                 ObString::make_string("request"), values, sql),
        "publication SQL builds");
  const std::string publication(sql.ptr(), sql.length());
  check(publication.find("`a``b` = ") != std::string::npos &&
        publication.find("`s``t` = 4") != std::string::npos &&
        publication.find("AND `r``q` = ") != std::string::npos,
        "publication quotes identifiers in both assignment and request fence");
  ObSEArray<int64_t, 4> states;
  check(OB_SUCCESS == states.push_back(5), "failed state allocated");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::publish_row(defs, ObString::make_string("`id`=1"),
      ObString::make_string("request"), values, sql, &states), "failed publication SQL builds");
  const std::string failed(sql.ptr(), sql.length());
  check(failed.find("`a``b` = NULL") != std::string::npos &&
        failed.find("`s``t` = 5") != std::string::npos,
        "failed sibling publishes NULL and FAILED atomically");
  states.at(0) = 1;
  check(OB_INVALID_ARGUMENT == AiColumnTaskSql::publish_row(defs, ObString::make_string("`id`=1"),
      ObString::make_string("request"), values, sql, &states), "nonterminal publication rejected");
  states.reuse();
  check(OB_INVALID_ARGUMENT == AiColumnTaskSql::publish_row(defs, ObString::make_string("`id`=1"),
      ObString::make_string("request"), values, sql, &states), "misaligned publication states rejected");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::pick_eligible_tasks(16, sql), "recovery SQL builds");
  const std::string recovery(sql.ptr(), sql.length());
  check(recovery.find("state IN (4, 5)") != std::string::npos &&
        recovery.find("NOT EXISTS") != std::string::npos,
        "terminal recovery skips groups with unfinished siblings to avoid starvation");
  oceanbase::observer::ai_column::AiColumnTaskRow task;
  task.table_id_ = 10001;
  task.column_id_ = 16;
  task.rowkey_predicate_ = ObString::make_string("`id`=1");
  task.request_id_ = ObString::make_string("request");
  task.definition_version_ = 7;
  check(OB_SUCCESS == AiColumnTaskSql::store_retry(task, 1793056000123456L,
      ObString::make_string("temporary upstream failure"), sql), "retry SQL builds");
  check(std::string(sql.ptr(), sql.length()).find("from_unixtime(1793056000.123456)") != std::string::npos,
        "retry eligibility retains microseconds instead of scheduling in the past");
  check(OB_SUCCESS == AiColumnTaskSql::claim_task(task, ObString::make_string("owner"),
      1500001L, sql), "fractional-second lease SQL builds");
  check(std::string(sql.ptr(), sql.length()).find("interval 1500001 microsecond") != std::string::npos,
        "lease expiry retains the complete requested duration");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::delete_task(task, sql), "exact task cleanup SQL builds");
  const std::string cleanup(sql.ptr() == nullptr ? "" : sql.ptr(), sql.length());
  check(cleanup.find("table_id = 10001") != std::string::npos &&
        cleanup.find("column_id = 16") != std::string::npos &&
        cleanup.find("rowkey_predicate = ") != std::string::npos &&
        cleanup.find("request_id = CONVERT(X'72657175657374' USING utf8mb4)") != std::string::npos &&
        cleanup.find("definition_version = 7") != std::string::npos,
        "orphan cleanup cannot delete sibling columns or superseding requests");
  auto sibling = def;
  sibling.column_id_ = 20;
  sibling.column_name_ = ObString::make_string("summary");
  sibling.state_column_name_ = ObString::make_string("summary_state");
  sibling.request_column_name_ = ObString::make_string("summary_request");
  check(OB_SUCCESS == defs.push_back(sibling), "sibling definition allocated");
  check(OB_SUCCESS == values.push_back(ObString::make_string("ok")), "sibling result allocated");
  check(OB_SUCCESS == states.push_back(5) && OB_SUCCESS == states.push_back(4), "mixed states allocated");
  for (int order = 0; order < 2; ++order) {
    sql.reset();
    check(OB_SUCCESS == AiColumnTaskSql::publish_row(defs, ObString::make_string("`id`=1"),
        ObString::make_string("request"), values, sql, &states), "mixed group publication builds");
    const std::string mixed(sql.ptr(), sql.length());
    check(mixed.find(order == 0 ? "`a``b` = NULL" : "`summary` = NULL") != std::string::npos &&
          mixed.find(order == 0 ? "`summary` = CONVERT(" : "`a``b` = CONVERT(") != std::string::npos &&
          mixed.find("AND `r``q` = ") != std::string::npos &&
          mixed.find("AND `summary_request` = ") != std::string::npos,
          "mixed success and failure retain both request fences in either order");
    states.at(0) = 4;
    states.at(1) = 5;
  }
  std::printf("AI task SQL builder: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
