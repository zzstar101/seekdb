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

#define USING_LOG_PREFIX STORAGE

#include "ob_sparse_daat_iter.h"

namespace oceanbase
{
namespace storage
{

ObSRDaaTBatchIdMerger::ObSRDaaTBatchIdMerger()
  : dim_iters_(nullptr),
    merge_cmp_(nullptr),
    merge_heap_(nullptr),
    current_ids_(nullptr),
    batch_ids_(),
    batch_counts_(),
    batch_positions_(),
    last_id_(),
    dim_cnt_(0),
    active_cnt_(0),
    batch_capacity_(0),
    has_last_id_(false),
    is_inited_(false)
{
}

ObSRDaaTBatchIdMerger::~ObSRDaaTBatchIdMerger()
{
  reset();
}

int ObSRDaaTBatchIdMerger::init(
    ObIArray<ObISRDaaTDimIter *> &dim_iters,
    ObIAllocator &allocator,
    const int64_t batch_capacity,
    ObSRMergeCmp &merge_cmp,
    ObSRMergeHeap *merge_heap,
    ObFixedArray<const ObDatum *, ObIAllocator> &current_ids)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(dim_iters.count() <= 0 || batch_capacity <= 0
                         || current_ids.count() < dim_iters.count()
                         || (dim_iters.count() > 3 && OB_ISNULL(merge_heap)))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // Logical reuse keeps these fixed arrays. A smaller dimension set can share the existing
    // capacity; growing beyond it would require abandoning arena memory that cannot be freed.
    const int64_t allocated_dim_cnt = batch_ids_.count();
    if (allocated_dim_cnt > 0
        && (allocated_dim_cnt < dim_iters.count()
            || batch_counts_.count() < dim_iters.count()
            || batch_positions_.count() < dim_iters.count())) {
      ret = OB_NOT_SUPPORTED;
    }
    dim_iters_ = &dim_iters;
    merge_cmp_ = &merge_cmp;
    merge_heap_ = merge_heap;
    current_ids_ = &current_ids;
    dim_cnt_ = dim_iters.count();
    active_cnt_ = dim_cnt_;
    batch_capacity_ = batch_capacity;
    if (OB_FAIL(ret)) {
    } else if (0 == allocated_dim_cnt) {
      batch_ids_.set_allocator(&allocator);
      batch_counts_.set_allocator(&allocator);
      batch_positions_.set_allocator(&allocator);
      if (OB_FAIL(batch_ids_.init(dim_cnt_))) {
      } else if (OB_FAIL(batch_ids_.prepare_allocate(dim_cnt_))) {
      } else if (OB_FAIL(batch_counts_.init(dim_cnt_))) {
      } else if (OB_FAIL(batch_counts_.prepare_allocate(dim_cnt_))) {
      } else if (OB_FAIL(batch_positions_.init(dim_cnt_))) {
      } else if (OB_FAIL(batch_positions_.prepare_allocate(dim_cnt_))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_cnt_; ++i) {
        ObISRDaaTDimIter *dim_iter = dim_iters.at(i);
        if (OB_ISNULL(dim_iter) || OB_UNLIKELY(!dim_iter->support_batch_doc_ids())) {
          ret = OB_NOT_SUPPORTED;
        } else if (OB_FAIL(load_batch(i))) {
          LOG_WARN("failed to load the first document-id batch", K(ret), K(i));
        }
      }
    }
    if (OB_SUCC(ret) && dim_cnt_ > 3) {
      if (OB_UNLIKELY(!merge_heap_->empty())) {
        ret = OB_ERR_UNEXPECTED;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_cnt_; ++i) {
        if (OB_NOT_NULL(current_ids_->at(i))) {
          ObSRMergeItem item;
          item.relevance_ = 1.0;
          item.iter_idx_ = i;
          if (OB_FAIL(merge_heap_->push(item))) {
            LOG_WARN("failed to seed the document-id merge heap", K(ret), K(i));
          }
        }
      }
      if (OB_SUCC(ret) && !merge_heap_->empty() && OB_FAIL(merge_heap_->rebuild())) {
        LOG_WARN("failed to build the document-id merge heap", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      has_last_id_ = false;
      is_inited_ = true;
    }
  }
  return ret;
}

void ObSRDaaTBatchIdMerger::reuse()
{
  last_id_.reset();
  dim_iters_ = nullptr;
  merge_cmp_ = nullptr;
  merge_heap_ = nullptr;
  current_ids_ = nullptr;
  dim_cnt_ = 0;
  active_cnt_ = 0;
  batch_capacity_ = 0;
  has_last_id_ = false;
  is_inited_ = false;
}

void ObSRDaaTBatchIdMerger::reset()
{
  reuse();
  batch_ids_.reset();
  batch_counts_.reset();
  batch_positions_.reset();
}

int ObSRDaaTBatchIdMerger::load_batch(const int64_t iter_idx)
{
  int ret = OB_SUCCESS;
  const ObDatum **doc_ids = nullptr;
  int64_t count = 0;
  if (OB_UNLIKELY(iter_idx < 0 || iter_idx >= dim_cnt_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(dim_iters_->at(iter_idx)->get_next_doc_id_batch(
                 batch_capacity_, doc_ids, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
      LOG_WARN("failed to load document-id batch", K(ret), K(iter_idx));
    } else if (0 == count) {
      ret = OB_SUCCESS;
      current_ids_->at(iter_idx) = nullptr;
      batch_ids_[iter_idx] = nullptr;
      batch_counts_[iter_idx] = 0;
      batch_positions_[iter_idx] = 0;
      --active_cnt_;
      return ret;
    } else {
      ret = OB_SUCCESS;
    }
  }
  if (OB_SUCC(ret) && OB_UNLIKELY(0 == count)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_SUCC(ret) && count > 0) {
    if (OB_ISNULL(doc_ids) || OB_UNLIKELY(count > batch_capacity_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      batch_ids_[iter_idx] = doc_ids;
      batch_counts_[iter_idx] = count;
      batch_positions_[iter_idx] = 0;
      current_ids_->at(iter_idx) = doc_ids[0];
    }
  }
  return ret;
}

int ObSRDaaTBatchIdMerger::advance(const int64_t iter_idx)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(iter_idx < 0 || iter_idx >= dim_cnt_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(batch_positions_[iter_idx] >= batch_counts_[iter_idx])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (++batch_positions_[iter_idx] < batch_counts_[iter_idx]) {
    current_ids_->at(iter_idx) = batch_ids_[iter_idx][batch_positions_[iter_idx]];
  } else if (OB_FAIL(load_batch(iter_idx))) {
  }
  return ret;
}

int ObSRDaaTBatchIdMerger::get_next_distinct_id(const ObDatum *&id_datum)
{
  int ret = OB_SUCCESS;
  id_datum = nullptr;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (0 == active_cnt_) {
    ret = OB_ITER_END;
  } else if (1 == dim_cnt_) {
    ret = get_next_from_one(id_datum);
  } else if (2 == dim_cnt_) {
    ret = get_next_from_two(id_datum);
  } else if (3 == dim_cnt_) {
    ret = get_next_from_three(id_datum);
  } else {
    ret = get_next_from_heap(id_datum);
  }
  return ret;
}

int ObSRDaaTBatchIdMerger::get_next_from_one(const ObDatum *&id_datum)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && active_cnt_ > 0) {
    const ObDatum *current = current_ids_->at(0);
    int64_t cmp_ret = 1;
    if (OB_ISNULL(current)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (has_last_id_
               && OB_FAIL(merge_cmp_->cmp_id(*current, last_id_.get_datum(), cmp_ret))) {
    } else if (0 != cmp_ret) {
      if (OB_FAIL(last_id_.set_datum_fast(*current))) {
      } else {
        has_last_id_ = true;
      }
    }
    const bool emit = OB_SUCC(ret) && 0 != cmp_ret;
    if (OB_SUCC(ret) && OB_FAIL(advance(0))) {
    } else if (OB_SUCC(ret) && emit) {
      id_datum = &last_id_.get_datum();
      return OB_SUCCESS;
    }
  }
  if (OB_SUCC(ret) && 0 == active_cnt_) {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObSRDaaTBatchIdMerger::get_next_from_two(const ObDatum *&id_datum)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && active_cnt_ > 0) {
    const ObDatum *left = current_ids_->at(0);
    const ObDatum *right = current_ids_->at(1);
    int64_t choose = -1;
    int64_t cmp_ret = 1;
    if (OB_ISNULL(left) && OB_ISNULL(right)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(left)) {
      choose = 1;
    } else if (OB_ISNULL(right)) {
      choose = 0;
    } else if (OB_FAIL(merge_cmp_->cmp_id(*left, *right, cmp_ret))) {
    } else {
      choose = cmp_ret <= 0 ? 0 : 1;
    }
    if (OB_SUCC(ret)) {
      const ObDatum *chosen = 0 == choose ? left : right;
      int64_t last_cmp_ret = 1;
      if (has_last_id_
          && OB_FAIL(merge_cmp_->cmp_id(*chosen, last_id_.get_datum(), last_cmp_ret))) {
      } else if (0 != last_cmp_ret && OB_FAIL(last_id_.set_datum_fast(*chosen))) {
      } else if (0 != last_cmp_ret) {
        has_last_id_ = true;
      }
      const bool emit = OB_SUCC(ret) && 0 != last_cmp_ret;
      if (OB_SUCC(ret) && OB_NOT_NULL(left) && OB_NOT_NULL(right) && 0 == cmp_ret) {
        if (OB_FAIL(advance(0))) {
        } else if (OB_FAIL(advance(1))) {
        }
      } else if (OB_SUCC(ret) && OB_FAIL(advance(choose))) {
      }
      if (OB_SUCC(ret) && emit) {
        id_datum = &last_id_.get_datum();
        return OB_SUCCESS;
      }
    }
  }
  if (OB_SUCC(ret) && 0 == active_cnt_) {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObSRDaaTBatchIdMerger::get_next_from_three(const ObDatum *&id_datum)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && active_cnt_ > 0) {
    int64_t choose = -1;
    uint64_t equal_head_mask = 0;
    // Select the minimum and all of its equal heads in at most two comparisons. A null
    // pointer marks an exhausted dimension; a NULL datum still participates in cmp_id().
    for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
      const ObDatum *current = current_ids_->at(i);
      int64_t cmp_ret = 0;
      if (OB_ISNULL(current)) {
      } else if (choose < 0) {
        choose = i;
        equal_head_mask = 1ULL << i;
      } else if (OB_FAIL(merge_cmp_->cmp_id(*current, *current_ids_->at(choose), cmp_ret))) {
      } else if (cmp_ret < 0) {
        choose = i;
        equal_head_mask = 1ULL << i;
      } else if (0 == cmp_ret) {
        equal_head_mask |= 1ULL << i;
      }
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(choose < 0)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_SUCC(ret)) {
      const ObDatum *chosen = current_ids_->at(choose);
      int64_t last_cmp_ret = 1;
      // Keep the chosen value before advancing any dimension can replace a batch buffer.
      // Comparing against it also deduplicates repeated ids within a single dimension.
      if (has_last_id_
          && OB_FAIL(merge_cmp_->cmp_id(*chosen, last_id_.get_datum(), last_cmp_ret))) {
      } else if (0 != last_cmp_ret && OB_FAIL(last_id_.set_datum_fast(*chosen))) {
      } else if (0 != last_cmp_ret) {
        has_last_id_ = true;
      }
      const bool emit = OB_SUCC(ret) && 0 != last_cmp_ret;
      for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
        if (0 != (equal_head_mask & (1ULL << i)) && OB_FAIL(advance(i))) {
        }
      }
      if (OB_SUCC(ret) && emit) {
        id_datum = &last_id_.get_datum();
        return OB_SUCCESS;
      }
    }
  }
  if (OB_SUCC(ret) && 0 == active_cnt_) {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObSRDaaTBatchIdMerger::get_next_from_heap(const ObDatum *&id_datum)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && !merge_heap_->empty()) {
    const ObSRMergeItem *top = nullptr;
    if (OB_FAIL(merge_heap_->top(top)) || OB_ISNULL(top)) {
      if (OB_SUCCESS == ret) {
        ret = OB_ERR_UNEXPECTED;
      }
    } else {
      const int64_t iter_idx = top->iter_idx_;
      const ObDatum *current = current_ids_->at(iter_idx);
      int64_t cmp_ret = 1;
      if (OB_ISNULL(current)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (has_last_id_
                 && OB_FAIL(merge_cmp_->cmp_id(*current, last_id_.get_datum(), cmp_ret))) {
      } else if (0 != cmp_ret && OB_FAIL(last_id_.set_datum_fast(*current))) {
      } else if (0 != cmp_ret) {
        has_last_id_ = true;
      }
      const bool emit = OB_SUCC(ret) && 0 != cmp_ret;
      if (OB_SUCC(ret) && OB_FAIL(merge_heap_->pop())) {
      } else if (OB_SUCC(ret) && OB_FAIL(advance(iter_idx))) {
      } else if (OB_SUCC(ret) && OB_NOT_NULL(current_ids_->at(iter_idx))) {
        ObSRMergeItem item;
        item.relevance_ = 1.0;
        item.iter_idx_ = iter_idx;
        if (OB_FAIL(merge_heap_->push(item))) {
          LOG_WARN("failed to advance the document-id merge heap", K(ret), K(iter_idx));
        } else if (!merge_heap_->empty() && OB_FAIL(merge_heap_->rebuild())) {
          LOG_WARN("failed to rebuild the document-id merge heap", K(ret), K(iter_idx));
        }
      }
      if (OB_SUCC(ret) && emit) {
        id_datum = &last_id_.get_datum();
        return OB_SUCCESS;
      }
    }
  }
  if (OB_SUCC(ret) && merge_heap_->empty()) {
    ret = OB_ITER_END;
  }
  return ret;
}

ObSRMergeCmp::ObSRMergeCmp()
  : cmp_func_(nullptr),
    cmp_mode_(CMP_MODE_GENERIC),
    iter_ids_(nullptr),
    is_inited_(false)
{
}

int ObSRMergeCmp::init(
    sql::ObDatumMeta id_meta,
    const ObFixedArray<const ObDatum *, ObIAllocator> *iter_ids)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(iter_ids)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    iter_ids_ = iter_ids;
    common::ObDatumBasicFuncs *basic_funcs = ObDatumFuncs::get_basic_func(id_meta.type_, id_meta.cs_type_);
    cmp_func_ = basic_funcs->null_first_cmp_;
    if (OB_ISNULL(cmp_func_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      // The document id is a fixed length key: an unsigned integer is compared as an integer and a
      // binary string is compared with memcmp, which is exactly what the binary collation does.
      // Every other case, including a null document id, keeps using the generic comparator.
      if (common::ObUInt64Type == id_meta.type_) {
        cmp_mode_ = CMP_MODE_UINT64;
      } else if (common::CS_TYPE_BINARY == id_meta.cs_type_ && common::ob_is_string_type(id_meta.type_)) {
        cmp_mode_ = CMP_MODE_BINARY;
      } else {
        cmp_mode_ = CMP_MODE_GENERIC;
      }
      is_inited_ = true;
    }
  }
  return ret;
}

int ObSRMergeCmp::cmp(
    const ObSRMergeItem &l,
    const ObSRMergeItem &r,
    int64_t &cmp_ret)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    // the document id comparison is the hottest operation of the merge, so a fixed length document
    // id is compared directly instead of going through the generic collation comparator
    if (OB_FAIL(cmp_id(get_id_datum(l.iter_idx_), get_id_datum(r.iter_idx_), cmp_ret))) {
    }
  }
  return ret;
}

ObSRDaaTIterImpl::ObSRDaaTIterImpl()
  : ObISparseRetrievalMergeIter(),
    iter_allocator_(nullptr),
    iter_param_(nullptr),
    dim_iters_(nullptr),
    merge_cmp_(),
    merge_heap_(nullptr),
    relevance_collector_(nullptr),
    iter_domain_ids_(),
    buffered_domain_ids_(),
    buffered_relevances_(),
    next_round_iter_idxes_(),
    batch_id_merger_(),
    next_round_cnt_(0),
    set_datum_func_(nullptr),
    need_score_(true),
    count_only_(false),
    count_only_batch_capable_(false),
    count_only_batch_started_(false)
{
}

int ObSRDaaTIterImpl::init(
    ObSparseRetrievalMergeParam &iter_param,
    ObIArray<ObISRDaaTDimIter *> &dim_iters,
    ObIAllocator &iter_allocator,
    ObSRDaaTRelevanceCollector &relevance_collector)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(iter_param.max_batch_size_ <= 0)) {
      ret = OB_ERR_UNEXPECTED;
  } else {
    iter_allocator_ = &iter_allocator;
    iter_param_ = &iter_param;
    dim_iters_ = &dim_iters;
    relevance_collector_ = &relevance_collector;
    // the relevance of a dimension is only required if the query really consumes it: it is
    // projected, it is used by the score filter, or it is used by the topk (dynamic pruning)
    // processing. Otherwise the dimension score is replaced by a constant positive value, which
    // means "this document is contained by this dimension".
    need_score_ = iter_param_->relevance_expr_ != nullptr
        || iter_param_->need_project_relevance()
        || iter_param_->need_filter()
        || iter_param_->need_pushdown_topk();
    // The merged rows are only observable through the projected document id, which is decided
    // above the retrieval layer: the scan counts its rows instead of producing them only when the
    // plan said so (a bare COUNT over the match predicate). It is never inferred from "the
    // relevance is not projected" alone.
    count_only_ = iter_param_->need_count_only();
    count_only_batch_capable_ = count_only_ && !need_score_ && !iter_param_->need_filter()
        && relevance_collector_->support_count_only_union() && dim_iters.count() > 0;
    for (int64_t i = 0; count_only_batch_capable_ && i < dim_iters.count(); ++i) {
      const ObISRDaaTDimIter *dim_iter = dim_iters.at(i);
      count_only_batch_capable_ = OB_NOT_NULL(dim_iter) && dim_iter->support_batch_doc_ids();
    }
    const int64_t max_batch_size = OB_MAX(iter_param_->eval_ctx_->max_batch_size_, iter_param_->max_batch_size_);
    if (iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObUInt64Type) {
      set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_int;
    } else {
      set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_shallow;
    }
    if (OB_UNLIKELY(dim_iters.count() == 0)) {
    } else if (OB_NOT_NULL(iter_param_->dim_weights_) && dim_iters.count() != iter_param_->dim_weights_->count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (FALSE_IT(iter_domain_ids_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(iter_domain_ids_.init(dim_iters.count()))) {
    } else if (OB_FAIL(iter_domain_ids_.prepare_allocate(dim_iters.count()))) {
    } else if (FALSE_IT(next_round_iter_idxes_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(next_round_iter_idxes_.init(dim_iters.count()))) {
    } else if (OB_FAIL(next_round_iter_idxes_.prepare_allocate(dim_iters.count()))) {
    } else if (FALSE_IT(buffered_domain_ids_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(buffered_domain_ids_.init(max_batch_size))) {
    } else if (OB_FAIL(buffered_domain_ids_.prepare_allocate(max_batch_size))) {
    } else if (FALSE_IT(buffered_relevances_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(buffered_relevances_.init(max_batch_size))) {
    } else if (OB_FAIL(buffered_relevances_.prepare_allocate(max_batch_size))) {
    } else if (OB_FAIL(merge_cmp_.init(iter_param_->id_proj_expr_->datum_meta_, &iter_domain_ids_))) {
    } else if (OB_FAIL(init_merge_heap(dim_iters_->count()))) {
    } else {
      for (int64_t i = 0; i < dim_iters.count(); ++i) {
        next_round_iter_idxes_[i] = i;
      }
    }

    if (OB_SUCC(ret)) {
      next_round_cnt_ = dim_iters.count();
      input_row_cnt_ = 0;
      output_row_cnt_ = 0;
      is_inited_ = true;
    }
  }
  return ret;
}

void ObSRDaaTIterImpl::reset()
{
  if (OB_NOT_NULL(merge_heap_)) {
    merge_heap_->~ObSRMergeHeap();
    merge_heap_ = nullptr;
  }
  if (OB_NOT_NULL(relevance_collector_)) {
    relevance_collector_->reset();
    relevance_collector_ = nullptr;
  }
  iter_domain_ids_.reset();
  buffered_domain_ids_.reset();
  buffered_relevances_.reset();
  next_round_iter_idxes_.reset();
  batch_id_merger_.reset();
  next_round_cnt_ = 0;
  input_row_cnt_ = 0;
  output_row_cnt_ = 0;
  count_only_batch_capable_ = false;
  count_only_batch_started_ = false;
  is_inited_ = false;
}

void ObSRDaaTIterImpl::reuse(const bool switch_tablet)
{
  if (OB_NOT_NULL(dim_iters_)) {
    if (OB_NOT_NULL(merge_heap_)) {
      merge_heap_->reuse();
      merge_heap_->open(dim_iters_->count());
    }
    next_round_cnt_ = dim_iters_->count();
    for (int64_t i = 0; i < next_round_cnt_; ++i) {
      next_round_iter_idxes_[i] = i;
    }
  }
  if (OB_NOT_NULL(relevance_collector_)) {
    relevance_collector_->reuse();
  }
  batch_id_merger_.reuse();
  input_row_cnt_ = 0;
  output_row_cnt_ = 0;
  count_only_batch_started_ = false;
}

  int ObSRDaaTIterImpl::get_query_max_score(double &score)
  {
    int ret = OB_SUCCESS;
    score = 0.0;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
      ObISRDaaTDimIter *dim_iter = dim_iters_->at(i);
      double dim_score = 0.0;
      if (OB_ISNULL(dim_iter) || OB_ISNULL(iter_param_->dim_weights_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(dim_iter->get_dim_max_score(dim_score))) {
      } else {
        score += dim_score * iter_param_->field_boost_ * iter_param_->dim_weights_->at(i);
      }
    }

    return ret;
  }

int ObSRMergeCmp::cmp_id(const ObDatum &l, const ObDatum &r, int64_t &cmp_ret)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(l.is_null() || r.is_null()) || CMP_MODE_GENERIC == cmp_mode_) {
    int cmp_res = 0;
    if (OB_FAIL(cmp_func_(l, r, cmp_res, nullptr))) {
    } else {
      cmp_ret = (cmp_res < 0) ? -1 : (cmp_res > 0 ? 1 : 0);
    }
  } else if (CMP_MODE_UINT64 == cmp_mode_) {
    const uint64_t left = l.get_uint64();
    const uint64_t right = r.get_uint64();
    cmp_ret = (left < right) ? -1 : (left > right ? 1 : 0);
  } else {
    const common::ObString &left = l.get_string();
    const common::ObString &right = r.get_string();
    const int64_t min_len = MIN(left.length(), right.length());
    const int cmp_res = (0 == min_len) ? 0 : MEMCMP(left.ptr(), right.ptr(), min_len);
    if (0 != cmp_res) {
      cmp_ret = (cmp_res < 0) ? -1 : 1;
    } else if (left.length() == right.length()) {
      cmp_ret = 0;
    } else {
      cmp_ret = (left.length() < right.length()) ? -1 : 1;
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::get_next_row()
{
  int ret = OB_SUCCESS;
  int64_t count = 0;
  if (OB_FAIL(get_next_rows(1, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else if (OB_UNLIKELY(count != 0)) {
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (OB_UNLIKELY(count != 1)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObSRDaaTIterImpl::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else if (count_only()) {
    ret = get_next_rows_count_only(capacity, count);
  } else if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (iter_param_->limit_param_->is_valid() && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else {
      count = 0;
    }
  } else {
    count = 0;
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      if (OB_FAIL(do_one_merge_round(count))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        }
      }
    }

    if (OB_SUCC(ret) || OB_ITER_END == ret) {
      if (count > 0 && OB_FAIL(project_results(count))) {
      }
    }

  }
  return ret;
}

int ObSRDaaTIterImpl::get_next_rows_count_only(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (iter_param_->limit_param_->is_valid() && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  } else if (count_only_batch_capable_ && support_count_only_batch()) {
    ret = get_next_rows_count_only_batch(capacity, count);
  } else {
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      if (OB_FAIL(do_one_count_round(count))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        }
      }
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::get_next_rows_count_only_batch(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (!count_only_batch_started_) {
    if (OB_FAIL(batch_id_merger_.init(*dim_iters_, *iter_allocator_, iter_param_->max_batch_size_,
                                      merge_cmp_, merge_heap_, iter_domain_ids_))) {
      LOG_WARN("failed to initialize the count-only batch merger", K(ret));
    } else {
      count_only_batch_started_ = true;
    }
  }
  const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
  while (OB_SUCC(ret) && count < real_capacity) {
    const ObDatum *id_datum = nullptr;
    if (OB_FAIL(batch_id_merger_.get_next_distinct_id(id_datum))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("failed to get the next distinct document id", K(ret));
      }
    } else if (OB_ISNULL(id_datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(count_result(count))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("failed to count a matching document", K(ret));
      }
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::do_one_count_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  bool need_project = true;
  bool got_valid_id = false;
  double relevance = 0.0;
  const ObDatum *id_datum = nullptr;
  if (OB_FAIL(fill_merge_heap())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  } else if (OB_FAIL(collect_dims_by_id(id_datum, relevance, need_project))) {
  } else if (need_project && OB_FAIL(count_result(count))) {
  }
  return ret;
}

int ObSRDaaTIterImpl::count_result(int64_t &count)
{
  int ret = OB_SUCCESS;
  const common::ObLimitParam *limit_param = iter_param_->limit_param_;
  ++input_row_cnt_;
  if (limit_param->is_valid() && input_row_cnt_ <= limit_param->offset_) {
    // the row is only skipped, it is not part of the counted rows
  } else {
    ++count;
    ++output_row_cnt_;
    if (limit_param->is_valid() && output_row_cnt_ >= limit_param->limit_) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}


int ObSRDaaTIterImpl::pre_process()
{
  return OB_NOT_IMPLEMENT;
}

int ObSRDaaTIterImpl::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  bool need_project = true;
  double relevance = 0.0;
  const ObDatum *id_datum = nullptr;
  if (OB_FAIL(fill_merge_heap())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  } else if (OB_FAIL(collect_dims_by_id(id_datum, relevance, need_project))) {
  } else if (need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
  } else if (need_project && OB_FAIL(cache_result(count, *id_datum, relevance))) {
    if (OB_ITER_END != ret) {
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::fill_merge_heap()
{
  int ret = OB_SUCCESS;
  ObSRMergeItem item;
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDaaTDimIter *dim_iter = nullptr;
    if (OB_ISNULL(dim_iter = dim_iters_->at(iter_idx))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dim_iter->get_next_row())) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("fail to try load next batch dimension data", K(ret));\
      } else {
        ret = OB_SUCCESS;
      }
    } else if (!need_score() && FALSE_IT(item.relevance_ = 1.0)) {
      // The relevance of this query is not consumed, so the score of the dimension is never
      // evaluated. A constant positive value keeps the "the document is contained by this
      // dimension" semantic expected by the relevance collector.
    } else if (need_score() && OB_FAIL(dim_iter->get_curr_score(item.relevance_))) {
    } else if (OB_NOT_NULL(iter_param_->dim_weights_) && FALSE_IT(item.relevance_ = item.relevance_ * iter_param_->field_boost_ * iter_param_->dim_weights_->at(iter_idx))) {
    } else if (OB_FAIL(dim_iter->get_curr_id(iter_domain_ids_[iter_idx]))) {
    } else if (FALSE_IT(item.iter_idx_ = iter_idx)) {
    } else if (OB_FAIL(merge_heap_->push(item))) {
    } else {
      LOG_DEBUG("push item to merge heap", K(ret), K(i), K(iter_idx), K(next_round_cnt_), K(item), K(iter_domain_ids_[iter_idx]));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (merge_heap_->empty()) {
    ret = OB_ITER_END;
  } else if (0 != next_round_cnt_ && OB_FAIL(merge_heap_->rebuild())) {
  } else {
    next_round_cnt_ = 0;
  }
  return ret;
}

int ObSRDaaTIterImpl::collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id)
{
  int ret = OB_SUCCESS;

  const ObSRMergeItem *top_item = nullptr;
  bool curr_doc_end = false;
  int64_t iter_idx = 0;
  relevance = 0.0;
  got_valid_id = false;

  while (OB_SUCC(ret) && !curr_doc_end && !merge_heap_->empty()) {
    if (merge_heap_->is_unique_champion()) {
      curr_doc_end = true;
    }
    if (OB_FAIL(merge_heap_->top(top_item))) {
    } else if (OB_FAIL(relevance_collector_->collect_one_dim(top_item->iter_idx_, top_item->relevance_))) {
    } else if (FALSE_IT(iter_idx = top_item->iter_idx_)) {
    } else if (OB_FAIL(merge_heap_->pop())) {
    } else {
      next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
    }
  }

  if (OB_SUCC(ret)) {
    id_datum = iter_domain_ids_[iter_idx];
    if (OB_ISNULL(id_datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(relevance_collector_->get_result(relevance, got_valid_id))) {
    } else if (got_valid_id && OB_FAIL(process_collected_row(*id_datum, relevance))) {
    }
  }

  return ret;
}

int ObSRDaaTIterImpl::process_collected_row(const ObDatum &id_datum, const double relevance)
{
  return OB_SUCCESS;
}

int ObSRDaaTIterImpl::filter_on_demand(const int64_t count, const double relevance, bool &need_project)
{
  int ret = OB_SUCCESS;
  ObExpr *filter_expr = iter_param_->filter_expr_;
  ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
  ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);

  if (!iter_param_->need_filter()) {
    need_project = true;
  } else if (eval_ctx->is_vectorized()) {
    if (OB_UNLIKELY(count >= eval_ctx->max_batch_size_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      guard.set_batch_idx(count);
      ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
      relevance_proj_datum.set_double(relevance);
      relevance_proj_expr->get_evaluated_flags(*eval_ctx).set(count);
      relevance_proj_expr->set_evaluated_projected(*eval_ctx);
      ObDatum *filter_res = nullptr;
      filter_expr->clear_evaluated_flag(*eval_ctx);
      if (OB_FAIL(filter_expr->eval(*eval_ctx, filter_res))) {
      } else {
        need_project = !(filter_res->is_null() || 0 == filter_res->get_int());
      }
    }
  } else if (OB_UNLIKELY(0 != count)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    guard.set_batch_idx(0);
    ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
    relevance_proj_datum.set_double(relevance);
    relevance_proj_expr->set_evaluated_projected(*eval_ctx);
  }
  return ret;
}

int ObSRDaaTIterImpl::cache_result(int64_t &count, const ObDatum &id_datum, const double relevance)
{
  int ret = OB_SUCCESS;
  const common::ObLimitParam *limit_param= iter_param_->limit_param_;
  const int64_t limit = limit_param->limit_;
  const int64_t offset = limit_param->offset_;
  ++input_row_cnt_;
  if (limit_param->is_valid() && input_row_cnt_ <= offset) {
    // TODO: Maybe we should not process offset logic here
    // don't need to project
  } else if (OB_UNLIKELY(count >= buffered_domain_ids_.count() || count >= buffered_relevances_.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    if (OB_FAIL(buffered_domain_ids_[count].set_datum_fast(id_datum))) {
    } else if (need_score()) {
      buffered_relevances_[count] = relevance;
    }
    ++count;
    ++output_row_cnt_;
    if (limit_param->is_valid() && output_row_cnt_ >= limit) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::project_results(const int64_t count)
{
  int ret = OB_SUCCESS;
  ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
  ObExpr *id_proj_expr = iter_param_->id_proj_expr_;
  ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);

  if (eval_ctx->is_vectorized()) {
    sql::ObBitVector &id_evaluated_flags = id_proj_expr->get_evaluated_flags(*eval_ctx);
    for (int64_t i = 0; i < count; ++i) {
      guard.set_batch_idx(i);
      ObDatum &id_proj_datum = id_proj_expr->locate_datum_for_write(*eval_ctx);
      set_datum_func_(id_proj_datum, buffered_domain_ids_[i]);
      id_evaluated_flags.set(i);
      id_proj_expr->set_evaluated_projected(*eval_ctx);
    }
    if (iter_param_->need_project_relevance()) {
      sql::ObBitVector &relevance_evaluated_flags = relevance_proj_expr->get_evaluated_flags(*eval_ctx);
      for (int64_t i = 0; i < count; ++i) {
        guard.set_batch_idx(i);
        ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
        relevance_proj_datum.set_double(buffered_relevances_[i]);
        relevance_evaluated_flags.set(i);
        relevance_proj_expr->set_evaluated_projected(*eval_ctx);
      }
    }
  } else if (OB_UNLIKELY(1 != count)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    guard.set_batch_idx(0);
    ObDatum &id_proj_datum = id_proj_expr->locate_datum_for_write(*eval_ctx);
    set_datum_func_(id_proj_datum, buffered_domain_ids_[0]);
    id_proj_expr->set_evaluated_projected(*eval_ctx);
    if (iter_param_->need_project_relevance()) {
      ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
      relevance_proj_datum.set_double(buffered_relevances_[0]);
      relevance_proj_expr->set_evaluated_projected(*eval_ctx);
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::init_merge_heap(const int64_t count)
{
  int ret = OB_SUCCESS;
  ObSRSimpleMerger *simple_merge = nullptr;
  ObSRMergeHeap *loser_tree = nullptr;
  if (count <= ObSRSimpleMerger::USE_SIMPLE_MERGER_MAX_TABLE_CNT) {
    if (OB_ISNULL(simple_merge = OB_NEWx(ObSRSimpleMerger, iter_allocator_, merge_cmp_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      merge_heap_ = simple_merge;
    }
  } else if (OB_ISNULL(loser_tree = OB_NEWx(ObSRLoserTree, iter_allocator_, merge_cmp_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    merge_heap_ = loser_tree;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(merge_heap_->init(count, count, *iter_allocator_))) {
  } else if (OB_FAIL(merge_heap_->open(count))) {
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
