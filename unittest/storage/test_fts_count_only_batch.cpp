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
#include <string>
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
      const int64_t batch_size,
      const int64_t fail_on_call = 0)
    : values_(), ids_(), views_(), next_idx_(0), batch_size_(batch_size),
      batch_calls_(0), fail_on_call_(fail_on_call)
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
  void reuse()
  {
    next_idx_ = 0;
    views_.clear();
    batch_calls_ = 0;
  }
  virtual int get_next_doc_id_batch(
      const int64_t capacity,
      const ObDatum **&doc_ids,
      int64_t &count) override
  {
    int ret = OB_SUCCESS;
    doc_ids = nullptr;
    count = 0;
    ++batch_calls_;
    if (fail_on_call_ == batch_calls_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
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
    }
    return ret;
  }

private:
  std::vector<int64_t> values_;
  std::vector<ObDatum> ids_;
  std::vector<const ObDatum *> views_;
  int64_t next_idx_;
  int64_t batch_size_;
  int64_t batch_calls_;
  int64_t fail_on_call_;
};

class FakeStringBatchDimIter final : public ObISRDaaTDimIter
{
public:
  FakeStringBatchDimIter(
      const std::vector<std::optional<std::string>> &ids,
      const int64_t batch_size,
      const int64_t fail_on_call = 0)
    : values_(), ids_(), views_(), next_idx_(0), batch_size_(batch_size),
      batch_calls_(0), fail_on_call_(fail_on_call)
  {
    values_.reserve(ids.size());
    for (const std::optional<std::string> &id : ids) {
      values_.push_back(id.value_or(std::string()));
    }
    ids_.reserve(ids.size());
    for (int64_t i = 0; i < static_cast<int64_t>(ids.size()); ++i) {
      ObDatum datum;
      if (ids[i].has_value()) {
        datum.set_string(values_[i].data(), static_cast<uint32_t>(values_[i].size()));
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
  void reuse()
  {
    next_idx_ = 0;
    views_.clear();
    batch_calls_ = 0;
  }
  virtual int get_next_doc_id_batch(
      const int64_t capacity,
      const ObDatum **&doc_ids,
      int64_t &count) override
  {
    int ret = OB_SUCCESS;
    doc_ids = nullptr;
    count = 0;
    ++batch_calls_;
    if (fail_on_call_ == batch_calls_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
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
    }
    return ret;
  }

private:
  std::vector<std::string> values_;
  std::vector<ObDatum> ids_;
  std::vector<const ObDatum *> views_;
  int64_t next_idx_;
  int64_t batch_size_;
  int64_t batch_calls_;
  int64_t fail_on_call_;
};

using IdList = std::vector<std::optional<uint64_t>>;
using ExpectedIds = std::vector<std::optional<uint64_t>>;
using StringIdList = std::vector<std::optional<std::string>>;

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

    ObSRLoserTree loser_tree(cmp);
    ObSRMergeHeap *heap_ptr = nullptr;
    if (dim_iters.count() > 3) {
      ASSERT_EQ(OB_SUCCESS, loser_tree.init(dim_iters.count(), dim_iters.count(), allocator));
      ASSERT_EQ(OB_SUCCESS, loser_tree.open(dim_iters.count()));
      heap_ptr = &loser_tree;
    }

    ObSRDaaTBatchIdMerger merger;
    ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, batch_size, cmp, heap_ptr, current_ids));
    if (INT64_MAX != first_limit) {
      collect(merger, actual, first_limit);
    }
    collect(merger, actual);
    const ObDatum *id_datum = nullptr;
    EXPECT_EQ(OB_ITER_END, merger.get_next_distinct_id(id_datum));
    EXPECT_EQ(nullptr, id_datum);
  }

  void run_bulk_count(
      const std::vector<IdList> &inputs,
      const int64_t batch_size,
      const int64_t capacity,
      std::vector<int64_t> &chunks,
      int64_t &total)
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

    ObSRLoserTree loser_tree(cmp);
    ObSRMergeHeap *heap_ptr = nullptr;
    if (dim_iters.count() > 3) {
      ASSERT_EQ(OB_SUCCESS, loser_tree.init(dim_iters.count(), dim_iters.count(), allocator));
      ASSERT_EQ(OB_SUCCESS, loser_tree.open(dim_iters.count()));
      heap_ptr = &loser_tree;
    }

    ObSRDaaTBatchIdMerger merger;
    ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, batch_size, cmp, heap_ptr, current_ids));
    chunks.clear();
    total = 0;
    bool ended = false;
    for (int64_t call = 0; !ended && call < 1000000; ++call) {
      int64_t batch_count = -1;
      const int ret = merger.get_next_distinct_count(capacity, batch_count);
      if (OB_ITER_END == ret) {
        ASSERT_GE(batch_count, 0);
        ASSERT_LE(batch_count, capacity);
        chunks.push_back(batch_count);
        total += batch_count;
        ended = true;
      } else {
        ASSERT_EQ(OB_SUCCESS, ret);
        ASSERT_EQ(capacity, batch_count);
        chunks.push_back(batch_count);
        total += batch_count;
      }
    }
    ASSERT_TRUE(ended);
    int64_t terminal_count = -1;
    EXPECT_EQ(OB_ITER_END, merger.get_next_distinct_count(capacity, terminal_count));
    EXPECT_EQ(0, terminal_count);
  }

  void run_string_bulk_count(
      const std::vector<StringIdList> &inputs,
      const int64_t batch_size,
      const int64_t capacity,
      const common::ObCollationType cs_type,
      std::vector<int64_t> &chunks,
      int64_t &total)
  {
    common::ObArenaAllocator allocator;
    common::ObSEArray<ObISRDaaTDimIter *, 4> dim_iters;
    common::ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
    std::vector<std::unique_ptr<FakeStringBatchDimIter>> owned_iters;
    owned_iters.reserve(inputs.size());
    for (const StringIdList &ids : inputs) {
      owned_iters.emplace_back(new FakeStringBatchDimIter(ids, batch_size));
      ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(owned_iters.back().get()));
    }

    current_ids.set_allocator(&allocator);
    ASSERT_EQ(OB_SUCCESS, current_ids.init(dim_iters.count()));
    ASSERT_EQ(OB_SUCCESS, current_ids.prepare_allocate(dim_iters.count()));
    ObSRMergeCmp cmp;
    sql::ObDatumMeta id_meta;
    id_meta.type_ = ObVarcharType;
    id_meta.cs_type_ = cs_type;
    ASSERT_EQ(OB_SUCCESS, cmp.init(id_meta, &current_ids));

    ObSRDaaTBatchIdMerger merger;
    ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, batch_size, cmp, nullptr, current_ids));
    chunks.clear();
    total = 0;
    bool ended = false;
    for (int64_t call = 0; !ended && call < 1000000; ++call) {
      int64_t batch_count = -1;
      const int ret = merger.get_next_distinct_count(capacity, batch_count);
      if (OB_ITER_END == ret) {
        ASSERT_GE(batch_count, 0);
        ASSERT_LE(batch_count, capacity);
        chunks.push_back(batch_count);
        total += batch_count;
        ended = true;
      } else {
        ASSERT_EQ(OB_SUCCESS, ret);
        ASSERT_EQ(capacity, batch_count);
        chunks.push_back(batch_count);
        total += batch_count;
      }
    }
    ASSERT_TRUE(ended);
    int64_t terminal_count = -1;
    EXPECT_EQ(OB_ITER_END, merger.get_next_distinct_count(capacity, terminal_count));
    EXPECT_EQ(0, terminal_count);
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

TEST_F(CountOnlyBatchMergerTest, three_way_deduplicates_nulls_and_repeated_batch_ids)
{
  for (const int64_t batch_size : {1, 2, 3}) {
    SCOPED_TRACE(batch_size);
    ExpectedIds actual;
    run_merge(
        {{std::nullopt, std::nullopt, 1, 1, 3, 6, 6, 9},
         {std::nullopt, 2, 3, 3, 6, 9, 9},
         {std::nullopt, std::nullopt, 1, 4, 6, 6, 9}},
        batch_size,
        actual,
        3);
    EXPECT_EQ(ExpectedIds({std::nullopt, 1, 2, 3, 4, 6, 9}), actual);
  }
}

TEST_F(CountOnlyBatchMergerTest, three_way_equal_final_id_is_emitted_before_all_end)
{
  ExpectedIds actual;
  const std::optional<uint64_t> final_id(42);
  run_merge({{final_id}, {final_id}, {final_id}}, 1, actual, 1);
  EXPECT_EQ(ExpectedIds{final_id}, actual);
}

TEST_F(CountOnlyBatchMergerTest, three_way_handles_empty_and_partial_exhaustion)
{
  // Exercise an empty first, middle, or last dimension and a survivor after its peer ends.
  for (int64_t empty_idx = 0; empty_idx < 3; ++empty_idx) {
    SCOPED_TRACE(empty_idx);
    std::vector<IdList> inputs = {{}, {1, 4}, {2, 3, 4, 5}};
    std::swap(inputs[0], inputs[empty_idx]);
    ExpectedIds actual;
    run_merge(inputs, 2, actual, 2);
    EXPECT_EQ(ExpectedIds({1, 2, 3, 4, 5}), actual);
  }
}

TEST_F(CountOnlyBatchMergerTest, three_way_handles_all_empty_or_one_remaining_dimension)
{
  ExpectedIds empty_result;
  run_merge({{}, {}, {}}, 2, empty_result);
  EXPECT_TRUE(empty_result.empty());
  for (int64_t active_idx = 0; active_idx < 3; ++active_idx) {
    SCOPED_TRACE(active_idx);
    std::vector<IdList> inputs(3);
    inputs[active_idx] = {std::nullopt, std::nullopt, 1, 1, 4, 4};
    ExpectedIds actual;
    run_merge(inputs, 2, actual, 1);
    EXPECT_EQ(ExpectedIds({std::nullopt, 1, 4}), actual);
  }
}

TEST_F(CountOnlyBatchMergerTest, bulk_count_matches_single_id_api_at_capacity_boundaries)
{
  const std::vector<IdList> inputs = {
      {std::nullopt, std::nullopt, 1, 1, 2, 4, 4, 7},
      {std::nullopt, 2, 2, 3, 7, 7, 9},
      {1, 3, 3, 4, 7, 9, 9}};
  for (const int64_t batch_size : {1, 2, 5}) {
    SCOPED_TRACE(batch_size);
    ExpectedIds legacy_result;
    run_merge(inputs, batch_size, legacy_result);
    for (const int64_t capacity : {1, 2, 32}) {
      SCOPED_TRACE(capacity);
      std::vector<int64_t> chunks;
      int64_t bulk_count = 0;
      run_bulk_count(inputs, batch_size, capacity, chunks, bulk_count);
      EXPECT_EQ(static_cast<int64_t>(legacy_result.size()), bulk_count);
    }
  }
}

TEST_F(CountOnlyBatchMergerTest, bulk_count_exact_capacity_and_final_partial_batch)
{
  std::vector<int64_t> exact_chunks;
  int64_t exact_count = 0;
  run_bulk_count({{1, 2, 3}}, 1, 3, exact_chunks, exact_count);
  EXPECT_EQ(3, exact_count);
  EXPECT_EQ(std::vector<int64_t>({3, 0}), exact_chunks);

  std::vector<int64_t> partial_chunks;
  int64_t partial_count = 0;
  run_bulk_count({{1, 2, 3}}, 1, 2, partial_chunks, partial_count);
  EXPECT_EQ(3, partial_count);
  EXPECT_EQ(std::vector<int64_t>({2, 1}), partial_chunks);
}

TEST_F(CountOnlyBatchMergerTest, bulk_count_three_way_large_overlap_matches_single_id_api)
{
  std::vector<IdList> inputs(3);
  for (uint64_t i = 0; i < 360; ++i) {
    inputs[0].push_back(i / 2);
    inputs[1].push_back(i / 3);
    inputs[2].push_back(i / 5);
  }
  for (const int64_t batch_size : {1, 3, 17, 64}) {
    SCOPED_TRACE(batch_size);
    ExpectedIds legacy_result;
    run_merge(inputs, batch_size, legacy_result);
    for (const int64_t capacity : {1, 2, 29}) {
      SCOPED_TRACE(capacity);
      std::vector<int64_t> chunks;
      int64_t bulk_count = 0;
      run_bulk_count(inputs, batch_size, capacity, chunks, bulk_count);
      EXPECT_EQ(static_cast<int64_t>(legacy_result.size()), bulk_count);
    }
  }
}

TEST_F(CountOnlyBatchMergerTest, bulk_count_binary_long_strings_cross_batch_boundaries)
{
  const std::string nul_b = std::string("b-pre\0fix", 9);
  const std::string long_b = std::string("b-") + std::string(128, 'x');
  const std::string long_c = std::string("c-") + std::string(160, 'y');
  const std::vector<StringIdList> inputs = {
      {std::nullopt, std::string("a-short"), std::string("a-short"),
       nul_b, long_b, long_b, std::string("z")},
      {std::nullopt, std::string("a-short"), nul_b, long_b, long_c, std::string("z")},
      {std::string("a-short"), nul_b, long_b, long_b, long_c, long_c, std::string("z")}};
  for (const int64_t batch_size : {1, 3, 9}) {
    SCOPED_TRACE(batch_size);
    for (const int64_t capacity : {1, 2, 16}) {
      SCOPED_TRACE(capacity);
      std::vector<int64_t> chunks;
      int64_t bulk_count = 0;
      run_string_bulk_count(inputs, batch_size, capacity, common::CS_TYPE_BINARY, chunks, bulk_count);
      EXPECT_EQ(6, bulk_count);
    }
  }
}

TEST_F(CountOnlyBatchMergerTest, bulk_count_generic_collation_deduplicates_equivalents)
{
  const std::vector<StringIdList> inputs = {
      {std::nullopt, std::string("Alpha"), std::string("ALPHA"), std::string("beta")},
      {std::string("alpha"), std::string("BETA"), std::string("gamma")},
      {std::nullopt, std::string("aLpHa"), std::string("Beta"), std::string("gamma")}};
  std::vector<int64_t> chunks;
  int64_t bulk_count = 0;
  run_string_bulk_count(
      inputs, 1, 2, common::CS_TYPE_UTF8MB4_GENERAL_CI, chunks, bulk_count);
  EXPECT_EQ(4, bulk_count);
}

TEST_F(CountOnlyBatchMergerTest, bulk_count_propagates_refill_error)
{
  common::ObArenaAllocator allocator;
  common::ObSEArray<ObISRDaaTDimIter *, 3> dim_iters;
  common::ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
  std::unique_ptr<FakeBatchDimIter> first(new FakeBatchDimIter({1, 2, 5, 6}, 2, 2));
  std::unique_ptr<FakeBatchDimIter> second(new FakeBatchDimIter({3, 4}, 2));
  std::unique_ptr<FakeBatchDimIter> third(new FakeBatchDimIter({7, 8}, 2));
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(first.get()));
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(second.get()));
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(third.get()));
  current_ids.set_allocator(&allocator);
  ASSERT_EQ(OB_SUCCESS, current_ids.init(3));
  ASSERT_EQ(OB_SUCCESS, current_ids.prepare_allocate(3));
  ObSRMergeCmp cmp;
  sql::ObDatumMeta id_meta;
  id_meta.type_ = ObUInt64Type;
  id_meta.cs_type_ = CS_TYPE_BINARY;
  ASSERT_EQ(OB_SUCCESS, cmp.init(id_meta, &current_ids));
  ObSRDaaTBatchIdMerger merger;
  ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, 2, cmp, nullptr, current_ids));

  int64_t count = -1;
  EXPECT_EQ(OB_ERR_UNEXPECTED, merger.get_next_distinct_count(100, count));
  EXPECT_EQ(0, count);
}

TEST_F(CountOnlyBatchMergerTest, bulk_count_leaves_four_way_to_single_id_path)
{
  common::ObArenaAllocator allocator;
  common::ObSEArray<ObISRDaaTDimIter *, 4> dim_iters;
  common::ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
  std::vector<std::unique_ptr<FakeBatchDimIter>> owned_iters;
  owned_iters.emplace_back(new FakeBatchDimIter({1, 5}, 1));
  owned_iters.emplace_back(new FakeBatchDimIter({2, 5}, 1));
  owned_iters.emplace_back(new FakeBatchDimIter({3, 5}, 1));
  owned_iters.emplace_back(new FakeBatchDimIter({4, 5}, 1));
  for (const std::unique_ptr<FakeBatchDimIter> &iter : owned_iters) {
    ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(iter.get()));
  }
  current_ids.set_allocator(&allocator);
  ASSERT_EQ(OB_SUCCESS, current_ids.init(4));
  ASSERT_EQ(OB_SUCCESS, current_ids.prepare_allocate(4));
  ObSRMergeCmp cmp;
  sql::ObDatumMeta id_meta;
  id_meta.type_ = ObUInt64Type;
  id_meta.cs_type_ = CS_TYPE_BINARY;
  ASSERT_EQ(OB_SUCCESS, cmp.init(id_meta, &current_ids));
  ObSRLoserTree loser_tree(cmp);
  ASSERT_EQ(OB_SUCCESS, loser_tree.init(4, 4, allocator));
  ASSERT_EQ(OB_SUCCESS, loser_tree.open(4));
  ObSRDaaTBatchIdMerger merger;
  ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, 1, cmp, &loser_tree, current_ids));

  int64_t count = -1;
  EXPECT_EQ(OB_NOT_SUPPORTED, merger.get_next_distinct_count(2, count));
  EXPECT_EQ(0, count);
  const ObDatum *id_datum = nullptr;
  ASSERT_EQ(OB_SUCCESS, merger.get_next_distinct_id(id_datum));
  ASSERT_NE(nullptr, id_datum);
  EXPECT_EQ(1, id_datum->get_uint64());
}

TEST_F(CountOnlyBatchMergerTest, bulk_count_rescans_after_reuse)
{
  common::ObArenaAllocator allocator;
  common::ObSEArray<ObISRDaaTDimIter *, 2> dim_iters;
  common::ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
  std::unique_ptr<FakeBatchDimIter> first(new FakeBatchDimIter({1, 2, 2, 5}, 2));
  std::unique_ptr<FakeBatchDimIter> second(new FakeBatchDimIter({2, 3, 5, 5}, 2));
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(first.get()));
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(second.get()));
  current_ids.set_allocator(&allocator);
  ASSERT_EQ(OB_SUCCESS, current_ids.init(2));
  ASSERT_EQ(OB_SUCCESS, current_ids.prepare_allocate(2));
  ObSRMergeCmp cmp;
  sql::ObDatumMeta id_meta;
  id_meta.type_ = ObUInt64Type;
  id_meta.cs_type_ = CS_TYPE_BINARY;
  ASSERT_EQ(OB_SUCCESS, cmp.init(id_meta, &current_ids));
  ObSRDaaTBatchIdMerger merger;

  auto count_scan = [&merger]() {
    int64_t total = 0;
    while (true) {
      int64_t count = 0;
      const int ret = merger.get_next_distinct_count(2, count);
      total += count;
      EXPECT_TRUE(OB_SUCCESS == ret || OB_ITER_END == ret);
      if (OB_ITER_END == ret || OB_SUCCESS != ret) {
        break;
      }
    }
    return total;
  };

  ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, 2, cmp, nullptr, current_ids));
  EXPECT_EQ(4, count_scan());
  first->reuse();
  second->reuse();
  merger.reuse();
  ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, 2, cmp, nullptr, current_ids));
  EXPECT_EQ(4, count_scan());
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
