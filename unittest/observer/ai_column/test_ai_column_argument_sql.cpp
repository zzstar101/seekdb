// Native regressions for the AI column argument contract (SQL side).
//
// The enqueuing statement must carry the prompt and the config of the AI
// function inside the versioned JSON input envelope, so the arguments keep the
// types they had in the user statement (JSON object vs plain string vs NULL) and
// so the NOT NULL task column can never be violated by a NULL source column.
// The publish side must be able to write a SQL NULL result (AI_EMBED of a NULL
// input) without confusing it with an empty string.
//
// Build and run with run_native_argument_contract_test.py after a release build.
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
using oceanbase::observer::ai_column::AiColumnTaskSql;
using oceanbase::observer::ai_column::AiColumnDef;
using oceanbase::observer::ai_column::AiColumnDefArray;
using oceanbase::observer::ai_column::AiColumnTaskRow;

int main()
{
  int failures = 0;
  auto check = [&failures](bool condition, const char *name) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
  };

  AiColumnDef def;
  def.table_id_ = 10001;
  def.column_id_ = 16;
  def.state_column_id_ = 17;
  def.request_column_id_ = 18;
  def.table_name_ = ObString::make_string("`db`.`tbl`");
  def.column_name_ = ObString::make_string("summary");
  def.state_column_name_ = ObString::make_string("summary_state");
  def.request_column_name_ = ObString::make_string("summary_request");
  def.model_name_ = ObString::make_string("model");
  def.prompt_expr_ = ObString::make_string("concat('Classify: ', content)");
  def.result_spec_ = ObString::make_string("varchar");

  // ---- A1: the prompt is snapshotted inside the versioned JSON envelope ------
  ObSqlString sql;
  check(OB_SUCCESS == AiColumnTaskSql::enqueue_task(def, ObString::make_string("`id`=1"),
                                                    ObString::make_string("request"), sql),
        "enqueue SQL builds");
  const std::string envelope(sql.ptr(), sql.length());
  check(envelope.find("JSON_OBJECT(") != std::string::npos,
        "the enqueued prompt is wrapped by the native JSON_OBJECT constructor");
  check(envelope.find("'v', 1, 'prompt', (") != std::string::npos,
        "the envelope carries its version and the prompt argument");
  check(envelope.find("concat('Classify: ', content)") != std::string::npos,
        "the prompt expression is evaluated by the enqueuing statement itself");
  check(envelope.find("'config'") == std::string::npos,
        "a definition without a config enqueues no config member");

  // the statement really is valid SQL: the real parser sees one value per column
  ObArenaAllocator parse_allocator("AiArgumentParse");
  oceanbase::sql::ObParser parser(parse_allocator, 0);
  ParseResult parsed{};
  check(OB_SUCCESS == parser.parse(ObString(sql.length(), sql.ptr()), parsed),
        "enqueue SQL parses with the real SQL parser");
  ParseNode *columns = find_node(parsed.result_tree_, T_COLUMN_LIST);
  ParseNode *projection = find_node(parsed.result_tree_, T_PROJECT_LIST);
  check(columns != nullptr && projection != nullptr
          && columns->num_child_ == projection->num_child_,
        "the envelope is one column value, so the queue insert stays aligned");

  // ---- A3: the config is snapshotted in the same statement -------------------
  def.config_expr_ = ObString::make_string("json_object('temperature', 0.2, 'max_tokens', 8)");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::enqueue_task(def, ObString::make_string("`id`=1"),
                                                    ObString::make_string("request"), sql),
        "enqueue SQL with a config builds");
  const std::string with_config(sql.ptr(), sql.length());
  check(with_config.find(", 'config', (json_object('temperature', 0.2, 'max_tokens', 8))")
            != std::string::npos,
        "the config expression is evaluated next to the prompt in the user transaction");
  def.config_expr_.reset();

  // ---- A5: a staged SQL NULL result is published as NULL ---------------------
  AiColumnDefArray defs;
  check(OB_SUCCESS == defs.push_back(def), "definition allocated");
  AiColumnTaskRow task;
  task.table_id_ = 10001;
  task.column_id_ = 16;
  task.rowkey_predicate_ = ObString::make_string("`id`=1");
  task.request_id_ = ObString::make_string("request");
  task.definition_version_ = 7;
  task.lease_owner_ = ObString::make_string("worker");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::store_success(task, ObString::make_string("ignored"), sql,
                                                     true),
        "staged NULL result SQL builds");
  check(std::string(sql.ptr(), sql.length()).find("result_data = NULL") != std::string::npos,
        "a SQL NULL model result is staged as NULL, not as an empty string");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::store_success(task, ObString::make_string(""), sql, false),
        "staged empty result SQL builds");
  check(std::string(sql.ptr(), sql.length()).find("result_data = CONVERT(X'' USING utf8mb4)")
            != std::string::npos,
        "an empty string result stays an empty string");

  ObSEArray<ObString, 4> results;
  check(OB_SUCCESS == results.push_back(ObString::make_string("")), "result allocated");
  ObSEArray<bool, 4> nulls;
  check(OB_SUCCESS == nulls.push_back(true), "null flag allocated");
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::publish_row(defs, ObString::make_string("`id`=1"),
                                                   ObString::make_string("request"),
                                                   results, sql, nullptr, &nulls),
        "publication of a NULL EMBED result builds");
  const std::string published_null(sql.ptr(), sql.length());
  check(published_null.find("`summary` = NULL") != std::string::npos
          && published_null.find("`summary_state` = 4") != std::string::npos,
        "a READY EMBED row of a NULL input publishes NULL, never an empty vector");
  nulls.at(0) = false;
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::publish_row(defs, ObString::make_string("`id`=1"),
                                                   ObString::make_string("request"),
                                                   results, sql, nullptr, &nulls),
        "publication of an empty-but-not-NULL result builds");
  check(std::string(sql.ptr(), sql.length()).find("`summary` = CONVERT(X'' USING utf8mb4)")
            != std::string::npos,
        "an empty string result is still published as an empty string");

  // the publish payload exposes the NULL flag to the worker
  sql.reset();
  check(OB_SUCCESS == AiColumnTaskSql::select_publish_payload(
      10001, ObString::make_string("`id`=1"), ObString::make_string("request"), sql),
        "publish payload SQL builds");
  check(std::string(sql.ptr(), sql.length()).find("result_data IS NULL AS result_is_null")
            != std::string::npos,
        "the publish payload tells a NULL staged result from an empty one");

  std::printf("AI column argument SQL: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
