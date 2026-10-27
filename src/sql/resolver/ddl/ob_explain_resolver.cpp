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
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/engine/basic/file_sql_scan.h"
#include "sql/optimizer/ob_optimizer_util.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
namespace {
int contains_file_table(const ObDMLStmt &stmt, bool &found, int64_t depth = 0)
{
  int ret = OB_SUCCESS;
  if (depth > 64) {
    ret = OB_SIZE_OVERFLOW;
  }
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < stmt.get_table_size(); ++i) {
    const TableItem *table = stmt.get_table_item(i);
    if (table != nullptr && table->is_file_table_) found = true;
  }
  ObSEArray<ObSelectStmt *, 8> children;
  if (OB_SUCC(ret) && !found) {
    ret = stmt.get_child_stmts(children);
    for (int64_t i = 0; OB_SUCC(ret) && !found && i < children.count(); ++i) {
      if (children.at(i) == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ret = contains_file_table(*children.at(i), found, depth + 1);
      }
    }
  }
  return ret;
}

const char *describe_file_type(file_sql::Type type)
{
  switch (type) {
    case file_sql::Type::Null: return "NULL";
    case file_sql::Type::BigInt: return "BIGINT";
    case file_sql::Type::Double: return "DOUBLE";
    case file_sql::Type::Boolean: return "BOOLEAN";
    case file_sql::Type::Varchar: return "VARCHAR";
    case file_sql::Type::Date: return "DATE";
    case file_sql::Type::DateTime: return "DATETIME";
  }
  return "VARCHAR";
}

int describe_file_projection(const ObSelectStmt &select, ObExplainStmt &explain)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < select.get_select_item_size(); ++i) {
    const SelectItem &item = select.get_select_item(i);
    FileDescribeColumn column;
    if (item.expr_ == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      column.name_ = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      const ObObjType type = item.expr_->get_result_type().get_type();
      const char *type_name = ob_obj_type_str(type);
      if (ob_is_integer_type(type)) type_name = "BIGINT";
      else if (ob_is_double_type(type) || ob_is_float_type(type)) type_name = "DOUBLE";
      else if (ob_is_string_type(type)) type_name = "VARCHAR";
      column.type_ = ObString::make_string(type_name);
      column.nullable_ = !item.expr_->get_result_type().has_result_flag(NOT_NULL_FLAG);
      if (item.expr_->is_column_ref_expr()) {
        const auto &ref = *static_cast<const ObColumnRefRawExpr *>(item.expr_);
        const TableItem *table = select.get_table_item_by_id(ref.get_table_id());
        if (table != nullptr && table->is_file_table_) {
          file_sql::FileScanDefinition definition;
          if (table->function_table_expr_ == nullptr || !table->function_table_expr_->is_const_expr()) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(definition.decode(
              static_cast<const ObConstRawExpr *>(table->function_table_expr_)->get_value().get_string()))) {
          } else if (ref.get_column_id() < OB_APP_MIN_COLUMN_ID
                     || ref.get_column_id() - OB_APP_MIN_COLUMN_ID >= definition.columns_.count()) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const auto &source = definition.columns_.at(ref.get_column_id() - OB_APP_MIN_COLUMN_ID);
            bool null_side = false;
            if (OB_FAIL(ObOptimizerUtil::is_table_on_null_side(&select, table->table_id_, null_side))) {
            } else {
              column.type_ = ObString::make_string(describe_file_type(static_cast<file_sql::Type>(source.type_)));
              column.nullable_ = source.nullable_ || null_side;
            }
          }
        }
      }
      if (OB_SUCC(ret)) ret = explain.get_describe_columns().push_back(column);
    }
  }
  if (OB_SUCC(ret)) explain.set_file_describe();
  return ret;
}
} // namespace

const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        if (parse_tree.is_file_describe_ && child_stmt->is_select_stmt()) {
          bool has_file = false;
          if (OB_FAIL(contains_file_table(*static_cast<ObDMLStmt *>(child_stmt), has_file))) {
          } else if (has_file) {
            ret = describe_file_projection(*static_cast<ObSelectStmt *>(child_stmt), *explain_stmt);
          }
        }
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (OB_SUCC(ret) && NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
