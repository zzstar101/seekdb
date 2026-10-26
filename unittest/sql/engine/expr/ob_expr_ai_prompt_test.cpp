/*
 * Copyright (c) 2026 OceanBase.
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
#include "lib/allocator/ob_allocator.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace {
class AIPromptTest : public ::testing::Test
{
protected:
  int render(const char *json, ObString &result)
  {
    ObString input = ObString::make_string(json);
    ObJsonObject *prompt = nullptr;
    int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, prompt);
    if (OB_SUCCESS == ret) {
      ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, result);
    }
    return ret;
  }
  ObArenaAllocator allocator_;
};

TEST_F(AIPromptTest, RepeatedArgumentsAreNotLimitedToSingleOccurrenceCapacity)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(
      R"({"template":"{0}{0}{0}{0}","args":["abcdefghijklmnop"]})", result));
  EXPECT_EQ(ObString::make_string("abcdefghijklmnopabcdefghijklmnopabcdefghijklmnopabcdefghijklmnop"), result);
}

TEST_F(AIPromptTest, ArgumentsAreNotReinterpretedAsTemplateSyntax)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{0}/{1}/{0}","args":["{1}","x"]})", result));
  EXPECT_EQ(ObString::make_string("{1}/x/{1}"), result);
}

TEST_F(AIPromptTest, JsonArraysAndObjectsKeepTheirStructure)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{0}|{1}","args":[[1,true],{"k":2}]})", result));
  EXPECT_EQ(ObString::make_string("[1, true]|{\"k\": 2}"), result);
}

TEST_F(AIPromptTest, NestedPromptRendersBeforeInsertion)
{
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(
      R"({"template":"outer {0}","args":[{"template":"inner {0}","args":["世界"]}]})", result));
  EXPECT_EQ(ObString::make_string("outer inner 世界"), result);
}

TEST_F(AIPromptTest, IndexOverflowMustNotWrapToExistingArgument)
{
  ObString result;
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(
      R"({"template":"{18446744073709551616}","args":["must not substitute"]})", result));
}

TEST_F(AIPromptTest, MalformedPromptTypesAreRejectedBeforeDowncast)
{
  ObString result;
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"({"template":[],"args":[]})", result));
  EXPECT_EQ(OB_INVALID_ARGUMENT, render(R"({"template":"{0}","args":{}})", result));
}
} // namespace
