# seekdb 全文索引性能优化：第二轮

## 结论

- 起点：`perf/fts-large-bench-optimize` 的 `962a9f91032ec23e43cf63b0537045e1515afab6`。用户反馈上一轮统一评测为 **FTS raw score 22.542/100**；没有取得该次评测的九项明细，本轮也没有新的统一 CI 分数。
- 实现两处等价热路径优化：二进制文档 ID 比较避免通用字符集分发；BENG 对已经归一化的小写 ASCII/数字 token 避免重复 lowercase 分配。没有修改分词规则、BM25 公式、倒排内容或测试/评分逻辑，也没有缓存查询结果。
- 完成 **8 次原版全量基准**：A/B/B/A 与反向 B/A/A/B；A 为上一轮提交，B 为本轮候选。LIMIT 查询分别下降 **9.44% / 7.63%**，合并平均下降 **8.55%**，是本轮最可重复的收益。
- 三类等权的观测综合下降为 **3.19%**，但两组分别是 **5.63% / 0.45%**，且未改动的 IK 分词也出现明显波动。**不能把 3.19% 全部归因于代码，更不能换算成新的 CI 分数。** 英文查询合并平均退化 **1.18%**，没有截断或隐藏。
- 正确性：同一数据库的 **49,360 行完整结果严格相等**，包括 619 个完整分词 JSON 和查询 ID/相关度/排序页；八次独立建库的完整分词与有序 ID 均一致；最终候选通过 **6 个 mysqltest 用例**。
- 所有原始二进制、采样、报告和诊断脚本在[本地证据目录](<build_fts/>)，不纳入源码提交。本报告保留完整指标与复现方式，不能假定远端能够打开本地证据。

## 1. 定位依据与实现

### 1.1 二进制文档 ID 比较

EXPLAIN 和原生采样确认本次查询主要走 DaaT/BMW，而不是早期怀疑的旧 TAAT 哈希表路径。因此没有修改那条未被本基准使用的 53-bucket 哈希表。

LIMIT 的采样中有 1,065 个活跃检索样本，`advance_to_doc` 下有 84 个落在二进制文档 ID 的通用比较链路。它反复经过 datum 比较、字符集查找、二进制排序规则，再调用字节比较。

[比较辅助函数](<src/storage/retrieval/ob_sparse_retrieval_util.h#L28-L43>) 直接执行 `ObString::compare` 并归一化为 -1/0/1，保留 NULL、扩展 datum 和空指针的原比较器回退。[通用域 ID 入口](<src/storage/retrieval/ob_sparse_retrieval_util.h#L58-L73>) 只在 `ObVarcharType + CS_TYPE_BINARY` 时选用新函数；[DaaT token 迭代器](<src/storage/retrieval/ob_text_retrieval_token_iter.cpp#L434-L443>) 原本就固定按二进制比较，仅为 VARCHAR 替换函数。数值 ID 继续使用原函数。

等价依据：[二进制排序规则](<src/oblib/lib/charset/ob_ctype_bin.cc#L134-L151>) 是 `memcmp(min(lengths))` 后比较长度，保留尾随空格，没有字符转换；[原 datum 比较](<src/share/datum/ob_datum_cmp_func_def.h#L374-L390>) 同样把结果归一化为符号。MIN/MAX 的处理仍保留在原 `ObDomainIdCmp::compare` 中。

### 1.2 BENG 已归一化 token 的分配

BENG 采样确认 `ObBasicEnglishNormalizer::get_next` 重复调用 `ObCharset::tolower` 并分配内存。此前即使 token 已经是小写 ASCII 字母/数字，也会重复复制。

[新分支](<src/share/text_analysis/ob_token_stream.cpp#L296-L315>) 限定 UTF8MB4，逐字节确认全为 `[a-z0-9]` 后借用上游 token；有大写、非 ASCII 或其他字符集则继续走原 `tolower`。没有更改前面的分隔、裁剪、停用词或长度规则。

借用寿命限于下一次 `get_next()/reuse()`。分组阶段仍复制需要保留的词，解析器及流均为调用内状态，不增加共享可变状态或全局结果缓存。分支消除了确定的分配，但其本地端到端收益较小：两组 BENG 分词分别下降 3.03% / 0.00%，不能声称稳定的大幅加速。

## 2. 测量方法和九项结果

- 平台：本地 Darwin ARM64，原生 RelWithDebInfo 构建；同一编译目录、依赖、启动参数。
- 每轮独立新数据目录，专用端口 2888，`memory_limit=2G`、`datafile_size=1G`、`datafile_maxsize=8G`、`log_disk_size=2G`。没有改动原实例 2881/2883。
- 固定参数：`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。
- 五个官方基准文件来自用户提供的[测试提交](https://github.com/oceanbase/seekdb/commit/5bce9e06d1281347ad61344e4b01fab9158c58a2)，逐字节验证未变；八份报告都通过原评分脚本的参数和指标校验。本地硬件与固定基线不同，评分脚本结果不作为正式成绩。
- 基线二进制 SHA-256：`d736e0100c75418d91fb069a824487949310c149883b727fbf6d80ed157ef8b1`。
- 候选二进制 SHA-256：`c206c324f2454a2c0c25eaf58a3ff4003ce53b17562bf7e88b7c14585ea1bbd8`。最终增量构建产物与实测候选逐字节一致。

下表是 **4 次基线与 4 次候选的均值**，不是选取最优一次。构建单位秒，分词/查询单位毫秒。

| 指标 | 基线均值 | 候选均值 | 耗时下降 |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 4.231500 | 4.021250 | 4.97% |
| `build_ik_content_sec` | 3.235250 | 3.131500 | 3.21% |
| `build_beng_en_sec` | 1.782500 | 1.731000 | 2.89% |
| `tokenize_ik_avg_ms` | 0.125750 | 0.118700 | 5.61% |
| `tokenize_beng_avg_ms` | 0.080825 | 0.079575 | 1.55% |
| `query_cn_avg_ms` | 1.608650 | 1.593775 | 0.92% |
| `query_beng_avg_ms` | 2.381375 | 2.409575 | **-1.18%** |
| `query_mixed_avg_ms` | 1.808150 | 1.792025 | 0.89% |
| `query_limit_avg_ms` | 2.153025 | 1.968875 | **8.55%** |

| 比较范围 | 构建平均下降 | 分词平均下降 | 查询平均下降 | 三类等权综合下降 |
| --- | ---: | ---: | ---: | ---: |
| A/B/B/A | 6.62% | 7.37% | 2.88% | 5.63% |
| B/A/A/B | 0.42% | -0.75% | 1.69% | 0.45% |
| 八次合并 | 3.69% | 3.58% | 2.30% | 3.19% |

完整单次数据：[八次结果汇总](<build_fts/round2-eight-run-summary.json>)、[A/B/B/A 清单](<build_fts/round2-abba-manifest.json>)、[B/A/A/B 清单](<build_fts/round2-baab-manifest.json>)。清单同时保存二进制哈希及当时宿主机负载。第一组 A1 的首次严格跨库校验失败也保留，没有删除或重跑其性能数据。

## 3. 正确性与失败诊断

### 3.1 完整输出差分

确定性语料包含 619 个 BENG 完整 JSON 结果，覆盖大小写、数字、重复词、UTF8 非 ASCII、NUL、长词和支持的字符集/排序规则；查询覆盖自然语言、boolean、重复关键词、显式相关度排序、LIMIT/OFFSET 与隐式 top-k。总计 49,360 行。

- 同一数据库更换二进制：全部行严格相等，包括相关度的完整输出。负向控制故意修改一个分词结果，比较器确实拒绝它。
- 八次独立建库：所有完整 token JSON、命中 ID 和排序分页 ID 相同；原始分数全部保留，但不要求跨库逐字节一致。
- **为什么区别对待跨库分数：** 第一轮 A1 使用的仍是原基线二进制，却与之前同样基线的分数相差最多 0.0184%，其全部 token 和 ID 一致，表明独立构建的统计估计存在变化。没有把这一基线自身可复现的差异误报为候选缺陷，也没有悄悄放宽同库分数验证。

证据：[严格同库校验](<build_fts/round2-validation-candidate.log>)、[基线跨库差异日志](<build_fts/round2-abba-1-baseline-validation.log>)、[差分驱动](<build_fts/round2_validate.py>)。

### 3.2 回归用例

小型用例编写由 `nerv-base/deepseek-flash` 完成，Lead 审查和运行。新增[回归 SQL](<tools/deploy/mysql_test/test_suite/fts_index/t/fts_hotpath_regression.test>)及[预期结果](<tools/deploy/mysql_test/test_suite/fts_index/r/mysql/fts_hotpath_regression.result>)，包含 95 条 SELECT：

- 固定 JSON 中的 token 字节、完整频次、文档长度及 token 组数；混合大小写、UTF8 重音/CJK、数字、标点、停用词、NULL、空串、3/84 字节边界、连续长短词与重复调用。
- 43 行小表上的自然语言/boolean 精确 ID 集合、重复关键词、分页、严格分数关系、更新与删除；无用户主键表保留整数文档 ID 路径。
- 不依赖无 ORDER BY 的并列分数顺序，不只核对命中数。

手写草稿第一次在**未优化基线**失败：`COUNT(*) ... LIMIT ... OFFSET 7` 跳过的是聚合行，并不会返回 0。修正为“先分页再对派生表计数”，没有把错误断言的预期改成通过值。修正后新用例分别在原基线和候选通过。

最终候选通过六个用例：

1. [IK 基础回归](<tools/deploy/mysql_test/test_suite/fts_index/t/ik_tokenizer_regression.test>)
2. [IK 词典复用](<tools/deploy/mysql_test/test_suite/fts_index/t/ik_dictionary_reuse.test>)
3. [中文词](<tools/deploy/mysql_test/test_suite/fts_index/t/cn_word.test>)
4. [TOKENIZE 函数](<tools/deploy/mysql_test/test_suite/fts_index/t/tokenize_function.test>)
5. [NULL 域 ID 合并](<tools/deploy/mysql_test/test_suite/fts_index/t/domain_id_merge_with_compare_null.test>)
6. [本轮热路径回归](<tools/deploy/mysql_test/test_suite/fts_index/t/fts_hotpath_regression.test>)

## 4. 复现和边界

保留上文启动参数，在独立测试实例运行用户原版完整基准命令（将端口替换为 2888，每次新报告，`SKIP_LOAD=0`）。回归使用仓库既有 mysqltest runner；本地使用自带兼容客户端，需准备测试框架账户及连接环境：

```bash
export OBMYSQL_MS0=127.0.0.1 OBMYSQL_PORT=2888 OBMYSQL_PWD=''
deps/3rd/u01/obclient/bin/mysqltest \
  --host=127.0.0.1 --port=2888 --user=root --database=test \
  --default-character-set=utf8mb4 \
  --test-file=tools/deploy/mysql_test/test_suite/fts_index/t/fts_hotpath_regression.test \
  --result-file=tools/deploy/mysql_test/test_suite/fts_index/r/mysql/fts_hotpath_regression.result
```

- 当前构建只支持 binary/UTF8MB4；不能声称运行了 latin1/gbk 回归。新分支对其他字符集保持原路径。
- 未运行新的统一 Linux CI、sanitizer 或 OOM 注入；没有把本地结果宣称为正式评分。
- 独立服务停止需要 SIGKILL 兜底；没有把正常完整析构路径当作已验证通过。
- 上一轮结果见[第一轮报告](<FTS_OPTIMIZATION_REPORT.md>)。本轮最明确的进展是 LIMIT 查询；更高分仍需统一环境的九项明细和后续热点定位。
