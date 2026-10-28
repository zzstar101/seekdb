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
#include <set>
#include <string>
#include <vector>

#include "data_plane/api/data_plane/access/ob_tablet_scan.h"
#include "lib/container/ob_se_array.h"
#include "query/engine/expr/ob_expr.h"
#include "storage/retrieval/ob_block_max_iter.h"
#include "storage/retrieval/ob_sparse_daat_iter.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
// The production token iterator intentionally keeps its batch state private. Drive that state
// directly here so the mixed batch-row interface is covered without a live tablet scan.
#define private public
#include "storage/retrieval/ob_text_retrieval_token_iter.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

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
    : delegate_(&delegate),
      alloc_calls_(0),
      free_calls_(0),
      invalid_free_calls_(0),
      live_allocations_()
  {
  }

  virtual void *alloc(const int64_t size) override
  {
    ++alloc_calls_;
    void *ptr = delegate_->alloc(size);
    if (OB_NOT_NULL(ptr)) {
      live_allocations_.insert(ptr);
    }
    return ptr;
  }

  virtual void *alloc(const int64_t size, const common::ObMemAttr &attr) override
  {
    ++alloc_calls_;
    void *ptr = delegate_->alloc(size, attr);
    if (OB_NOT_NULL(ptr)) {
      live_allocations_.insert(ptr);
    }
    return ptr;
  }

  virtual void free(void *ptr) override
  {
    if (OB_NOT_NULL(ptr)) {
      if (0 == live_allocations_.erase(ptr)) {
        ++invalid_free_calls_;
      } else {
        ++free_calls_;
        delegate_->free(ptr);
      }
    }
  }

  virtual int64_t total() const override { return delegate_->total(); }
  virtual int64_t used() const override { return delegate_->used(); }

  int64_t alloc_calls() const { return alloc_calls_; }
  int64_t free_calls() const { return free_calls_; }
  int64_t invalid_free_calls() const { return invalid_free_calls_; }
  int64_t live_allocations() const { return static_cast<int64_t>(live_allocations_.size()); }

private:
  ObIAllocator *delegate_;
  int64_t alloc_calls_;
  int64_t free_calls_;
  int64_t invalid_free_calls_;
  std::set<void *> live_allocations_;
};

class FakeBatchDimIter final : public ObISRDaaTDimIter
{
public:
  FakeBatchDimIter(
      const std::vector<std::optional<uint64_t>> &ids,
      const int64_t batch_size,
      const int64_t fail_on_call = 0)
    : values_(), ids_(), next_idx_(0), batch_size_(batch_size),
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
    batch_calls_ = 0;
  }
  virtual int get_next_doc_id_batch(
      const int64_t capacity,
      const ObDatum *&doc_ids,
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
        doc_ids = ids_.data() + next_idx_;
        next_idx_ += count;
      }
    }
    return ret;
  }

private:
  std::vector<int64_t> values_;
  std::vector<ObDatum> ids_;
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
    : values_(), ids_(), next_idx_(0), batch_size_(batch_size),
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
    batch_calls_ = 0;
  }
  virtual int get_next_doc_id_batch(
      const int64_t capacity,
      const ObDatum *&doc_ids,
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
        doc_ids = ids_.data() + next_idx_;
        next_idx_ += count;
      }
    }
    return ret;
  }

private:
  std::vector<std::string> values_;
  std::vector<ObDatum> ids_;
  int64_t next_idx_;
  int64_t batch_size_;
  int64_t batch_calls_;
  int64_t fail_on_call_;
};

class OverwritingBinaryBatchDimIter final : public ObISRDaaTDimIter
{
public:
  OverwritingBinaryBatchDimIter(
      const std::vector<std::optional<std::string>> &ids,
      const int64_t batch_size)
    : ids_(ids),
      datums_(batch_size),
      payload_(),
      next_idx_(0),
      batch_size_(batch_size)
  {
    int64_t max_length = 1;
    for (const std::optional<std::string> &id : ids_) {
      if (id.has_value()) {
        max_length = MAX(max_length, static_cast<int64_t>(id->size()));
      }
    }
    payload_.resize(max_length * batch_size_);
  }

  virtual int get_next_row() override { return OB_NOT_IMPLEMENT; }
  virtual int get_next_batch(const int64_t, int64_t &) override { return OB_NOT_IMPLEMENT; }
  virtual int get_curr_score(double &) const override { return OB_NOT_IMPLEMENT; }
  virtual int get_curr_id(const ObDatum *&) const override { return OB_NOT_IMPLEMENT; }
  virtual bool support_batch_doc_ids() const override { return true; }
  void reuse() { next_idx_ = 0; }
  virtual int get_next_doc_id_batch(
      const int64_t capacity,
      const ObDatum *&doc_ids,
      int64_t &count) override
  {
    int ret = OB_SUCCESS;
    doc_ids = nullptr;
    count = 0;
    // Every refill overwrites the same datum objects and raw payload buffer.
    for (int64_t i = 0; i < batch_size_; ++i) {
      datums_[i].set_null();
    }
    const int64_t remaining = static_cast<int64_t>(ids_.size()) - next_idx_;
    if (remaining <= 0) {
      ret = OB_ITER_END;
    } else {
      count = MIN(capacity, MIN(batch_size_, remaining));
      int64_t payload_offset = 0;
      for (int64_t i = 0; i < count; ++i) {
        const std::optional<std::string> &id = ids_[next_idx_ + i];
        if (id.has_value()) {
          char *payload = payload_.data() + payload_offset;
          if (!id->empty()) {
            MEMCPY(payload, id->data(), id->size());
          }
          datums_[i].set_string(payload, static_cast<uint32_t>(id->size()));
          payload_offset += static_cast<int64_t>(id->size());
        }
      }
      next_idx_ += count;
      doc_ids = datums_.data();
    }
    return ret;
  }

private:
  std::vector<std::optional<std::string>> ids_;
  std::vector<ObDatum> datums_;
  std::vector<char> payload_;
  int64_t next_idx_;
  int64_t batch_size_;
};

using IdList = std::vector<std::optional<uint64_t>>;
using ExpectedIds = std::vector<std::optional<uint64_t>>;
using StringIdList = std::vector<std::optional<std::string>>;

class TestCountOnlyDaaTIter final : public ObSRDaaTIterImpl
{
public:
  TestCountOnlyDaaTIter() = default;
  virtual ~TestCountOnlyDaaTIter() override { reset(); }

  int init_for_count_test(
      ObSparseRetrievalMergeParam &iter_param,
      ObIArray<ObISRDaaTDimIter *> &dim_iters,
      ObIAllocator &allocator,
      const sql::ObDatumMeta &id_meta)
  {
    int ret = OB_SUCCESS;
    iter_param_ = &iter_param;
    dim_iters_ = &dim_iters;
    iter_allocator_ = &allocator;
    merge_heap_ = nullptr;
    relevance_collector_ = nullptr;
    need_score_ = false;
    count_only_ = true;
    count_only_batch_capable_ = true;
    count_only_batch_started_ = false;
    input_row_cnt_ = 0;
    output_row_cnt_ = 0;
    if (0 == dim_iters.count()) {
      is_inited_ = true;
    } else {
      iter_domain_ids_.set_allocator(&allocator);
      if (OB_FAIL(iter_domain_ids_.init(dim_iters.count()))) {
      } else if (OB_FAIL(iter_domain_ids_.prepare_allocate(dim_iters.count()))) {
      } else if (OB_FAIL(merge_cmp_.init(id_meta, &iter_domain_ids_))) {
      } else {
        is_inited_ = true;
      }
    }
    return ret;
  }

protected:
  virtual int pre_process() override { return OB_SUCCESS; }
  virtual bool support_count_only_batch() const override { return true; }
};

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

  void scan_bulk_count(
      ObSRDaaTBatchIdMerger &merger,
      const int64_t capacity,
      int64_t &total)
  {
    total = 0;
    bool ended = false;
    for (int64_t call = 0; !ended && call < 1000000; ++call) {
      int64_t count = 0;
      const int ret = merger.get_next_distinct_count(capacity, count);
      ASSERT_TRUE(OB_SUCCESS == ret || OB_ITER_END == ret);
      ASSERT_GE(count, 0);
      ASSERT_LE(count, capacity);
      if (OB_SUCCESS == ret) {
        ASSERT_EQ(capacity, count);
      }
      total += count;
      ended = OB_ITER_END == ret;
    }
    ASSERT_TRUE(ended);
    int64_t terminal_count = -1;
    EXPECT_EQ(OB_ITER_END, merger.get_next_distinct_count(capacity, terminal_count));
    EXPECT_EQ(0, terminal_count);
  }

  void run_overwriting_binary_bulk_count(
      const std::vector<StringIdList> &inputs,
      const int64_t batch_size,
      const int64_t capacity,
      int64_t &total)
  {
    common::ObArenaAllocator allocator;
    common::ObSEArray<ObISRDaaTDimIter *, 4> dim_iters;
    common::ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
    std::vector<std::unique_ptr<OverwritingBinaryBatchDimIter>> owned_iters;
    owned_iters.reserve(inputs.size());
    for (const StringIdList &ids : inputs) {
      owned_iters.emplace_back(new OverwritingBinaryBatchDimIter(ids, batch_size));
      ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(owned_iters.back().get()));
    }
    current_ids.set_allocator(&allocator);
    ASSERT_EQ(OB_SUCCESS, current_ids.init(dim_iters.count()));
    ASSERT_EQ(OB_SUCCESS, current_ids.prepare_allocate(dim_iters.count()));
    ObSRMergeCmp cmp;
    sql::ObDatumMeta id_meta;
    id_meta.type_ = ObVarcharType;
    id_meta.cs_type_ = common::CS_TYPE_BINARY;
    ASSERT_EQ(OB_SUCCESS, cmp.init(id_meta, &current_ids));
    ObSRDaaTBatchIdMerger merger;
    ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, allocator, batch_size, cmp, nullptr, current_ids));

    total = 0;
    bool ended = false;
    for (int64_t call = 0; !ended && call < 1000000; ++call) {
      int64_t count = 0;
      const int ret = merger.get_next_distinct_count(capacity, count);
      ASSERT_TRUE(OB_SUCCESS == ret || OB_ITER_END == ret);
      ASSERT_GE(count, 0);
      ASSERT_LE(count, capacity);
      if (OB_SUCCESS == ret) {
        ASSERT_EQ(capacity, count);
      }
      total += count;
      ended = OB_ITER_END == ret;
    }
    ASSERT_TRUE(ended);
    int64_t terminal_count = -1;
    EXPECT_EQ(OB_ITER_END, merger.get_next_distinct_count(capacity, terminal_count));
    EXPECT_EQ(0, terminal_count);
  }

  void run_outer_count(
      const std::vector<IdList> &inputs,
      const int64_t offset,
      const int64_t limit,
      const int64_t capacity,
      const int expected_ret,
      const int64_t expected_count)
  {
    common::ObLimitParam limit_param;
    limit_param.offset_ = offset;
    limit_param.limit_ = limit;
    ObSparseRetrievalMergeParam param;
    param.limit_param_ = &limit_param;
    param.max_batch_size_ = MAX(capacity, 1);
    param.count_only_ = true;

    common::ObArenaAllocator allocator;
    common::ObSEArray<ObISRDaaTDimIter *, 4> dim_iters;
    std::vector<std::unique_ptr<FakeBatchDimIter>> owned_iters;
    owned_iters.reserve(inputs.size());
    for (const IdList &ids : inputs) {
      owned_iters.emplace_back(new FakeBatchDimIter(ids, 2));
      ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(owned_iters.back().get()));
    }
    sql::ObDatumMeta id_meta;
    id_meta.type_ = ObUInt64Type;
    id_meta.cs_type_ = CS_TYPE_BINARY;
    TestCountOnlyDaaTIter iter;
    ASSERT_EQ(OB_SUCCESS, iter.init_for_count_test(param, dim_iters, allocator, id_meta));
    int64_t count = -1;
    EXPECT_EQ(expected_ret, iter.get_next_rows(capacity, count));
    EXPECT_EQ(expected_count, count);
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

TEST_F(CountOnlyBatchMergerTest, bulk_count_survives_reused_binary_batch_storage)
{
  // Duplicate "c" crosses refill boundaries in every dimension. At capacity 5, the final
  // distinct id "d" comes only from the third dimension.
  const std::vector<StringIdList> inputs = {
      {std::nullopt, std::string("a"), std::string("c"), std::string("c"), std::string("f")},
      {std::nullopt, std::string("a"), std::string("b"), std::string("c"), std::string("e")},
      {std::nullopt, std::string("b"), std::string("c"), std::string("d"), std::string("f")}};
  for (const int64_t batch_size : {1, 2, 3, 4}) {
    SCOPED_TRACE(batch_size);
    for (const int64_t capacity : {1, 2, 5, 7}) {
      SCOPED_TRACE(capacity);
      int64_t total = 0;
      run_overwriting_binary_bulk_count(inputs, batch_size, capacity, total);
      EXPECT_EQ(7, total);
    }
  }
}

TEST_F(CountOnlyBatchMergerTest, bulk_boundary_buffer_tracks_allocator_ownership)
{
  const std::string long_a = std::string("a-") + std::string(158, 'x');
  const std::string long_b = std::string("b-") + std::string(158, 'y');
  common::ObArenaAllocator first_arena;
  common::ObArenaAllocator second_arena;
  CountingAllocator first_allocator(first_arena);
  CountingAllocator second_allocator(second_arena);
  common::ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
  current_ids.set_allocator(&first_allocator);
  ASSERT_EQ(OB_SUCCESS, current_ids.init(2));
  ASSERT_EQ(OB_SUCCESS, current_ids.prepare_allocate(2));

  std::vector<StringIdList> inputs = {
      {long_a, long_a, long_b},
      {long_a, long_b, long_b}};
  std::unique_ptr<OverwritingBinaryBatchDimIter> first(
      new OverwritingBinaryBatchDimIter(inputs[0], 1));
  std::unique_ptr<OverwritingBinaryBatchDimIter> second(
      new OverwritingBinaryBatchDimIter(inputs[1], 1));
  common::ObSEArray<ObISRDaaTDimIter *, 2> dim_iters;
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(first.get()));
  ASSERT_EQ(OB_SUCCESS, dim_iters.push_back(second.get()));
  ObSRMergeCmp cmp;
  sql::ObDatumMeta id_meta;
  id_meta.type_ = ObVarcharType;
  id_meta.cs_type_ = common::CS_TYPE_BINARY;
  ASSERT_EQ(OB_SUCCESS, cmp.init(id_meta, &current_ids));
  ObSRDaaTBatchIdMerger merger;

  ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, first_allocator, 1, cmp, nullptr, current_ids));
  const int64_t allocs_before_first_scan = first_allocator.alloc_calls();
  int64_t total = 0;
  scan_bulk_count(merger, 1, total);
  EXPECT_EQ(2, total);
  EXPECT_EQ(allocs_before_first_scan + 1, first_allocator.alloc_calls());

  first->reuse();
  second->reuse();
  merger.reuse();
  const int64_t allocs_after_first_scan = first_allocator.alloc_calls();
  ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, first_allocator, 1, cmp, nullptr, current_ids));
  scan_bulk_count(merger, 1, total);
  EXPECT_EQ(2, total);
  EXPECT_EQ(allocs_after_first_scan, first_allocator.alloc_calls());

  first->reuse();
  second->reuse();
  merger.reuse();
  const int64_t live_before_switch = first_allocator.live_allocations();
  ASSERT_EQ(OB_SUCCESS, merger.init(dim_iters, second_allocator, 1, cmp, nullptr, current_ids));
  EXPECT_EQ(live_before_switch - 1, first_allocator.live_allocations());
  EXPECT_EQ(0, second_allocator.alloc_calls());
  scan_bulk_count(merger, 1, total);
  EXPECT_EQ(2, total);
  EXPECT_EQ(1, second_allocator.alloc_calls());
  EXPECT_EQ(1, second_allocator.live_allocations());

  merger.reset();
  EXPECT_EQ(0, second_allocator.live_allocations());
  EXPECT_EQ(1, second_allocator.free_calls());
  EXPECT_EQ(1, first_allocator.live_allocations());
  current_ids.reset();
  EXPECT_EQ(0, first_allocator.live_allocations());
  EXPECT_EQ(first_allocator.alloc_calls(), first_allocator.free_calls());
  EXPECT_EQ(second_allocator.alloc_calls(), second_allocator.free_calls());
  EXPECT_EQ(0, first_allocator.invalid_free_calls());
  EXPECT_EQ(0, second_allocator.invalid_free_calls());
}

TEST_F(CountOnlyBatchMergerTest, outer_count_extreme_offset_is_not_an_overflow)
{
  std::vector<std::vector<IdList>> datasets;
  datasets.push_back(std::vector<IdList>());
  datasets.push_back(std::vector<IdList>(3));
  datasets.push_back({
      {1, 2, 3, 3, 5},
      {2, 3, 4, 7},
      {1, 4, 5, 6, 7}});
  for (const std::vector<IdList> &inputs : datasets) {
    for (const int64_t target_count : {1, 2, 256}) {
      SCOPED_TRACE(target_count);
      run_outer_count(inputs, INT64_MAX, target_count, target_count, OB_ITER_END, 0);
    }
  }
}

TEST_F(CountOnlyBatchMergerTest, outer_count_segments_offset_before_result_capacity)
{
  const std::vector<IdList> inputs = {
      {1, 2, 3, 3, 5},
      {2, 3, 4, 7},
      {1, 4, 5, 6, 7}};
  run_outer_count(inputs, 3, 2, 2, OB_ITER_END, 2);
  run_outer_count(inputs, 7, 2, 2, OB_ITER_END, 0);
  run_outer_count(inputs, 0, 2, 2, OB_ITER_END, 2);
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

TEST(ProductionTokenIterBatchTest, advance_uses_active_batch_view)
{
  common::ObArenaAllocator allocator;
  ObTextRetrievalDaaTTokenIter iter;
  std::vector<uint64_t> batch_values = {10, 20, 30};
  std::vector<uint64_t> stale_values = {91, 92, 93};
  std::vector<ObDatum> source_datums(batch_values.size());
  const int64_t batch_count = static_cast<int64_t>(source_datums.size());
  for (int64_t i = 0; i < batch_count; ++i) {
    source_datums[i].ptr_ = reinterpret_cast<const char *>(&batch_values[i]);
    source_datums[i].pack_ = sizeof(uint64_t);
  }
  sql::ObDatumVector source;
  source.set_batch(true);
  source.datums_ = source_datums.data();

  iter.allocator_ = &allocator;
  iter.max_batch_size_ = batch_count;
  iter.owned_batch_datums_.set_allocator(&allocator);
  ASSERT_EQ(OB_SUCCESS, iter.owned_batch_datums_.init(batch_count));
  ASSERT_EQ(OB_SUCCESS, iter.owned_batch_datums_.prepare_allocate(batch_count));
  iter.doc_id_.set_allocator(&allocator);
  ASSERT_EQ(OB_SUCCESS, iter.doc_id_.init(batch_count));
  ASSERT_EQ(OB_SUCCESS, iter.doc_id_.prepare_allocate(batch_count));
  for (int64_t i = 0; i < static_cast<int64_t>(stale_values.size()); ++i) {
    ObDatum stale;
    stale.ptr_ = reinterpret_cast<const char *>(&stale_values[i]);
    stale.pack_ = sizeof(uint64_t);
    ASSERT_EQ(OB_SUCCESS, iter.doc_id_[i].set_datum_fast(stale));
  }

  common::ObDatumBasicFuncs *basic_funcs =
      ObDatumFuncs::get_basic_func(ObUInt64Type, CS_TYPE_BINARY);
  ASSERT_NE(nullptr, basic_funcs);
  iter.cmp_func_ = basic_funcs->null_first_cmp_;
  int64_t built_count = 0;
  ASSERT_EQ(OB_SUCCESS, iter.build_owned_doc_id_batch(
      source, nullptr, nullptr, batch_count, built_count));
  ASSERT_EQ(batch_count, built_count);
  iter.count_ = batch_count;
  iter.cur_idx_ = -1;

  ASSERT_EQ(OB_SUCCESS, iter.get_next_row());
  const ObDatum *current = nullptr;
  ASSERT_EQ(OB_SUCCESS, iter.get_curr_id(current));
  EXPECT_EQ(10, current->get_uint64());

  auto make_target = [](uint64_t &value) {
    ObDatum target;
    target.ptr_ = reinterpret_cast<const char *>(&value);
    target.pack_ = sizeof(uint64_t);
    return target;
  };
  uint64_t target_20 = 20;
  ASSERT_EQ(OB_SUCCESS, iter.advance_to(make_target(target_20)));
  ASSERT_EQ(OB_SUCCESS, iter.get_curr_id(current));
  EXPECT_EQ(20, current->get_uint64());

  uint64_t target_30 = 30;
  ASSERT_EQ(OB_SUCCESS, iter.advance_to(make_target(target_30)));
  ASSERT_EQ(OB_SUCCESS, iter.get_curr_id(current));
  EXPECT_EQ(30, current->get_uint64());

  uint64_t target_25 = 25;
  ASSERT_EQ(OB_SUCCESS, iter.advance_to(make_target(target_25)));
  ASSERT_EQ(OB_SUCCESS, iter.get_curr_id(current));
  EXPECT_EQ(30, current->get_uint64());

  iter.cur_idx_ = -1;
  EXPECT_EQ(OB_ARRAY_OUT_OF_RANGE, iter.get_curr_id(current));
  double score = 0.0;
  EXPECT_EQ(OB_ARRAY_OUT_OF_RANGE, iter.get_curr_score(score));
}

TEST(ProductionTokenIterBatchTest, shared_frame_payload_is_copied_per_iterator)
{
  common::ObArenaAllocator allocator;
  ObTextRetrievalDaaTTokenIter first;
  ObTextRetrievalDaaTTokenIter second;
  const std::vector<uint64_t> expected_values = {10, 20, 30};
  std::vector<uint64_t> shared_values = expected_values;
  std::vector<ObDatum> shared_datums(shared_values.size());
  const int64_t batch_count = static_cast<int64_t>(shared_datums.size());
  for (int64_t i = 0; i < batch_count; ++i) {
    shared_datums[i].ptr_ = reinterpret_cast<const char *>(&shared_values[i]);
    shared_datums[i].pack_ = sizeof(uint64_t);
  }
  sql::ObDatumVector shared_source;
  shared_source.set_batch(true);
  shared_source.datums_ = shared_datums.data();

  auto init_iterator = [&](ObTextRetrievalDaaTTokenIter &iter) {
    iter.allocator_ = &allocator;
    iter.max_batch_size_ = batch_count;
    iter.owned_batch_datums_.set_allocator(&allocator);
    ASSERT_EQ(OB_SUCCESS, iter.owned_batch_datums_.init(batch_count));
    ASSERT_EQ(OB_SUCCESS, iter.owned_batch_datums_.prepare_allocate(batch_count));
  };
  init_iterator(first);
  init_iterator(second);

  int64_t first_count = 0;
  int64_t second_count = 0;
  ASSERT_EQ(OB_SUCCESS, first.build_owned_doc_id_batch(
      shared_source, nullptr, nullptr, batch_count, first_count));
  ASSERT_EQ(OB_SUCCESS, second.build_owned_doc_id_batch(
      shared_source, nullptr, nullptr, batch_count, second_count));
  ASSERT_EQ(batch_count, first_count);
  ASSERT_EQ(batch_count, second_count);

  // Loading the next token iterator overwrites both the shared datum structs and their payload.
  shared_values = {99, 98, 97};
  std::vector<ObDatum> overwritten(shared_datums.size());
  for (int64_t i = 0; i < batch_count; ++i) {
    overwritten[i].ptr_ = reinterpret_cast<const char *>(&shared_values[i]);
    overwritten[i].pack_ = sizeof(uint64_t);
  }
  MEMCPY(shared_datums.data(), overwritten.data(), sizeof(ObDatum) * batch_count);

  for (int64_t i = 0; i < batch_count; ++i) {
    EXPECT_EQ(expected_values[i], first.batch_doc_ids_[i].get_uint64());
    EXPECT_EQ(expected_values[i], second.batch_doc_ids_[i].get_uint64());
  }
}

TEST(ProductionTokenIterBatchTest, shared_frame_long_payload_grows_owned_buffer)
{
  common::ObArenaAllocator allocator;
  CountingAllocator counting(allocator);
  ObTextRetrievalDaaTTokenIter first;
  ObTextRetrievalDaaTTokenIter second;
  const std::vector<std::string> expected = {
      std::string(8, 'a'), std::string(8, 'b'), std::string(40, 'c')};
  std::vector<std::string> shared_payload = expected;
  std::vector<ObDatum> shared_datums(shared_payload.size());
  const int64_t batch_count = static_cast<int64_t>(shared_datums.size());
  for (int64_t i = 0; i < batch_count; ++i) {
    shared_datums[i].set_string(
        shared_payload[i].data(), static_cast<uint32_t>(shared_payload[i].size()));
  }
  sql::ObDatumVector shared_source;
  shared_source.set_batch(true);
  shared_source.datums_ = shared_datums.data();

  auto init_iterator = [&](ObTextRetrievalDaaTTokenIter &iter) {
    iter.allocator_ = &counting;
    iter.max_batch_size_ = batch_count;
    iter.owned_batch_datums_.set_allocator(&counting);
    ASSERT_EQ(OB_SUCCESS, iter.owned_batch_datums_.init(batch_count));
    ASSERT_EQ(OB_SUCCESS, iter.owned_batch_datums_.prepare_allocate(batch_count));
  };
  init_iterator(first);
  init_iterator(second);

  int64_t first_count = 0;
  int64_t second_count = 0;
  ASSERT_EQ(OB_SUCCESS, first.build_owned_doc_id_batch(
      shared_source, nullptr, nullptr, batch_count, first_count));
  ASSERT_EQ(OB_SUCCESS, second.build_owned_doc_id_batch(
      shared_source, nullptr, nullptr, batch_count, second_count));
  ASSERT_EQ(batch_count, first_count);
  ASSERT_EQ(batch_count, second_count);
  const int64_t alloc_calls = counting.alloc_calls();
  ASSERT_EQ(OB_SUCCESS, first.build_owned_doc_id_batch(
      shared_source, nullptr, nullptr, batch_count, first_count));
  EXPECT_EQ(alloc_calls, counting.alloc_calls());

  for (std::string &value : shared_payload) {
    std::fill(value.begin(), value.end(), 'z');
  }
  for (int64_t i = 0; i < batch_count; ++i) {
    const ObDatum &first_id = first.batch_doc_ids_[i];
    const ObDatum &second_id = second.batch_doc_ids_[i];
    EXPECT_EQ(expected[i], std::string(first_id.ptr_, first_id.len_));
    EXPECT_EQ(expected[i], std::string(second_id.ptr_, second_id.len_));
  }
}

} // namespace
} // namespace storage
} // namespace oceanbase
