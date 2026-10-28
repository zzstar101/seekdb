/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "sql/das/ob_das_domain_utils.h"

namespace oceanbase
{
namespace sql
{
namespace
{

class FTSWordRowBufferTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    meta_.set_varchar();
    meta_.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
    property_.ngram_token_size_ = 3;
    ASSERT_EQ(OB_SUCCESS, parser_.init(common::ObString::make_string("ngram")));
    ASSERT_EQ(OB_SUCCESS, helper_.init(&allocator_, parser_, property_));
  }

  ObDatum doc_id(const char *value)
  {
    ObDatum datum;
    datum.set_string(value, static_cast<uint32_t>(strlen(value)));
    return datum;
  }

  static ObString as_ob_string(const std::string &value)
  {
    return ObString(static_cast<int32_t>(value.size()), value.data());
  }

  static std::string word_at(const blocksstable::ObStorageDatum *rows,
                             const int64_t row,
                             const bool is_fts_index_aux)
  {
    const int64_t word_idx = is_fts_index_aux ? 0 : 1;
    const ObString word = rows[row * FTSWordRowBuffer::COLUMN_COUNT + word_idx].get_string();
    return std::string(word.ptr(), word.length());
  }

  static std::string document_at(const blocksstable::ObStorageDatum *rows,
                                 const int64_t row,
                                 const bool is_fts_index_aux)
  {
    const int64_t doc_id_idx = is_fts_index_aux ? 1 : 0;
    const ObString doc_id = rows[row * FTSWordRowBuffer::COLUMN_COUNT + doc_id_idx].get_string();
    return std::string(doc_id.ptr(), doc_id.length());
  }

  common::ObArenaAllocator allocator_;
  common::ObObjMeta meta_;
  storage::ObFTParser parser_;
  storage::ObFTParserProperty property_;
  storage::ObFTParseHelper helper_;
};

TEST_F(FTSWordRowBufferTest, batches_across_documents_and_keeps_pending_rows_stable)
{
  FTSWordRowBuffer buffer;
  std::string ids[] = {"doc-0", "doc-1", "doc-2", "doc-3"};
  std::string texts[] = {
    "aaa bbb aaa",
    "ccc ddd eee",
    "fff ggg hhh iii jjj",
    "kkk lll"};

  for (int64_t i = 0; i < 3; ++i) {
    ObDatum id = doc_id(ids[i].c_str());
    ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, id, as_ob_string(texts[i]), true));
  }
  ASSERT_EQ(10, buffer.get_row_count());

  const blocksstable::ObStorageDatum *first_batch = nullptr;
  int64_t first_count = 0;
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(first_batch, first_count, 4));
  ASSERT_EQ(4, first_count);
  EXPECT_EQ(6, buffer.get_pending_row_count());

  // Growing the contiguous datum buffer while rows are pending must not change
  // either pending word bytes or their document ids.
  ObDatum fourth_id = doc_id(ids[3].c_str());
  ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, fourth_id, as_ob_string(texts[3]), true));
  ASSERT_EQ(12, buffer.get_row_count());
  ASSERT_EQ(8, buffer.get_pending_row_count());

  std::vector<std::pair<std::string, std::string>> seen;
  for (int64_t i = 0; i < first_count; ++i) {
    seen.emplace_back(word_at(first_batch, i, true), document_at(first_batch, i, true));
  }
  const blocksstable::ObStorageDatum *batch = nullptr;
  int64_t count = 0;
  while (OB_SUCCESS == buffer.get_next_batch(batch, count, 3)) {
    ASSERT_GT(count, 0);
    for (int64_t i = 0; i < count; ++i) {
      seen.emplace_back(word_at(batch, i, true), document_at(batch, i, true));
    }
  }
  EXPECT_EQ(12, seen.size());
  EXPECT_EQ(OB_ITER_END, buffer.get_next_batch(batch, count, 3));
  EXPECT_EQ(0, count);

  int64_t aaa_count = 0;
  int64_t doc_zero_words = 0;
  for (const auto &row : seen) {
    if ("aaa" == row.first && "doc-0" == row.second) {
      ++aaa_count;
    }
    if ("doc-0" == row.second) {
      ++doc_zero_words;
    }
  }
  EXPECT_EQ(1, aaa_count);
  EXPECT_EQ(2, doc_zero_words);
}

TEST_F(FTSWordRowBufferTest, copies_word_and_document_bytes_out_of_source_lifetime)
{
  FTSWordRowBuffer buffer;
  std::string text = "aaa bbb";
  std::string id = "source-doc";
  ObDatum id_datum = doc_id(id.c_str());
  ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, id_datum, as_ob_string(text), true));
  memset(text.data(), 'x', text.size());
  memset(id.data(), 'y', id.size());

  const blocksstable::ObStorageDatum *rows = nullptr;
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, 2));
  ASSERT_EQ(2, count);
  bool saw_aaa = false;
  bool saw_bbb = false;
  for (int64_t i = 0; i < count; ++i) {
    const std::string word = word_at(rows, i, true);
    saw_aaa = saw_aaa || "aaa" == word;
    saw_bbb = saw_bbb || "bbb" == word;
    EXPECT_EQ("source-doc", document_at(rows, i, true));
  }
  EXPECT_TRUE(saw_aaa);
  EXPECT_TRUE(saw_bbb);
}

TEST_F(FTSWordRowBufferTest, empty_documents_are_skipped_without_rows)
{
  FTSWordRowBuffer buffer;
  ObDatum id = doc_id("empty-doc");
  EXPECT_EQ(OB_ITER_END, buffer.append(allocator_, &helper_, meta_, id, ObString(), true));
  EXPECT_EQ(OB_ITER_END, buffer.append(allocator_, &helper_, meta_, id, ObString::make_string("   "), true));
  EXPECT_EQ(0, buffer.get_row_count());

  ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, id,
                                      ObString::make_string("aaa"), true));
  const blocksstable::ObStorageDatum *rows = nullptr;
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, 1));
  ASSERT_EQ(1, count);
  EXPECT_EQ("aaa", word_at(rows, 0, true));
  EXPECT_EQ("empty-doc", document_at(rows, 0, true));
}

TEST_F(FTSWordRowBufferTest, supports_doc_word_column_order)
{
  FTSWordRowBuffer buffer;
  ObDatum id = doc_id("doc-word-id");
  ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, id,
                                      ObString::make_string("aaa bbb"), false));
  const blocksstable::ObStorageDatum *rows = nullptr;
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, 2));
  ASSERT_EQ(2, count);
  for (int64_t i = 0; i < count; ++i) {
    EXPECT_EQ("doc-word-id", document_at(rows, i, false));
    EXPECT_EQ(1, rows[i * FTSWordRowBuffer::COLUMN_COUNT + 2].get_uint());
    EXPECT_EQ(2, rows[i * FTSWordRowBuffer::COLUMN_COUNT + 3].get_uint());
  }
}

TEST_F(FTSWordRowBufferTest, one_document_can_span_multiple_output_batches)
{
  FTSWordRowBuffer buffer;
  std::string text;
  for (int64_t i = 0; i < 300; ++i) {
    if (!text.empty()) {
      text.push_back(' ');
    }
    const char token[] = {
      static_cast<char>('a' + i % 26),
      static_cast<char>('a' + (i / 26) % 26),
      static_cast<char>('a' + (i / (26 * 26)) % 26)};
    text.append(token, sizeof(token));
  }
  const std::string id_value = "large-doc";
  ObDatum id = doc_id(id_value.c_str());
  ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, id, as_ob_string(text), true));
  ASSERT_EQ(300, buffer.get_row_count());

  const blocksstable::ObStorageDatum *rows = nullptr;
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, 256));
  EXPECT_EQ(256, count);
  EXPECT_EQ(44, buffer.get_pending_row_count());
  for (int64_t i = 0; i < count; ++i) {
    EXPECT_EQ(id_value, document_at(rows, i, true));
  }

  // The remaining rows must survive after the caller has consumed the first
  // output batch and the source fulltext can be discarded.
  text.assign(text.size(), 'x');
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, 256));
  EXPECT_EQ(44, count);
  for (int64_t i = 0; i < count; ++i) {
    EXPECT_EQ(id_value, document_at(rows, i, true));
    EXPECT_FALSE(word_at(rows, i, true).empty());
  }
}

TEST_F(FTSWordRowBufferTest, empty_and_tiny_documents_share_a_batch)
{
  FTSWordRowBuffer buffer;
  for (int64_t i = 0; i < 64; ++i) {
    std::string id_value = "tiny-" + std::to_string(i);
    ObDatum id = doc_id(id_value.c_str());
    if (0 == i % 2) {
      EXPECT_EQ(OB_ITER_END, buffer.append(allocator_, &helper_, meta_, id, ObString(), true));
    } else {
      const char token[] = {
        static_cast<char>('a' + i % 26),
        static_cast<char>('a' + (i / 26) % 26),
        'z'};
      std::string text(token, sizeof(token));
      ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, id, as_ob_string(text), true));
    }
  }
  EXPECT_EQ(32, buffer.get_row_count());
  const blocksstable::ObStorageDatum *rows = nullptr;
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, 64));
  EXPECT_EQ(32, count);
  for (int64_t i = 0; i < count; ++i) {
    EXPECT_EQ("tiny-" + std::to_string(i * 2 + 1), document_at(rows, i, true));
  }
}

TEST_F(FTSWordRowBufferTest, nondivisible_documents_refill_only_from_an_empty_buffer)
{
  FTSWordRowBuffer buffer;
  const int64_t batch_capacity = 10;
  const int64_t document_count = 40;
  const int64_t words_per_document = 7;
  int64_t appended_documents = 0;
  int64_t emitted_rows = 0;
  int64_t max_buffered_rows = 0;

  while (appended_documents < document_count || buffer.get_pending_row_count() > 0) {
    if (0 == buffer.get_pending_row_count()) {
      buffer.reuse();
      while (appended_documents < document_count
             && buffer.get_pending_row_count() < batch_capacity) {
        std::string text;
        for (int64_t i = 0; i < words_per_document; ++i) {
          const int64_t value = appended_documents * words_per_document + i;
          const char token[] = {
            static_cast<char>('a' + value % 26),
            static_cast<char>('a' + (value / 26) % 26),
            static_cast<char>('a' + (value / (26 * 26)) % 26)};
          if (!text.empty()) {
            text.push_back(' ');
          }
          text.append(token, sizeof(token));
        }
        std::string id_value = "nondiv-" + std::to_string(appended_documents);
        ObDatum id = doc_id(id_value.c_str());
        ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, id, as_ob_string(text), true));
        ++appended_documents;
      }
      max_buffered_rows = std::max(max_buffered_rows, buffer.get_row_count());
    }

    const blocksstable::ObStorageDatum *rows = nullptr;
    int64_t count = 0;
    ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, batch_capacity));
    emitted_rows += count;
  }

  EXPECT_EQ(document_count * words_per_document, emitted_rows);
  EXPECT_EQ(batch_capacity + words_per_document, max_buffered_rows);
}

TEST_F(FTSWordRowBufferTest, batch_does_not_cross_source_task_boundaries)
{
  FTSWordRowBuffer buffer;
  std::string first_id_value = "task-zero";
  std::string second_id_value = "task-one";
  ObDatum first_id = doc_id(first_id_value.c_str());
  ObDatum second_id = doc_id(second_id_value.c_str());
  ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, first_id,
                                      ObString::make_string("aaa bbb ccc"), true, 0));
  ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, first_id,
                                      ObString::make_string("ddd eee"), true, 0));
  ASSERT_EQ(OB_SUCCESS, buffer.append(allocator_, &helper_, meta_, second_id,
                                      ObString::make_string("fff ggg hhh iii"), true, 1));

  const blocksstable::ObStorageDatum *rows = nullptr;
  int64_t count = 0;
  int64_t source_task_idx = OB_INVALID_INDEX;
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, 10, &source_task_idx));
  EXPECT_EQ(5, count);
  EXPECT_EQ(0, source_task_idx);
  ASSERT_EQ(OB_SUCCESS, buffer.get_next_batch(rows, count, 10, &source_task_idx));
  EXPECT_EQ(4, count);
  EXPECT_EQ(1, source_task_idx);
}

} // namespace
} // namespace sql
} // namespace oceanbase
