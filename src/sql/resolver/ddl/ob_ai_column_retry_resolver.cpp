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

#include "sql/resolver/ddl/ob_ai_column_retry_resolver.h"
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_table_schema.h"
#include "sql/session/ob_sql_session_info.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{

ObAIColumnRetryResolver::ObAIColumnRetryResolver(ObResolverParams &params)
  : ObDDLResolver(params),
    table_schema_(NULL)
{
}

ObAIColumnRetryResolver::~ObAIColumnRetryResolver()
{
}

int ObAIColumnRetryResolver::find_retry_action(const ParseNode *action_list,
                                               int64_t &retry_action_index,
                                               int64_t &action_count)
{
  int ret = OB_SUCCESS;
  retry_action_index = -1;
  action_count = 0;
  if (NULL != action_list) {
    if (OB_UNLIKELY(T_ALTER_TABLE_ACTION_LIST != action_list->type_)
        || OB_UNLIKELY(action_list->num_child_ < 0)
        || OB_UNLIKELY(action_list->num_child_ > 0 && OB_ISNULL(action_list->children_))) {
      ret = OB_ERR_UNEXPECTED;
      SQL_RESV_LOG(WARN, "invalid alter table action list", K(ret),
                   K(action_list->type_), K(action_list->num_child_));
    } else {
      action_count = action_list->num_child_;
      for (int64_t i = 0; OB_SUCC(ret) && i < action_count; ++i) {
        const ParseNode *action = action_list->children_[i];
        if (OB_ISNULL(action)) {
          ret = OB_ERR_UNEXPECTED;
          SQL_RESV_LOG(WARN, "null alter table action", K(ret), K(i));
        } else if (T_AI_COLUMN_RETRY == action->type_) {
          retry_action_index = i;
        } else {
          // not a retry action, nothing to do here
        }
      }
    }
  }
  return ret;
}

int ObAIColumnRetryResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObAIColumnRetryStmt *stmt = NULL;
  int64_t retry_action_index = -1;
  int64_t action_count = 0;
  if (OB_ISNULL(session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "session info is null", K(ret));
  } else if (OB_UNLIKELY(T_ALTER_TABLE != parse_tree.type_)
             || OB_UNLIKELY(ALTER_TABLE_NODE_COUNT != parse_tree.num_child_)
             || OB_ISNULL(parse_tree.children_)) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "invalid parse tree", K(ret),
                 K(parse_tree.type_), K(parse_tree.num_child_));
  } else if (OB_ISNULL(parse_tree.children_[TABLE_NODE])
             || OB_ISNULL(parse_tree.children_[ACTION_LIST_NODE])) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "invalid retry ai column parse tree", K(ret));
  } else if (OB_FAIL(find_retry_action(parse_tree.children_[ACTION_LIST_NODE],
                                      retry_action_index, action_count))) {
    SQL_RESV_LOG(WARN, "invalid retry action list", K(ret));
  } else if (action_count != 1 || retry_action_index != 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
  } else if (OB_ISNULL(stmt = create_stmt<ObAIColumnRetryStmt>())) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    SQL_RESV_LOG(WARN, "failed to create ai column retry stmt", K(ret));
  } else if (OB_FAIL(stmt->set_tz_info_wrap(session_info_->get_tz_info_wrap()))) {
    SQL_RESV_LOG(WARN, "failed to set tz info wrap", K(ret));
  } else if (OB_FAIL(stmt->fill_session_vars(*session_info_))) {
    SQL_RESV_LOG(WARN, "failed to fill session vars", K(ret));
  } else if (OB_FAIL(resolve_table(*parse_tree.children_[TABLE_NODE], *stmt))) {
    SQL_RESV_LOG(WARN, "failed to resolve the table of the retry statement", K(ret));
  } else if (OB_FAIL(resolve_retry_action(
                 *parse_tree.children_[ACTION_LIST_NODE]->children_[retry_action_index], *stmt))) {
    SQL_RESV_LOG(WARN, "failed to resolve the retried ai column", K(ret));
  }
  return ret;
}

int ObAIColumnRetryResolver::resolve_table(const ParseNode &table_node,
                                           ObAIColumnRetryStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObString table_name;
  ObString database_name;
  if (OB_FAIL(resolve_table_relation_node(&table_node, table_name, database_name, false))) {
    SQL_RESV_LOG(WARN, "failed to resolve table relation node", K(ret));
  } else if (OB_FAIL(set_database_name(database_name))) {
    SQL_RESV_LOG(WARN, "failed to set database name", K(ret));
  } else if (OB_FAIL(set_table_name(table_name))) {
    SQL_RESV_LOG(WARN, "failed to set table name", K(ret));
  } else if (OB_FAIL(stmt.set_origin_database_name(database_name_))) {
    SQL_RESV_LOG(WARN, "failed to set origin database name", K(ret));
  } else if (OB_FAIL(stmt.set_origin_table_name(table_name_))) {
    SQL_RESV_LOG(WARN, "failed to set origin table name", K(ret));
  } else if (OB_FAIL(schema_checker_->get_table_schema(database_name_,
                                                      table_name_,
                                                      false/*not index table*/,
                                                      table_schema_))) {
    if (OB_TABLE_NOT_EXIST == ret) {
      ObCStringHelper helper;
      LOG_USER_ERROR(OB_TABLE_NOT_EXIST,
                     helper.convert(database_name_),
                     helper.convert(table_name_));
    }
    SQL_RESV_LOG(WARN, "failed to get table schema", K(ret), K_(table_name), K_(database_name));
  } else if (OB_ISNULL(table_schema_)) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "table schema is null", K(ret), K_(table_name));
  } else {
    ObAIColumnRetryArg &arg = stmt.get_ai_column_retry_arg();
    ObBasedSchemaObjectInfo based_schema_object_info(table_schema_->get_table_id(),
                                                     TABLE_SCHEMA,
                                                     table_schema_->get_schema_version());
    stmt.set_table_id(table_schema_->get_table_id());
    arg.table_id_ = table_schema_->get_table_id();
    arg.database_name_ = database_name_;
    arg.table_name_ = table_name_;
    if (OB_FAIL(stmt.get_alter_table_arg().based_schema_object_infos_.push_back(
                  based_schema_object_info))) {
      SQL_RESV_LOG(WARN, "failed to push based schema object info", K(ret));
    }
  }
  return ret;
}

int ObAIColumnRetryResolver::resolve_retry_action(const ParseNode &action_node,
                                                  ObAIColumnRetryStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObAIColumnRetryArg &arg = stmt.get_ai_column_retry_arg();
  const ParseNode *column_node = NULL;
  const ParseNode *where_node = NULL;
  const ObColumnSchemaV2 *column = NULL;
  ObString column_name;
  if (OB_UNLIKELY(T_AI_COLUMN_RETRY != action_node.type_)
      || OB_UNLIKELY(ACTION_CHILD_COUNT != action_node.num_child_)
      || OB_ISNULL(action_node.children_)) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "invalid retry ai column action", K(ret),
                 K(action_node.type_), K(action_node.num_child_));
  } else if (OB_ISNULL(column_node = action_node.children_[ACTION_COLUMN_NODE])
             || OB_ISNULL(column_node->str_value_)
             || OB_UNLIKELY(column_node->str_len_ <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "invalid retried column name", K(ret));
  } else if (OB_ISNULL(table_schema_)) {
    ret = OB_ERR_UNEXPECTED;
    SQL_RESV_LOG(WARN, "table schema is null", K(ret));
  } else {
    column_name.assign_ptr(column_node->str_value_,
                           static_cast<int32_t>(column_node->str_len_));
    where_node = action_node.children_[ACTION_WHERE_NODE];
    if (OB_ISNULL(column = table_schema_->get_column_schema(column_name))) {
      const ObString &table_name = table_schema_->get_table_name_str();
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR,
                     column_name.length(), column_name.ptr(),
                     table_name.length(), table_name.ptr());
      SQL_RESV_LOG(WARN, "no such column to retry", K(ret), K(column_name), K(table_name));
    } else if (!ObAIColumnResolver::is_ai_column(*column)) {
      // An ordinary column has no tasks to re-queue. Not a syntax error: the name is
      // valid, only the action does not apply to it.
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
      SQL_RESV_LOG(WARN, "retried column is not an ai column", K(ret), K(column_name));
    } else if (OB_UNLIKELY(column->is_hidden())) {
      // The state / request / result spec columns belong to the runtime; they are not
      // AI columns themselves, so this is only a safety net.
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a hidden AI column internal column");
      SQL_RESV_LOG(WARN, "retried column is a hidden column", K(ret), K(column_name));
    } else {
      arg.column_id_ = column->get_column_id();
      arg.column_name_ = column->get_column_name_str();
      if (NULL != where_node) {
        if (OB_ISNULL(where_node->str_value_)) {
          ret = OB_ERR_UNEXPECTED;
          SQL_RESV_LOG(WARN, "the predicate text of the retry statement is lost", K(ret));
        } else {
          arg.where_clause_.assign_ptr(where_node->str_value_,
                                       static_cast<int32_t>(where_node->str_len_));
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
