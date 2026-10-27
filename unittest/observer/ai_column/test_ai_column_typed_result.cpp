// Standalone behavior coverage; compile with -Isrc and the repository rapidjson include path.
#include "observer/ai_service/ai_column/ob_ai_column_typed_result.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using oceanbase::observer::ai_column::AiColumnTypedResult;
static int checks = 0;
static void check(const char *spec, const std::string &value, bool expected)
{
  ++checks;
  if (AiColumnTypedResult::validate(spec, value) != expected) {
    std::fprintf(stderr, "FAIL spec=%s value=%s expected=%d\n", spec, value.c_str(), expected);
    std::exit(1);
  }
}
int main()
{
  check("json", "not json", false);
  check("json", "{\"a\":[true,null,7,\"hello\"]}", true);
  check("json", "7", true);
  check("json", "{\"a\":1} trailing", false);
  check("json", "[NaN]", false);
  check("json", std::string("{}\0junk", 7), false);
  check("json", std::string(101, '[') + "0" + std::string(101, ']'), false);
  check("int:64", "9223372036854775807", true);
  check("int:64", "-9223372036854775808", true);
  check("int:64", "9223372036854775808", false);
  check("int:64", "-9223372036854775809", false);
  check("uint:64", "18446744073709551615", true);
  check("uint:64", "18446744073709551616", false);
  check("uint:64", "-1", false);
  check("int:8", "127", true);
  check("int:8", "128", false);
  check("int:8", "-128", true);
  check("int:24", "8388608", false);
  check("int:32", "2147483648", false);
  check("int:64", "1.0", false);
  check("int:64", "1e3", false);
  check("int:64", " 7\n", true);
  check("int:64", "", false);
  check("float", "3.4e38", true);
  check("float", "3.5e38", false);
  check("double", "1.7e308", true);
  check("double", "1e309", false);
  check("double", "NaN", false);
  check("double", "Infinity", false);
  check("double", "1 garbage", false);
  check("decimal:5:2", "999.99", true);
  check("decimal:5:2", "1000.00", false);
  check("decimal:5:2", "0.001", false);
  check("decimal:5:2", "-001.20", true);
  check("decimal:2:2", "0.12", true);
  check("decimal:2:2", "1.12", false);
  check("decimal:65:0", std::string(65, '9'), true);
  check("decimal:65:0", std::string(66, '9'), false);
  check("vector:3", "[1,2.5,-3e2]", true);
  check("vector:3", "[1,2]", false);
  check("vector:3", "[1,2,3,4]", false);
  check("vector:3", "[1,\"2\",3]", false);
  check("vector:3", "[1,[2],3]", false);
  check("vector:3", "[1,null,3]", false);
  check("vector:3", "[1,1e39,3]", false);
  check("vector:3", "[1,NaN,3]", false);
  check("vector:3", "{\"a\":3}", false);
  check("varchar", "unmodified ' text", true);
  check("unknown", "7", false);
  if (std::string(AiColumnTypedResult::instruction("json")).find("valid JSON value") == std::string::npos ||
      std::string(AiColumnTypedResult::instruction("int:64")).find("base-10 integer") == std::string::npos) return 2;
  std::printf("PASS %d typed-result behavior cases and system instructions\n", checks);
}
