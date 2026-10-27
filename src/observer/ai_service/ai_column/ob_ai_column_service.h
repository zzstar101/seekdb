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

#pragma once

#include "lib/ob_define.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_array.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "observer/ai_service/ai_column/ob_ai_column_task.h"
#include "observer/ai_service/ai_column/ob_ai_column_worker.h"
#include "sql/engine/dml/ob_table_modify_op.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
class ObIAllocator;
namespace sqlclient
{
class ObMySQLResult;
}
}
namespace share
{
namespace schema
{
class ObTableSchema;
}
}
namespace observer
{
namespace ai_column
{

// Server side of the AI COLUMN runtime.
//
// Two halves live here:
//
//   * the DML hook of the engine (sql::IAiColumnDmlHook).  The engine can not do
//     anything AI column specific itself: it does not know which columns are AI
//     columns, it can not build the catalog and task SQL, and it must not call a
//     model.  So the engine collects the rows it changed and calls back into this
//     service, which produces the statements that are executed through the inner
//     connection of the operator, i.e. inside the very same transaction as the
//     business DML.  That is the whole point: a rolled back insert / update /
//     delete leaves no task behind, and no model is ever called for a row that
//     never committed.
//
//   * the background worker.  It only runs after the commit: it claims task rows
//     from __all_ai_column_task, calls the model outside of any user transaction
//     and publishes the results with the request id CAS.
//
// The service is bound into the server runtime as sql::IAiColumnDmlHook, so the
// engine reaches it without a link dependency, and it is a process wide singleton
// because there is exactly one task queue (seekdb runs a single server).
class AiColumnService : public sql::IAiColumnDmlHook
{
public:
  static AiColumnService &get_instance();

  int init(common::ObMySQLProxy &sql_proxy);
  int start();
  void stop();
  void destroy();

  // ---- sql::IAiColumnDmlHook --------------------------------------------

  // The table schema is the authority on the primary key (a task row is addressed
  // by the printed primary key of its row) and on whether the table is an AI column
  // table at all (the persisted AI_COLUMN_FLAG); the catalog is only read for a
  // table the schema flags, and it is then the authority on the definitions.
  int get_table_ai_columns(common::ObIAllocator &allocator,
                           const uint64_t table_id,
                           bool &has_ai_column,
                           common::ObIArray<sql::AiColumnKeyColumn> &pk_columns) override;

  // The statements that queue / invalidate / drop the tasks of the changed rows.
  int build_row_change_sql(common::ObIAllocator &allocator,
                           const uint64_t table_id,
                           const common::ObIArray<sql::AiColumnRowChange> &changes,
                           const common::ObIArray<uint64_t> &updated_column_ids,
                           common::ObIArray<common::ObString> &stmts) override;

  // ---- strict read -------------------------------------------------------

  // Checked by the statement that references an AI column when the system
  // variable ai_column_strict_read is on.  The check is per column, not per row:
  // one row whose result is not READY rejects every statement that reads the
  // column, because the reader can not know which rows the statement will touch.
  // Returns the "AI column has unfinished tasks" error when such a row exists.
  int check_strict_read(const uint64_t table_id, const common::ObIArray<uint64_t> &column_ids) override;

  // Definitions of the AI columns of one table, deep copied into `allocator`.
  // The background worker uses the very same loader as the DML hook, so the SQL
  // of both halves is always built from the same definition text.
  int get_ai_column_defs(common::ObIAllocator &allocator,
                         const uint64_t table_id,
                         AiColumnDefArray &defs);

private:
  // One catalog snapshot of one table, owned by the stack frame of the call that
  // needs it.  The strings of the definitions are allocated from that frame's
  // arena, so the lifetime of an entry is exactly the lifetime of the call: no
  // shared state, no lock, and nothing that has to be invalidated when the schema
  // version of the table moves.
  //
  // A caller must never let a pointer into defs_ escape the frame that owns the
  // arena; every output of the service is deep copied into the allocator of the
  // caller instead.
  struct TableDefEntry
  {
    TableDefEntry() : table_id_(OB_INVALID_ID), has_ai_column_(false), defs_() {}
    uint64_t table_id_;
    bool has_ai_column_;
    AiColumnDefArray defs_;
  };

  AiColumnService();
  virtual ~AiColumnService();

  // Whether the trusted schema snapshot says the table has an AI column at all.
  //
  // The persisted column flag (AI_COLUMN_FLAG, i.e. is_ai_column_column) is the
  // authority, and the guard already holds the snapshot, so the answer costs a scan
  // of one in-memory column array and no database access.  This is the gate in front
  // of the catalog: a table without the flag is not an AI column table, and
  // __all_ai_column is not read for it.
  static bool table_has_ai_column(const share::schema::ObTableSchema &table_schema);

  // Fill `entry` with the AI column definitions of the table, allocating the
  // definition strings from `allocator` (the arena of the calling frame).
  //
  // The catalog is read only when the trusted schema says the table has an AI
  // column, so a statement on an ordinary table never pays for it.
  int load_table_defs(common::ObIAllocator &allocator,
                      share::schema::ObSchemaGetterGuard &schema_guard,
                      const share::schema::ObTableSchema &table_schema,
                      TableDefEntry &entry);
  int read_catalog_row(common::ObIAllocator &allocator,
                       const share::schema::ObTableSchema &table_schema,
                       const common::ObString &db_name,
                       common::sqlclient::ObMySQLResult &result,
                       AiColumnDef &def);
  // Which AI columns of the table have to be recomputed by an update that changed
  // the given columns.  A column keeps its result when none of the columns its
  // expression refers to is touched.
  bool depends_on_updated_columns(const AiColumnDef &def,
                                  const common::ObIArray<common::ObString> &updated_names);
  int new_request_id(common::ObIAllocator &allocator, common::ObString &request_id);
  // read the single scalar of a count(*) statement
  int read_count(const common::ObSqlString &sql, int64_t &count);
  int build_change_sql(common::ObIAllocator &allocator,
                       const TableDefEntry &entry,
                       const sql::AiColumnRowChange &change,
                       const common::ObIArray<common::ObString> &updated_names,
                       common::ObIArray<common::ObString> &stmts);

private:
  bool is_inited_;
  bool is_started_;
  common::ObMySQLProxy *sql_proxy_;
  AiColumnWorker worker_;
  int64_t request_seq_;
};

}  // namespace ai_column
}  // namespace observer
}  // namespace oceanbase
