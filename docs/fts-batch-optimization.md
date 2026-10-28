# FTS 批处理优化与回归记录

本次从 `8b567cf64beef624bdac4d74f851d7351ef9ee20` 继续优化。
竞争基线使用 `2026-competition` 对应的 `5bce9e06d1281347ad61344e4b01fab9158c58a2`。
用户提供的上一版正式 raw score 为 69.4734/100；本文的本机结果不能代替统一评测成绩。

## 完整性能对照

相对上一版 `8b567cf`，三类等权综合耗时下降 **2.49%**。
相对本机重新编译的 competition，综合下降 **30.96%**；这仍低于 50% 的满分目标。
完整对照显示较明确的构建收益，分词和查询均值受运行波动影响，不能声称正式满分。

| 指标 | 8b567cf 三次均值 | 候选三次均值 | 耗时下降 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 4.654333 | 4.372333 | +6.06% |
| build_ik_content_sec | 3.778667 | 3.245667 | +14.11% |
| build_beng_en_sec | 1.791000 | 1.694000 | +5.42% |
| tokenize_ik_avg_ms | 0.117533 | 0.109567 | +6.78% |
| tokenize_beng_avg_ms | 0.085767 | 0.091700 | -6.92% |
| query_cn_avg_ms | 1.099867 | 1.177667 | -7.07% |
| query_beng_avg_ms | 1.669300 | 1.610200 | +3.54% |
| query_mixed_avg_ms | 1.222433 | 1.206367 | +1.31% |
| query_limit_avg_ms | 0.245667 | 0.250000 | -1.76% |

构建指标单位为秒，其余计分指标单位为毫秒。类别下降率：构建 **8.53%**，分词 **-0.07%**，查询 **-1.00%**。

每版完整运行三次，使用原始 benchmark 的 20,000 行、batch 500、分词 3,000 次、查询 200 次、
三组采样、30 次预热以及 `SKIP_LOAD=0`，每次重新导入并重建三个索引。
顺序为旧版、候选、competition；competition、候选、旧版；旧版、competition、候选。
每次切换二进制后统一等待 10 秒再运行。计时期间没有编译、原生测试或 CPU 采样。

原始报告中的 `git_head` 是脚本所在工作树的版本，不是当时运行二进制的版本。
必须结合 [environment.json](fts-batch-results/environment.json) 中的源提交、嵌入版本和 SHA-256 识别三版。
[relative-to-prior.json](fts-batch-results/relative-to-prior.json) 与
[relative-to-competition.json](fts-batch-results/relative-to-competition.json) 保存三次原始指标、类别权重和计算结果。
构建、分词、查询先分别求内部下降率均值，再三类等权；退化不在单项截断。

## 独立 CPU 诊断

完整墙钟对照中候选出现明显抖动，因此另外进行两次每项 5,000 条查询的同数据对照。
以服务端进程 CPU 时间差计量，包含后台线程；此诊断不修改也不替代正式计分，不能折算正式得分。

| 查询 | 旧版 CPU 秒 | 候选 CPU 秒 | 下降 |
| --- | ---: | ---: | ---: |
| cn | 5.190 | 3.925 | 24.37% |
| beng | 7.835 | 6.020 | 23.17% |
| mixed | 5.550 | 4.395 | 20.81% |

两次方向一致，支持查询热路径减少了 CPU 工作量的判断。原始数据见 [cpu-diagnostics.json](fts-batch-results/cpu-diagnostics.json)。

## 修改与瓶颈依据

- 查询：原 count-only 路径依然逐文档调用维度迭代器、collector 和计数逻辑。
  现在一次提取一批真实 posting ID，保持去重和 LIMIT/OFFSET 语义。
  一、二、三路使用直接比较；三路在同一轮推进所有相同头部，避免为重复 ID 反复 pop/push/rebuild。
  更多路仍使用 loser tree；需要分数、额外过滤或其他合并语义时沿用原路径。
  固定数组跨 rescan 复用，避免 arena 上反复重新分配。
- 构建：原表扫描把一篇文档展开为大量独立 `ObDatumRow` 并逐行交给下游。
  现在用连续的四列词条缓冲批量投影，表达式只绑定一次；先消费剩余词条再读取新文档。
  解析临时内存每篇文档释放，批次只保留词条和文档 ID，附加 1 MiB 软阈值限制多文档聚合。
  单篇大文档可以超过阈值，随后跨输出批次排空，保持完整输入。
  原源行校验和仍由逐行源扫描处理；扩展批次不重复累计或上报校验和。
- IK：同一字符匹配多个活动前缀时，按具体 DAT 复用字符编码。
  缓存局限于当前字符和最多四个 DAT；缺失、不同 DAT 以及溢出继续走原匹配逻辑。
  没有改词典内容、切词规则、匹配顺序或分词模式。
- 分词正确性：上一版单次查找只更新频次，使 collation 等价 token 保留了首次拼写。
  competition 原实现会覆盖键，代表拼写是最后一次出现的拼写。
  本版在一个 bucket 写锁和一次查找内更新键与频次，恢复重音字符、大小写和 emoji 的原有 JSON 输出。

三路归并还做了单独开关版本对照，原始诊断报告保留于 `fts-batch-results/ablation-*`。
这些诊断使用 `SKIP_LOAD=1`，不纳入上面的完整评分对照。

## 正确性与审查

- 40 项原生测试链接真实生产库并通过：字符分类、所有支持 collation 的比较、词频聚合、
  IK 字典缓存、归并边界和复用、词条缓冲的生命周期与完整行字段。
  原始输出见 [native-tests.txt](fts-batch-results/native-tests.txt)。
- `no_score_projection`、`count_only_boundaries`、`postbuild_word_rows` 全部通过。
  覆盖数值/字符串/heap ID、NULL、重复词、多路归并、批次边界、预编译换参、DML、事务回滚，
  分区与非分区建索引、大文档、空输入以及普通二级索引。
- 与 competition 对照 533 个完整 TOKENIZE JSON，包括代表拼写、顺序、词频、五种 parser 和不同 collation。
  `tokenize_function` 的全部语句也通过基线/候选对照：本地 MySQL 客户端不支持仓库的输出格式 4，
  仅在临时副本把显示格式设为 1，SQL 和预期错误保持不变；仓库测试没有进行这项适配。
- 对四组查询保留全部 ID 和 12 位小数评分，在同一物理索引上切换二进制进行逐字节核对。
  分别重建索引时，已有平均文档长度估计可能产生分数差异，因此没有把跨重建的分数相同当作前提。
  词条、频次和文档长度另用原行生成器作为原生测试的独立对照。
- 实现与审查由不同子代理完成，主代理集成、构建并运行回归。两次实际失败均修复后重测：
  一是测试 ID 缺少 payload 存储；二是批量跨分区时源扫描覆盖 PDML 分区标识。
  分区 FTS 构建现保留原逐行路径；非分区构建启用批处理。

回归运行方法见 [unittest/fts/README.md](../unittest/fts/README.md)。
[verification.json](fts-batch-results/verification.json) 记录测试结果和完整结果集校验和。

## 环境与适用范围

macOS 27.2 / Apple arm64，10 个逻辑 CPU、24 GiB 物理内存；seekdb 配置 `cpu_count=4`、内存上限 2 GiB、
数据文件初始 1 GiB/最大 8 GiB、日志盘 2 GiB。三版均使用同一个 Ninja unity 构建，Apple Clang 21，
`RelWithDebInfo`、`-O2 -g -DNDEBUG`。该电脑仍有其他应用运行，不是隔离的正式评测机器。

官方五个 benchmark 文件与 competition 逐字节相同；没有更改工作流、数据、SQL、计时、评分或基线。
本机与固定 CI baseline 的硬件和开销不同，直接套用固定数值会过早触及 100 分上限，不能证明正式满分。
正式成绩需由统一评测复核。
