# seekdb 全文索引优化报告

> 本文记录第一轮的历史测量。其提交后用户反馈统一评测为 22.542/100；第二轮的实现、八次本地对照及验证边界见[第二轮报告](<FTS_OPTIMIZATION_ROUND2_REPORT.md>)。

## 结论与适用范围

- 起点：`perf/fts-large-bench-optimize`，`bfc46d6dd12eec96b8d788b23413ab643ec35a8d`。性能测量在补丁提交前完成，原版和候选版由下文二进制哈希区分。
- 本报告与内核改动、回归用例一同提交；文中指向 [本地证据目录](<build_fts/>) 的原始日志、报告和二进制保留在本地工作树，不纳入 Git 提交。远端阅读时请以本文完整指标表、输出哈希和可重跑的回归用例为准。
- 优化内容：复用 IK 内置只读词典视图，并消除词典分片查找中的通用字符集比较开销；不缓存 TOKENIZE 或 MATCH 的结果，不修改分词规则、倒排数据或评分脚本。
- 本地独立数据目录 A/B/B/A 对照中，IK 分词平均耗时从 **0.19800 ms 降至 0.13195 ms，下降 33.36%**。九项指标按三类等权计算的观测综合下降为 **20.60%**。
- **上述结果不是新的正式成绩，也不能把所有观测收益归因于代码。** 宿主机存在显著负载和换页干扰，未修改的 BENG 指标也变快。用户提供的正式起点 **6.184/100** 仍是当前已知 CI 分数；本轮尚未运行统一 Linux CI。
- 正确性证据不只依赖命中数量：四份 mysqltest 通过；两轮各 12,246 次完整分词输出与原版本一致；四次全量数据集查询的 35,686 行 ID/相关度输出逐字节一致。

## 1. 瓶颈依据与修改

### 1.1 每篇文档重复初始化词典视图

原路径每次创建 IK 解析器都会重新获取主词典、量词词典和停用词词典的 KV cache handles，创建三个范围词典及其子视图，并分配/销毁相关 arena。词典数据本身是不可变的内置数据，匹配状态在调用方维护。

在 [词典管理器](<src/storage/fts/dict/ob_ft_dict_hub.cpp#L31-L101>) 中增加 `BuiltinIKDicts`：

- 成功初始化后使用 acquire/release 原子指针发布；仅首次初始化加锁，热路径只读取已发布指针。
- 只共享只读词典和对应 cache handles；解析上下文、分词模式、命中状态仍归每个解析器独立所有。
- 不发布部分初始化的对象；失败时回收，允许下次重试。
- [解析器初始化与释放](<src/storage/fts/ob_ik_ft_parser.cpp#L231-L327>) 改为借用 `const ObIFTDict *`，不再逐文档构造、回收词典视图。
- 生命周期顺序为“视图 → cache handles → allocator”；[服务销毁顺序](<src/observer/ob_server.cpp#L1080-L1093>) 中先释放 FTS，再销毁词典和全局 KV cache。

**资源取舍：** 内置词典对应 cache handles 会存活到 FTS hub 销毁，避免每篇文档重复引用和分配，但也意味着这部分内置词典内存在此期间保持被引用。它不是按查询或文档增长的缓存，也不适用于未来可动态修改的用户词典。

### 1.2 词典分片查找中的字符集分派

[范围查找](<src/storage/fts/dict/ob_ft_range_dict.cpp#L381-L399>) 原先在每个范围边界比较时调用通用 `ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, ...)`。此处非前缀二进制比较等价于按字节比较后比较长度，改为 `ObString::compare`。保留原遍历顺序、闭区间边界及未命中处理，不改变分片或词序。

探索阶段在另一个已有本地版本上进行的 30 秒采样中，主工作线程 306 个 TOKENIZE 样本中，45 个位于初始 CJK 范围匹配，20 个位于解析器初始化。**该采样仅用于定位方向，不是指定 SHA 的前后收益证明。** 修改的实际前后验证使用下述两个独立保留的二进制。

## 2. 环境和基准完整性

- macOS arm64，10 CPU、24 GiB RAM；同一 CMake `RelWithDebInfo` 构建目录、Apple Clang 工具链，增量构建候选版本。
- 专用端口 2888；`memory_limit=2G`、`datafile_size=1G`、`datafile_maxsize=8G`、`log_disk_size=2G`。未操作原有 2881/2883 实例。
- 基准五个文件从用户提供的 [5bce9e06 提交](https://github.com/oceanbase/seekdb/commit/5bce9e06d1281347ad61344e4b01fab9158c58a2) 原样提取到 [本地基准副本](<build_fts/tools/benchmark/fts_large_bench.sh>) 所在目录；执行前逐字节核对该提交的文件。
- 每次完整运行均为 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`，使用全新报告文件，重新导入全部数据并构建全部索引。
- 未修改官方数据、SQL、计时、评分或固定基线；正确性检查在计时完成后执行。

| 二进制 | SHA-256 |
| --- | --- |
| [原版本](<build_fts/seekdb-baseline-bfc46d6d>) | `e1b38b05608b3442bdd44e952ae3bf7742d9a8b2840d41a23f8178c9737215fe` |
| [候选版本](<build_fts/seekdb-candidate-ik-share>) | `d736e0100c75418d91fb069a824487949310c149883b727fbf6d80ed157ef8b1` |

原始测量报告中的 `git_head` 均为起点 SHA，因为测量时补丁尚未提交；应结合上述二进制哈希区分版本，不能只看报告中的 `git_dirty`。

## 3. 完整 A/B/B/A 对照

每一轮使用全新数据库目录，顺序为原版、候选、候选、原版。第三轮首次启动探测超过 5 秒而中止，**没有产生性能报告**；修正探测超时重试后以另一个新目录完成第三、第四轮。没有重跑或替换前两轮已完成结果。

- A1：[原版本报告](<build_fts/controlled-1-baseline.report>)，17:57。
- B1：[候选报告](<build_fts/controlled-2-candidate.report>)，17:58。
- B2：[候选报告](<build_fts/controlled-retry-3-candidate.report>)，18:01。
- A2：[原版本报告](<build_fts/controlled-retry-4-baseline.report>)，18:02。

下表先对同版本两次完整运行的每项计分指标取算术平均，再计算耗时下降率；不是采用最低耗时或中位数代替官方组内均值。

| 指标 | 原版均值 | 候选均值 | 观测耗时下降 |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 6.63450 s | 6.08750 s | 8.24% |
| `build_ik_content_sec` | 4.90850 s | 4.46500 s | 9.04% |
| `build_beng_en_sec` | 2.68450 s | 2.27150 s | 15.38% |
| `tokenize_ik_avg_ms` | 0.19800 ms | 0.13195 ms | 33.36% |
| `tokenize_beng_avg_ms` | 0.11825 ms | 0.09805 ms | 17.08% |
| `query_cn_avg_ms` | 2.21470 ms | 1.67480 ms | 24.38% |
| `query_beng_avg_ms` | 3.49990 ms | 3.01315 ms | 13.91% |
| `query_mixed_avg_ms` | 3.33550 ms | 2.14175 ms | 35.79% |
| `query_limit_avg_ms` | 3.49080 ms | 2.48910 ms | 28.70% |

- 构建类平均下降：10.89%。
- 分词类平均下降：25.22%。
- 查询类平均下降：25.69%。
- 三类等权综合观测下降：**20.60%**。
- 辅助 `SELECT 1` 均值：0.07365 → 0.07595 ms，略有退化。

**重要限制：** BENG 路径未作修改，但其指标也下降约 14%–17%，说明环境因素仍不可忽略。A/B/B/A 只能减弱时间顺序偏差，不能替代空闲、稳定的统一 CI。这里不宣称查询执行器或 BENG 获得了相应比例的内核优化。

### 首批非交错测量，保留而不用于归因

此前在同一数据库目录连续运行了两次原版、三次候选，结果明显不稳定，候选部分指标反而大幅变慢：

| 报告 | IK 分词 ms | BENG 分词 ms | 中文查询 ms | SELECT 1 ms |
| --- | ---: | ---: | ---: | ---: |
| [原版 1](<build_fts/baseline-full-1.report>) | 0.1673 | 0.1146 | 2.2739 | 0.0777 |
| [原版 2](<build_fts/baseline-full-2.report>) | 0.2052 | 0.0916 | 1.6705 | 0.1133 |
| [候选 1](<build_fts/candidate-full-1.report>) | 0.2473 | 0.2173 | 4.3241 | 0.0799 |
| [候选 2](<build_fts/candidate-full-2.report>) | 0.3034 | 0.2912 | 3.2475 | 0.2316 |
| [候选 3](<build_fts/candidate-full-3.report>) | 0.3691 | 0.3274 | 3.7743 | 0.0938 |

17:53 的宿主机快照显示另有两个编译器进程各占约 96% CPU，load average 10.63，swap 使用约 5.98 GiB。因此保留这些报告作为噪声证据，并额外进行全新目录的交错对照，而不是混用不同阶段的最快结果。

### 固定 CI 评分

原样评分模块校验了四份 A/B/B/A 报告的所有参数，无缺失指标或参数不一致。四份本地报告对固定 CI 基线计算均为 100/100，**包括未修改原版本**，说明此跨环境分数已饱和，不能用来宣称从 6.184 分提高到 100 分。本轮有效结论是本地对照数据及功能证据；正式新分数仍待统一 CI。

## 4. 正确性验证

### mysqltest

使用仓库随附的 mysqltest 客户端，候选版本下四份用例均返回 `ok`：

1. [IK 基础回归](<tools/deploy/mysql_test/test_suite/fts_index/t/ik_tokenizer_regression.test>) — [日志](<build_fts/final-ik_tokenizer_regression.log>)。
2. [新增词典复用回归](<tools/deploy/mysql_test/test_suite/fts_index/t/ik_dictionary_reuse.test>) — [日志](<build_fts/final-ik_dictionary_reuse.log>)。
3. [中文全文检索](<tools/deploy/mysql_test/test_suite/fts_index/t/cn_word.test>) — [日志](<build_fts/final-cn_word.log>)。
4. [通用 TOKENIZE](<tools/deploy/mysql_test/test_suite/fts_index/t/tokenize_function.test>) — [日志](<build_fts/final-tokenize_function.log>)。

新增用例覆盖 smart/max_word 交错、逐行重用、词集合与词频及 doc_len 的完整 JSON 结构比较、NULL/空串、未知英文词、超过 1000 字符的批次边界、当前构建支持的 UTF8MB4 collations/binary 往返、缓存刷新、两个连接，以及索引插入/查询/更新/删除的固定 ID 集合。GBK/latin1 未在本分支中注册，未假称已覆盖。

原有 IK 基础回归本身存在失败：MATCH 列与索引不一致、用子串搜索判断完整词、默认 parser 不是 IK，以及长表达式推断为 LOB。已修正这些测试问题，并先在未修改二进制上确认两份 IK 回归通过；没有改动性能基准。

### 跨版本差分与并发

- 6,123 个确定性文本，两个 IK 模式，共 **12,246 次**分词；覆盖全部 **5,854 个词典首字符**、五处分片边界、混合 Unicode/ASCII/NUL/全角及长文本。
- 8 客户端冷启动：12,246 次与原版完整输出比较，**0 差异**。
- 8 客户端并发分词，同时执行 100 次词典缓存刷新：12,246 次比较，**0 差异**。
- 证据：[并发核对日志](<build_fts/candidate-concurrency.log>)、[差分检查驱动](<build_fts/check_dictionary_concurrency.py>)、[原版输出](<build_fts/dictionary-baseline.tsv>)。
- 全量 20,000 文档四组检索的全部 ID 和相关度，加上 LIMIT 20 的 ID，合计 **35,686 行**；四轮输出 SHA-256 均为 `76150c5f1d56e9ebcbc7c7011abec1aed0dea76544664e8f35dd7d38bee68a56`。参见 [原版全量结果](<build_fts/controlled-1-baseline-query-results.tsv>)、[候选全量结果](<build_fts/controlled-2-candidate-query-results.tsv>)。

### 尚未验证的边界

- 未运行 ASan/TSan、OOM 故障注入或完整仓库测试集。
- 专用 standalone 实例在收到 SIGTERM 后最终以 SIGKILL 退出；不能把进程重启当作 `ObServer::destroy()` 完整释放路径的运行时证明。已完成所有权与销毁顺序审查，完整正常销毁路径仍需专门的生命周期测试。
- 未运行统一 Linux CI，尚不能确认正式分数或稳定收益。

## 5. 复现与后续

已完成同一配置的原版构建、候选构建及最终增量构建检查。官方基准调用方式如下；务必使用专用实例，脚本会重建测试库。

```bash
export PATH="/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH" # 本次 macOS 需要 GNU date
export MYSQL='mysql -h127.0.0.1 -P2888 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2888 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-bench-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  /opt/homebrew/bin/bash build_fts/tools/benchmark/fts_large_bench.sh
python3 build_fts/tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
```

下一步应在统一 CI 上运行这份补丁，获取与 6.184/100 对应环境可比较的九项结果。若继续扩展优化，先取得该环境的构建和查询 profile，避免把本地噪声误判成存储或查询链路收益。

小型环境/测试梳理与新增回归初稿由 `nerv-base/deepseek-flash` 完成；内核修改、预期结果修正、实际测试、性能核对与最终审查由 Lead 完成。
