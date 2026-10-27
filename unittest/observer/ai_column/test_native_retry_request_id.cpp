// Execute the real retry request generator, without schema, SQL or model mocks.
#include <cstdio>
#include <set>
#include <string>
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "observer/ai_service/ai_column/ob_ai_column_task.h"
#include "sql/engine/cmd/ob_ai_column_retry_executor.h"
#define private public
#include "observer/ai_service/ai_column/ob_ai_column_retry_runner.h"
#undef private
int main()
{
  using namespace oceanbase::common;
  using namespace oceanbase::observer::ai_column;
  ObArenaAllocator allocator("AiRetryReqTest");
  AiColumnRetryRunner &runner = AiColumnRetryRunner::get_instance();
  std::set<std::string> ids;
  int failures = 0;
  for (int i = 0; i < 1000; ++i) {
    ObString id = ObString::make_string("old value");
    const int ret = runner.new_request_id(allocator, id);
    if (ret != OB_SUCCESS || id.empty() || id.length() >= AI_COLUMN_REQUEST_ID_LEN) {
      if (failures < 3) std::printf("FAIL retry request %d ret=%d length=%d\n", i, ret, id.length());
      ++failures;
    } else {
      std::string value(id.ptr(), id.length());
      const size_t separator = value.find('$', 4);
      if (value.compare(0, 4, "req$") != 0 || separator == std::string::npos ||
          separator == 4 || separator + 1 == value.size() ||
          value.find_first_not_of("0123456789", 4) != separator ||
          value.find_first_not_of("0123456789", separator + 1) != std::string::npos ||
          !ids.insert(value).second) ++failures;
    }
  }
  std::printf("native retry request IDs: calls=1000 distinct=%zu failures=%d\n", ids.size(), failures);
  return failures == 0 ? 0 : 1;
}
