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

#ifndef OB_SPARSE_DAAT_ITER_H_
#define OB_SPARSE_DAAT_ITER_H_

#include "ob_i_sparse_retrieval_iter.h"
#include "ob_sparse_utils.h"
#include "storage/access/ob_scan_merge_loser_tree.h"
#include "storage/access/ob_simple_rows_merger.h"

namespace oceanbase
{
namespace storage
{

struct ObSRMergeItem
{
  ObSRMergeItem() : relevance_(0.0), iter_idx_(-1), equal_with_next_(false) {}
  ~ObSRMergeItem() = default;
  TO_STRING_KV(K_(iter_idx), K_(relevance), K_(equal_with_next));

  double relevance_;
  int64_t iter_idx_;
  bool equal_with_next_;
};

struct ObSRMergeCmp
{
  // The document id of the inverted index is compared with the generic datum comparator by
  // default. When the id is an unsigned integer or a pure binary string, the comparison can be
  // done inline instead, which is what the merge loop pays for every output row.
  enum CmpMode
  {
    CMP_MODE_GENERIC = 0,
    CMP_MODE_UINT64 = 1,
    CMP_MODE_BINARY = 2,
  };

  ObSRMergeCmp();
  virtual ~ObSRMergeCmp() {}

  int init(sql::ObDatumMeta id_meta, const ObFixedArray<const ObDatum *, ObIAllocator> *iter_ids);
  int cmp(const ObSRMergeItem &l, const ObSRMergeItem &r, int64_t &cmp_ret);
  int cmp_id(const ObDatum &l, const ObDatum &r, int64_t &cmp_ret);
private:
  inline const ObDatum &get_id_datum(const int64_t iter_idx)
  {
    const ObDatum *datum = iter_ids_->at(iter_idx);
    OB_ASSERT(nullptr != datum);
    return *datum;
  }
private:
  common::ObDatumCmpFuncType cmp_func_;
  CmpMode cmp_mode_;
  // TODO: if memory lifetime of docid datum is guaranteed by dim_iters, we can use pointer to datum directly
  //       and avoid deep copy into merge heap here
  const ObFixedArray<const ObDatum *, ObIAllocator> *iter_ids_;
  bool is_inited_;
};

typedef ObSimpleRowsMerger<ObSRMergeItem, ObSRMergeCmp> ObSRSimpleMerger;
typedef ObMergeLoserTree<ObSRMergeItem, ObSRMergeCmp> ObSRLoserTree;
typedef common::ObRowsMerger<ObSRMergeItem, ObSRMergeCmp> ObSRMergeHeap;

// Merges sorted document-id batches from several dimensions and returns each distinct id once.
// It is used by the count-only union path, so it deliberately avoids the per-row dimension
// virtual calls, relevance collector, and materialized output of the generic merge loop.
class ObSRDaaTBatchIdMerger
{
public:
  ObSRDaaTBatchIdMerger();
  ~ObSRDaaTBatchIdMerger();
  int init(
      ObIArray<ObISRDaaTDimIter *> &dim_iters,
      ObIAllocator &allocator,
      const int64_t batch_capacity,
      ObSRMergeCmp &merge_cmp,
      ObSRMergeHeap *merge_heap,
      ObFixedArray<const ObDatum *, ObIAllocator> &current_ids);
  // Clear logical execution state while retaining the fixed-array storage for the next scan.
  void reuse();
  void reset();
  // The returned pointer refers to storage owned by this merger and stays valid until the next
  // call to get_next_distinct_id().
  int get_next_distinct_id(const ObDatum *&id_datum);
  // Counts up to capacity distinct ids in one call. This entry point is only used for the
  // one-, two-, and three-way merges; wider merges keep using get_next_distinct_id(). A scan
  // selects one of the two entry points. The count is meaningful only on OB_SUCCESS and
  // OB_ITER_END; the latter may carry a non-zero count for the final partial batch.
  int get_next_distinct_count(const int64_t capacity, int64_t &count);
private:
  int load_batch(const int64_t iter_idx);
  int advance(const int64_t iter_idx);
  int get_next_from_one(const ObDatum *&id_datum);
  int get_next_from_two(const ObDatum *&id_datum);
  int get_next_from_three(const ObDatum *&id_datum);
  int get_next_from_heap(const ObDatum *&id_datum);
  int preserve_bulk_last_id();
private:
  ObIArray<ObISRDaaTDimIter *> *dim_iters_;
  ObSRMergeCmp *merge_cmp_;
  ObSRMergeHeap *merge_heap_;
  ObFixedArray<const ObDatum *, ObIAllocator> *current_ids_;
  ObFixedArray<const ObDatum **, ObIAllocator> batch_ids_;
  ObFixedArray<int64_t, ObIAllocator> batch_counts_;
  ObFixedArray<int64_t, ObIAllocator> batch_positions_;
  ObDocIdExt last_id_;
  ObIAllocator *allocator_;
  const ObDatum *bulk_last_id_;
  ObDatum bulk_last_id_copy_;
  char *bulk_last_id_buf_;
  int64_t bulk_last_id_buf_capacity_;
  int64_t bulk_last_id_source_dim_;
  bool has_bulk_last_id_;
  int64_t dim_cnt_;
  int64_t active_cnt_;
  int64_t batch_capacity_;
  bool has_last_id_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObSRDaaTBatchIdMerger);
};

// implementation of basic DaaT query processing algorithm primitives
class ObSRDaaTIterImpl : public ObISparseRetrievalMergeIter
{
public:
  ObSRDaaTIterImpl();
  virtual ~ObSRDaaTIterImpl() {}
  virtual int get_next_row() override;
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  int init(
      ObSparseRetrievalMergeParam &iter_param,
      ObIArray<ObISRDaaTDimIter *> &dim_iters,
      ObIAllocator &iter_allocator,
      ObSRDaaTRelevanceCollector &relevance_collector);
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  virtual int get_query_max_score(double &score) override;
  
  INHERIT_TO_STRING_KV("ObISparseRetrievalMergeIter", ObISparseRetrievalMergeIter,
      K_(next_round_iter_idxes), K_(next_round_cnt));
protected:
  virtual int pre_process();
  virtual int do_one_merge_round(int64_t &count);
  virtual int fill_merge_heap();
  virtual int collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id);
  virtual int process_collected_row(const ObDatum &id_datum, const double relevance);
  virtual int filter_on_demand(const int64_t count, const double relevance, bool &need_project);
  virtual int cache_result(int64_t &count, const ObDatum &id_datum, const double relevance);
  virtual int project_results(const int64_t count);
  // Counting-only merge: the merged rows are not consumed, only their number is. The merge loop
  // then counts the matching documents instead of materializing them into the output buffer, so
  // neither the document id copy nor the projection of the output row is done.
  int get_next_rows_count_only(const int64_t capacity, int64_t &count);
  virtual int do_one_count_round(int64_t &count);
  int get_next_rows_count_only_batch(const int64_t capacity, int64_t &count);
  int count_result(int64_t &count);
  // Whether the concrete retrieval mode and its merge predicate permit replacing the generic
  // count loop with a deduplicated union of document-id batches.
  virtual bool support_count_only_batch() const { return false; }
  // Merge the dimension iterators with a direct comparison of their current document ids
  // instead of the generic merge heap. It can only be used when the per-dimension relevance is
  // not consumed by the query, so the merge only has to produce every distinct document id in
  // ascending order together with the set of dimensions that contain it.
  int init_merge_heap(const int64_t count);
  // Whether the per-dimension relevance is really consumed by this query (projected, used by the
  // score filter, or used by the top-k pushdown). If not, the dimension score does not have to be
  // computed: a constant positive value keeps the "the document appears in this dimension"
  // semantic of the relevance collector while avoiding the score evaluation.
  inline bool need_score() const { return need_score_; }
  // Whether only the number of merged rows is consumed by the query.
  inline bool count_only() const { return count_only_; }
protected:
  ObIAllocator *iter_allocator_;
  ObSparseRetrievalMergeParam *iter_param_;
  ObIArray<ObISRDaaTDimIter *> *dim_iters_;
  ObSRMergeCmp merge_cmp_;
  ObSRMergeHeap *merge_heap_;
  ObSRDaaTRelevanceCollector *relevance_collector_;
  ObFixedArray<const ObDatum *, ObIAllocator> iter_domain_ids_; // record every dim iter's output domain id, one (ObDatum *) for one dim iter
  ObFixedArray<ObDocIdExt, ObIAllocator> buffered_domain_ids_; // cache for output
  ObFixedArray<double, ObIAllocator> buffered_relevances_;
  ObFixedArray<int64_t, ObIAllocator> next_round_iter_idxes_;
  ObSRDaaTBatchIdMerger batch_id_merger_;
  int64_t next_round_cnt_;
  void (*set_datum_func_)(ObDatum &, const ObDocIdExt &);
  bool need_score_;
  bool count_only_;
  bool count_only_batch_capable_;
  bool count_only_batch_started_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObSRDaaTIterImpl);
};

} // namespace storage
} // namespace oceanbase

#endif
