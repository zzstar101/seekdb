// Exercise the actual service method linked from the kernel, without a server.
#include <cstdio>
#include <set>
#include <string>
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_array.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "observer/ai_service/ai_column/ob_ai_column_task.h"
#include "observer/ai_service/ai_column/ob_ai_column_worker.h"
#include "sql/engine/dml/ob_table_modify_op.h"
// Dependencies are included before exposing only this class's private test seam.
#define private public
#include "observer/ai_service/ai_column/ob_ai_column_service.h"
#undef private
int main()
{
  using namespace oceanbase::common;
  using namespace oceanbase::observer::ai_column;
  ObArenaAllocator allocator("AiRequestTest");
  AiColumnService &service = AiColumnService::get_instance();
  std::set<std::string> ids;
  int failures = 0;
  for (int i = 0; i < 1000; ++i) {
    ObString id;
    const int ret = service.new_request_id(allocator, id);
    if (ret != OB_SUCCESS || id.empty() || id.length() >= AI_COLUMN_REQUEST_ID_LEN) {
      ++failures;
      std::printf("FAIL request %d ret=%d length=%d\n", i, ret, id.length());
    } else {
      std::string value(id.ptr(), id.length());
      if (value.compare(0, 4, "req$") != 0 || !ids.insert(value).second) ++failures;
    }
  }
  std::printf("native AI request IDs: calls=1000 distinct=%zu failures=%d\n", ids.size(), failures);
  return failures == 0 ? 0 : 1;
}
