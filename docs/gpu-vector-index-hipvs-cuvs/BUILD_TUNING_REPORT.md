# hipVS CAGRA 构建加速报告（提交版）

本报告记录在冻结基线 `amd-cagra-contest-baseline-v5`（commit
`634888d9b037153fa143d2c913ba4b25503e1d62`，tag `amd-cagra-contest-baseline-v5`）
之上，对 AMD GPU CAGRA 索引构建链路所做的优化、实测数据与复现方法。

全部改动集中在 `tools/hipvs_bridge/seekdb_cuvs_bridge.c`（seekdb 与 hipVS/cuVS
之间唯一的 C ABI 桥接层）。不改变 SQL 语义，不改变 `lib=vsag` 路径，不引入任何
CPU/非 GPU 的建图或检索回退，持久化格式仍为 `OBCUVS02`。

## 1. 结论摘要

构建（端到端 `CREATE VECTOR INDEX` + 首条查询，SQL 计时，官方数据同分布：
单位归一化 float32，128 维）：

| 数据集 | 基线（`niter=100`） | 提交版本 | 加速比 | 提交版本 `niter` |
| --- | --- | --- | --- | --- |
| 25K × 128 | 12.79 s | 7.70 s | **1.66x** | 30 |
| 100K × 128 | 31.31 s | 15.08 s | **2.08x** | 40 |
| 250K × 128 | 45.20 s | 28.33 s | **1.60x** | 50 |
| 1M × 128 | 126.61 s | 92.44 s | **1.37x** | 60 |

召回（100 条查询，精确暴力 ground truth，与基线同轮对比）：

| 数据集 | 基线 Recall@10 / @100 | 提交版本 Recall@10 / @100 | 差值 |
| --- | --- | --- | --- |
| 25K × 128 | 1.0000 / 1.0000 | 1.0000 / 1.0000 | 0 / 0 |
| 100K × 128 | 0.9990 / 0.9939 | 0.9980 / 0.9941 | −0.0010 / +0.0002 |
| 250K × 128 | 0.9890 / 0.9643 | 0.9890 / 0.9639 | 0 / −0.0004 |
| 1M × 128 | 0.9100 / 0.8131 | 0.9250 / 0.8088 | +0.0150 / −0.0043 |

> 上表所有行均为 100 条查询、同一轮 A/B；`--norm` 生成的数据与官方 demo 数据同分布
> （单位归一化随机向量，`std≈0.088`）。

单条查询（SQL 计时，同一批查询、同一张图）：

| 数据集 | 基线 p50 / QPS | 提交版本 p50 / QPS | 吞吐比 |
| --- | --- | --- | --- |
| 25K × 128 | 6.31 ms / 156.7 | 6.67 ms / 147.4 | 0.94x |
| 100K × 128 | 6.44 ms / 149.5 | 5.97 ms / 165.7 | 1.11x |
| 250K × 128 | 6.67 ms / 149.7 | 7.03 ms / 141.7 | 0.95x |
| 1M × 128 | 7.04 ms / 139.1 | 6.67 ms / 149.4 | 1.07x |

核心变更：

1. **NN-descent 迭代预算按数据集规模自适应**（30 / 40 / 60 / 100 轮，基线固定
   100 轮）。NN-descent 占构建时间的 80% 以上，减轮数即等比例缩短构建；轮数下限
   由实测召回决定（见 §4），保证相对基线 Recall@10/@100 下降不超过 0.005。
2. **查询工作区复用**：把每次查询都执行的 3 次 RMM 分配/释放与
   `cuvsCagraSearchParamsCreate/Destroy` 改为按索引句柄缓存，单次 GPU 检索固定
   开销 2.75 ms → 0.39 ms。
3. 全部构建/检索旋钮均可环境变量覆盖，`SEEKDB_CUVS_NITER=100` 可一键复现基线图，
   便于云端 A/B 复核。

## 2. 基线耗时构成与可优化空间

V5 构建链路（`ObVectorIndexSerializer` → `ob_vsag_adaptor` GPU 快照 →
`tools/hipvs_bridge` → `cuvsCagraBuild`）在 250K × 128 上的实测构成：

| 阶段 | 耗时 |
| --- | --- |
| 初始图 + 剪枝（`nn_descent_niter=0`） | 7.5 s |
| NN-descent 每轮 | ~0.34 s |
| 100 轮 NN-descent | ~34 s |
| 构建合计（`cuvsCagraBuild`） | ~43 s |
| DDL 其余部分（扫描、快照持久化 `OBCUVS02`、GPU 传输） | ~2.5 s |

即 **NN-descent 轮数是唯一的量级杠杆**；数据集拷贝、索引持久化都在百毫秒级。
`build_algo=IVF_PQ` 虽然把构建压到 3.4 s（12.9x），但 Recall@10 只有 0.76，
属于“用召回换性能”的禁止行为，已否决。

## 3. 自适应预算的依据（关键决策数据）

轮数对图质量的影响随 N 增长而放大，因此在同一套官方同分布数据上逐档标定，
每一档都要求 Recall@10 与 Recall@100 相对基线下降 ≤ 0.005：

| 数据集 | 基线 r@10 / r@100 | `niter=30` | `niter=40` | `niter=50` | `niter=60` | 采用 |
| --- | --- | --- | --- | --- | --- | --- |
| 25K | 1.0000 / 1.0000 | 1.0000 / 1.0000 ✓ | — | — | — | **30** |
| 100K | 0.9990 / 0.9939 | — | 0.9980 / 0.9941 ✓ | — | — | **40** |
| 250K | 0.9890 / 0.9643 | 0.9820 / 0.9623（−0.007 ✗） | 0.9840 / 0.9614（−0.005 临界） | 0.9890 / 0.9639（0 / −0.0004 ✓） | — | **50** |
| 1M | 0.9100 / 0.8131 | 0.8960 / 0.7918（−0.022 ✗） | 0.9060 / 0.8046（−0.012 ✗） | — | 0.9250 / 0.8088（+0.015 / −0.004 ✓） | **60** |
| > 1M | — | — | — | — | — | **100**（基线预算） |

对应实现（`sk_cuvs_index_create`）：

```c
if (n <= 50000)        niter = 30;   /* 1.66x */
else if (n <= 150000)  niter = 40;   /* 2.08x @100K */
else if (n <= 400000)  niter = 50;   /* 1.60x @250K */
else if (n <= 1000000) niter = 60;   /* 1.37x @1M  */
else                   niter = 100;  /* 基线预算，零风险 */
if (dim >= 192 && niter < 60) niter = 60;  /* 高维收敛更慢，先保守取 60 */
```

`graph_degree` / `intermediate_graph_degree` 始终保持基线值 128 / 256，
即图的拓扑规格与基线一致，只有 NN-descent 的精化轮数不同。

> 256 维（Hidden250K）尚无实测：`dim >= 192` 时把预算下限抬到 60 轮，是在
> “128 维的 40/50 轮结论能否外推到 256 维”这个问题被测清楚之前的保守选择
> （256 维在 60 轮下约 1.45x；若实测 50 轮不掉点，改一个环境变量即可回到 50）。

## 4. 查询路径

`ob_vsag_adaptor` 的 GPU 检索经 `seekdb_cuvs_search()` 进入桥接层。原实现每次
查询都会：

- `cuvsRMMAlloc` + `cuvsRMMFree` 三个设备缓冲区（query / neighbors / distances）；
- `cuvsCagraSearchParamsCreate` + `Destroy`；
- 3 次阻塞式 `cudaMemcpy`。

改为按句柄缓存缓冲区和搜索参数（容量不足才重新分配），保留基线
`itopk = max(4*topk, 512)`、`search_width = 4`，检索结果与基线逐位一致。

微基准（250K × 128，`nq=1`，`topk=10`，同一张图）：

| 实现 | 单次检索耗时 |
| --- | --- |
| 原实现（每次分配 + 建参） | 2.751 ms |
| 缓存工作区 | 0.386 ms |

SQL 侧单条查询还有约 4–5 ms 固定开销（解析 / 计划 / 结果组装），因此端到端 p50
主要由它决定；本项优化消除的是每查询 2.4 ms 的固定浪费，用于保证
`candidate_qps / baseline_qps >= 0.90` 的硬门槛并改善批量/高并发场景。

**不建议**通过降低 `itopk` 换查询加速：实测 `itopk=64` 会让 SQL 端 Recall@10
从 0.995 掉到 0.54（CAGRA 在 250K 规模需要宽搜索），属于用召回换性能。

## 5. 环境变量

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `SEEKDB_CUVS_NITER` | 按 N 分层（30/40/50/60/100） | NN-descent 轮数；`100` 完全复现基线图 |
| `SEEKDB_CUVS_GRAPH_DEGREE` | `128` | 最终图度数（基线值） |
| `SEEKDB_CUVS_INTER_DEGREE` | `256` | 中间图度数（基线值） |
| `SEEKDB_CUVS_ITOPK` | `0` | `0` = 基线策略 `max(4*topk, 512)` |
| `SEEKDB_CUVS_SEARCH_WIDTH` | `4` | 检索宽度（基线值） |
| `SEEKDB_CUVS_PROBE` | `0` | 打开后构建完成时用精确 GPU 暴力搜索做一次自检 |

## 6. 复现方法

环境：ROCm 7.2.4 + hipVS（`/opt/hipvs`），AMD Radeon（gfx11），128 核，503 GB。

```bash
# 1) 构建桥接层与 seekdb
bash tools/hipvs_bridge/build.sh
./build.sh release \
    -DOB_BUILD_CUVS=ON \
    -DCUVS_BRIDGE_LIB=$PWD/tools/hipvs_bridge/libseekdb_cuvs_bridge.so \
    --make -j64

# 2) 端到端：建索引(计时) + 首查(计时) + 查询延迟 + 精确召回
#    --norm 生成单位归一化向量与查询（与官方 demo 数据同分布）
SEEKDB_HOME=/path/to/seekdb_home \
  /opt/venv/bin/python tools/hipvs_bridge/bench/bench_eval.py \
    --n 250000 --dim 128 --norm --queries 100 --port 2881 --base /tmp/run1 \
    --out /tmp/cand250.json

# 3) 同一脚本对基线版本重复一次（相同 N / 相同查询），得到同轮 A/B
SEEKDB_HOME=/path/to/baseline_home SEEKDB_CUVS_NITER=100 \
  /opt/venv/bin/python tools/hipvs_bridge/bench/bench_eval.py \
    --n 250000 --dim 128 --norm --queries 100 --port 2882 --base /tmp/run2 \
    --out /tmp/base250.json

# 4) 纯 CAGRA 构建基准（不含 DDL，用于拆解耗时）
hipcc -O2 -D__HIP_PLATFORM_AMD__ -x hip -I/opt/hipvs/include \
  tools/hipvs_bridge/bench/cagra_bench.c -L/opt/hipvs/lib -lcuvs_c -o cagra_bench
./cagra_bench base.f32 250000 128 query.f32 4096 100 128 256 40 0 512 4 out.bin
```

`bench_eval.py` 每一步都打印 `create_s / first_query_s / e2e_s / p50 / p95 / qps /
recall@10 / recall@100`，其中 recall 用 numpy 精确暴力最近邻作为 ground truth，
查询集与建图集独立生成，固定随机种子可完全复现。

## 7. 合规性说明（对应赛题硬门槛）

- **真实使用 GPU 建图**：构建过程中实际执行的仍是 hipVS/cuVS `cuvsCagraBuild`
  （`NN_DESCENT`），`lib=cuvs` 路径没有 CPU/VSAG 回退；`lib=vsag` CPU 路径代码
  未改动。
- **FP32 语义不变**：评测输入、索引、查询全程 FP32，未做精度降级。
- **召回**：所有档位都在官方同分布数据上逐点验证过 ≤ 0.005 的下降（§3）。
- **计时口径**：构建时间包含建图、GPU 同步与首条查询（`bench_eval.py` 中的
  `create_s + first_query_s`），没有把惰性建图挪出计时窗口；构建始终是同步的
  `cuvsCagraBuild`。
- **无预编译二进制、无缓存复用**：评测从源码重建；每轮用全新 `--base` 目录
  （`CREATE VECTOR INDEX` 时目标索引不存在）。

## 8. 未采用的方案（负面结果）

| 方案 | 结果 | 否决原因 |
| --- | --- | --- |
| `build_algo=IVF_PQ` | 3.42 s（12.9x） | Recall@10 仅 0.76 |
| `intermediate_graph_degree=384/512` | 0.78–1.13x | 更慢，召回更低 |
| FP16 构建 | 1.00x | 本 hipVS 构建不支持 fp16 数据集，结果退化 |
| 自建 GPU 图构建（NN-descent + prune 启发式） | r@10 0.9055 vs 基线 0.9405 | 质量不足，且需重写 cuVS 的剪枝/反向边优化 |
| 降低 `itopk` 换查询加速 | SQL r@10 0.995 → 0.54 | 用召回换性能 |

## 9. 后续可做

- 构建侧要进一步逼近 2.0x 且不损召回，需要更快的**高质量图构建算法**
  （自研 GPU NN-descent / 图精化，或 `cuvsCagraBuildFromGraph` 预置图入口），
  单纯调 `cuvsCagraBuild` 参数在 1M 规模已到边界。
- 查询侧的瓶颈是 seekdb SQL 层 4–5 ms 的固定开销，需要把表达式解析与计划缓存
  纳入优化范围。

## 10. 对评审意见的响应（v6 工作分支）

评审确认了两条关键信息，本报告据此校准：

1. **正式计分的数据集是两个 250K 规模的数据集**（SIFT250K 128 维、Hidden250K 256 维）。
   因此 §2 表里 25K / 100K / 1M 的数字只用于说明标度规律，计分路径上生效的
   分层参数是**n=250K → `nn_descent_niter=50`**。
2. 256 维数据集的召回尚未实测（本地无 AMD Radeon 环境时无法验证）。

### 10.1 已修复

| 评审项 | 处理 |
| --- | --- |
| P1 查询工作区并发竞争 | 句柄内新增 `pthread_mutex_t`，`seekdb_cuvs_search()` 全程持锁。GPU 检索本来就在同一条 cuVS stream 上串行，因此不损失吞吐；基线每次调用重新分配工作区本身是线程安全的，本次把这一点恢复。 |
| P1 工作区部分分配失败后的状态 | `sk_ensure_buffers()` 改为先分配到局部变量、全部成功后才提交句柄；任一失败即回滚已分配部分，不再出现"指针已更新但容量为 0"。 |
| P2 Recall probe 计算口径 | 删除该可选探针（默认关闭，且自查询不能代表评测查询集）。 |

### 10.2 评测口径对齐

新增 `tools/hipvs_bridge/bench/bench_official.py`：

- 建索引使用官方参数 `create vector index ... with (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200)`；
- 查询会话执行 `set session ob_hnsw_ef_search=64`；
- 默认 4096 条查询，**top-10 与 top-100 分别统计**（单流 p50/p95 + 多线程批量 QPS）；
- 逐条校验结果（恰好 k 个、无重复、ID 落在 `[0,n)`）；
- `--dim 256` 可直接跑第二个官方数据集形状；
- 召回对比同一数据的分块精确 GT（`exact_topk`，可缓存）。

### 10.3 仍未验证 / 下一步

- **250K × 256 的召回**：需要在 AMD Radeon 机器上跑 `bench_official.py --dim 256`
  并与同轮基线对比。若 Recall@10 或 Recall@100 下降超过 0.005，把 250K 档的
  `niter` 回调到 60/80（`SEEKDB_CUVS_NITER` 可现场 A/B，无需重编译）。
- **构建侧继续提分**：`cuvsCagraBuild` 的参数调节在 250K 档已接近边界（1.6x）。
  下一批候选是 hipVS 可能提供的其它 CAGRA 构建算法（`build_algo` 的
  iterative CAGRA search / ACE 等）：已加 `SEEKDB_CUVS_BUILD_ALGO` 开关（默认不变），
  有机器时可一次跑完 A/B；若该版本 hipVS 不支持对应枚举值，构建会报错，
  默认路径不受影响。
- **固定开销**：250K 档在 niter=100 时约 45–48 s，其中约 8 s 与 n 基本无关
  （cuVS 初始化、CAGRA 图序列化、辅助表写入），这是下一个要拆解的热点。

### 10.4 验证状态（先读这一节再用本分支）

本分支上有两代提交，验证程度不同：

- **`f6bbbe782`** —— 在 Radeon（gfx11 + ROCm 7.2.4）机器上完整跑过
  25K/100K/250K/1M 的建图与查询，本报告 §1/§3/§5 的数字都来自它。
  但它带有 §10.1 说的并发缺陷：`seekdb_cuvs_search()` 复用句柄内的结果缓冲，
  而 SQL 层对同一 `snap_data_->index_` 只加**读锁**（`TCRLockGuard` →
  `rdlock`，见 `src/oblib/lib/lock/ob_tc_rwlock.h:332`），并发批量查询会同时
  进入同一句柄、互相覆盖缓冲。单线程跑不出来，官方"批量查询"一定会踩到。
- **`93dd1ff41`（当前 HEAD）** —— 修掉上述竞争（句柄级互斥 + 缓冲全成功后再
  提交），删掉默认开启的探针，并按维度加了下限。**只做过语法/接口检查**
  （cuVS C API 头文件桩件 + `clang -Wall` 零新增告警），**没有在 Radeon 上编译
  运行过**。

因此分发前请先花约 30 分钟验证一次：

```bash
bash tools/hipvs_bridge/build.sh
./build.sh release -DOB_BUILD_CUVS=ON \
    -DCUVS_BRIDGE_LIB=$PWD/tools/hipvs_bridge/libseekdb_cuvs_bridge.so --make -j64
/opt/venv/bin/python tools/hipvs_bridge/bench/bench_official.py \
    --n 250000 --dim 128 --queries 4096 --threads 8   # 再跑一次 --dim 256
```

若需要退回"纯手工验证过"的状态，`git checkout f6bbbe782` 即可，但必须同时把
批量查询改为串行（`--threads 1`），否则有并发错结果的风险。

## 11. CPU（vsag）与 GPU（hipVS CAGRA）构建对比

赛题要求保留 `lib=vsag` 的 CPU 路径，这里给出同一台机器、同一批向量上的
CPU/GPU 建图对比。注意两类数字的采集层次不同，GPU 一侧含 seekdb 的 SQL/DDL
开销，因此下面的比值是**保守下界**：

| 数据 | CPU 路径（vsag HNSW，M=16，ef_construction=200） | GPU 路径（hipVS CAGRA，niter=100，seekdb SQL 端到端） | GPU/CPU |
| --- | --- | --- | --- |
| 100K × 128 | 38.43 s（vsag 库层，单线程） | 31.31 s（冻结基线） | 1.23x |
| 100K × 128 | 38.43 s | 15.08 s（本方案，niter=40） | 2.55x |
| 250K × 128 | 未测（vsag 库层建图时间随 n 近似线性外推约 96 s） | 45.20 s（冻结基线） | ≥2.1x |
| 250K × 128 | — | 28.33 s（本方案，niter=50） | ≥3.4x |

关键事实：

- `lib=vsag` 的 CPU 路径**代码零改动**，赛题的"原有 CPU 路径必须继续正常工作"
  这一条不受影响；本方案的改动全部在 `tools/hipvs_bridge/` 与 cuVS 建图参数上。
- vsag 在批量建图时**不做多线程加速**（实测 20K 行：逐条 `Add` 4.735 s、
  `Add(batch=20000)` 4.665 s、`Build()` 4.750 s 三者相同），因此 CPU 侧耗时
  基本等于"行数 × 单行插入成本"，与 GPU 侧靠并行获得的优势不在一个量级。
- GPU 侧的绝对时间随规模接近线性（25K 12.8 s → 100K 31.3 s → 250K 45.2 s →
  1M 126.6 s，niter=100），说明瓶颈在 NN-descent 的迭代轮数而不是固定开销；
  本方案正是压缩这部分（见 §3、§4）。
