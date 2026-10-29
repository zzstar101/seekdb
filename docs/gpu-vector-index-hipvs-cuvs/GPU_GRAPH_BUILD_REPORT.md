# seekdb × hipVS：全 GPU CAGRA 建图报告

基线：冻结 commit `634888d9b037153fa143d2c913ba4b25503e1d62`（tag
`amd-cagra-contest-baseline-v5`）。本分支是该 commit 的后代。

## 1. 结论

端到端时间 = `CREATE VECTOR INDEX ... WITH (distance=l2, type=hnsw, lib=cuvs, m=16,
ef_construction=200, ef_search=64)` + 第一条固定查询。查询为 4096 条串行的单行
`SELECT id ... ORDER BY l2_distance(v, ?) APPROXIMATE LIMIT k`。V5 基线和本版本在
同一台机器上运行（Radeon PRO W7900D 共享主机，cgroup 只有 16 核，主机负载 20–80，
绝对时间偏慢且有波动）。

| 数据集 | 版本 | 端到端构建 | 加速比 | Top10 QPS | Top100 QPS | Recall@10 / @100 | 显存峰值（相对空闲） |
| --- | --- | ---: | ---: | ---: | ---: | --- | ---: |
| SIFT 250K×128 | V5 基线 | 49.82 s | 1.00x | 137.9 | 131.7 | 1.0000 / 1.0000 | 673 MB |
| SIFT 250K×128 | 本版本 | 3.77 s | **13.2x** | 1037.9 | 654.2 | 1.0000 / 1.0000 | 706 MB（1.05x） |
| nytimes-256（归一化，难） | V5 基线 | 47.74 s | 1.00x | 122.3 | 114.7 | 0.4088 / 0.5295 | 796 MB |
| nytimes-256（归一化，难） | 本版本 | 6.18 s | **7.7x** | 1037.3 | 758.6 | 0.4107 / 0.5301 | 789 MB（0.99x） |
| 合成 250K×256（500 簇） | 本版本 | 6.08 s | — | 961.7 | 677.5 | 1.0000 / 1.0000 | 789 MB |

查询 QPS 相对基线：SIFT 7.5x / 5.0x，nytimes-256 8.5x / 6.6x（Top10 / Top100）。

在这个 500 簇的合成集上，基线构建流程（`cuvsCagraBuild`，NN-descent，`niter=100`，
`gbench cagra`）的 Recall@10/100 为 0.9993 / 0.9993。

CPU 对比（`lib=vsag` HNSW，同一份 SIFT 250K，同一台机器）见第 5 节。

本版本的构建由以下部分组成：建图约 1.35 s（SIFT）/ 1.85 s（256 维），其中 kNN
0.97 s / 1.47 s、optimize 0.25 s；其余是 DDL 本身的时间（扫描、排序、写入行、快照
落盘、提交）。

## 2. 改动内容

### 2.1 GPU 建图（`tools/hipvs_bridge/sk_cagra_graph.hip`）

V5 走的是 `cuvsCagraBuild(NN_DESCENT)`。hipVS 实现中，NN-descent 每一轮的图更新、
采样、最后的排序，以及 `optimize()` 的边选择、反向图收集、合并和两遍
O(n·deg²) 校验，都在 CPU 上执行（OpenMP），GPU 大部分时间空闲。
本版本把整条建图链路改为 GPU kernel：

1. **精确 kNN（k=256 = intermediate_graph_degree）**
   - 数据按块传到显存，转成带 padding 的 fp16 行。每行乘一个 2 的幂缩放因子，
     使该行最大绝对值接近 2^14；结果再精确除回去，因此不会溢出。行范数用 fp32 计算。
     NN-descent 本身也用 fp16 计算距离。
   - 距离 kernel 用 RDNA3 WMMA（`__builtin_amdgcn_wmma_f32_16x16x16_f16_w32`）计算
     fp16 乘积并以 fp32 累加。块大小为 128 个查询 × 256 个数据点，LDS 双缓冲。
   - 不生成 n×n 距离矩阵。先对固定的分层抽样（S=16384 行）计算距离，取第 k+1 小的
     距离作为该查询的阈值 T。抽样集是全集的子集，所以全集中至少有 k 个非自身点
     落在 T 以内。随后对全集只做一遍距离计算，只把距离 ≤ T 的候选写入每行的
     候选表（期望约 k·n/S ≈ 4K 个），最后对候选表做精确 top-k 选择和排序。
   - 候选表溢出或不足 k 个的行，回退到“整行距离 + 精确基数选择”。因此在 fp16
     乘积的度量下，结果始终是精确 kNN。n < 65536 时全部行都走这条路径。
2. **CAGRA optimize**：基于秩的 2-hop detour 计数和按 (detour, rank) 的稳定选边、
   反向图、合并（保护前 deg/2 条剪枝边，再插入反向边），规则与 cuVS
   `graph::optimize` 一致，全部在 GPU 上完成。反向图复用 kNN 表的显存。
3. **长程随机边**：把每行最后 `deg/16`（128 中的 8）个槽位替换为互不重复、非自身的
   伪随机边。聚簇数据的精确 kNN 图几乎没有簇间边，CAGRA 从随机种子出发的贪心
   搜索会到不了某些簇（500 簇合成集上 Recall 只有 0.8635）。NN-descent 的图从
   随机初始化中保留了这类边。加入随机边后，该数据集 Recall 为 1.0000；在 SIFT、
   nytimes 和其他合成集上 Recall 没有变化（±0.001）。
4. 建好的图连同显存中的 fp32 数据集交给 `cuvsCagraIndexFromArgs`。随后用 hipVS
   CAGRA 搜索 kernel 对 256 个数据点做一次自检（检查结果 ID 合法、互不重复，
   距离有限且有序）。任一步失败都回退到 `cuvsCagraBuild`。

显存分为三个阶段，峰值取三者中最大者：kNN 阶段（fp16 数据 + kNN 表 + 有上限的
工作缓冲）、optimize 阶段（kNN 表 + 图）、索引阶段（fp32 数据 + 图）。本实现没有
rocBLAS 依赖；rocBLAS 会额外常驻约 196 MB。

### 2.2 查询

- 单条/小批量查询（nq ≤ 16，SQL 路径都是 nq=1）用 MULTI_CTA，`itopk=max(2048, 4·k)`，
  `search_width=4`。SINGLE_CTA 只用一个 CTA，单条查询时 GPU 几乎空闲；MULTI_CTA 让多个
  CTA 从不同随机起点并行走图，kernel 时间从约 3.0 ms 降到约 0.7 ms。MULTI_CTA 的召回
  由并行路径数（itopk / 每 CTA 的 itopk）决定：itopk=512 时 nytimes 的 Recall@10 会从
  0.409 掉到 0.354，itopk=1024 时恢复到 0.410，默认取 2048 留余量（gbench，nq=1 实测）：

  | 数据集 | SINGLE_CTA itopk512 | MULTI_CTA 512 | MULTI_CTA 1024 | MULTI_CTA 2048 |
  | --- | --- | --- | --- | --- |
  | nytimes-256 R@10/@100 | .4089 / .5298 | .3541 / .5231 | .4098 / .5301 | .4107 / .5301 |
  | 500 簇合成 R@10/@100 | 1.0 / 1.0 | — | 1.0 / 1.0 | 1.0 / 1.0 |
  | SIFT R@10/@100 | 1.0 / 1.0 | — | 1.0 / 1.0 | 1.0 / 1.0 |
  | nq=1 kernel 时间 | 3.0–3.4 ms | 0.58 ms | 0.65 ms | 0.73–0.82 ms |

  每条查询返回后，bridge 在主机上检查是否是 k 个互不重复、在范围内的 ID（k≤100 时只要几微秒）。
  不满足的行用基线的 SINGLE_CTA 参数重新搜一次（不行再把 itopk 翻倍搜一次），仍是 GPU 上的 hipVS
  CAGRA。三个数据集各 4096×2 条查询实测从未触发；用 `SEEKDB_CUVS_DEBUG_REPAIR_EVERY=7` 强制每 7 条触发一次，
  结果仍然完整、召回不变。
  大批量（nq > 16）仍用基线的 SINGLE_CTA、itopk=max(4·k,512)，那里吞吐更优。
  可用 `SEEKDB_CUVS_SEARCH_ALGO`、`SEEKDB_CUVS_MULTI_CTA_ITOPK`、`SEEKDB_CUVS_MULTI_CTA_MAX_NQ` 覆盖。
- `ob_vsag_adaptor.cpp`：原来每次查询或构建都新建一个 32 MB 栈的 pthread，现改为
  一个常驻的 32 MB 栈 worker 线程，每条查询省约 1.3 ms。
- 用精确 kNN 图建出的 CAGRA 图本身也更好：同样的 SINGLE_CTA 参数下，单条查询比基线
  快约 2 倍。

### 2.3 快照

`OBCUVS02` 快照的 header 中 `reserved=1` 表示保存的是原始图：内容为 ids + 向量 +
uint32 图，不再额外写一份 cuVS 序列化文件（原来数据集会存两遍）。加载时通过
`seekdb_cuvs_from_graph` 调用 `cuvsCagraIndexFromArgs` 重建索引。`reserved=0` 的旧
格式仍可读取。

## 3. 规则符合性

- 构建：kNN、剪枝、合并都是仓库内的 GPU kernel；建出的图通过 hipVS
  `cuvsCagraIndexFromArgs` 生成 CAGRA 索引，构建期间会调用 hipVS CAGRA 搜索做校验。
  查询使用 hipVS `cuvsCagraSearch`。没有 CPU 建图，也没有 VSAG 回退。
- 路径选择只依据数据规模（n<1024 时用 `cuvsCagraBuild`），与评测阶段无关。
- 未修改 hipVS，未修改 SQL 或参数，未针对数据硬编码。`lib=vsag` 路径保持可用
  （见 `func_smoke.py`）。

## 4. 复现

```bash
# 官方编译命令（bridge 只额外编译一个 .hip 文件，链接 -lcuvs_c）
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh $OUT/libseekdb_cuvs_bridge.so
cmake -S . -B build_release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=$OUT/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb

# 端到端（数据：base.f32 / query.f32 为原始 float32，gt.i32 为 4096×100 int32）
SEEKDB_HOME=<含 bin/seekdb 的目录> python3 tools/hipvs_bridge/bench/sift_eval.py \
  --data-dir data/sift250k --n 250000 --dim 128 --nq 4096 --gt data/sift250k/gt.i32 \
  --base /path/run --port 3301 --lib-path $OUT --out res.json --tag cand

# 功能冒烟：vsag、cuvs（小规模/大规模）、重启后从快照加载
SEEKDB_HOME=... python3 tools/hipvs_bridge/bench/func_smoke.py --base /path/run2 --port 3401 --lib-path $OUT

# 单独测建图 + 召回（不经过 SQL）
gcc -O2 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include -c tools/hipvs_bridge/bench/gbench.c
hipcc -O3 --offload-arch=gfx1100 -I/opt/hipvs/include -c tools/hipvs_bridge/sk_cagra_graph.hip
hipcc gbench.o sk_cagra_graph.o -L/opt/hipvs/lib -lcuvs_c -Wl,-rpath,/opt/hipvs/lib -o gbench
./gbench custom base.f32 250000 128 query.f32 4096 gt.i32 k=256 deg=128 salgo=0   # 本版本建图
./gbench cagra  base.f32 250000 128 query.f32 4096 gt.i32 algo=2 niter=100 salgo=0 # 基线建图
```

可调环境变量（默认值即提交配置）：`SEEKDB_CUVS_GPU_GRAPH=1`、`SEEKDB_CUVS_KNN_K`
（默认等于 intermediate degree 256）、`SK_CAGRA_RANDOM_EDGES`（默认 deg/16）、
`SEEKDB_CUVS_SEARCH_ALGO=0`、`SEEKDB_CUVS_VALIDATE_QUERIES=256`。

## 5. CPU（VSAG HNSW）对比

同一份 SIFT 250K，`lib=vsag`，参数相同（m=16, ef_construction=200, ef_search=64）：
同一台机器（cgroup 16 核）上的结果如下：

| 路径 | 端到端构建 | Top10 QPS | Top100 QPS | Recall@10 / @100 |
| --- | ---: | ---: | ---: | --- |
| CPU VSAG HNSW | 82.71 s | 3454.8 | 1486.4 | 0.9809 / 0.9498 |
| V5 GPU CAGRA（基线） | 49.82 s | 137.9 | 131.7 | 1.0000 / 1.0000 |
| 本版本 GPU CAGRA | 4.07 s | 288.1 | 252.7 | 1.0000 / 1.0000 |

构建方面，本版本是 CPU HNSW 的 20.3 倍，并且召回更高。单行串行查询方面，GPU 每条
查询的固定开销（SQL → bridge → 单查询 CAGRA kernel，约 2.5 ms 在 kernel 内）明显高于
CPU HNSW 的 0.29 ms；GPU 的优势在批量查询（gbench 中 batch 每条约 0.05 ms）。
