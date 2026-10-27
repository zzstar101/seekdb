// Native parser regression runner; no parser mocks or source substitution.
#include <cstdio>
#include "lib/allocator/ob_allocator.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_define.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
int main()
{
  int failures = 0;
  const char *queries[] = {
    "SELECT 1", "EXPLAIN SELECT 1", "DESCRIBE SELECT 1",
    "DESCRIBE SELECT * FROM read_csv('/tmp/a.csv')",
    "COPY (SELECT 42 AS id) TO '/tmp/a.csv' (FORMAT CSV, HEADER, DELIMITER ';')",
    "COPY t TO '/tmp/a.csv' (FORMAT CSV, HEADER, DELIMITER ';')"
  };
  for (const char *query : queries) {
    ObArenaAllocator allocator("ParserTest");
    ObParser parser(allocator, 0);
    ParseResult result{};
    const int ret = parser.parse(ObString::make_string(query), result);
    std::printf("%s: ret=%d error=%s\n", query, ret, result.error_msg_);
    if (ret != OB_SUCCESS) ++failures;
    if (ret == OB_SUCCESS && query[0] == 'C') {
      ParseNode *select = result.result_tree_->children_[0];
      if (query[5] != '(') {
        ParseNode *from = select->children_[PARSE_SELECT_FROM];
        ParseNode *table = from != nullptr && from->num_child_ == 1 ? from->children_[0] : nullptr;
        if (table == nullptr || table->type_ != T_ORG || table->num_child_ != 4 ||
            table->children_[1] != nullptr || table->children_[2] != nullptr ||
            table->children_[3] != nullptr) {
          std::puts("COPY table must use the ordinary four-child table-reference layout");
          ++failures;
        }
      }
      ParseNode *into = select->children_[PARSE_SELECT_INTO_EXTRA];
      if (into == nullptr || !into->is_tree_not_param_) {
        std::puts("COPY path/options must not be parameterized");
        ++failures;
      }
      ParseNode *option = into == nullptr ? nullptr : into->children_[1];
      int count = 0;
      while (option != nullptr) {
        ParseNode *head = option->type_ == T_LINK_NODE ? option->children_[0] : option;
        if (head == nullptr || head->type_ != T_COPY_OPTION) {
          std::puts("COPY option list must be a right-linked chain");
          ++failures;
          break;
        }
        ++count;
        option = option->type_ == T_LINK_NODE ? option->children_[1] : nullptr;
      }
      if (count != 3) ++failures;
    }
  }
  struct RetryCase { const char *sql; const char *predicate; };
  const RetryCase retries[] = {
    {"ALTER TABLE t RETRY AI COLUMN c WHERE id IN (1, 3)", "id IN (1, 3)"},
    {"ALTER TABLE t RETRY AI COLUMN c WHERE status = 'FAILED' AND `status` = '业务'",
     "status = 'FAILED' AND `status` = '业务'"},
    {"ALTER TABLE t RETRY AI COLUMN c", nullptr}
  };
  for (const RetryCase &test : retries) {
    ObArenaAllocator allocator("RetryParserTest");
    ObParser parser(allocator, 0);
    ParseResult result{};
    const int ret = parser.parse(ObString::make_string(test.sql), result);
    ParseNode *alter = ret == OB_SUCCESS ? result.result_tree_->children_[0] : nullptr;
    ParseNode *actions = alter != nullptr && alter->num_child_ == 4 ? alter->children_[1] : nullptr;
    ParseNode *action = actions != nullptr && actions->type_ == T_ALTER_TABLE_ACTION_LIST &&
        actions->num_child_ == 1 ? actions->children_[0] : nullptr;
    if (action == nullptr || action->type_ != T_AI_COLUMN_RETRY || action->num_child_ != 2) {
      std::printf("invalid RETRY action-list layout: %s\n", test.sql);
      ++failures;
    } else {
      ParseNode *where = action->children_[1];
      if ((test.predicate == nullptr && where != nullptr) ||
          (test.predicate != nullptr && (where == nullptr || where->str_value_ == nullptr ||
           ObString(where->str_len_, where->str_value_) != ObString::make_string(test.predicate)))) {
        std::printf("RETRY must retain only the exact predicate, excluding WHERE: %s\n", test.sql);
        ++failures;
      }
    }
  }
  return failures == 0 ? 0 : 1;
}
