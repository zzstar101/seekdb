/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/ddl/ob_ai_column_resolver.h"

#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_print_utils.h"
#include "share/ob_errno.h"
#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"
#include "query/api/query/vector/ob_vector_index_util.h"
#include "sql/parser/ob_item_type.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{

const char *ObAIColumnResolver::AI_COLUMN_META_PREFIX = share::schema::kAIColumnMetaPrefix;
const char *ObAIColumnResolver::AI_STATE_COLUMN_PREFIX = share::schema::kAIStateColumnPrefix;
const char *ObAIColumnResolver::AI_REQUEST_COLUMN_PREFIX = share::schema::kAIRequestColumnPrefix;
const char *ObAIColumnResolver::AI_RESULT_SPEC_COLUMN_PREFIX =
    share::schema::kAIResultSpecColumnPrefix;
const char *ObAIColumnResolver::AI_JSON_RESULT_INSTRUCTION =
    share::schema::kAIJsonResultInstruction;
const char *ObAIColumnResolver::AI_VECTOR_RESULT_INSTRUCTION =
    share::schema::kAIVectorResultInstruction;

namespace
{

int copy_str(ObIAllocator &alloc, const ObString &src, ObString &dst)
{
  return ob_write_string(alloc, src, dst);
}

int copy_literal(ObIAllocator &alloc, const char *src, ObString &dst)
{
  ObString tmp;
  tmp.assign_ptr(src, static_cast<int32_t>(strlen(src)));
  return copy_str(alloc, tmp, dst);
}

int add_hidden_column(ObTableSchema &table, ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  const uint64_t max_id = table.get_max_used_column_id();
  if (max_id < OB_APP_MIN_COLUMN_ID || max_id >= OB_MIN_SHADOW_COLUMN_ID - 1) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    column.set_column_id(max_id + 1);
    ret = table.add_column(column);
  }
  return ret;
}

// Prefer the exact SQL captured for AI-call arguments, not decoded literal text.
void node_text(const ParseNode *node, ObString &text)
{
  if (NULL != node && NULL != node->raw_text_ && node->text_len_ > 0) {
    text.assign_ptr(node->raw_text_, node->text_len_);
  } else if (NULL != node && NULL != node->str_value_ && node->str_len_ > 0) {
    text.assign_ptr(node->str_value_, node->str_len_);
  }
}

bool is_ai_function_node(const ParseNode *node)
{
  bool bret = false;
  if (NULL != node) {
    if (T_FUN_SYS_AI_COMPLETE == node->type_ || T_FUN_SYS_AI_EMBED == node->type_) {
      bret = true;
    } else if (T_FUN_SYS == node->type_ && NULL != node->children_ && node->num_child_ > 0 &&
               NULL != node->children_[0] && NULL != node->children_[0]->str_value_) {
      const ParseNode *name = node->children_[0];
      const ObString function_name(name->str_len_, name->str_value_);
      bret = 0 == function_name.case_compare("ai_complete") ||
             0 == function_name.case_compare("ai_embed");
    }
  }
  return bret;
}

bool node_is_ai_embed(const ParseNode *node)
{
  bool bret = false;
  if (NULL != node) {
    if (T_FUN_SYS_AI_EMBED == node->type_) {
      bret = true;
    } else if (T_FUN_SYS == node->type_ && NULL != node->children_ && node->num_child_ > 0 &&
               NULL != node->children_[0] && NULL != node->children_[0]->str_value_) {
      const ParseNode *name = node->children_[0];
      bret = (name->str_len_ == 8 && 0 == strncasecmp(name->str_value_, "ai_embed", 8));
    }
  }
  return bret;
}

// Collect the column references of a prompt expression subtree.
int collect_column_refs(const ParseNode *node,
                        ObIAllocator &alloc,
                        ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  if (NULL == node) {
  } else if (T_COLUMN_REF == node->type_) {
    if (3 != node->num_child_ || OB_ISNULL(node->children_) ||
        OB_ISNULL(node->children_[2]) || OB_ISNULL(node->children_[2]->str_value_)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const ParseNode &identifier = *node->children_[2];
      const ObString raw(identifier.str_len_, identifier.str_value_);
      ObString name;
      bool found = false;
      for (int64_t i = 0; !found && i < names.count(); ++i) {
        found = 0 == names.at(i).case_compare(raw);
      }
      if (!found && OB_FAIL(copy_str(alloc, raw, name))) {
      } else if (!found && OB_FAIL(names.push_back(name))) {
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      if (OB_FAIL(collect_column_refs(node->children_[i], alloc, names))) {
      }
    }
  }
  return ret;
}

} // anonymous namespace

int ObAIColumnResolver::build_result_spec(ObIAllocator &alloc,
                                          const ObColumnSchemaV2 &column,
                                          int64_t &dimension,
                                          ObString &result_spec)
{
  int ret = OB_SUCCESS;
  dimension = -1;
  if (ob_is_json_tc(column.get_data_type())) {
    ret = copy_literal(alloc, "json", result_spec);
  } else if (ob_is_collection_sql_type(column.get_data_type())) {
    int64_t dim = -1;
    ObSqlString buf;
    if (OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                  column.get_extended_type_info(), dim))) {
      LOG_WARN("fail to get vector dimension", K(ret), K(column));
    } else if (FALSE_IT(dimension = dim)) {
    } else if (OB_FAIL(buf.append_fmt("vector:%ld", dim))) {
    } else if (OB_FAIL(copy_str(alloc, buf.string(), result_spec))) {
    }
  } else if (ob_is_string_type(column.get_data_type())) {
    ret = copy_literal(alloc, "varchar", result_spec);
  } else if (ob_is_integer_type(column.get_data_type())) {
    const ObObjType type = column.get_data_type();
    const bool is_unsigned = type >= ObUTinyIntType;
    const int64_t widths[] = {8, 16, 24, 32, 64};
    const int64_t index = type - (is_unsigned ? ObUTinyIntType : ObTinyIntType);
    ObSqlString spec;
    if (index < 0 || index >= 5) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(spec.append_fmt("%s:%ld", is_unsigned ? "uint" : "int", widths[index]))) {
    } else if (OB_FAIL(copy_str(alloc, spec.string(), result_spec))) {
    }
  } else if (ObFloatType == column.get_data_type() || ObDoubleType == column.get_data_type()) {
    // FLOAT(M,D)/DOUBLE(M,D) have a separate fixed-scale conversion contract.
    // Do not accept it until the worker can enforce that range before staging.
    if (column.get_data_scale() >= 0) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "fixed-scale floating AI columns are");
    } else {
      ret = copy_literal(alloc, ObFloatType == column.get_data_type() ? "float" : "double", result_spec);
    }
  } else if (ObNumberType == column.get_data_type() || ObDecimalIntType == column.get_data_type()) {
    const int64_t precision = column.get_data_precision();
    const int64_t scale = column.get_data_scale();
    ObSqlString spec;
    if (precision < 1 || precision > 65 || scale < 0 || scale > 30 || scale > precision) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI column decimal precision/scale");
    } else if (OB_FAIL(spec.append_fmt("decimal:%ld:%ld", precision, scale))) {
    } else if (OB_FAIL(copy_str(alloc, spec.string(), result_spec))) {
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI column of this result type is");
    LOG_WARN("unsupported ai column result type", K(ret), K(column.get_data_type()));
  }
  return ret;
}

int ObAIColumnResolver::parse_option_value(const ObString &name,
                                           const int64_t int_value,
                                           const ObString &str_value,
                                           ObAIColumnDefinition &def)
{
  int ret = OB_SUCCESS;
  if (name.case_compare("sync_mode") == 0) {
    if (str_value.case_compare("async") == 0) {
      def.sync_mode_ = str_value;
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "sync_mode other than async is");
      LOG_WARN("invalid sync_mode", K(ret), K(str_value));
    }
  } else if (name.case_compare("max_retries") == 0) {
    if (int_value < 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("max_retries must not be negative", K(ret), K(int_value));
    } else {
      def.max_retries_ = int_value;
    }
  } else {
    // Unknown options are rejected: the definition is part of the persisted
    // schema, so silently dropping an option would make the schema lie.
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "this AI column option is");
    LOG_WARN("unsupported ai column option", K(ret), K(name));
  }
  return ret;
}

int ObAIColumnResolver::resolve_ai_column_attribute(const ParseNode *ai_column_node,
                                                    ObColumnSchemaV2 &column,
                                                    ObIAllocator &alloc,
                                                    const ObString &user_comment,
                                                    ObAIColumnDefinition &def)
{
  int ret = OB_SUCCESS;
  const ParseNode *func_node = NULL;
  const ParseNode *args_node = NULL;
  int64_t arg_count = 0;
  def.reset();
  if (OB_ISNULL(ai_column_node) || T_AI_COLUMN != ai_column_node->type_ ||
      2 != ai_column_node->num_child_ || OB_ISNULL(ai_column_node->children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ai column node", K(ret));
  } else if (OB_ISNULL(func_node = ai_column_node->children_[0]) ||
             !is_ai_function_node(func_node)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN with a function other than AI_COMPLETE / AI_EMBED is");
    LOG_WARN("unsupported ai column function", K(ret), K(static_cast<int>(func_node->type_)));
  } else {
    // Argument layout of both AI functions: (model, prompt [, dim | config]).
    if (T_FUN_SYS == func_node->type_) {
      // children_: [function name, arg0, arg1, ...] with arg0 an expression list
      args_node = func_node->num_child_ > 1 ? func_node->children_[1] : NULL;
      arg_count = func_node->num_child_ - 1;
    } else {
      args_node = func_node->children_[0];
      arg_count = func_node->num_child_;
    }
    if (OB_ISNULL(args_node)) {
      ret = OB_ERR_PARAM_SIZE;
      LOG_WARN("ai column function without arguments", K(ret));
    } else if (T_EXPR_LIST == args_node->type_) {
      arg_count = args_node->num_child_;
    }
    if (OB_FAIL(ret)) {
    } else {
      def.call_type_ = node_is_ai_embed(func_node) ? AI_COLUMN_CALL_EMBED : AI_COLUMN_CALL_COMPLETE;
      if (OB_FAIL(copy_literal(alloc, AI_COLUMN_CALL_EMBED == def.call_type_ ? "AI_EMBED" : "AI_COMPLETE",
                               def.ai_function_name_))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (arg_count < 2 || arg_count > 3) {
    ret = OB_ERR_PARAM_SIZE;
    LOG_WARN("ai column function expects 2 or 3 arguments", K(ret), K(arg_count));
  } else if (AI_COLUMN_CALL_EMBED == def.call_type_ && arg_count != 2 && arg_count != 3) {
    ret = OB_ERR_PARAM_SIZE;
  }
  // model name: must be a string literal, it is part of the persisted schema
  if (OB_FAIL(ret)) {
  } else {
    const ParseNode *model_node = T_EXPR_LIST == args_node->type_ ? args_node->children_[0]
                                                                 : args_node;
    const ParseNode *prompt_node = T_EXPR_LIST == args_node->type_ ? args_node->children_[1]
                                                                   : NULL;
    const ParseNode *third_node = (T_EXPR_LIST == args_node->type_ && arg_count > 2)
                                      ? args_node->children_[2] : NULL;
    if (OB_ISNULL(model_node) || T_VARCHAR != model_node->type_ || NULL == model_node->str_value_) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI column model name (a string literal)");
      LOG_WARN("ai column model name must be a string literal", K(ret));
    } else {
      ObString model;
      model.assign_ptr(model_node->str_value_, model_node->str_len_);
      if (OB_FAIL(copy_str(alloc, model, def.model_name_))) {
      }
    }
    if (OB_SUCC(ret)) {
      ObString prompt_text;
      node_text(prompt_node, prompt_text);
      if (prompt_text.empty()) {
        // Never evaluate the enclosing AI call as a prompt inside a business transaction.
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI column prompt expression");
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(copy_str(alloc, prompt_text, def.prompt_expr_))) {
      } else if (OB_FAIL(collect_column_refs(prompt_node, alloc, def.dep_column_names_))) {
      }
    }
    // AI_EMBED third argument: the declared dimension of the vector result
    if (OB_SUCC(ret) && NULL != third_node && AI_COLUMN_CALL_EMBED == def.call_type_) {
      if (T_INT == third_node->type_ && third_node->value_ > 0) {
        def.dimension_ = third_node->value_;
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("ai_embed dimension must be an integer literal", K(ret), K(third_node->type_));
      }
    }
    // AI_COMPLETE third argument: the per-call model config, kept as evaluable
    // SQL text over the user table. It is snapshotted by the enqueue statement
    // inside the user transaction (never read back by the background worker), a
    // SQL NULL config behaves like an omitted one, and a config column is
    // collected as a dependency so updating it re-enqueues the AI column. The
    // text is deliberately not interpreted here: a legal shape such as
    // json_object('temperature', 0.2) or a json column reference must never be
    // rejected up front, and a malformed one is reported by the worker as a
    // terminal failure instead of silently changing the request.
    if (OB_SUCC(ret) && NULL != third_node && AI_COLUMN_CALL_COMPLETE == def.call_type_) {
      ObString config_text;
      node_text(third_node, config_text);
      if (config_text.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI column config expression");
        LOG_WARN("ai column config expression is empty", K(ret));
      } else if (OB_FAIL(copy_str(alloc, config_text, def.config_expr_))) {
      } else if (OB_FAIL(collect_column_refs(third_node, alloc, def.dep_column_names_))) {
      }
    }
  }
  // result type / declared dimension of the result column
  if (OB_FAIL(ret)) {
  } else {
    int64_t declared_dim = -1;
    ObString result_spec;
    if (OB_FAIL(build_result_spec(alloc, column, declared_dim, result_spec))) {
    } else if (OB_FAIL(copy_str(alloc, result_spec, def.result_spec_))) {
    } else if (AI_COLUMN_CALL_EMBED == def.call_type_) {
      if (-1 == declared_dim) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED on a non vector column is");
        LOG_WARN("ai_embed requires a vector result column", K(ret), K(column));
      } else if (def.dimension_ > 0 && def.dimension_ != declared_dim) {
        // e.g. vector(4) AI COLUMN (AI_EMBED('ep', content, 3))
        ret = OB_ERR_INVALID_VECTOR_DIM;
        LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM,
                       static_cast<uint32_t>(declared_dim),
                       static_cast<uint32_t>(def.dimension_));
        LOG_WARN("ai_embed dimension conflicts with the column dimension",
                 K(ret), K(declared_dim), K(def.dimension_));
      } else {
        def.dimension_ = declared_dim;
      }
    }
  }
  // WITH (sync_mode = 'async', max_retries = N)
  if (OB_FAIL(ret)) {
  } else {
    const ParseNode *options = ai_column_node->children_[1];
    if (NULL == options) {
      if (OB_FAIL(copy_literal(alloc, "async", def.sync_mode_))) {
      }
    } else if (T_AI_COLUMN_OPTION_LIST != options->type_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid ai column option list", K(ret), K(static_cast<int>(options->type_)));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < options->num_child_; ++i) {
        const ParseNode *option = options->children_[i];
        if (OB_ISNULL(option) || 2 != option->num_child_ ||
            OB_ISNULL(option->children_[0]) || OB_ISNULL(option->children_[1])) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("invalid ai column option", K(ret));
        } else {
          const ParseNode *name_node = option->children_[0];
          const ParseNode *value_node = option->children_[1];
          ObString name;
          ObString str_value;
          int64_t int_value = 0;
          name.assign_ptr(name_node->str_value_, name_node->str_len_);
          if (0 == name.case_compare("max_retries") && T_INT != value_node->type_) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI column max_retries (a nonnegative integer)");
          } else if (T_VARCHAR == value_node->type_ && NULL != value_node->str_value_) {
            str_value.assign_ptr(value_node->str_value_, value_node->str_len_);
          } else if (T_INT == value_node->type_) {
            int_value = value_node->value_;
          } else {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("unsupported ai column option value", K(ret), K(name), K(value_node->type_));
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(parse_option_value(name, int_value, str_value, def))) {
          }
        }
      }
      if (OB_SUCC(ret) && def.sync_mode_.empty()) {
        if (OB_FAIL(copy_literal(alloc, "async", def.sync_mode_))) {
        }
      }
    }
  }
  // normalized expression: prompt + the instruction the model needs for the
  // declared result type (the json one is asserted by the DDL regression test)
  if (OB_FAIL(ret)) {
  } else {
    ObSqlString buf;
    if (OB_FAIL(buf.append(def.prompt_expr_))) {
    } else if (ob_is_json_tc(column.get_data_type())) {
      if (OB_FAIL(buf.append(AI_JSON_RESULT_INSTRUCTION))) {
      }
    } else if (ob_is_collection_sql_type(column.get_data_type()) && def.dimension_ > 0) {
      if (OB_FAIL(buf.append(AI_VECTOR_RESULT_INSTRUCTION)) ||
          OB_FAIL(buf.append_fmt("%ld", def.dimension_))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(copy_str(alloc, buf.string(), def.normalized_expr_))) {
    }
  }
  // hidden column names, derived from the result column name so that they can be
  // created in the same pass as the AI column itself
  if (OB_FAIL(ret)) {
  } else {
    const ObString &result_column_name = column.get_column_name_str();
    if (OB_FAIL(build_hidden_column_name(alloc, AI_STATE_COLUMN_PREFIX, result_column_name,
                                         def.state_column_name_)) ||
        OB_FAIL(build_hidden_column_name(alloc, AI_REQUEST_COLUMN_PREFIX, result_column_name,
                                         def.request_column_name_)) ||
        OB_FAIL(build_hidden_column_name(alloc, AI_RESULT_SPEC_COLUMN_PREFIX, result_column_name,
                                         def.result_spec_column_name_))) {
      LOG_WARN("fail to build ai column hidden column names", K(ret), K(result_column_name));
    }
  }
  // Persist the definition in the comment of the AI result column and mark the
  // column as an AI column. The user comment is stored inside the payload, so a
  // COMMENT clause is preserved and can never be mistaken for the definition.
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(set_definition(alloc, def, user_comment, column))) {
    LOG_WARN("fail to set ai column definition", K(ret), K(column.get_column_name_str()));
  }
  return ret;
}

int ObAIColumnResolver::check_ai_column_dependencies(const ObTableSchema &table_schema,
                                                     const ObAIColumnDefinition &def)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < def.dep_column_names_.count(); ++i) {
    const ObString &name = def.dep_column_names_.at(i);
    const ObColumnSchemaV2 *dep = table_schema.get_column_schema(name);
    if (NULL != dep && is_ai_column(*dep)) {
      // AI columns never chain: the runtime would have to wait for the result of
      // another asynchronous column, which the task queue does not model.
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("ai column depends on ai column", K(ret), K(name));
    }
  }
  return ret;
}

int ObAIColumnResolver::append_hidden_columns(ObTableSchema &table_schema,
                                              ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  const int64_t column_count = table_schema.get_column_count();
  ObSEArray<uint64_t, 8> ai_column_ids;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (NULL != column && !column->is_hidden() && is_ai_column(*column) &&
        OB_FAIL(ai_column_ids.push_back(column->get_column_id()))) {
      LOG_WARN("fail to collect ai column id", K(ret), KPC(column));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_column_ids.count(); ++i) {
    const ObColumnSchemaV2 *ai_column = table_schema.get_column_schema(ai_column_ids.at(i));
    ObAIColumnDefinition def;
    if (OB_ISNULL(ai_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column vanished", K(ret), K(ai_column_ids.at(i)));
    } else if (OB_FAIL(decode_definition(alloc, ai_column->get_comment_str(), def))) {
      LOG_WARN("fail to decode ai column definition", K(ret), KPC(ai_column));
    } else if (def.state_column_name_.empty() || def.request_column_name_.empty() ||
               def.result_spec_column_name_.empty()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column definition without hidden column names", K(ret), K(def));
    }
    // Reuse only genuine hidden columns of the expected shape. Otherwise a
    // business column could become the worker's state/request storage.
    const ObString hidden_names[] = {def.state_column_name_, def.request_column_name_,
                                     def.result_spec_column_name_};
    const ObObjType hidden_types[] = {ObIntType, ObVarcharType, ObLongTextType};
    for (int64_t j = 0; OB_SUCC(ret) && j < 3; ++j) {
      const ObColumnSchemaV2 *existing = table_schema.get_column_schema(hidden_names[j]);
      if (NULL != existing && (!existing->is_hidden() || !existing->is_nullable() ||
          existing->get_data_type() != hidden_types[j] ||
          (1 == j && existing->get_data_length() != 128))) {
        ret = OB_ERR_COLUMN_DUPLICATE;
        LOG_USER_ERROR(OB_ERR_COLUMN_DUPLICATE, hidden_names[j].length(), hidden_names[j].ptr());
      }
    }
    // state: 1 PENDING, 2 RUNNING, 3 RETRY_WAIT, 4 READY, 5 FAILED
    if (OB_FAIL(ret)) {
    } else if (NULL != table_schema.get_column_schema(def.state_column_name_)) {
      // idempotent: the hidden columns were already added (ALTER ... MODIFY path)
    } else {
      ObColumnSchemaV2 hidden;
      hidden.set_is_hidden(true);
      hidden.set_nullable(true);
      hidden.set_data_type(ObIntType);
      hidden.set_charset_type(CHARSET_BINARY);
      hidden.set_collation_type(CS_TYPE_BINARY);
      if (OB_FAIL(hidden.set_column_name(def.state_column_name_))) {
      } else if (OB_FAIL(hidden.set_comment("AI column state: 1 pending, 2 running, "
                                            "3 retry_wait, 4 ready, 5 failed"))) {
      } else if (OB_FAIL(add_hidden_column(table_schema, hidden))) {
        LOG_WARN("fail to add ai column state column", K(ret), K(def.state_column_name_));
      }
    }
    // request id watermark of the row, used as the anti-late-overwrite CAS key
    if (OB_FAIL(ret)) {
    } else if (NULL != table_schema.get_column_schema(def.request_column_name_)) {
    } else {
      ObColumnSchemaV2 hidden;
      hidden.set_is_hidden(true);
      hidden.set_nullable(true);
      hidden.set_data_type(ObVarcharType);
      hidden.set_charset_type(CHARSET_UTF8MB4);
      hidden.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      ObAccuracy accuracy;
      accuracy.set_length(128);
      hidden.set_accuracy(accuracy);
      if (OB_FAIL(hidden.set_column_name(def.request_column_name_))) {
      } else if (OB_FAIL(hidden.set_comment("AI column request id watermark"))) {
      } else if (OB_FAIL(add_hidden_column(table_schema, hidden))) {
        LOG_WARN("fail to add ai column request column", K(ret), K(def.request_column_name_));
      }
    }
    // result spec: how the model output of this column is converted
    if (OB_FAIL(ret)) {
    } else if (NULL != table_schema.get_column_schema(def.result_spec_column_name_)) {
    } else {
      ObColumnSchemaV2 hidden;
      hidden.set_is_hidden(true);
      hidden.set_nullable(true);
      hidden.set_data_type(ObLongTextType);
      hidden.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
      hidden.set_charset_type(CHARSET_UTF8MB4);
      hidden.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      if (OB_FAIL(hidden.set_column_name(def.result_spec_column_name_))) {
      } else if (OB_FAIL(hidden.set_comment("AI column result type spec"))) {
      } else if (OB_FAIL(add_hidden_column(table_schema, hidden))) {
        LOG_WARN("fail to add ai column result spec column", K(ret), K(def.result_spec_column_name_));
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
