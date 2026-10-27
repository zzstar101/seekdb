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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/timezone/ob_time_convert.h"
#include <charconv>
#include <cmath>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    is_file_scan_, file_column_ids_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (MY_SPEC.is_file_scan_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file;
    ret = open_file_scan();
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (MY_SPEC.is_file_scan_) {
    ret = file_reader_ == nullptr ? OB_NOT_INIT
        : file_sql::sql_error(file_reader_->rescan(), file_reader_->error());
  } else {
    node_idx_ = 0;
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (file_reader_ != nullptr) file_reader_->close();
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (file_reader_ != nullptr) {
    file_reader_->~FileScanReader();
    ctx_.get_allocator().free(file_reader_);
    file_reader_ = nullptr;
  }
  ObOperator::destroy();
}


int ObFunctionTableOp::open_file_scan()
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  ObDatum *datum = nullptr;
  ObString secure_root, root_copy;
  file_sql::FileScanDefinition definition;
  if (OB_ISNULL(session)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "File SQL without enable_file_sql");
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(session->get_secure_file_priv(secure_root))) {
  } else if (OB_FAIL(ob_write_string(ctx_.get_allocator(), secure_root, root_copy, true))) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, datum))) {
  } else if (datum == nullptr || datum->is_null()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(definition.decode(datum->get_string()))) {
  } else {
    if (file_reader_ == nullptr) {
      void *storage = ctx_.get_allocator().alloc(sizeof(file_sql::FileScanReader));
      if (storage == nullptr) ret = OB_ALLOCATE_MEMORY_FAILED;
      else file_reader_ = new (storage) file_sql::FileScanReader();
    }
    if (OB_SUCC(ret)) {
      OZ (file_reader_->open(definition, ctx_.get_allocator(), root_copy.ptr()));
      OX (file_reader_->set_projection(MY_SPEC.file_column_ids_.get_data(),
                                       MY_SPEC.file_column_ids_.count()));
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (file_reader_ == nullptr || MY_SPEC.column_exprs_.count() != MY_SPEC.file_column_ids_.count()) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(file_sql::sql_error(file_reader_->next(), file_reader_->error()))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      const int64_t column = MY_SPEC.file_column_ids_.at(i);
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      if (column < 0 || column >= file_reader_->column_count() || expr == nullptr) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObDatum &out = expr->locate_datum_for_write(eval_ctx_);
        if (file_reader_->is_null(column)) {
          out.set_null();
        } else {
          auto text = file_reader_->value(column);
          const ObString str(text.size(), text.data());
          switch (file_reader_->column_type(column)) {
            case file_sql::Type::BigInt: {
              if (!text.empty() && text.front() == '+') text.remove_prefix(1);
              int64_t value = 0;
              auto result = std::from_chars(text.data(), text.data() + text.size(), value);
              if (result.ec != std::errc() || result.ptr != text.data() + text.size()) ret = OB_INVALID_ARGUMENT;
              else out.set_int(value);
              break;
            }
            case file_sql::Type::Double: {
              if (!text.empty() && text.front() == '+') text.remove_prefix(1);
              double value = 0;
              auto result = std::from_chars(text.data(), text.data() + text.size(), value);
              if (result.ec != std::errc() || result.ptr != text.data() + text.size() || !std::isfinite(value)) ret = OB_INVALID_ARGUMENT;
              else out.set_double(value);
              break;
            }
            case file_sql::Type::Boolean:
              out.set_int(str.case_compare("true") == 0 ? 1 : 0);
              break;
            case file_sql::Type::Date: {
              int32_t value = 0;
              OZ (ObTimeConverter::str_to_date(str, value));
              if (OB_SUCC(ret)) out.set_date(value);
              break;
            }
            case file_sql::Type::DateTime: {
              int64_t value = 0;
              const ObTimeConvertCtx convert(nullptr, false);
              OZ (ObTimeConverter::str_to_datetime(str, convert, value));
              if (OB_SUCC(ret)) out.set_datetime(value);
              break;
            }
            case file_sql::Type::Varchar: out.set_string(str); break;
            case file_sql::Type::Null: out.set_null(); break;
          }
        }
        if (OB_SUCC(ret)) expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
