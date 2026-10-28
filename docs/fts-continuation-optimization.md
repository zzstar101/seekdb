# FTS 后续优化与完整复测

本轮以已提交的 `f14fdbd386d903fa3cd360f1320043c2d8a5a671` 为起点。
用户提供的统一评测结果是 **39.004/50**（raw **78.007043/100**，综合耗时下降 **39.003522%**）。
本文的同机复测不是正式成绩；只有统一评测的新结果能确认是否达到满分。

## 完整性能结果

三次均值对比 `f14fdbd`：综合耗时下降 **7.58%**，构建 **9.00%**、分词 **8.38%**、查询 **5.35%**。

| 指标 | f14fdbd 三次均值 | 本轮三次均值 | 耗时下降 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 4.044667 | 3.798333 | +6.09% |
| build_ik_content_sec | 3.330000 | 2.958000 | +11.17% |
| build_beng_en_sec | 1.741000 | 1.571333 | +9.75% |
| tokenize_ik_avg_ms | 0.146267 | 0.128467 | +12.17% |
| tokenize_beng_avg_ms | 0.085833 | 0.081900 | +4.58% |
| query_cn_avg_ms | 1.058533 | 0.933567 | +11.81% |
| query_beng_avg_ms | 1.589733 | 1.280833 | +19.43% |
| query_mixed_avg_ms | 1.106633 | 1.088500 | +1.64% |
| query_limit_avg_ms | 0.249733 | 0.278367 | -11.47% |

构建单位为秒，其余计分指标为毫秒。LIMIT 项退化 **11.47%**，按负提升率计入，没有剔除。
相对同机重新运行的 competition，三类综合耗时下降 **36.08%**，低于 50% 的目标；不据此宣称正式满分。

[完整原始报告及二进制来源](fts-continuation-results/environment.json)、[相对 f14fdbd 的计算](fts-continuation-results/relative-to-delivered.json)、[相对 competition 的计算](fts-continuation-results/relative-to-competition.json) 均随代码保留。

## 实现与依据

- **查询去重计数**：一至三路 posting 合并直接累计 distinct 数量，保留 LIMIT/OFFSET、NULL、重复 ID 和批次边界语义；更多路和需要评分的查询沿用通用路径。最后一个 ID 在源批次被覆盖前复制，避免跨批去重依赖失效内存。
- **查询数据存储**：每个 token 迭代器持有连续 datum 数组和独立 payload 缓冲，容量复用，8/16 字节 ID 使用固定长度复制；变长数据、NULL 和评分路径仍受支持。原始扫描表达式 frame 在不同 token 之间共享，因此没有采用直接返回该 frame 的借用方案。批接口与逐行/advance 接口读取同一份当前有效数据。
- **二进制 ID 比较**：16 字节 ID 使用两次安全的 8 字节载入，按字节序比较；未对齐数据通过 memcpy 载入，非 16 字节、NULL、非二进制语义保留原比较方式。
- **IK 范围定位与字符处理**：只有严格有序且不重叠的字典范围才启用二分；其他布局保持原来的首个匹配规则。短范围键使用等价字节序编码，长边界回退。UTF-8 字符校验与分类合并一次解码，失败时回到旧逻辑；中文数字集合保持原来的 33 个唯一码点。
- **分词临时数据**：文档私有词频 map 移除不必要的 bucket 锁，共享停用词集合未修改；BENG 仅在 token 指向当前文档输入时借用其存储，指向分析器临时区的 token 仍复制。词频、代表拼写和输出顺序保持 competition 语义。
- **构建源扫描修复**：生成词条继续批量输出，但源文档扫描及其 lookup/attached scan 保持标量调用约定。旧版本在源表已落入 SSTable 时会在 `OB_PUSHDOWN_STATUS_CHANGED` 后重复标量调用而无法前进；已在专用实例重现并修复。
- **构建采样**：非分区倒排辅助表按实际生成的 token 行采样，使 PX 排序范围与其首排序键对应。doc-word 辅助表仍按源行采样；采样后的 rescan、源校验和与分区标识沿用已有约束。

源码采样显示查询仍花费大量工作在 ID 比较、逐 ID 复制和传递，IK 花费在字典范围、字符解码和中文数字判断。
采样专用的 stripped 二进制只用于定位热点，不用于下文性能计时。

## 正确性与审查

**65 项原生测试、8 个测试组全部通过**；测试链接本轮真实生产库，覆盖字符与比较差分、范围布局回退、词频聚合、归并边界、内存复用和批量词条字段。
公开批接口回归通过真实 EvalCtx/Expr 与测试 DAS 源，能够捕获借用共享 descriptor 或 payload 的错误。
SQL 回归包括 `no_score_projection`、`count_only_boundaries`、`postbuild_word_rows`、`postbuild_sstable_scan` 和 `tokenize_function`。
本机 mysqltest 适配仅在临时副本调整 result_format 和等待命令名称，仓库用例的 SQL 未因此改变。
强制 SSTable 测试等待当前 major SCN 与非空源 tablet 落盘，启用采样并建立两次索引，验证持久化 doc-ID lookup 及采样后 rescan。

完整 TOKENIZE 对照包含 533 个 JSON 结果，比较词、频次、顺序和代表拼写。
四组 MATCH 查询在同一物理索引上切换二进制，逐字节比较全部文档 ID 与保留 12 位小数的评分；命中数只是辅助检查。
分别重新构建索引可能改变已有文档长度估计，因此不把跨重建的评分完全相同当成前提。

实现和独立审查由不同子代理执行，主代理集成并运行验证。
被否决的共享扫描 frame 借用方案未进入提交；测试中不兼容的 allocator 指针赋值已修正。

## 测量方法与局限

使用同一台 Apple arm64 开发机、同一 CMake/Clang 构建配置和专用 seekdb 实例；实例固定 CPU 4、内存 2 GiB，端口 29981/29982。
每版运行三次完整原始基准，20,000 行、batch 500、TOKENIZE 3,000 次、query 200 次、3 组采样、预热 30 次、`SKIP_LOAD=0`。
三轮顺序为 delivered/candidate/competition、competition/candidate/delivered、delivered/competition/candidate；每次切换二进制后统一等待 10 秒。
所有原始样本保留，包括慢样本；计时期间本任务没有编译、执行原生测试或采样剖析。其他桌面应用仍可能影响本机墙钟和 CPU 计时。

`delivered` 的运行二进制源提交为 `d3270a4c1`，其生产代码与 `f14fdbd` 完全相同，之后仅有文档和测试调整。
`competition` 是 `5bce9e06d1281347ad61344e4b01fab9158c58a2`。
报告中的 `git_head` 表示基准脚本所在工作树；实际运行版本以二进制嵌入 revision、SHA-256 和 manifest 为准。
仓库历史 baseline JSON 的绝对耗时与本机不同，不直接用其数值宣称正式满分。

独立消融中，固定长度比较及连续 owned buffer 的查询 CPU 对照有收益；采样实验的平均构建收益较小，且有个别指标退化。
分词融合及中文数字 switch 的长循环 CPU 对照受波动影响，均出现候选慢样本，不能把它们单独宣传为已证明的稳定收益。
这些单项实验不代替完整评测，原始消融数据与不利样本一并保存。

五个官方 benchmark 文件与 competition 逐字节相同，工作流与 `f14fdbd` 相同。
没有修改输入数据、SQL、计时、评分或基线来提高成绩。测试添加在既有 unittest 和 mysqltest 目录；独立编排脚本位于仓库外。

[回归结果](fts-continuation-results/verification.json)、[原生测试原始输出](fts-continuation-results/native-tests.txt)、[评测文件完整性](fts-continuation-results/benchmark-integrity.json)；运行方法见 [unittest/fts/README.md](../unittest/fts/README.md)。
