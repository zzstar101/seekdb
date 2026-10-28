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

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "lib/container/ob_se_array.h"
#include "storage/retrieval/ob_sparse_daat_iter.h"

namespace oceanbase
{
namespace storage
{
namespace
{

class CountingAllocator final : public ObIAllocator
{
public:
  explicit CountingAllocator(ObIAllocator &delegate)
    : delegate_(&delegate), alloc_calls_(0)
  {
  }

  virtual void *alloc(const int64_t size) override
  {
    ++alloc_calls_;
    return delegate_->alloc(size);
  }

  virtual void *alloc(const int64_t size, const common::ObMemAttr &attr) override
  {
    ++alloc_calls_;
    return delegate_->alloc(size, attr);
  }

  virtual void free(void *ptr) override { delegate_->free(ptr); }
  virtual int64_t total() const override { return delegate_->total(); }
  virtual int64_t used() const override { return delegate_->used(); }

  int64_t alloc_calls() const { return alloc_calls_; }

private:
  ObIAllocator *delegate_;
  int64_t alloc_calls_;
};

class FakeBatchDimIter final : public ObISRDaaTDimIter
{
public:
  FakeBatchDimIter(
      const std::vector<std::optional<uint64_t>> &ids,
      const int64_t batch_size)
    : values_(), ids_(), views_(), next_idx_(0), batch_size_(batch_size)
  {
    // ObDatum::set_int() writes through ptr_; a default-constructed ObDatum has a null data
    // pointer. Keep the integers in stable storage and make each datum point at its value.
    values_.reserve(ids.size());
    for (const std::optional<uint64_t> &id : ids) {
      values_.push_back(id.value_or(0));
    }
    ids_.reserve(ids.size());
    for (int64_t i = 0; i < static_cast<int64_t>(ids.size()); ++i) {
      ObDatum datum;
      if (ids[i].has_value()) {
        datum.ptr_ = reinterpret_cast<const char *>(&values_[i]);
        datum.pack_ = sizeof(int64_t);
      } else {
        datum.set_null();
      }
      ids_.push_back(datum);
    }
  }

  virtual int get_next_row() override { return OB_NOT_IMPLEMENT; }
  virtual int get_next_batch(const int64_t, int64_t &) override { return OB_NOT_IMPLEMENT; }
  virtual int get_curr_score(double &) const override { return OB_NOT_IMPLEMENT; }
  virtual int get_curr_id(const ObDatum *&) const override { return OB_NOT_IMPLEMENT; }
  virtual bool support_batch_doc_ids() const override { return true; }
  void reuse() { next_idx_ = 0; views_.clear(); }
  virtual int get_next_doc_id_batch(
      const int64_t capacity,
      const ObDatum **&doc_ids,
      int64_t &count) override
  {
    int ret = OB_SUCCESS;
    doc_ids = nullptr;
    count = 0;
    const int64_t remaining = static_cast<int64_t>(ids_.size()) - next_idx_;
    if (remaining <= 0) {
      ret = OB_ITER_END;
    } else {
      count = MIN(capacity, MIN(batch_size_, remaining));
      views_.clear();
      views_.reserve(count);
      for (int64_t i = 0; i < count; ++i) {
        views_.push_back(&ids_[next_idx_ + i]);
      }
      next_idx_ += count;
      doc_ids = views_.data();
    }
    return ret;
  }

private:
  std::vector<int64_t> values_;
  std::vector<ObDatum> ids_;
  std::vector<const ObDatum *> views_;
  int64_t next_idx_;
  int64_t batch_size_;
};

using IdList = std::vector<std::optional<uint64_t>>;
using ExpectedIds = std::vector<std::optional<uint64_t>>;

class CountOnlyBatchMergerTest : public ::testing::Test
{
protected:
  void collect(ObSRDaaTBatchIdMerger &merger, ExpectedIds &ids, int64_t limit = INT64_MAX)
  {
    const ObDatum *id_datum = nullptr;
    while (static_cast<int64_t>(ids.size()) < limit) {
      const int ret = merger.get_next_distinct_id(id_datum);
      if (OB_ITER_END == ret) {
        return;
      }
      ASSERT_EQ(OB_SUCCESS, ret);
      ASSERT_NE(nullptr, id_datum);
      ids.push_back(id_datum->is_null()
          ? std::nullopt
          : std::optional<uint64_t>(id_datum->get_uint64()));
    }
  }

  void run_merge(
      const std::vector<IdList> &inputs,
      const int64_t batch_size,
      ExpectedIds &actual,
      const int64_t first_limit = INT64_MAX)
  {
    common::ObArenaAllocator allocator;
    common::ObSEArray<ObISRDaaTDimIter *, 4> dim_iters;
    common::ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
    std::vector<std::unique_ptr<FakeBatchDimIter>> owned_iters;
    owned_iters.reserve(inputs.size());
    for (const IdList &ids : inputs) {
      owned_iters.emplace_back(new FakeBatchDimIter(ids, batch_size));
      ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(owned_iters.back().get()));
    }

    current_ids.set_allocator(&allocator);
    ASSERT_EQ(OB_SUCCESS, current_ids.init(dim_iters.count()));
    ASSERT_EQ(OB_SUCCESS, current_ids.prepare_allocate(dim_iters.count()));
    ObSRMergeCmp cmp;
    sql::ObDatumMeta id_meta;
    id_meta.type_ = ObUInt64Type;
    id_meta.cs_type_ = CS_TYPE_BINARY;
    ASSERT_EQ(OB_SUCCESS, cmp.init(id_meta, &current_ids));

    ObSRSimpleMerger heap(cmp);
    ObSRLoserTree loser_tree(cmp);
    ObSRMergeHeap *heap_ptr = nullptr;
    if (dim_iters.count() > 3) {
      ASSERT_EQ(OB_SUCCESS, loser_tree.init(dim_iters.count(), dim_iters.count(), allocator));
      ASSERT_EQ(OB_SUCCESS, loser_tree.open(dim_iters.count()));
      heap_ptr = &loser_tree;
    } else if (dim_iters.count() > 2) {
      ASSERT_EQ(OB_SUCCESS, heap.init(dim_iters.count(), dim_iters.count(), allocator));
      ASSERT_EQ(OB_SUCCESS, heap.open(dim_iters.count()));
      heap_ptr = &heap;
    }

    ObSRDaaTBatchIdMerger merger;
    ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, batch_size, cmp, heap_ptr, current_ids));
    if (INT64_MAX != first_limit) {
      collect(merger, actual, first_limit);
    }
    collect(merger, actual);
  }
};

TEST_F(CountOnlyBatchMergerTest, one_way_crosses_batch_boundaries)
{
  ExpectedIds actual;
  run_merge({{1, 2, 3, 4, 5}}, 2, actual);
  EXPECT_EQ(ExpectedIds({1, 2, 3, 4, 5}), actual);
}

TEST_F(CountOnlyBatchMergerTest, final_single_id_is_emitted_before_eof)
{
  ExpectedIds actual;
  run_merge({{1, 2, 3}}, 2, actual);
  EXPECT_EQ(ExpectedIds({1, 2, 3}), actual);
}

TEST_F(CountOnlyBatchMergerTest, two_way_counts_union_and_deduplicates)
{
  ExpectedIds actual;
  run_merge({{1, 2, 4, 7, 8}, {2, 3, 4, 8, 9, 10}}, 2, actual);
  EXPECT_EQ(ExpectedIds({1, 2, 3, 4, 7, 8, 9, 10}), actual);
}

TEST_F(CountOnlyBatchMergerTest, two_way_equal_last_id_is_emitted_once)
{
  ExpectedIds actual;
  run_merge({{1, 2, 4}, {3, 4}}, 2, actual);
  EXPECT_EQ(ExpectedIds({1, 2, 3, 4}), actual);
}

TEST_F(CountOnlyBatchMergerTest, n_way_resumes_after_partial_batch)
{
  ExpectedIds actual;
  run_merge(
      {{1, 4, 5, 9}, {2, 4, 6, 9, 10}, {3, 5, 7, 9, 11}},
      2,
      actual,
      4);
  EXPECT_EQ(ExpectedIds({1, 2, 3, 4, 5, 6, 7, 9, 10, 11}), actual);
}

TEST_F(CountOnlyBatchMergerTest, loser_tree_four_way_handles_empty_and_partial_exhaustion)
{
  ExpectedIds actual;
  run_merge(
      {{1, 5, 9, 20}, {}, {2, 5, 10, 12, 20}, {3, 5, 11, 13, 20}},
      2,
      actual,
      4);
  EXPECT_EQ(ExpectedIds({1, 2, 3, 5, 9, 10, 11, 12, 13, 20}), actual);
}

TEST_F(CountOnlyBatchMergerTest, repeated_reuse_keeps_batch_allocations_stable)
{
  common::ObArenaAllocator arena;
  CountingAllocator counting(arena);
  common::ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
  current_ids.set_allocator(&counting);
  ASSERT_EQ(OB_SUCCESS, current_ids.init(3));
  ASSERT_EQ(OB_SUCCESS, current_ids.prepare_allocate(3));

  std::unique_ptr<FakeBatchDimIter> first(new FakeBatchDimIter({1, 2, 4, 7, 8}, 2));
  std::unique_ptr<FakeBatchDimIter> second(new FakeBatchDimIter({2, 3, 4, 8, 9, 10}, 2));
  common::ObSEArray<ObISRDaaTDimIter *, 3> dim_iters;
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(first.get()));
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(second.get()));

  ObSRMergeCmp cmp;
  sql::ObDatumMeta id_meta;
  id_meta.type_ = ObUInt64Type;
  id_meta.cs_type_ = CS_TYPE_BINARY;
  ASSERT_EQ(OB_SUCCESS, cmp.init(id_meta, &current_ids));
  ObSRSimpleMerger heap(cmp);
  ASSERT_EQ(OB_SUCCESS, heap.init(3, 3, counting));
  ASSERT_EQ(OB_SUCCESS, heap.open(2));

  ObSRDaaTBatchIdMerger merger;
  ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, counting, 2, cmp, &heap, current_ids));
  ExpectedIds baseline_result;
  collect(merger, baseline_result);
  const int64_t baseline_alloc_calls = counting.alloc_calls();
  EXPECT_EQ(ExpectedIds({1, 2, 3, 4, 7, 8, 9, 10}), baseline_result);

  for (int64_t round = 0; round < 32; ++round) {
    first->reuse();
    second->reuse();
    heap.reuse();
    ASSERT_EQ(OB_SUCCESS, heap.open(2));
    merger.reuse();
    ASSERT_EQ(OB_SUCCESS, merger.init(
        dim_iters, counting, 1 + (round % 4), cmp, &heap, current_ids));
    ExpectedIds actual;
    collect(merger, actual);
    EXPECT_EQ(ExpectedIds({1, 2, 3, 4, 7, 8, 9, 10}), actual);
    EXPECT_EQ(baseline_alloc_calls, counting.alloc_calls());
  }

  first->reuse();
  common::ObSEArray<ObISRDaaTDimIter *, 1> one_dim;
  ASSERT_EQ(OB_SUCCESS, one_dim.push_back(first.get()));
  heap.reuse();
  ASSERT_EQ(OB_SUCCESS, heap.open(1));
  merger.reuse();
  ASSERT_EQ(OB_SUCCESS, merger.init(one_dim, counting, 3, cmp, &heap, current_ids));
  ExpectedIds one_dim_result;
  collect(merger, one_dim_result);
  EXPECT_EQ(ExpectedIds({1, 2, 4, 7, 8}), one_dim_result);
  EXPECT_EQ(baseline_alloc_calls, counting.alloc_calls());

  std::unique_ptr<FakeBatchDimIter> third(new FakeBatchDimIter({5, 6}, 2));
  common::ObSEArray<ObISRDaaTDimIter *, 3> three_dims;
  ASSERT_EQ(OB_SUCCESS, three_dims.push_back(first.get()));
  ASSERT_EQ(OB_SUCCESS, three_dims.push_back(second.get()));
  ASSERT_EQ(OB_SUCCESS, three_dims.push_back(third.get()));
  heap.reuse();
  ASSERT_EQ(OB_SUCCESS, heap.open(3));
  merger.reuse();
  EXPECT_EQ(OB_NOT_SUPPORTED, merger.init(three_dims, counting, 2, cmp, &heap, current_ids));
  EXPECT_EQ(baseline_alloc_calls, counting.alloc_calls());
}

TEST_F(CountOnlyBatchMergerTest, null_document_id_is_emitted_once)
{
  ExpectedIds actual;
  run_merge({{std::nullopt, 1, 3}, {std::nullopt, 2, 3}}, 2, actual);
  EXPECT_EQ(ExpectedIds({std::nullopt, 1, 2, 3}), actual);
}

TEST_F(CountOnlyBatchMergerTest, union_capability_requires_at_most_one_match)
{
  ObSRDaaTInnerProductRelevanceCollector collector;
  ASSERT_EQ(OB_SUCCESS, collector.init(0));
  EXPECT_TRUE(collector.support_count_only_union());
  ASSERT_EQ(OB_SUCCESS, collector.init(1));
  EXPECT_TRUE(collector.support_count_only_union());
  ASSERT_EQ(OB_SUCCESS, collector.init(2));
  EXPECT_FALSE(collector.support_count_only_union());
}

} // namespace
} // namespace storage
} // namespace oceanbase
