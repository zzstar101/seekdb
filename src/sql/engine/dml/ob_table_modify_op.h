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

#ifndef OCEANBASE_SQL_ENGINE_DML_OB_TABLE_MODIFY_OP_
#define OCEANBASE_SQL_ENGINE_DML_OB_TABLE_MODIFY_OP_

#include "sql/engine/ob_operator.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/dml/ob_fk_checker.h"

namespace oceanbase
{
namespace sql
{

enum ObDmlSnapshotOptState
{
  WITHOUT_SNAPSHOT_OPT_STATE = 0,
  USE_RESPONSE_SNAPSHOT_STATE = 1,
  USE_STMT_SNAPSHOT_STATE = 2,
};
  
class ForeignKeyHandle
{
public:
  struct ObFkRowResInfo
  {
    ObExpr* rt_expr_;
    ObDatum ori_datum_;
    TO_STRING_KV(K_(rt_expr), K_(ori_datum));
  };
  static int do_handle(ObTableModifyOp &op,
                       const ObDMLBaseCtDef &dml_ctdef,
                       ObDMLBaseRtDef &dml_rtdef);
  
private:
  static int value_changed(ObTableModifyOp &op,
                           const common::ObIArray<ObForeignKeyColumn> &columns,
                           const ObExprPtrIArray &old_row,
                           const ObExprPtrIArray &new_row,
                           bool &has_changed);
  static int check_exist(ObTableModifyOp &modify_op,
                         const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row,
                         ObForeignKeyChecker *fk_checker,
                         bool expect_zero,
                         bool use_das_scan);
  static int check_exist_inner_sql(ObTableModifyOp &modify_op,
                                   const ObForeignKeyArg &fk_arg,
                                   const ObExprPtrIArray &row,
                                   bool expect_zero,
                                   bool iter_uncommitted_row);
  static int check_exist_scan_task(ObTableModifyOp &modify_op,
                                   const ObForeignKeyArg &fk_arg,
                                   const ObExprPtrIArray &row,
                                   ObForeignKeyChecker *fk_checker);
  static int cascade(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                     const ObExprPtrIArray &old_row, const ObExprPtrIArray &new_row);
  
  static int set_null(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                     const ObExprPtrIArray &old_row);
  
  static int gen_set(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                     const common::ObIArray<ObForeignKeyColumn> &columns,
                     const ObExprPtrIArray &row, common::ObIAllocator &alloc,
                     const common::ObObjPrintParams &print_params);
  static int gen_where(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                       const common::ObIArray<ObForeignKeyColumn> &columns,
                       const ObExprPtrIArray &row, common::ObIAllocator &alloc,
                       const common::ObObjPrintParams &print_params);
  static int gen_column_value(ObEvalCtx &ctx, char *&buf, int64_t &len, int64_t &pos,
                              const common::ObIArray<ObForeignKeyColumn> &columns,
                              const ObExprPtrIArray &row, const char *delimiter,
                              common::ObIAllocator &alloc,
                              const common::ObObjPrintParams &print_params, bool forbid_null);
  
  static int gen_column_null_value(ObEvalCtx &ctx, char *&buf, int64_t &len, int64_t &pos,
                              const common::ObIArray<ObForeignKeyColumn> &columns,
                              common::ObIAllocator &alloc,
                              const common::ObObjPrintParams &print_params);
  
  static int is_self_ref_row(ObEvalCtx &ctx, const ObExprPtrIArray &row,
                             const ObForeignKeyArg &fk_arg, bool &is_self_ref);
};

class ObTableModifyOp;
class ObTableModifySpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObTableModifySpec(common::ObIAllocator &alloc, const ObPhyOperatorType type);
  virtual ~ObTableModifySpec() {}

  virtual bool is_dml_operator() const override { return true; }
  //This interface is only allowed to be used in a single-table DML operator,
  //it is invalid when multiple tables are modified in one DML operator
  int get_single_table_loc_id(common::ObTableID &table_loc_id,
                              common::ObTableID &ref_table_id) const
  {
    const ObDMLBaseCtDef *dml_ctdef = nullptr;
    int ret = get_single_dml_ctdef(dml_ctdef);
    if (common::OB_SUCCESS == ret) {
      table_loc_id = dml_ctdef->das_base_ctdef_.table_id_;
      ref_table_id = dml_ctdef->das_base_ctdef_.index_tid_;
    }
    return ret;
  }
  //This interface is only allowed to be used in a single-table DML operator,
  //it is invalid when multiple tables are modified in one DML operator
  virtual int get_single_dml_ctdef(const ObDMLBaseCtDef *&dml_ctdef) const
  {
    UNUSED(dml_ctdef);
    return common::OB_NOT_IMPLEMENT;
  }
  void set_table_location_uncertain(bool v) { table_location_uncertain_ = v; }
  bool is_table_location_uncertain() const { return table_location_uncertain_; }
  inline bool use_dist_das() const { return use_dist_das_; }
public:
  // Expr frame info for partial expr serialization. (serialize is not need for it self)
  ObExprFrameInfo *expr_frame_info_;
  ObExpr *ab_stmt_id_; //mark the stmt id for array binding batch execution
  union {
    uint64_t flags_;
    struct {
      uint64_t is_ignore_                       : 1;
      uint64_t gi_above_                        : 1;
      uint64_t is_returning_                    : 1;
      uint64_t is_pdml_index_maintain_          : 1; // indicates whether the current dml operator is the operator used for index maintenance in pdml (index maintain)
      uint64_t table_location_uncertain_        : 1; // Target access partition location uncertain, need full table scan
      uint64_t use_dist_das_                    : 1;
      uint64_t is_pdml_update_split_            : 1; // Mark whether delete, insert op is split from update
      uint64_t check_fk_batch_                  : 1; // mark if the foreign key constraint can be checked in batch    
      uint64_t is_pdml_                         : 1;
      uint64_t need_foreign_key_check_          : 1; // mark if need foreign key check  
      uint64_t need_trigger_fire_               : 1; // mark if need trigger fire
      uint64_t reserved_                        : 53;
    };
  };
  int64_t das_dop_; // default is 0
private:
  DISALLOW_COPY_AND_ASSIGN(ObTableModifySpec);
};

class ObTableModifyOpInput : public ObOpInput
{
public:
  friend class ObTableModifyOp;
  OB_UNIS_VERSION_V(1);
public:
  ObTableModifyOpInput(ObExecContext &ctx, const ObOpSpec &spec)
    : ObOpInput(ctx, spec),
      table_loc_(nullptr),
      tablet_loc_(nullptr)
  { }
  virtual ~ObTableModifyOpInput() { }
  virtual void reset()
  {
    table_loc_ = nullptr;
    tablet_loc_ = nullptr;
  }
  inline void set_tablet_loc(ObDASTabletLoc *tablet_loc) { tablet_loc_ = tablet_loc; }
  inline ObDASTabletLoc *get_tablet_loc() { return tablet_loc_; }
  inline ObDASTableLoc *get_table_loc() { return table_loc_; }
  const ObTableModifySpec &get_spec() const
  {
    return static_cast<const ObTableModifySpec &>(spec_);
  }
  TO_STRING_KV(KPC_(table_loc), KPC_(tablet_loc));
protected:
  ObDASTableLoc *table_loc_;
  ObDASTabletLoc *tablet_loc_; //for single table modify op
private:
  DISALLOW_COPY_AND_ASSIGN(ObTableModifyOpInput);
};

enum AiColumnRowEvent
{
  AI_COLUMN_ROW_INSERTED = 0,
  AI_COLUMN_ROW_UPDATED = 1,
  AI_COLUMN_ROW_DELETED = 2,
};

struct AiColumnKeyColumn
{
  AiColumnKeyColumn() : column_id_(common::OB_INVALID_ID), column_name_() {}
  TO_STRING_KV(K_(column_id), K_(column_name));
  uint64_t column_id_;
  common::ObString column_name_;
};

// One row touched by the current DML statement. The rowkey predicates are the
// printed form of the primary key ("`id` = 1"), which is also the key under
// which the runtime queues its tasks, so the engine - not the runtime - has to
// build them: only the engine has the evaluated row expressions.
struct AiColumnRowChange
{
  AiColumnRowChange()
    : event_(AI_COLUMN_ROW_INSERTED), old_rowkey_(), new_rowkey_() {}
  TO_STRING_KV(K_(event), K_(old_rowkey), K_(new_rowkey));
  AiColumnRowEvent event_;
  common::ObString old_rowkey_;  // deleted or updated row, empty when inserted
  common::ObString new_rowkey_;  // inserted or updated row, empty when deleted
};

// The AI column runtime is owned by the observer (it talks to the model
// endpoints and runs the background worker) while the DML operators live in the
// engine, so the engine reaches it through a server service slot instead of a
// link dependency. A server without the runtime installed simply has no service
// and every AI column table then behaves like a plain table.
class IAiColumnDmlHook
{
public:
  virtual ~IAiColumnDmlHook() {}
  // Coordinator precondition for strict user reads. Implementations ignore
  // ordinary columns and reject if any row in a referenced AI column is not READY.
  virtual int check_strict_read(const uint64_t table_id,
                               const common::ObIArray<uint64_t> &column_ids) = 0;
  // Whether the table has AI columns, plus the primary key columns the engine
  // must print to build the task rowkey predicate. The caller caches the answer
  // so this runs at most once per DML statement.
  virtual int get_table_ai_columns(common::ObIAllocator &allocator,
                                   const uint64_t table_id,
                                   bool &has_ai_column,
                                   common::ObIArray<AiColumnKeyColumn> &pk_columns) = 0;
  // Statements that must commit or roll back together with the business DML:
  // they reset the AI column results of the touched rows, reset the hidden
  // state / request columns and (re)queue one task per AI column and row. The
  // caller runs them through the operator inner connection, i.e. inside the
  // current transaction. updated_column_ids is the column list of the SET clause
  // of an update, so that an AI column whose dependencies are not touched keeps
  // its result; it is empty for an insert or a delete.
  virtual int build_row_change_sql(common::ObIAllocator &allocator,
                                   const uint64_t table_id,
                                   const common::ObIArray<AiColumnRowChange> &changes,
                                   const common::ObIArray<uint64_t> &updated_column_ids,
                                   common::ObIArray<common::ObString> &stmts) = 0;
};

// Guards against re-entering the hook from the statements the runtime itself
// issues, i.e. the AI column result reset and the result publication.
class AiColumnHookGuard
{
public:
  AiColumnHookGuard();
  ~AiColumnHookGuard();
  AiColumnHookGuard(const AiColumnHookGuard &) = delete;
  AiColumnHookGuard &operator=(const AiColumnHookGuard &) = delete;
  static bool is_active();
};

class ObTableModifyOp: public ObOperator
{
public:
  ObTableModifyOp(ObExecContext &ctx, const ObOpSpec &spec, ObOpInput *input);
  virtual ~ObTableModifyOp() {}

  virtual int inner_switch_iterator() override;

  int is_valid();

  sql::ObSQLSessionInfo::StmtSavedValue &get_saved_session()
  {
    if (NULL == saved_session_) {
      saved_session_ = new (saved_session_buf_) sql::ObSQLSessionInfo::StmtSavedValue();
    }
    return *saved_session_;
  }

  const ObTableModifySpec &get_spec() const
  {
    return static_cast<const ObTableModifySpec &>(spec_);
  }
  ObTableModifyOpInput *get_input() const
  {
    return static_cast<ObTableModifyOpInput *>(input_);
  }

  virtual void destroy() override
  {
    dml_rtctx_.cleanup();
    trigger_clear_exprs_.reset();
    fk_checkers_.reset();
    last_store_row_.reset();
    ObOperator::destroy();
  }

public:
  int open_inner_conn();
  int close_inner_conn();
  int begin_nested_session(bool skip_cur_stmt_tables);
  int end_nested_session();
  int execute_write(const char *sql);
  int execute_read(const char *sql, common::ObMySQLProxy::MySQLResult &res);
  int check_stack();
  bool is_nested_session() { return ObSQLUtils::is_nested_sql(&ctx_); }
  bool is_fk_nested_session() { return ObSQLUtils::is_fk_nested_sql(&ctx_); }
  bool need_foreign_key_checks() { return need_foreign_key_check_; }
  bool has_before_row_trigger(const ObDMLBaseCtDef &dml_ctdef) { return dml_ctdef.is_primary_index_ && dml_ctdef.trig_ctdef_.all_tm_points_.has_before_row(); }
  bool has_after_row_trigger(const ObDMLBaseCtDef &dml_ctdef) { return dml_ctdef.is_primary_index_ && dml_ctdef.trig_ctdef_.all_tm_points_.has_after_row(); }
  bool need_foreign_key_check(const ObDMLBaseCtDef &dml_ctdef) { return dml_ctdef.is_primary_index_ && dml_ctdef.fk_args_.count() > 0; }
  bool need_after_row_process(const ObDMLBaseCtDef &dml_ctdef) { return need_foreign_key_check(dml_ctdef) || has_after_row_trigger(dml_ctdef); }
  void set_execute_single_row() { execute_single_row_ = true; }
  void unset_execute_single_row() { execute_single_row_ = false; }
  bool get_execute_single_row() const { return execute_single_row_; }
  bool is_fk_root_session();
  const ObObjPrintParams &get_obj_print_params() { return obj_print_params_; }
  int init_foreign_key_operation();
  void clear_dml_evaluated_flag();
  void clear_dml_evaluated_flag(int64_t parent_cnt, ObExpr **parent_exprs);
  void clear_dml_evaluated_flag(ObExpr *clear_expr);

  ObDMLModifyRowsList& get_dml_modify_row_list() { return dml_modify_rows_;}
  int submit_all_dml_task();

  int perform_batch_fk_check();

  // ---- AI column runtime hooks -------------------------------------------
  // Collect one row touched by the current statement. Called from the per row
  // loops of the insert / update / delete operators right after the row has
  // been handed to DAS, so the rowkey predicate can be printed from the
  // evaluated row expressions. Nothing happens for a table without AI column.
  int ai_column_collect_row(const ObDMLBaseCtDef &dml_ctdef,
                            const AiColumnRowEvent event,
                            const ExprFixedArray *old_row,
                            const ExprFixedArray *new_row);
  // Run the collected statements through the inner connection. Called after the
  // DAS tasks of the statement have been flushed (submit_all_dml_task) and
  // before the statement returns, so that the queued task rows commit or roll
  // back together with the business rows.
  int ai_column_run_hook();
protected:
  int ai_column_prepare_table(const ObDMLBaseCtDef &dml_ctdef, bool &enabled);
  int ai_column_build_rowkey(const ObDMLBaseCtDef &dml_ctdef,
                             const ExprFixedArray &row_exprs,
                             common::ObString &rowkey_predicate);

  OperatorOpenOrder get_operator_open_order() const;
  virtual int inner_open();
  virtual int inner_close();

  int get_gi_task();
  //It is used for the execution without routing through DAS.
  //In this case, the DML operator only allows to modify a single table,
  //and the table location is specified by the scheduling framework.
  int calc_single_table_loc();

  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int check_need_exec_single_row();
  int get_next_row_from_child();
  //Override this interface to complete the write semantics of the DML operator,
  //and write a row to the DAS Write Buffer according to the specific DML behavior
  virtual int write_row_to_das_buffer() { return common::OB_NOT_IMPLEMENT; }
  //Override this interface to post process the DML info after
  //writing all data to the storage or returning one row
  //such as: set affected_rows to query context, rewrite some error code
  virtual int write_rows_post_proc(int last_errno)
  { UNUSED(last_errno); return common::OB_NOT_IMPLEMENT; }
  virtual ObDasParallelType check_das_parallel_type();

  int init_das_dml_ctx();
  //to merge array binding cusor info when array binding is executed in batch mode
  int merge_implict_cursor(int64_t affected_rows,
                           int64_t found_rows,
                           int64_t matched_rows,
                           int64_t duplicated_rows);

  int prepare_implict_cursor(int64_t affected_rows,
                             int64_t found_rows,
                             int64_t matched_rows_,
                             int64_t duplicated_rows_,
                             ObImplicitCursorInfo &implicit_cursor);

  int replace_implict_cursor(int64_t insert_rows,
                             int64_t update_rows,
                             int64_t delete_rows,
                             int64_t found_rows);
  int discharge_das_write_buffer();
  virtual void record_err_for_load_data(int err_ret, int row_num) { UNUSED(err_ret); UNUSED(row_num); }
public:
  common::ObMySQLProxy *sql_proxy_;
  common::sqlclient::ObISQLConnectionGuard inner_conn_guard_;
  ObIInnerSQLConnection *inner_conn_;
  ObIInnerSQLConnection::SavedValue saved_conn_;
  bool need_foreign_key_check_;
  bool need_close_conn_;

  ObObjPrintParams obj_print_params_;
  bool iter_end_;
  ObDMLRtCtx dml_rtctx_;
  bool execute_single_row_;
  ObSEArray<ObExpr *, 4> trigger_clear_exprs_;
  ObDMLModifyRowsList dml_modify_rows_;
  ObSEArray<ObForeignKeyChecker *, 4> fk_checkers_;
  ObChunkDatumStore::ShadowStoredRow last_store_row_;
  // AI column runtime state, see ai_column_collect_row().
  common::ObArenaAllocator ai_column_allocator_;
  ObSEArray<AiColumnKeyColumn, 4> ai_column_pk_columns_;
  ObSEArray<AiColumnRowChange, 8> ai_column_changes_;
  ObSEArray<uint64_t, 8> ai_column_updated_column_ids_;
  uint64_t ai_column_table_id_;
  // the table the collected row changes belong to; a statement that touches rows of
  // two ai column tables is refused instead of writing tasks of one table with the
  // row key of another
  uint64_t ai_column_changes_table_id_;
  bool ai_column_checked_;
  bool ai_column_enabled_;
private:
  ObSQLSessionInfo::StmtSavedValue *saved_session_;
  char saved_session_buf_[sizeof(ObSQLSessionInfo::StmtSavedValue)] __attribute__((aligned (16)));;

  // used by check_rowkey_whether_distinct
  static const int64_t MIN_ROWKEY_DISTINCT_BUCKET_NUM = 1 * 1024;
  static const int64_t MAX_ROWKEY_DISTINCT_BUCKET_NUM = 1 * 1024 * 1024;
};

}  // namespace sql
}  // namespace oceanbase
#endif
