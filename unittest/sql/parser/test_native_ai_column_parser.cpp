// Native AI COLUMN AST contract: links the production ObParser, no mocks.
#include <cstdio>
#include <cstring>
#include "lib/allocator/ob_allocator.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_define.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
static void dump(const ParseNode *node, int depth = 0)
{
  if (node == nullptr) { std::printf("%*sNULL\n", depth * 2, ""); return; }
  std::printf("%*s%s children=%d value=%lld str=[%.*s] raw=[%.*s] loc=%d:%d off=%lld str_len=%lld text_len=%lld raw_null=%d\n",
      depth * 2, "", get_type_name(node->type_), node->num_child_, (long long)node->value_,
      (int)node->str_len_, node->str_value_ == nullptr ? "" : node->str_value_,
      (int)node->text_len_, node->raw_text_ == nullptr ? "" : node->raw_text_,
      node->stmt_loc_.first_column_, node->stmt_loc_.last_column_, (long long)node->sql_str_off_,
      (long long)node->str_len_, (long long)node->text_len_, node->raw_text_ == nullptr);
  for (int i = 0; i < node->num_child_; ++i) dump(node->children_[i], depth + 1);
}
static const ParseNode *find_ai(const ParseNode *node)
{
  if (node == nullptr || node->type_ == T_AI_COLUMN) return node;
  for (int i = 0; i < node->num_child_; ++i) {
    if (const ParseNode *found = find_ai(node->children_[i])) return found;
  }
  return nullptr;
}
static int failures = 0;
static int checks = 0;
static void expect(bool ok, const char *message)
{
  ++checks;
  if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
static bool text_is(const ParseNode *node, const char *text)
{
  return node != nullptr && node->str_value_ != nullptr &&
      node->str_len_ == (int64_t)std::strlen(text) &&
      std::memcmp(node->str_value_, text, node->str_len_) == 0;
}
static int check_refs(const ParseNode *node)
{
  if (node == nullptr) return 0;
  if (node->type_ == T_COLUMN_REF) {
    expect(node->num_child_ == 3, "column reference has [database, table, column]");
    expect(node->num_child_ == 3 && node->children_[2] != nullptr &&
        node->children_[2]->type_ == T_IDENT, "column name is IDENT child[2]");
    return 1;
  }
  int count = 0;
  for (int i = 0; i < node->num_child_; ++i) count += check_refs(node->children_[i]);
  return count;
}
int main()
{
  expect(T_REF_COLUMN != T_COLUMN_REF, "raw parser column nodes differ from resolved reference nodes");
  const char *queries[] = {
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', content)))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (ai_complete('m', CONCAT('prefix:', content))))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', 'literal prompt')))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', CONCAT(content, `content`, a.content))))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result VECTOR(4) AI COLUMN (AI_EMBED('m', content, 4)))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result VECTOR(4) AI COLUMN (AI_EMBED('m', content)))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', content)) WITH (sync_mode='async', max_retries=2))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result VECTOR(4) AI COLUMN (AI_EMBED('m', content, 4.0)))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', `content`)))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', a.content)))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', CONCAT('it''s: ', content))))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', content)) WITH (max_retries=0))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', id + 1)))",
    "CREATE TABLE a (id INT PRIMARY KEY, content VARCHAR(128), result JSON AI COLUMN (AI_COMPLETE('m', NULL)))"
  };
  const int reference_counts[] = {1, 1, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
  const char *prompt_sql[] = {"content", "CONCAT('prefix:', content)", "'literal prompt'",
    "CONCAT(content, `content`, a.content)", "content", "content", "content", "content",
    "`content`", "a.content", "CONCAT('it''s: ', content)", "content", "id + 1", "NULL"};
  int case_index = 0;
  for (const char *query : queries) {
    ObArenaAllocator allocator("AiParserTest");
    ObParser parser(allocator, 0);
    ParseResult result{};
    const int ret = parser.parse(ObString::make_string(query), result);
    std::printf("SQL %s\nret=%d error=%s\n", query, ret, result.error_msg_);
    const ParseNode *ai = ret == OB_SUCCESS ? find_ai(result.result_tree_) : nullptr;
    expect(ret == OB_SUCCESS && ai != nullptr, "DDL parses and contains AI COLUMN");
    if (ai != nullptr) {
      dump(ai);
      expect(ai->num_child_ == 2, "AI COLUMN has expression and optional WITH list");
      const ParseNode *func = ai->num_child_ == 2 ? ai->children_[0] : nullptr;
      expect(func != nullptr && func->type_ == T_FUN_SYS && func->num_child_ == 2,
          "AI function uses generic [name, args] layout");
      const ParseNode *args = func != nullptr && func->num_child_ == 2 ? func->children_[1] : nullptr;
      const bool embed = case_index == 4 || case_index == 5 || case_index == 7;
      if (func != nullptr && func->num_child_ == 2) {
        expect(text_is(func->children_[0], embed ? "AI_EMBED" : case_index == 1 ? "ai_complete" : "AI_COMPLETE"),
            "function identifier preserves spelling; AI_COMPLETE has eleven characters");
      }
      const int expected_args = case_index == 4 || case_index == 7 ? 3 : 2;
      expect(args != nullptr && args->type_ == T_EXPR_LIST && args->num_child_ == expected_args,
          "function arguments are children of EXPR_LIST");
      if (args != nullptr && args->num_child_ >= 2) {
        const ParseNode *prompt = args->children_[1];
        expect(prompt != nullptr && prompt->raw_text_ != nullptr &&
            prompt->text_len_ == static_cast<int64_t>(std::strlen(prompt_sql[case_index])) &&
            std::memcmp(prompt->raw_text_, prompt_sql[case_index], prompt->text_len_) == 0,
            "prompt retains exact SQL, not decoded text or the enclosing AI call");
        expect(args->children_[0]->type_ == T_VARCHAR && text_is(args->children_[0], "m"),
            "model is a decoded VARCHAR literal");
        expect(check_refs(prompt) == reference_counts[case_index], "prompt dependencies are raw T_COLUMN_REF nodes");
        if (case_index == 2) {
          expect(text_is(prompt, "literal prompt"), "literal str_value is decoded content, not SQL");
          expect(prompt->raw_text_ != nullptr && prompt->text_len_ == 16 &&
              std::memcmp(prompt->raw_text_, "'literal prompt'", 16) == 0,
              "literal raw_text preserves SQL quotes");
        }
        if (case_index == 8 || case_index == 9) {
          expect(text_is(prompt, "content"), "column str_value omits quoting and qualification");
          expect(prompt->type_ == T_COLUMN_REF && prompt->num_child_ == 3 &&
              text_is(prompt->children_[2], "content"), "leaf identifier is the dependency name");
          if (case_index == 9) expect(text_is(prompt->children_[1], "a"), "table qualifier is child[1]");
        }
        if (case_index == 4) expect(args->children_[2]->type_ == T_INT && args->children_[2]->value_ == 4,
            "integer dimension uses T_INT/value_, not T_NUMBER");
        if (case_index == 7) expect(args->children_[2]->type_ == T_NUMBER &&
            text_is(args->children_[2], "4.0"), "decimal dimension uses T_NUMBER/string, not integer value_");
      }
      const ParseNode *options = ai->num_child_ == 2 ? ai->children_[1] : nullptr;
      if (case_index == 6 || case_index == 11) {
        const int expected_count = case_index == 6 ? 2 : 1;
        expect(options != nullptr && options->type_ == T_AI_COLUMN_OPTION_LIST &&
            options->num_child_ == expected_count, "WITH list preserves one node per option (not flattened keys/values)");
        if (options != nullptr) {
          for (int i = 0; i < options->num_child_; ++i) {
            const ParseNode *option = options->children_[i];
            expect(option != nullptr && option->type_ == T_ASSIGN_ITEM && option->num_child_ == 2,
                "WITH item is an atomic T_ASSIGN_ITEM key/value pair");
            if (option != nullptr && option->type_ == T_ASSIGN_ITEM && option->num_child_ == 2 &&
                text_is(option->children_[0], "max_retries")) {
              expect(option->children_[1]->type_ == T_INT && option->children_[1]->value_ == (case_index == 6 ? 2 : 0),
                  "max_retries uses T_INT, including zero");
            }
          }
        }
      } else expect(options == nullptr, "omitted WITH has null options");
    }
    ++case_index;
  }
  std::printf("native AI AST: cases=%d checks=%d failures=%d\n", case_index, checks, failures);
  return failures == 0 ? 0 : 1;
}
