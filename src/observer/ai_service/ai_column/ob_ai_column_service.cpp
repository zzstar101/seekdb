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

#define USING_LOG_PREFIX SERVER

#include "config_bridge.h"
#include "observer/ai_service/ai_column/ob_ai_column_service.h"

#include "lib/atomic/ob_atomic.h"
#include "lib/string/ob_string.h"
#include "lib/time/ob_time_utility.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
// is_ai_column_column(): the persisted AI_COLUMN_FLAG of a column, which is the
// trusted answer to "is this table an AI column table".
#include "share/schema/ob_ai_column_definition.h"

namespace oceanbase
{
using namespace common;
namespace observer
{
namespace ai_column
{

namespace
{

int copy_varchar(ObIAllocator &allocator,
                 sqlclient::ObMySQLResult &result,
                 const char *column,
                 ObString &value)
{
  int ret = OB_SUCCESS;
  ObString tmp;
  value.reset();
  if (OB_FAIL(result.get_varchar(column, tmp))) {
    if (OB_ERR_NULL_VALUE == ret || OB_ERR_COLUMN_NOT_FOUND == ret) {
      // a nullable catalog column, or one the catalog of this version does not
      // provide; both are not fatal here
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("get catalog varchar failed", K(ret), K(column));
    }
  } else if (tmp.empty()) {
  } else if (OB_FAIL(ob_write_string(allocator, tmp, value))) {
    LOG_WARN("copy catalog varchar failed", K(ret), K(column));
  }
  return ret;
}

int get_int_or_default(sqlclient::ObMySQLResult &result,
                       const char *column,
                       const int64_t default_value,
                       int64_t &value)
{
  int ret = OB_SUCCESS;
  value = default_value;
  if (OB_FAIL(result.get_int(column, value))) {
    if (OB_ERR_NULL_VALUE == ret || OB_ERR_COLUMN_NOT_FOUND == ret) {
      ret = OB_SUCCESS;
      value = default_value;
    } else {
      LOG_WARN("get catalog int failed", K(ret), K(column));
    }
  }
  return ret;
}

// The prompt of an AI column is the *argument* of AI_COMPLETE / AI_EMBED, e.g.
// "concat('Classify: ', content)" for
// "AI_COMPLETE('model', concat('Classify: ', content))".
//
// Only the prompt may be evaluated inside the enqueuing transaction: evaluating
// the whole AI_COMPLETE expression there would call the model from a transaction
// that can still roll back, which the feature must never do.
int extract_prompt_expr(ObIAllocator &allocator, const ObString &normalized_expr,
                        ObString &prompt_expr)
{
  int ret = OB_SUCCESS;
  prompt_expr.reset();
  int64_t start = -1;
  int64_t end = -1;
  if (normalized_expr.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty normalized expression of an ai column", K(ret));
  } else {
    const char *ptr = normalized_expr.ptr();
    const int64_t len = normalized_expr.length();
    int64_t pos = 0;
    while (pos < len && '(' != ptr[pos]) {
      ++pos;
    }
    if (pos < len) {
      int64_t depth = 1;
      bool in_quote = false;
      int64_t arg_index = 0;
      int64_t arg_begin = pos + 1;
      for (int64_t i = pos + 1; i < len && start < 0; ++i) {
        const char c = ptr[i];
        if ('\'' == c) {
          in_quote = !in_quote;
        } else if (in_quote) {
        } else if ('(' == c || '[' == c) {
          ++depth;
        } else if (')' == c || ']' == c) {
          --depth;
          if (0 == depth) {
            if (1 == arg_index) {
              start = arg_begin;
              end = i;
            }
          }
        } else if (',' == c && 1 == depth) {
          if (0 == arg_index) {
            ++arg_index;
            arg_begin = i + 1;
          } else if (1 == arg_index) {
            start = arg_begin;
            end = i;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && (start < 0 || end <= start)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("the normalized expression of the ai column has no prompt argument",
             K(ret), K(normalized_expr));
  }
  if (OB_SUCC(ret)) {
    int64_t begin = start;
    int64_t finish = end;
    while (begin < finish && isspace(static_cast<unsigned char>(normalized_expr.ptr()[begin]))) {
      ++begin;
    }
    while (finish > begin && isspace(static_cast<unsigned char>(normalized_expr.ptr()[finish - 1]))) {
      --finish;
    }
    if (finish <= begin) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("empty prompt argument of an ai column", K(ret), K(normalized_expr));
    } else {
      const ObString trimmed(static_cast<int32_t>(finish - begin),
                             normalized_expr.ptr() + begin);
      if (OB_FAIL(ob_write_string(allocator, trimmed, prompt_expr))) {
        LOG_WARN("copy prompt expression failed", K(ret));
      }
    }
  }
  return ret;
}

int build_quoted_table_name(ObIAllocator &allocator,
                            const ObString &db_name,
                            const ObString &table_name,
                            ObString &quoted_name)
{
  int ret = OB_SUCCESS;
  ObSqlString name;
  quoted_name.reset();
  if (db_name.empty() || table_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty database or table name of an ai column", K(ret), K(db_name), K(table_name));
  } else if (OB_FAIL(name.append_fmt("`%.*s`.`%.*s`",
                                     db_name.length(), db_name.ptr(),
                                     table_name.length(), table_name.ptr()))) {
    LOG_WARN("build quoted table name failed", K(ret), K(db_name), K(table_name));
  } else if (OB_FAIL(ob_write_string(allocator, ObString(name.length(), name.ptr()), quoted_name))) {
    LOG_WARN("copy quoted table name failed", K(ret));
  }
  return ret;
}

// Plain text search used when an expression was printed without quoted
// identifiers.  A hit means "this ai column may depend on the column", i.e. it is
// the conservative direction: recompute rather than keep a stale result.
bool contains_identifier(const ObString &text, const ObString &identifier)
{
  bool found = false;
  if (!identifier.empty() && identifier.length() <= text.length()) {
    const int64_t limit = text.length() - identifier.length();
    for (int64_t i = 0; !found && i <= limit; ++i) {
      found = (0 == strncasecmp(text.ptr() + i, identifier.ptr(), identifier.length()));
    }
  }
  return found;
}

int copy_sql(ObIAllocator &allocator, const ObSqlString &sql, ObIArray<ObString> &stmts)
{
  int ret = OB_SUCCESS;
  ObString stmt;
  if (sql.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty ai column statement", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, ObString(sql.length(), sql.ptr()), stmt))) {
    LOG_WARN("copy ai column statement failed", K(ret));
  } else if (OB_FAIL(stmts.push_back(stmt))) {
    LOG_WARN("store ai column statement failed", K(ret));
  }
  return ret;
}

// The catalog stores the hidden columns of an ai column by column id only, so the
// names have to come from the table schema.  A missing hidden column means the DDL
// of that ai column is not complete: the runtime then refuses to maintain it.
int resolve_hidden_column_names(ObIAllocator &allocator,
                                const share::schema::ObTableSchema &table_schema,
                                AiColumnDef &def)
{
  int ret = OB_SUCCESS;
  const share::schema::ObColumnSchemaV2 *state_column = nullptr;
  const share::schema::ObColumnSchemaV2 *request_column = nullptr;
  if (nullptr == (state_column = table_schema.get_column_schema(
                    static_cast<uint64_t>(def.state_column_id_)))) {
    ret = OB_ERR_COLUMN_NOT_FOUND;
    LOG_WARN("the hidden state column of the ai column is missing", K(ret), K(def));
  } else if (nullptr == (request_column = table_schema.get_column_schema(
                           static_cast<uint64_t>(def.request_column_id_)))) {
    ret = OB_ERR_COLUMN_NOT_FOUND;
    LOG_WARN("the hidden request column of the ai column is missing", K(ret), K(def));
  } else if (OB_FAIL(ob_write_string(allocator, state_column->get_column_name_str(),
                                     def.state_column_name_))) {
    LOG_WARN("copy hidden state column name failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, request_column->get_column_name_str(),
                                     def.request_column_name_))) {
    LOG_WARN("copy hidden request column name failed", K(ret));
  }
  return ret;
}

int copy_def(ObIAllocator &allocator, const AiColumnDef &src, AiColumnDef &dst)
{
  int ret = OB_SUCCESS;
  dst.table_id_ = src.table_id_;
  dst.column_id_ = src.column_id_;
  dst.state_column_id_ = src.state_column_id_;
  dst.request_column_id_ = src.request_column_id_;
  dst.result_spec_column_id_ = src.result_spec_column_id_;
  dst.max_retries_ = src.max_retries_;
  dst.call_type_ = src.call_type_;
  dst.dimension_ = src.dimension_;
  dst.definition_version_ = src.definition_version_;
  if (OB_FAIL(ob_write_string(allocator, src.column_name_, dst.column_name_))) {
    LOG_WARN("copy ai column name failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.state_column_name_,
                                     dst.state_column_name_))) {
    LOG_WARN("copy ai state column name failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.request_column_name_,
                                     dst.request_column_name_))) {
    LOG_WARN("copy ai request column name failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.model_name_, dst.model_name_))) {
    LOG_WARN("copy ai model name failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.prompt_expr_, dst.prompt_expr_))) {
    LOG_WARN("copy ai prompt expression failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.config_expr_, dst.config_expr_))) {
    LOG_WARN("copy ai config expression failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.normalized_expr_, dst.normalized_expr_))) {
    LOG_WARN("copy ai normalized expression failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.deps_, dst.deps_))) {
    LOG_WARN("copy ai column dependencies failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.result_spec_, dst.result_spec_))) {
    LOG_WARN("copy ai result spec failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, src.table_name_, dst.table_name_))) {
    LOG_WARN("copy ai table name failed", K(ret));
  }
  return ret;
}

// The config expression is not a column of __all_ai_column: it is part of the AI
// column definition persisted in the result column comment, so read it from the
// very schema snapshot this task already uses.  A column without a config (or a
// payload written before the config field existed) yields an empty expression,
// which enqueues no config at all -- exactly like a config that evaluates to
// SQL NULL.
int read_config_expr(ObIAllocator &allocator,
                     const share::schema::ObTableSchema &table_schema,
                     const int64_t column_id,
                     ObString &config_expr)
{
  int ret = OB_SUCCESS;
  const share::schema::ObColumnSchemaV2 *column = nullptr;
  share::schema::AIColumnDefinition definition;
  config_expr.reset();
  if (OB_ISNULL(column = table_schema.get_column_schema(
                    static_cast<uint64_t>(column_id)))) {
    // the result column of this catalog row is gone; read_catalog_row() drops the
    // row later.  There is no definition to read a config from.
    LOG_DEBUG("ai column has no schema column, no config to read", K(column_id));
  } else if (!share::schema::has_ai_column_definition(*column)) {
    LOG_DEBUG("ai column has no persisted definition, no config to read", K(column_id));
  } else if (OB_FAIL(share::schema::decode_ai_column_definition(
                       allocator, column->get_comment_str(), definition))) {
    LOG_WARN("decode the ai column definition failed", K(ret), K(column_id));
  } else if (OB_FAIL(ob_write_string(allocator, definition.config_expr_, config_expr))) {
    LOG_WARN("copy the ai config expression failed", K(ret), K(column_id));
  }
  return ret;
}

}  // namespace

AiColumnService &AiColumnService::get_instance()
{
  static AiColumnService instance;
  return instance;
}

AiColumnService::AiColumnService()
  : is_inited_(false),
    is_started_(false),
    sql_proxy_(nullptr),
    worker_(),
    request_seq_(0)
{
}

AiColumnService::~AiColumnService()
{
  // Nothing is owned here any more: the definitions of a table live in the arena of
  // the frame that asked for them, so there is no cache to tear down.
}

int AiColumnService::init(ObMySQLProxy &sql_proxy)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ai column service init twice", K(ret));
  } else if (OB_FAIL(worker_.init(sql_proxy))) {
    LOG_WARN("init ai column worker failed", K(ret));
  } else {
    sql_proxy_ = &sql_proxy;
    // The engine reaches the runtime through the server runtime slot, see
    // sql::IAiColumnDmlHook.  Binding here (and unbinding in destroy()) keeps the
    // registration next to the object it exposes.
    share::bind_server_service<sql::IAiColumnDmlHook>(this);
    is_inited_ = true;
    LOG_INFO("ai column service inited");
  }
  return ret;
}

int AiColumnService::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column service not init", K(ret));
  } else if (is_started_) {
  } else if (OB_FAIL(worker_.start())) {
    LOG_WARN("start ai column worker failed", K(ret));
  } else {
    is_started_ = true;
    LOG_INFO("ai column service started");
  }
  return ret;
}

void AiColumnService::stop()
{
  if (is_started_) {
    worker_.stop();
    worker_.wait();
    is_started_ = false;
  }
}

void AiColumnService::destroy()
{
  stop();
  if (is_inited_) {
    worker_.destroy();
    share::unbind_server_service<sql::IAiColumnDmlHook>();
    is_inited_ = false;
  }
}

bool AiColumnService::table_has_ai_column(
    const share::schema::ObTableSchema &table_schema)
{
  // The persisted column flag of the schema snapshot is the trusted answer: it is
  // written by set_ai_column_definition() together with the catalog row, and it is
  // already in memory, so asking costs a scan of one column array and no database
  // access at all.  A table that carries no such column is not an AI column table.
  bool has_ai_column = false;
  for (int64_t i = 0; !has_ai_column && i < table_schema.get_column_count(); ++i) {
    const share::schema::ObColumnSchemaV2 *column =
        table_schema.get_column_schema_by_idx(i);
    has_ai_column = (nullptr != column) && share::schema::is_ai_column_column(*column);
  }
  return has_ai_column;
}

int AiColumnService::load_table_defs(common::ObIAllocator &allocator,
                                     share::schema::ObSchemaGetterGuard &schema_guard,
                                     const share::schema::ObTableSchema &table_schema,
                                     TableDefEntry &entry)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString db_name;
  const share::schema::ObDatabaseSchema *db_schema = nullptr;
  entry.table_id_ = table_schema.get_table_id();
  entry.has_ai_column_ = false;
  entry.defs_.reuse();
  if (!table_has_ai_column(table_schema)) {
    // The trusted schema already answered: the table has no AI column, so the
    // catalog is not read for it.  The strings of `entry` stay empty, which means no
    // allocation came from `allocator` either.
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema.get_database_id(), db_schema))) {
    LOG_WARN("get database schema of an ai column table failed", K(ret),
             K(table_schema.get_database_id()));
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null database schema of an ai column table", K(ret),
             K(table_schema.get_database_id()));
  } else if (FALSE_IT(db_name = db_schema->get_database_name_str())) {
  } else if (OB_FAIL(AiColumnTaskSql::select_catalog(entry.table_id_, sql))) {
    LOG_WARN("build ai column catalog sql failed", K(ret), K(entry.table_id_));
  } else if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column service has no sql proxy", K(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("read ai column catalog failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null ai column catalog result", K(ret));
      } else {
        while (OB_SUCC(ret)) {
          AiColumnDef def;
          def.table_id_ = entry.table_id_;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("iterate ai column catalog failed", K(ret));
            }
          } else if (OB_FAIL(read_catalog_row(allocator, table_schema, db_name,
                                              *result, def))) {
            LOG_WARN("read one ai column catalog row failed", K(ret), K(entry.table_id_));
          } else if (OB_FAIL(entry.defs_.push_back(def))) {
            LOG_WARN("store ai column definition failed", K(ret));
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    entry.has_ai_column_ = !entry.defs_.empty();
    LOG_TRACE("ai column definitions loaded", K(entry.table_id_), K(entry.has_ai_column_),
              "count", entry.defs_.count());
  }
  return ret;
}

int AiColumnService::read_catalog_row(common::ObIAllocator &allocator,
                                      const share::schema::ObTableSchema &table_schema,
                                      const ObString &db_name,
                                      sqlclient::ObMySQLResult &result,
                                      AiColumnDef &def)
{
  int ret = OB_SUCCESS;
  ObString normalized_expr;
  ObString prompt_expr;
  ObString config_expr;
  if (OB_FAIL(result.get_int("column_id", def.column_id_))) {
    LOG_WARN("get ai column id failed", K(ret));
  } else if (OB_FAIL(get_int_or_default(result, "state_column_id", OB_INVALID_ID,
                                        def.state_column_id_))) {
  } else if (OB_FAIL(get_int_or_default(result, "request_column_id", OB_INVALID_ID,
                                        def.request_column_id_))) {
  } else if (OB_FAIL(get_int_or_default(result, "result_spec_column_id", OB_INVALID_ID,
                                        def.result_spec_column_id_))) {
  } else if (OB_FAIL(get_int_or_default(result, "max_retries", 0, def.max_retries_))) {
  } else if (OB_FAIL(get_int_or_default(result, "definition_version", 1,
                                        def.definition_version_))) {
  } else if (OB_FAIL(get_int_or_default(result, "call_type", AI_COLUMN_CALL_COMPLETE,
                                        def.call_type_))) {
  } else if (OB_FAIL(get_int_or_default(result, "dimension", -1, def.dimension_))) {
  } else if (OB_FAIL(copy_varchar(allocator, result, "column_name", def.column_name_))) {
  } else if (OB_FAIL(copy_varchar(allocator, result, "model_name", def.model_name_))) {
  } else if (OB_FAIL(copy_varchar(allocator, result, "result_spec", def.result_spec_))) {
  } else if (OB_FAIL(copy_varchar(allocator, result, "deps", def.deps_))) {
    LOG_WARN("get the dependencies of the ai column failed", K(ret));
  } else if (OB_FAIL(copy_varchar(allocator, result, "normalized_expr", normalized_expr))) {
  } else if (OB_FAIL(resolve_hidden_column_names(allocator, table_schema, def))) {
    LOG_WARN("resolve the hidden columns of the ai column failed", K(ret), K(def));
  } else if (OB_FAIL(read_config_expr(allocator, table_schema, def.column_id_, config_expr))) {
    LOG_WARN("read the config expression of the ai column failed", K(ret), K(def.column_id_));
  } else if (OB_FAIL(ob_write_string(allocator, config_expr, def.config_expr_))) {
    LOG_WARN("copy config expression failed", K(ret));
  } else if (OB_FAIL(build_quoted_table_name(allocator, db_name,
                                             table_schema.get_table_name_str(),
                                             def.table_name_))) {
    LOG_WARN("build quoted table name failed", K(ret), K(db_name),
             K(table_schema.get_table_name_str()));
  } else if (OB_FAIL(ob_write_string(allocator, normalized_expr, def.normalized_expr_))) {
    LOG_WARN("copy normalized expression failed", K(ret));
  } else if (OB_FAIL(copy_varchar(allocator, result, "prompt_expr", prompt_expr))) {
    LOG_WARN("get prompt expression failed", K(ret));
  } else if (prompt_expr.empty() &&
             OB_FAIL(extract_prompt_expr(allocator, normalized_expr, prompt_expr))) {
    // Without the prompt expression the runtime can not queue this column: the
    // prompt has to be materialized inside the enqueuing transaction, and
    // evaluating the whole AI function there would call the model too early.
    LOG_WARN("the ai column has no usable prompt expression, it is not maintained",
             K(ret), K(normalized_expr), K(def.column_name_));
    ret = OB_SUCCESS;
    prompt_expr.reset();
  } else if (OB_FAIL(ob_write_string(allocator, prompt_expr, def.prompt_expr_))) {
    LOG_WARN("copy prompt expression failed", K(ret));
  }
  if (OB_SUCC(ret) && !def.is_valid()) {
    // a catalog row the runtime can not work with.  Skip it instead of failing
    // the whole table.
    LOG_WARN("skip an incomplete ai column catalog row", K(def));
    def.table_id_ = OB_INVALID_ID;
  }
  return ret;
}

int AiColumnService::get_table_ai_columns(ObIAllocator &allocator,
                                          const uint64_t table_id,
                                          bool &has_ai_column,
                                          ObIArray<sql::AiColumnKeyColumn> &pk_columns)
{
  int ret = OB_SUCCESS;
  has_ai_column = false;
  share::schema::ObSchemaGetterGuard guard;
  share::schema::ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  const share::schema::ObTableSchema *table_schema = nullptr;
  ObSEArray<uint64_t, 4> rowkey_column_ids;
  ObArenaAllocator defs_allocator;
  TableDefEntry entry;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column service not init", K(ret));
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
    LOG_WARN("get runtime schema guard failed", K(ret));
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("the table of the dml does not exist", K(ret), K(table_id));
  } else if (OB_FAIL(table_schema->get_rowkey_column_ids(rowkey_column_ids))) {
    LOG_WARN("get rowkey column ids failed", K(ret), K(table_id));
  } else if (OB_FAIL(load_table_defs(defs_allocator, guard, *table_schema, entry))) {
    LOG_WARN("load the ai columns of the table failed", K(ret), K(table_id));
  } else {
    has_ai_column = entry.has_ai_column_;
    for (int64_t i = 0; OB_SUCC(ret) && has_ai_column && i < rowkey_column_ids.count(); ++i) {
      const uint64_t column_id = rowkey_column_ids.at(i);
      const share::schema::ObColumnSchemaV2 *column_schema =
          table_schema->get_column_schema(column_id);
      sql::AiColumnKeyColumn key;
      key.column_id_ = column_id;
      if (OB_ISNULL(column_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the rowkey column is not found in the schema", K(ret), K(column_id));
      } else if (OB_FAIL(ob_write_string(allocator, column_schema->get_column_name_str(),
                                         key.column_name_))) {
        LOG_WARN("copy rowkey column name failed", K(ret), K(column_id));
      } else if (OB_FAIL(pk_columns.push_back(key))) {
        LOG_WARN("store rowkey column failed", K(ret), K(column_id));
      }
    }
  }
  return ret;
}

bool AiColumnService::depends_on_updated_columns(const AiColumnDef &def,
                                                 const ObIArray<ObString> &updated_names)
{
  bool depends = true;
  const ObString &expr = def.normalized_expr_;
  if (!def.deps_.empty() && !updated_names.empty()) {
    // the catalog knows the exact dependencies, use them instead of guessing from
    // the printed expression
    depends = false;
    int64_t start = 0;
    while (!depends && start <= def.deps_.length()) {
      int64_t end = start;
      while (end < def.deps_.length() && ',' != def.deps_.ptr()[end]) {
        ++end;
      }
      if (end > start) {
        const ObString name(static_cast<int32_t>(end - start), def.deps_.ptr() + start);
        for (int64_t i = 0; !depends && i < updated_names.count(); ++i) {
          depends = (0 == name.case_compare(updated_names.at(i)));
        }
      }
      start = end + 1;
    }
  } else if (expr.empty() || updated_names.empty()) {
    // unknown dependencies, or an update statement with no column list: recompute
  } else {
    depends = false;
    bool has_printed_identifier = false;
    int64_t pos = 0;
    while (!depends && pos < expr.length()) {
      if ('`' != expr.ptr()[pos]) {
        ++pos;
      } else {
        int64_t end = pos + 1;
        while (end < expr.length() && '`' != expr.ptr()[end]) {
          ++end;
        }
        if (end >= expr.length()) {
          pos = expr.length();
        } else {
          const ObString identifier(static_cast<int32_t>(end - pos - 1), expr.ptr() + pos + 1);
          has_printed_identifier = true;
          for (int64_t i = 0; !depends && i < updated_names.count(); ++i) {
            depends = (0 == identifier.case_compare(updated_names.at(i)));
          }
          pos = end + 1;
        }
      }
    }
    if (!has_printed_identifier) {
      // the expression was not printed with quoted identifiers, fall back to a
      // plain search so that a dependency is never missed silently
      for (int64_t i = 0; !depends && i < updated_names.count(); ++i) {
        depends = contains_identifier(expr, updated_names.at(i));
      }
    }
  }
  return depends;
}

int AiColumnService::new_request_id(ObIAllocator &allocator, ObString &request_id)
{
  int ret = OB_SUCCESS;
  char buf[AI_COLUMN_REQUEST_ID_LEN];
  int64_t pos = 0;
  request_id.reset();
  if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "req$%ld$%ld",
                              ObTimeUtility::current_time(),
                              ATOMIC_FAA(&request_seq_, 1)))) {
    LOG_WARN("format ai column request id failed", K(ret));
  } else if (pos <= 0 || pos >= static_cast<int64_t>(sizeof(buf))) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("build ai column request id failed", K(ret), K(pos));
  } else if (OB_FAIL(ob_write_string(allocator, ObString(static_cast<int32_t>(pos), buf),
                                     request_id))) {
    LOG_WARN("copy ai column request id failed", K(ret));
  }
  return ret;
}

int AiColumnService::build_change_sql(ObIAllocator &allocator,
                                      const TableDefEntry &entry,
                                      const sql::AiColumnRowChange &change,
                                      const ObIArray<ObString> &updated_names,
                                      ObIArray<ObString> &stmts)
{
  int ret = OB_SUCCESS;
  const bool is_delete = (sql::AI_COLUMN_ROW_DELETED == change.event_);
  const bool is_update = (sql::AI_COLUMN_ROW_UPDATED == change.event_);
  const bool pk_changed = is_update &&
                          !change.old_rowkey_.empty() &&
                          !change.new_rowkey_.empty() &&
                          0 != change.old_rowkey_.compare(change.new_rowkey_);
  ObSqlString sql;
  AiColumnDefArray selected_defs;
  if (is_delete) {
    if (OB_FAIL(AiColumnTaskSql::delete_tasks_of_row(entry.table_id_, change.old_rowkey_, sql))) {
      LOG_WARN("build delete task sql failed", K(ret), K(change.old_rowkey_));
    } else if (OB_FAIL(copy_sql(allocator, sql, stmts))) {
      LOG_WARN("copy delete task sql failed", K(ret));
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < entry.defs_.count(); ++i) {
      const AiColumnDef &def = entry.defs_.at(i);
      bool need = true;
      if (is_update && !pk_changed) {
        // an AI column that does not depend on any of the changed columns keeps
        // its result, so an update of an unrelated column neither nulls it nor
        // calls the model again
        need = depends_on_updated_columns(def, updated_names);
      }
      if (need && OB_FAIL(selected_defs.push_back(def))) {
        LOG_WARN("select the ai column to recompute failed", K(ret));
      }
    }
    if (OB_SUCC(ret) && selected_defs.empty()) {
      // nothing depends on the columns this statement changed
    } else if (OB_SUCC(ret)) {
      ObString request_id;
      if (OB_FAIL(new_request_id(allocator, request_id))) {
        LOG_WARN("build the ai column request id failed", K(ret));
      } else if (pk_changed &&
                 (OB_FAIL(AiColumnTaskSql::delete_tasks_of_row(entry.table_id_,
                                                               change.old_rowkey_, sql)) ||
                  OB_FAIL(copy_sql(allocator, sql, stmts)))) {
        // The row moved: the task of the old primary key must not survive.
        LOG_WARN("build delete task sql for the old rowkey failed", K(ret),
                 K(change.old_rowkey_));
      } else if (OB_FAIL(AiColumnTaskSql::invalidate_row(selected_defs, change.new_rowkey_,
                                                         request_id, sql))) {
        LOG_WARN("build ai column invalidate sql failed", K(ret), K(change.new_rowkey_));
      } else if (OB_FAIL(copy_sql(allocator, sql, stmts))) {
        LOG_WARN("copy ai column invalidate sql failed", K(ret));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < selected_defs.count(); ++i) {
          if (OB_FAIL(AiColumnTaskSql::enqueue_task(selected_defs.at(i), change.new_rowkey_,
                                                    request_id, sql))) {
            LOG_WARN("build ai column enqueue sql failed", K(ret),
                     K(selected_defs.at(i).column_name_));
          } else if (OB_FAIL(copy_sql(allocator, sql, stmts))) {
            LOG_WARN("copy ai column enqueue sql failed", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int AiColumnService::build_row_change_sql(ObIAllocator &allocator,
                                          const uint64_t table_id,
                                          const ObIArray<sql::AiColumnRowChange> &changes,
                                          const ObIArray<uint64_t> &updated_column_ids,
                                          ObIArray<ObString> &stmts)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  share::schema::ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  const share::schema::ObTableSchema *table_schema = nullptr;
  ObArenaAllocator defs_allocator;
  TableDefEntry entry;
  ObSEArray<ObString, 8> updated_names;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column service not init", K(ret));
  } else if (changes.empty()) {
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
    LOG_WARN("get runtime schema guard failed", K(ret));
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("the table of the dml does not exist", K(ret), K(table_id));
  } else if (OB_FAIL(load_table_defs(defs_allocator, guard, *table_schema, entry))) {
    LOG_WARN("load the ai columns of the table failed", K(ret), K(table_id));
  } else if (!entry.has_ai_column_) {
    // a table without ai columns never reaches this code path, the engine caches
    // the answer, but be defensive
  } else if (updated_column_ids.empty()) {
    // insert / delete: every ai column of the row is affected
    for (int64_t i = 0; OB_SUCC(ret) && i < changes.count(); ++i) {
      if (OB_FAIL(build_change_sql(allocator, entry, changes.at(i), updated_names, stmts))) {
        LOG_WARN("build the ai column statements of a row change failed", K(ret));
      }
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < updated_column_ids.count(); ++i) {
      const share::schema::ObColumnSchemaV2 *column_schema =
          table_schema->get_column_schema(updated_column_ids.at(i));
      ObString column_name;
      if (OB_ISNULL(column_schema)) {
        // hidden column, or a column of another index: not interesting
      } else if (OB_FAIL(ob_write_string(allocator, column_schema->get_column_name_str(),
                                         column_name))) {
        LOG_WARN("copy updated column name failed", K(ret), K(updated_column_ids.at(i)));
      } else if (OB_FAIL(updated_names.push_back(column_name))) {
        LOG_WARN("store updated column name failed", K(ret));
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < changes.count(); ++i) {
      if (OB_FAIL(build_change_sql(allocator, entry, changes.at(i), updated_names, stmts))) {
        LOG_WARN("build the ai column statements of a row change failed", K(ret));
      }
    }
  }
  return ret;
}

int AiColumnService::check_strict_read(const uint64_t table_id,
                                       const ObIArray<uint64_t> &column_ids)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  share::schema::ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  const share::schema::ObTableSchema *table_schema = nullptr;
  ObArenaAllocator defs_allocator;
  TableDefEntry entry;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column service not init", K(ret));
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
    LOG_WARN("get runtime schema guard failed", K(ret));
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("the table of the statement does not exist", K(ret), K(table_id));
  } else if (OB_FAIL(load_table_defs(defs_allocator, guard, *table_schema, entry))) {
    LOG_WARN("load the ai columns of the table failed", K(ret), K(table_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
      const AiColumnDef *def = nullptr;
      for (int64_t j = 0; nullptr == def && j < entry.defs_.count(); ++j) {
        if (entry.defs_.at(j).column_id_ == static_cast<int64_t>(column_ids.at(i))) {
          def = &entry.defs_.at(j);
        }
      }
      if (nullptr == def) {
        // not an AI column, nothing to check
      } else {
        ObSqlString sql;
        int64_t not_ready = 0;
        if (OB_FAIL(AiColumnTaskSql::count_not_ready_rows(*def, sql))) {
          LOG_WARN("build the strict read check sql failed", K(ret), K(def->column_name_));
        } else if (OB_FAIL(read_count(sql, not_ready))) {
          LOG_WARN("count the rows that are not ready failed", K(ret), K(def->column_name_));
        } else if (not_ready > 0) {
          ret = OB_AI_COLUMN_UNFINISHED_TASK;
          LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED_TASK, table_id, def->column_id_);
          LOG_WARN("strict read rejected a statement that reads an unfinished ai column",
                   K(ret), K(table_id), K(def->column_id_), K(def->column_name_), K(not_ready));
        }
      }
    }
  }
  return ret;
}

int AiColumnService::read_count(const ObSqlString &sql, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column service has no sql proxy", K(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("read ai column count failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null ai column count result", K(ret));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("iterate ai column count result failed", K(ret));
      } else if (OB_FAIL(result->get_int(static_cast<int64_t>(0), count))) {
        LOG_WARN("get ai column count failed", K(ret));
      }
    }
  }
  return ret;
}

int AiColumnService::get_ai_column_defs(ObIAllocator &allocator,
                                       const uint64_t table_id,
                                       AiColumnDefArray &defs)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  share::schema::ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  const share::schema::ObTableSchema *table_schema = nullptr;
  ObArenaAllocator defs_allocator;
  TableDefEntry entry;
  defs.reuse();
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai column service not init", K(ret));
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(guard))) {
    LOG_WARN("get runtime schema guard failed", K(ret), K(table_id));
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("the table of an ai column task does not exist", K(ret), K(table_id));
  } else if (OB_FAIL(load_table_defs(defs_allocator, guard, *table_schema, entry))) {
    LOG_WARN("load ai column definitions failed", K(ret), K(table_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < entry.defs_.count(); ++i) {
      AiColumnDef def;
      if (OB_FAIL(copy_def(allocator, entry.defs_.at(i), def))) {
        LOG_WARN("copy ai column definition failed", K(ret));
      } else if (OB_FAIL(defs.push_back(def))) {
        LOG_WARN("store ai column definition failed", K(ret));
      }
    }
  }
  return ret;
}

}  // namespace ai_column
}  // namespace observer
}  // namespace oceanbase
