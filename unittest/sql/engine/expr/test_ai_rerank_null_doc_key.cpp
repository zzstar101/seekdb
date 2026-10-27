/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Run with run_ai_rerank_null_doc_key_test.py, which supplies the exact production
// eval function as eval_under_test.inc. This tests dispatch, not full SQL/HTTP.
#include <iostream>
#include <string>
#include <vector>

// Dependency doubles for executing the unchanged production eval function.
// SQL parsing, networking, allocation and JSON serialization are outside this
// focused test; datum pointers follow ObExpr::eval_param_value's real contract.
#define INIT_SUCC(name) int name = OB_SUCCESS
#define OB_FAIL(value) ((ret = (value)) != OB_SUCCESS)
#define OB_SUCC(value) ((value) == OB_SUCCESS)
#define OB_ISNULL(value) ((value) == nullptr)
#define OB_NOT_NULL(value) ((value) != nullptr)
#define LOG_USER_ERROR(...) ((void)0)
#define N_AI_RERANK "ai_rerank"

namespace oceanbase {
namespace common {
constexpr int OB_SUCCESS = 0;
constexpr int OB_INVALID_ARGUMENT = -4002;
constexpr int OB_ERR_UNEXPECTED = -4016;
using ObString = std::string;
struct ObIAllocator {};
struct MultimodeAlloctor : ObIAllocator {
  explicit MultimodeAlloctor(ObIAllocator &) {}
};
struct CallState {
  int unkeyed = 0;
  int keyed = 0;
  int metadata = 0;
  int null_reads = 0;
};
inline CallState calls;
struct ObDatum {
  bool null = false;
  ObString text;
  bool is_null() const { return null; }
  void set_null() { null = true; }
  ObString get_string() const
  {
    if (null) ++calls.null_reads;
    return text;
  }
};
enum class ObJsonNodeType { J_ARRAY, J_OBJECT, J_STRING };
struct ObIJsonBase {
  virtual ~ObIJsonBase() = default;
  virtual ObJsonNodeType json_type() const = 0;
};
struct ObJsonNode : ObIJsonBase {
  explicit ObJsonNode(ObJsonNodeType type) : type_(type) {}
  ObJsonNodeType json_type() const override { return type_; }
  ObJsonNodeType type_;
};
struct ObJsonArray : ObJsonNode {
  ObJsonArray() : ObJsonNode(ObJsonNodeType::J_ARRAY) {}
  std::vector<ObJsonNode *> nodes;
  int64_t element_count() const { return static_cast<int64_t>(nodes.size()); }
  ObJsonNode *get_value(int64_t index) { return nodes.at(index); }
  int get_raw_binary(ObString &out, ObIAllocator *)
  {
    out = "unkeyed result";
    return OB_SUCCESS;
  }
};
}
namespace share {
struct ObAiModelEndpointInfo {};
template<class T> T *server_service()
{
  static T service;
  return &service;
}
}
namespace query {
struct ObIAiEndpointResolver {
  int resolve_by_model_name(const common::ObString &, common::ObIAllocator &,
                            share::ObAiModelEndpointInfo &)
  { return common::OB_SUCCESS; }
};
}
namespace lib {
struct ObMemAttr { explicit ObMemAttr(const char *) {} };
struct ObMallocHookAttrGuard { explicit ObMallocHookAttrGuard(ObMemAttr) {} };
}
namespace sql {
using namespace common;
struct ObEvalCtx {
  ObJsonArray *documents = nullptr;
  struct TempAllocGuard {
    explicit TempAllocGuard(ObEvalCtx &) {}
    ObIAllocator &get_allocator() { return allocator; }
    ObIAllocator allocator;
  };
};
struct ObExpr {
  std::vector<ObDatum *> args;
  int eval_param_value(ObEvalCtx &, ObDatum *&model, ObDatum *&query,
                       ObDatum *&documents, ObDatum *&key) const
  {
    ObDatum **destinations[] = {&model, &query, &documents, &key};
    for (size_t i = 0; i < args.size(); ++i) *destinations[i] = args[i];
    return OB_SUCCESS;
  }
};
struct ObAIFuncExprInfo {};
struct ObAIFuncUtils {
  static int get_ai_func_info(ObIAllocator &, const ObString &, ObAIFuncExprInfo *&info)
  {
    static ObAIFuncExprInfo value;
    info = &value;
    ++calls.metadata;
    return OB_SUCCESS;
  }
};
struct ObJsonExprHelper {
  static int get_json_doc(const ObExpr &, ObEvalCtx &ctx, ObIAllocator &, int,
                          ObIJsonBase *&document, bool &is_null)
  {
    document = ctx.documents;
    is_null = false;
    return OB_SUCCESS;
  }
  static int pack_json_str_res(const ObExpr &, ObEvalCtx &, ObDatum &result,
                               const ObString &text)
  {
    result.null = false;
    result.text = text;
    return OB_SUCCESS;
  }
};
struct ObAIFuncModel {
  ObAIFuncModel(ObIAllocator &, ObAIFuncExprInfo &, const share::ObAiModelEndpointInfo &) {}
  int call_rerank(const ObString &, ObJsonArray *documents, ObJsonArray *&results)
  {
    ++calls.unkeyed;
    results = documents;
    return OB_SUCCESS;
  }
};
struct ObExprAIRerank {
  static constexpr int DOCUMENTS_IDX = 2;
  static int eval_ai_rerank(const ObExpr &, ObEvalCtx &, ObDatum &);
  static int eval_ai_rerank_with_doc_key(const ObExpr &, ObEvalCtx &, ObIAllocator &,
      ObString &, ObString &, ObJsonArray *, ObString &key, const ObAIFuncExprInfo &,
      const share::ObAiModelEndpointInfo &, ObDatum &result)
  {
    ++calls.keyed;
    result.text = "keyed result";
    return key == "text" ? OB_SUCCESS : OB_INVALID_ARGUMENT;
  }
};
}
}

#include "eval_under_test.inc"

using namespace oceanbase::common;
using namespace oceanbase::sql;

int main()
{
  int failures = 0;
  int cases = 0;
  ObDatum model{false, "arbitrary-model"};
  ObDatum query{false, "query"};
  ObDatum documents{false, "documents"};
  ObDatum null_value{true, ""};
  ObDatum text_key{false, "text"};
  ObJsonNode string(ObJsonNodeType::J_STRING);
  ObJsonNode object(ObJsonNodeType::J_OBJECT);
  ObJsonArray strings, objects;
  strings.nodes = {&string, &string};
  objects.nodes = {&object, &object};

  const auto run = [&](const char *name, std::vector<ObDatum *> args, ObJsonArray &array,
                       int expected_ret, int expected_unkeyed, int expected_keyed) {
    ++cases;
    calls = CallState{};
    ObExpr expr{args};
    ObEvalCtx ctx{&array};
    ObDatum result;
    const int actual_ret = ObExprAIRerank::eval_ai_rerank(expr, ctx, result);
    if (actual_ret != expected_ret || calls.unkeyed != expected_unkeyed
        || calls.keyed != expected_keyed || calls.null_reads != 0) {
      std::cerr << name << ": ret=" << actual_ret << " expected=" << expected_ret
                << " unkeyed=" << calls.unkeyed << " keyed=" << calls.keyed
                << " null_reads=" << calls.null_reads << "\n";
      ++failures;
    }
  };
  run("omitted key selects strings", {&model, &query, &documents}, strings,
      OB_SUCCESS, 1, 0);
  run("SQL NULL key selects strings", {&model, &query, &documents, &null_value}, strings,
      OB_SUCCESS, 1, 0);
  run("non-NULL key selects objects", {&model, &query, &documents, &text_key}, objects,
      OB_SUCCESS, 0, 1);
  run("SQL NULL key rejects objects without reading NULL", {&model, &query, &documents, &null_value}, objects,
      OB_INVALID_ARGUMENT, 0, 0);
  run("omitted key rejects objects", {&model, &query, &documents}, objects,
      OB_INVALID_ARGUMENT, 0, 0);
  run("non-NULL key rejects strings", {&model, &query, &documents, &text_key}, strings,
      OB_INVALID_ARGUMENT, 0, 0);
  run("required model NULL rejects", {&null_value, &query, &documents, &null_value}, strings,
      OB_INVALID_ARGUMENT, 0, 0);
  run("required query NULL rejects", {&model, &null_value, &documents, &null_value}, strings,
      OB_INVALID_ARGUMENT, 0, 0);
  run("required documents NULL rejects", {&model, &query, &null_value, &null_value}, strings,
      OB_INVALID_ARGUMENT, 0, 0);
  std::cout << "actual eval_ai_rerank function: " << cases - failures << "/" << cases << " passed\n";
  return failures ? 1 : 0;
}
