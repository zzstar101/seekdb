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
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "storage/retrieval/ob_sparse_daat_iter.h"

namespace oceanbase
{
namespace storage
{
namespace
{
using namespace common;

void expect_same_order(ObSRMergeCmp &cmp, const ObDatum &left, const ObDatum &right)
{
  const auto oracle = ObDatumFuncs::get_basic_func(ObVarcharType, CS_TYPE_BINARY)->null_first_cmp_;
  int expected = 0;
  int64_t actual = 0;
  ASSERT_EQ(OB_SUCCESS, oracle(left, right, expected, nullptr));
  ASSERT_EQ(OB_SUCCESS, cmp.cmp_id(left, right, actual));
  EXPECT_EQ((expected > 0) - (expected < 0), actual);
}

TEST(DocIdBinaryComparison, every_differing_byte_and_unaligned_offset)
{
  ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
  ObSRMergeCmp cmp;
  sql::ObDatumMeta meta;
  meta.type_ = ObVarcharType;
  meta.cs_type_ = CS_TYPE_BINARY;
  ASSERT_EQ(OB_SUCCESS, cmp.init(meta, &current_ids));
  for (int offset = 0; offset < 8; ++offset) {
    for (int byte = 0; byte < 16; ++byte) {
      for (int value = 0; value < 256; ++value) {
        std::array<char, 24> left = {};
        std::array<char, 24> right = {};
        std::memset(left.data(), 0x80, left.size());
        std::memset(right.data(), 0x80, right.size());
        right[offset + byte] = static_cast<char>(value);
        ObDatum l;
        ObDatum r;
        l.set_string(left.data() + offset, 16);
        r.set_string(right.data() + offset, 16);
        expect_same_order(cmp, l, r);
        expect_same_order(cmp, r, l);
      }
    }
  }
}

TEST(DocIdBinaryComparison, random_binary_ids_and_length_null_fallbacks)
{
  ObFixedArray<const ObDatum *, ObIAllocator> current_ids;
  ObSRMergeCmp cmp;
  sql::ObDatumMeta meta;
  meta.type_ = ObVarcharType;
  meta.cs_type_ = CS_TYPE_BINARY;
  ASSERT_EQ(OB_SUCCESS, cmp.init(meta, &current_ids));
  uint64_t state = 0x1425364758697a8bULL;
  for (int trial = 0; trial < 10000; ++trial) {
    char bytes[32];
    for (char &byte : bytes) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      byte = static_cast<char>(state);
    }
    ObDatum l;
    ObDatum r;
    l.set_string(bytes, 16);
    r.set_string(bytes + 16, 16);
    expect_same_order(cmp, l, r);
    expect_same_order(cmp, l, l);
  }
  const std::vector<std::string> values = {
      "", "a", std::string(7, '\xff'), std::string(8, '\0'),
      std::string(15, 'a'), std::string(16, 'a'), std::string(17, 'a'),
      std::string(32, '\xff')};
  for (const auto &left : values) {
    for (const auto &right : values) {
      ObDatum l;
      ObDatum r;
      l.set_string(left.data(), left.size());
      r.set_string(right.data(), right.size());
      expect_same_order(cmp, l, r);
      l.set_null();
      expect_same_order(cmp, l, r);
      expect_same_order(cmp, r, l);
      r.set_null();
      expect_same_order(cmp, l, r);
    }
  }
}
} // namespace
} // namespace storage
} // namespace oceanbase
