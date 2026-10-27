# SeekDB 2026 竞赛分支：FileSQL / COPY / AI COLUMN 交付与复现说明

本文件是 `competition/2026-ai-file-sql-complete` 分支的唯一验证说明，所有结论都来自
**macOS arm64 单机上对 release 二进制的真实实例执行**，命令可直接复现。未在 Linux / CI
上验证，未做性能评估。

本文中的 `$WORKTREE` 指分支工作树根目录：

```bash
export WORKTREE=$(git rev-parse --show-toplevel)
```

## 1. 交付范围与提交

分支 `competition/2026-ai-file-sql-complete`，起点 `76f871eb5`；未合并其他分支、未 push、
未改写既往提交。集成提交（`git log --oneline -1` 可查 hash，可用 `git show --stat HEAD`
核对范围）新增/修改 202 个文件、+27289/-174 行：

| 区域 | 文件数 | 内容 |
|---|---|---|
| `src/**` | 97 | 文件表函数（CSV/JSON/NDJSON/Parquet/glob）、`COPY ... TO`、`DESCRIBE <select>`、AI COLUMN 全链路、`max_concurrency` 端点参数、错误码/会话变量/内表 schema |
| `unittest/**` | 35 | AI COLUMN 运行时与静态不变量、file SQL 读取/导出/glob、parser 与 resolver 原生用例、公开 `file_sql_basic` 的本地 runner |
| `tools/deploy/mysql_test/test_suite/**` | 69 | `ai_column`、`ai_function`、`file_sql` 三套用例（`.test`/`.result`/`data`/`include`/mock） |
| `docs/**` | 1 | 本文件 |

两族能力：

- **文件 SQL**：`read_csv` / `read_json` / `read_ndjson` / `read_parquet` / `glob` 表函数（语法白名单门控）、`COPY ... TO` 流式 CSV 导出、`DESCRIBE <select>` 列元数据。
- **AI COLUMN**：`CREATE/ALTER TABLE ... AI COLUMN`、`ADD AI COLUMN`、`SHOW`/`DESCRIBE` 元数据、事务内入队、异步 worker、`RETRY AI COLUMN`、typed result 落盘、写守卫（普通 DML 不得写 AI 列）、部分刷新、发布原子性、`ai_column_strict_read` 会话变量。

## 2. 环境与前提

- **平台/构建**：macOS arm64；CMake `RelWithDebInfo`（`./build.sh release`），产物 `build_release/src/observer/seekdb`，单机单实例。
- **客户端**：`mysqltest` 来自 MySQL 客户端安装或 obclient 包。已知差异：
  - MySQL 9.x 客户端已移除 `mysql_native_password`，连接带密码用户报 `CR_AUTH_PLUGIN_CANNOT_LOAD(2059)`；`ai_model_endpoint_ddl`、`ai_column_security_regression` 两个用例必须用带该插件的客户端（MySQL 8.4 客户端，或 obclient 包内的 `mysqltest`）。
  - 公开 `ai_column` 用例使用 `--real_sleep`，本地 Homebrew 客户端不认识该命令（见 §4.2）。
- **实例 fixture**（属环境准备，不属于实现）：公开 `ai_function/t/ai_model_endpoint_ddl.test` 在 line 166/177 用 `admin` 账号连接 `test` 库，需要先准备：

```sql
create database test;
create user 'admin'@'%' identified by '';
grant all privileges on *.* to 'admin'@'%' with grant option;
```

- **内表不手工建**：新实例由 bootstrap 生成 `oceanbase.__all_ai_column`、`__all_ai_column_aux_lob_*`、`oceanbase.__all_ai_column_task`（含 `rowkey_predicate`/`request_id`/`state`/`retry_count`/`definition_version`/`lease_*`/`result_*` 等列）与 `__all_ai_model_endpoint.max_concurrency`；`DBA_OB_AI_MODEL_ENDPOINTS` 尾列为 `MAX_CONCURRENCY`。任何 `ALTER` 内表的做法都不属于验证流程。
- **`secure_file_priv`**：公开 `file_sql_basic` runner 需要 `'/'`；`file_sql_surface_regression` 用例内自查 `/tmp` 并在结束时恢复 `'/'`。修改该 GLOBAL 变量必须走 unix socket（TCP 报 `ER_NOT_SUPPORTED_YET(1235): modify SECURE_FILE_PRIV not by unix socket connection not supported`）。macOS `sun_path` 上限 104 字节，实例 socket 绝对路径通常超长，需要短符号链接：

```bash
ln -sfn "$WORKTREE/build_release/<runtime>/run/sql.sock" /tmp/sdb.sock
```

- **cwd**：套件用例默认 cwd=`$WORKTREE/tools/deploy`（用例内路径都相对它）；`file_sql_formats_regression` 例外，cwd=`$WORKTREE`（用例以仓库根相对路径引用 `data/*.b64`）。
- **mock**：AI 用例自带 `test_suite/ai_column/include/*mock.py`，在 19121–19136 / 19229 上由用例自身 `start/stop`。跑前须确认这些端口无残留监听、状态目录无陈旧 pidfile（本机曾因其它会话遗留 mock 造成假失败，属环境问题）。

## 3. 构建与启动

```bash
cd "$WORKTREE"
./build.sh release --init                        # 首次；后续可只做下一行
cmake --build build_release --target seekdb -j2   # 增量

build_release/src/observer/seekdb --nodaemon \
  --base-dir build_release/local-runtime --port 12891 \
  --parameter rpc_port=12892 --parameter memory_budget=2G \
  --parameter datafile_size=1G --parameter datafile_maxsize=2G \
  --parameter log_disk_size=2G
```

就绪判定：日志出现 `5.7.25-OceanBase seekdb-v1.4.0.0` 版本行；`stat -f %Sm build_release/src/observer/seekdb` 的 mtime 必须晚于最后一次源码改动。

## 4. 用例运行方式

### 4.1 mysqltest 单项（普通用例）

```bash
cd "$WORKTREE/tools/deploy"
mysqltest -h127.0.0.1 -P12891 -uroot --database=oceanbase \
  --test-file=mysql_test/test_suite/ai_column/t/ai_column_ddl.test \
  --result-file=mysql_test/test_suite/ai_column/r/mysql/ai_column_ddl.result
```

判定标准：成功打印 `ok` 且 exit 0；不一致打印 `not ok` 且 exit 1（并在 cwd 写 `*.reject`/`*.log`）。**只以 exit code + `ok/not ok` 判定，不以"没有报错输出"当通过。**

`--real_sleep` 用例（`ai_column_retry_strict`、`ai_column_t1_dml`、`ai_column_vector_index`、`ai_column_worker`）改用 obclient 包内 `mysqltest`；需要 `mysql_native_password` 的 `ai_column_security_regression` 同理。

### 4.2 公开 ai_column 用例的客户端兼容跑法

上面 4 个公开用例用 `--real_sleep N` 轮询 worker 状态，而本地 Homebrew 9.x / MySQL 8.4 的
`mysqltest` 不支持该命令（`Found line 'real_sleep 0.1' ... didn't contain a valid mysqltest
command`）。仓库中这些用例保持**上游原始字节**；本地执行时复制到临时目录并只把
`--real_sleep N` 改写为等价的 `--sleep N`，断言、SQL 与 oracle 一律不动：

```bash
cd "$WORKTREE/tools/deploy"
sed 's/--real_sleep /--sleep /' \
  mysql_test/test_suite/ai_column/t/ai_column_worker.test > /tmp/ai_column_worker.test
mysqltest -h127.0.0.1 -P12891 -uroot --database=oceanbase \
  --test-file=/tmp/ai_column_worker.test \
  --result-file=mysql_test/test_suite/ai_column/r/mysql/ai_column_worker.result
```

上游 CI 使用支持 `--real_sleep` 的客户端，因此仓库内不需要任何适配。

### 4.3 公开 `file_sql_basic`（集群部署 include → 本地 fixture）

```bash
python3 "$WORKTREE/unittest/sql/engine/basic/run_native_file_sql_mysqltest.py" --port 12891
```

该 runner 仅把用例里的两处集群部署 include 换成本地 fixture（`let $OBSERVER_DIR` 指向临时
目录），公共 SQL 与 oracle 不变；要求 `secure_file_priv='/'`。

### 4.4 单测与静态不变量

```bash
for t in test_ai_column_worker_invariants test_ai_column_service_isolation \
         test_ai_argument_contract test_ai_column_dml_safety test_barrier_mock_lifecycle; do
  python3 "$WORKTREE/unittest/observer/ai_column/$t.py" || echo "FAILED $t"
done
python3 "$WORKTREE/unittest/observer/ai_column/run_sql_builder_test.py"
python3 "$WORKTREE/unittest/sql/parser/run_native_file_parser_test.py"
python3 "$WORKTREE/unittest/sql/parser/run_native_ai_column_parser_test.py"
python3 "$WORKTREE/unittest/sql/resolver/run_native_ai_column_definition_test.py"
python3 "$WORKTREE/unittest/sql/resolver/run_native_ai_column_schema_test.py"
python3 "$WORKTREE/unittest/sql/resolver/run_native_ai_column_write_guard_test.py"
```

`test_ai_column_worker_invariants.py`（55 项）、`test_ai_column_service_isolation.py`（92 项）、
`test_ai_argument_contract.py`（5 项）、`test_ai_column_dml_safety.py`（4 项）是**静态源码
断言**，只证明代码结构符合约束，不构成运行时证据；它们的敏感性可用「对改动前源码跑同一
脚本必失败」自证（例：把 `execute_write()` 的日志改回打印整条 SQL，worker invariants 会
报 2 条 FAIL）。

### 4.5 门禁

```bash
cd "$WORKTREE"
git diff --check
python3 tools/module_check/sql_header_ownership_check.py
python3 tools/module_check/sql_header_ownership_check.py --strict-deps
```

## 5. 最终验证矩阵

release 二进制 192,943,992 字节，mtime 晚于全部源码改动；实例 12891（自有新 catalog，
未手工改内表）。下表为本轮实跑结果，`log` 列为逐项 mysqltest 输出（`ok`/exit 0）。

### 5.1 主台账（26 项，全部 PASS）

| # | 套件 | 用例 | 结果 |
|---|---|---|---|
| 1 | 公开 | `file_sql_basic`（经 §4.3 runner） | PASS |
| 2–7 | `ai_function` | `ai_prompt_regression`、`ai_complete_regression`、`ai_embed_regression`、`ai_ollama_regression`、`ai_embed_batch_regression`、`ai_rerank_regression` | 6/6 PASS |
| 8–18 | `ai_column` | `ai_column_ddl`、`ai_column_t1_dml`、`ai_column_worker`、`ai_column_retry_strict`、`ai_column_vector_index`、`ai_column_partial_refresh`、`ai_column_write_guard`、`ai_column_write_guard_views`、`ai_column_transaction_atomicity`、`ai_column_argument_contract`、`ai_column_strict_procedure` | 11/11 PASS |
| 19–26 | `file_sql` | `file_sql_parser`、`file_sql_csv_regression`、`file_sql_datetime_regression`、`file_sql_describe_regression`、`file_sql_glob_regression`、`file_sql_formats_regression`、`file_sql_security_regression`、`file_sql_surface_regression` | 8/8 PASS |

其中 5 个公开用例（`ai_column_ddl`、`ai_column_retry_strict`、`ai_column_t1_dml`、
`ai_column_vector_index`、`ai_column_worker`）按 **§4.2** 用上游原始字节 + 临时
`--real_sleep`→`--sleep` 改写复核，均 PASS。

### 5.2 补充用例与探针（不计入 26 项台账）

| 用例 | 用途 | 结果 |
|---|---|---|
| `ai_function/ai_model_endpoint_ddl` | `max_concurrency` 公开用例（需 8.4/obclient 客户端 + §2 fixture） | PASS |
| `ai_column/ai_column_security_regression` | RETRY 用户谓词越权与 MODIFY/CHANGE 拒绝 | PASS |
| `ai_column/ai_column_generated_key` | AUTO_INCREMENT 生成主键的行仍入队、按生成值建任务、最终发布 | PASS |
| `ai_column/ai_column_key_identity` | 任务行身份（自增探针，手写 oracle） | PASS |
| `ai_column/ai_column_quoted_key` | 引号/大小写敏感主键的任务身份（自增探针，手写 oracle） | PASS |
| `ai_column/ai_column_alter_regression` | `ALTER TABLE` 元数据回归 | PASS |
| `ai_column/ai_column_catalog_regression` | 目录一致性回归 | PASS |
| `ai_column/ai_column_ordinary_sql_regression` | 普通 SQL 不受 AI 列逻辑影响 | PASS |
| `ai_column/ai_column_session_settings` | 会话变量行为 | PASS |
| `ai_column/ai_column_strict_regression` | `ai_column_strict_read` 读路径 | PASS |
| `file_sql/file_sql_settings` | 文件 SQL 相关设置项（自增探针，手写 oracle） | PASS |

### 5.3 构建、静态检查与门禁

| 项 | 结果 | 说明 |
|---|---|---|
| `cmake --build build_release --target seekdb -j2` | PASS | exit 0，无 `error:` |
| `git diff --check`（工作树） | PASS | exit 0，无输出 |
| `git diff --cached --check`（staged 内容） | 仅 5 行 `.result` 尾随空白 | 见 §8 第 11 条（属期望输出，必须保留） |
| `sql_header_ownership_check.py` | PASS | 1280 tracked / 153 public roots / 256 interface closure / 4 composition / 867 private；upper-layer 0，non-downward 0 |
| 同上 `--strict-deps` | PASS | 同上，`[FAIL]` 0 行 |
| 静态不变量/单测 | PASS | 55 + 92 + 5 + 4 + `barrier_mock_lifecycle` + `run_sql_builder_test.py`（native，链接已构建静态库） |
| 用例真实性负控 | PASS | 在 `ai_complete_regression.result` 与 `file_sql_csv_regression.result` 各追加一行伪造内容后重跑，均 `not ok`/exit 1，证明比较逻辑真实生效 |

### 5.4 由前次会话取得、本轮未重跑的慢速证据

- **ASan/UBSan**：`file_sql_glob`、`file_sql_export`、`json_file_reader`、`parquet_file_reader` 四个新增 CTest 目标在消毒器构建下通过。
- **真实进程 crash recovery**：`unittest/observer/ai_column/test_ai_column_crash_recovery.py` 以 SIGKILL 校验任务不丢、精确上游调用次数。

两者属前一轮记录，本轮未重复执行，也未据此扩大任何声明。

## 6. 关键实现锚点

| 能力 | 位置 |
|---|---|
| 文件表函数语法门控 | `src/sql/parser/sql_parser_mysql_mode.y`（5 个名字通过，否则 `unknown file table function`）、`src/sql/parser/non_reserved_keywords_mysql_mode.c` |
| 表函数计划与扫描 | `src/sql/engine/basic/ob_function_table_op.cpp`、`file_sql_scan.cpp`、`file_scan_utils.cpp` |
| 路径安全与 glob | `src/sql/engine/basic/file_scan_utils.cpp`、`file_sql_glob.cpp` |
| JSON/NDJSON/Parquet 读取（含内存预算） | `src/sql/engine/basic/json_file_reader.cpp`、`parquet_file_reader.cpp` |
| `COPY ... TO` 流式导出 | `src/sql/engine/basic/file_sql_export.cpp`、`ob_select_into_op.cpp`、`src/sql/optimizer/ob_log_select_into.h` |
| `DESCRIBE <select>` | `src/sql/optimizer/ob_explain_log_plan.cpp`、`src/sql/resolver/ddl/ob_explain_resolver.cpp` |
| AI COLUMN 服务与 worker | `src/observer/ai_service/ai_column/ob_ai_column_service.cpp`、`ob_ai_column_worker.cpp`（`src/observer/ob_server.cpp` 注册） |
| `RETRY AI COLUMN` 与调用者权限 | `ob_ai_column_retry_runner.cpp`（`AiColumnCallerPrivGuard`）、`src/sql/engine/cmd/ob_ai_column_retry_executor.cpp` |
| AI COLUMN DDL/元数据 | `src/sql/resolver/ddl/ob_ai_column_resolver.cpp`、`src/share/schema/ob_ai_column_definition.h`、`src/share/schema/ob_table_sql_service.cpp` |
| 写守卫 | `src/sql/resolver/dml/ai_column_write_guard.h`（update/insert resolver 接入） |
| DML 入队钩子 | `src/sql/engine/dml/ob_table_modify_op.cpp`（`ai_column_enabled_`、行主键谓词构造） |
| 会话变量 `ai_column_strict_read` | `src/share/system_variable/ob_system_variable_init.json` |
| 端点 `max_concurrency` | `src/share/ai_service/ob_ai_service_struct.{h,cpp}`、`src/observer/ai_service/ob_ai_service_executor.cpp`、`src/share/inner_table/ob_inner_table_schema_def.py` |

## 7. 历史疑点的处置结论

1. **端点 `max_concurrency`（此前"未实现"）**：已实现并验证——`CREATE`/`ALTER` 接受 1–1024，非法值（1025、0、字符串、null、小数、布尔）统一报 `ERROR 11116: The value of param 'max_concurrency' is invalid`，失败不污染原值，省略参数保留原值；目录列与 `DBA_OB_AI_MODEL_ENDPOINTS.MAX_CONCURRENCY` 由 bootstrap 生成。公开用例 `ai_model_endpoint_ddl` PASS。**限制：仅元数据校验/持久化/视图，未接入实际请求限流。**
2. **`RETRY AI COLUMN ... WHERE` 用户谓词越权（此前 Blocker）**：已修。用户谓词求值阶段用 `AiColumnCallerPrivGuard` 临时打开 `set_check_priv(true)` 并恢复；枚举任务的 SELECT 去掉 `FOR UPDATE`（否则只有 ALTER+SELECT 权限的调用者连正常 RETRY 都会报 1142）。实测：exists / in 子查询 → 1142；正常 `where id=1` 成功；仅 ALTER 权限的调用者在 RETRY 阶段同样 1142。
3. **`MODIFY`/`CHANGE COLUMN` 重定义 AI 列（此前会静默不同步目录）**：现在显式拒绝，`OB_NOT_SUPPORTED` → `ERROR 1235 (0A000)`，报文说明不支持用 MODIFY/CHANGE 重定义 AI 结果列或用其把普通列转成 AI 列；实测拒绝后目录行数、`definition_version`、隐藏列与数据均不变。普通列 `MODIFY`、`ADD` 普通列、`ADD` AI 列不受影响。
4. **AUTO_INCREMENT 行不入队（此前疑点）**：不成立。`ai_column_generated_key` 断言：语句未写主键值时，任务行 key 等于执行器为该行生成的值、在语句事务内即可见、最终发布进本行；实测 PASS。实现上，行主键谓词由行表达式里**已生成**的主键值构造（`ob_table_modify_op.cpp` 的 `ai_column_build_rowkey()`）。
5. **"隐式主键/无主键表"说法**：不按推测表述。`ob_table_modify_op.cpp` 对 `is_table_without_pk_` 的表直接跳过，即**无主键表不在 AI COLUMN 支持范围**，本分支未覆盖、也不声称支持；有主键表（含 AUTO_INCREMENT 生成值、多列主键、带引号/大小写敏感主键）由上述用例覆盖。
6. **`file_sql_csv_regression` oracle 调整**：自增用例 `file_sql_csv_regression.result` 的 `created` 列有 3 行去掉了 `.000000`（输入为整秒时间，按实现的时间精度推断语义对齐）。只改这 3 行，其余字节不变；未改动任何上游公开 oracle。

## 8. 已知限制与未验证项（必须如实保留）

1. `max_concurrency` 仅做 DDL 校验、持久化与视图展示，**未接入实际请求限流**；`0` 表示"未设置"与显式 0 不可区分；旧 catalog 数据原地升级路径未验证；未发真实模型请求。
2. **单 worker 顺序消费**，未做并发/性能评估，无吞吐、延迟或扩展性结论。
3. `MODIFY` / `CHANGE` 重定义 AI 列不支持（显式 1235）；这是设计选择，不是遗漏。
4. `ADD AI COLUMN` 不回填历史行：既有行需要后续 `UPDATE` 或 `RETRY` 才产生结果。
5. 验证平台仅 **macOS arm64 单机**；**Linux / CI / 多副本 / 混合版本 / 共享存储均未验证**。
6. **未执行的公开用例**：`ai_function/t/{ai_model_ddl, ai_model_lower_case_name_mysql0, ai_model_lower_case_name_mysql1, ai_prompt, ai_function_enhancement}`。它们随用例文件一并入库，但本轮未运行，**不得称为 PASS**。
7. RETRY 的任务枚举不再加行锁：并发下同一任务可能被重复执行一次；发布正确性由 request id 水位与 `definition_version` 保证。
8. 三个未运行的 unittest 草案（`unittest/sql/engine/expr/test_ob_openai_utils.cpp`、`unittest/sql/resolver/ddl/test_ob_ai_column.cpp`、`unittest/sql/parser/test_file_sql_parser.cpp`）未纳入提交，不构成测试证据。
9. 本地客户端不支持 `--real_sleep`，公开 ai_column 用例按 §4.2 适配后才可运行；仓库内文件保持上游原始字节。
10. 未验证旧版本数据/目录的原地升级与降级兼容性。
11. 提交时 `git diff --cached --check` 仅在 2 个 `.result` 上报尾随空白（`ai_column_transaction_atomicity.result` 4 行、上游公开 `ai_column_vector_index.result` 1 行），它们是 `show columns ...`/执行计划输出的期望内容，**必须保留**（实测去掉后该用例立即 `not ok`）。

## 9. 明确不作声称

- 不声称隐藏评测集通过率：本文只覆盖上表列出的 26 项主台账 + 11 项补充用例 + 静态检查与门禁。
- 不声称跨平台、多副本、升级兼容、性能或容量特性成立。
- 不把静态源码断言、`--help` 文案匹配、未运行的草案当执行证据。
- 不声称 `ai_function_enhancement` 等未执行用例通过。

## 10. 复现顺序（一条链）

```bash
export WORKTREE=$(git rev-parse --show-toplevel)
cd "$WORKTREE" && cmake --build build_release --target seekdb -j2
build_release/src/observer/seekdb --nodaemon --base-dir build_release/local-runtime \
  --port 12891 --parameter rpc_port=12892 --parameter memory_budget=2G   # 见 §3
mysql -h127.0.0.1 -P12891 -uroot -e "create database test; create user 'admin'@'%' identified by ''; grant all privileges on *.* to 'admin'@'%' with grant option;"
python3 unittest/sql/engine/basic/run_native_file_sql_mysqltest.py --port 12891   # 主台账 #1
# 套件用例：按 §4.1 / §4.2 逐项跑；补充用例按 §5.2
python3 unittest/observer/ai_column/test_ai_column_worker_invariants.py            # §4.4
python3 tools/module_check/sql_header_ownership_check.py --strict-deps             # §4.5
```
