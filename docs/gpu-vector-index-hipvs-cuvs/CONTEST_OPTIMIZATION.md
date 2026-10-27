# AMD CAGRA 候选实现与复现实验记录

## 1. 当前结论

本候选从比赛冻结 commit
`08c76418a8e1fef3c1aae83010658ff56a4faa57` 开发，保留 FP32、L2、
`type=hnsw,lib=cuvs` 的 SQL 契约、CAGRA 默认算法参数及原有 VSAG CPU 路径。
第一轮实现聚焦查询工作区复用、大栈线程复用、失败后的状态一致性，以及确保
候选 bridge 源码实际进入构建产物。

**当前只有 host-side 验证，没有真实 AMD GPU 的编译、正确性或性能结果。**
当前环境没有 `/dev/kfd`、Radeon render 设备、`hipcc` 或 `rocminfo`。
下文的测试通过不能证明 Recall 达标、GPU 路径实际执行、完整 seekdb 编译成功，
也不能证明达到比赛暂定的 2.0× 构建或 1.5× 查询满分线。

开发、代码审查和 host-side 测试可以在普通 CPU 环境完成。真实 ROCm 编译、
CAGRA 执行、显存峰值和性能验证需要可访问的兼容 AMD GPU 环境；SSH 终端足够，
不要求有桌面图形界面。正式成绩仍由组织方的统一环境产生。

基线来源：

- 仓库：<https://github.com/zihaomu/seekdb>
- 比赛分支：`amd-oceanbase-official-contest-base`
- 冻结 tag：`amd-cagra-contest-baseline-v1`
- 冻结源码：<https://github.com/zihaomu/seekdb/tree/08c76418a8e1fef3c1aae83010658ff56a4faa57>

## 2. 本轮实现

| 改动 | 实现位置 | 目的及边界 |
| --- | --- | --- |
| 每索引复用 query / neighbor / distance 工作区及默认 search params | `tools/hipvs_bridge/seekdb_cuvs_bridge.c` | 后续同尺寸或更小查询无需重复分配这三个 GPU buffer；首次查询仍需分配 |
| 同一 resource stream 执行复制与搜索，返回前同步 | 同上 | 保证输出与输入生命周期；提交失败也 drain 已提交任务 |
| 输入、尺寸乘法、复制与同步错误检查 | 同上及 `.h` | 普通分配失败可重试；同步或旧 buffer 释放失败后拒绝继续使用该 handle |
| 复用 32 MiB 栈的 pthread | `src/oblib/lib/vector/ob_cuvs_worker.h`、`ob_vsag_adaptor.cpp` | 避免每条查询 create/join；最多保留两个空闲 worker，不限制原有 raw batch 活跃并发 |
| 图与 ID 映射的一致发布 | `ob_vsag_adaptor.cpp` | 先完成可能失败的 ID 分配，再构图；成功后通过 swap 发布，防止新图配旧映射 |
| 默认从候选源码编译 PIC 静态 bridge | `cmake/Cuvs.cmake`、`cmake/Env.cmake`、`src/oblib/lib/CMakeLists.txt`、`tools/hipvs_bridge/build.sh` | 防止旧外部 `.so` 覆盖候选实现，显式链接 hipVS C 库及 HIP runtime |
| 源码构建与外部 bridge 两种 smoke 入口 | `validation/run_gpu_smoke.sh` | 源码静态模式可省略 `--bridge`，基线的外部库模式仍支持 |

同一 handle 的可变工作区仍由现有 registry mutex 保护。raw batch 每次持有独立
handle。新增 worker cache 只保留线程，不保存图、向量或查询结果；跨轮测试应
重启服务，不能复用旧索引或进程状态。

本轮没有改变图参数、距离精度、CPU mirror、全局 registry 锁范围、索引新鲜度
规则或持久化格式。`dbms_vector.batch_knn` 仍按基线行为每次重新构图。

### 资源代价

每个存活 GPU handle 保留三个 buffer 的各自最大需求。维度固定时，稳定容量约为：

```text
4 × max(nq × dim) + 8 × max(nq × topk) 字节
```

扩容先分配新 buffer、再释放旧 buffer，短时峰值还会增加一个替换 buffer。
最多两个空闲 worker 共保留 64 MiB 栈虚拟地址空间，这不等于 64 MiB 常驻内存；
线程也可能保留 HIP TLS 或运行时缓存。正式配额下必须测量实际 RSS 和 VRAM。

检测到 C API 设备释放报错时，不能确认它是否已释放原地址。本候选将该地址从工作区
移除、拒绝继续使用 handle，并且不重复释放不确定地址。如果错误发生在实际
释放前，该块设备内存可能残留到进程退出；这属于失败轮次，需重启服务并检查
设备状态。不能把正常路径的完整释放测试解读为所有设备错误都保证零残留。

## 3. Host-side 验证

以下验证在 Linux、GCC/G++ 13.3.0、Python 3.12.14 环境执行。构建集成 fixture
使用 CMake 4.4.4；项目要求 CMake 3.20+。

| 验证 | 结果 | 能证明什么 |
| --- | --- | --- |
| 实际 bridge 源码以 C11 和 C++11 编译执行 | PASS | 资源复用、缓冲区尺寸、延迟 stream 顺序、失败后的 drain/清理、C ABI |
| bridge 的 ASan + UBSan + LSan | PASS | 本地 mock 覆盖路径未发现相应内存/未定义行为问题 |
| 实际 worker helper 的 C++11 测试 | PASS | 顺序复用、6 个并发 callback、32 MiB 栈、创建失败、异常、关闭及 join |
| worker 的 ThreadSanitizer | PASS | 该测试覆盖的线程交互未报告数据竞争 |
| worker 的 ASan + UBSan | PASS，LSan 未启用 | 当前受限执行环境的 `/proc` 限制不支持该轮 leak scan |
| 实际 adaptor callback 的分配故障注入 | PASS，含 ASan + UBSan | 单条与批量路径在 ID 分配/建图失败后保留旧图、映射及 generation，重试可发布 |
| CMake + 生产 build.sh 的最小链接测试 | PASS | 从 object library 传播到 shared consumer、PIC、两项直接库依赖、旧 wrapper 隔离、增量重编译 |
| 同一 CMake fixture 的 CPU-only 与外部 bridge 模式 | PASS | CPU 模式不探测 HIP；显式兼容入口仍可用 |
| 开发测量脚本的 mock DB 测试 | PASS | 导入位于计时外、首次 fetch 位于计时内、非法 ID 立即失败、已有表不被改写 |
| `git diff --check`、shell 语法及 header inventory | PASS | 补丁格式、脚本语法、私有头文件登记 |
| 完整 seekdb CPU/GPU 构建、真实 SQL 回归 | 待测 | 当前环境不具备完整依赖及 AMD GPU |

桥接器 mock 连续执行 100 次同尺寸搜索时，只出现最初的 3 次工作区分配和 1 次
search params 创建。它使用测试用精确 FP32 搜索检查输出与 stream 顺序，
**不模拟真实 CAGRA，也不产生有效的 GPU Recall 或性能成绩。**

从仓库根目录运行：

```bash
bash tools/hipvs_bridge/tests/run_mock_tests.sh
bash tools/hipvs_bridge/tests/worker_test.sh
python3 tools/hipvs_bridge/tests/adaptor_job_test.py
python3 tools/hipvs_bridge/tests/build_integration_test.py
python3 tools/hipvs_bridge/tests/test_measure_index_build.py
git diff --check
```

可选 sanitizer 复验：

```bash
SANITIZE=1 bash tools/hipvs_bridge/tests/run_mock_tests.sh
SANITIZE=1 python3 tools/hipvs_bridge/tests/adaptor_job_test.py
```

LeakSanitizer 需要读取本进程的 `/proc`。受限容器无法支持时，可显式加
`ASAN_OPTIONS=detect_leaks=0`，并将结果记为 ASan/UBSan，不可记为 LSan 通过。
这些测试的故障注入会主动输出 build/search 错误，最终的 PASS 及退出码才是
测试判定。

## 4. AMD 环境编译与功能检查

先按照组织方部署资料准备完整 seekdb 编译依赖、ROCm 与 hipVS SDK，并记录
GPU 型号/显存、操作系统、ROCm 和 hipVS 版本、SDK 安装路径与构建命令。
文档中的历史 `gfx1100`、ROCm 7.2.4 记录不等于正式评测硬件已公布。

固定版本并检查祖先关系：

```bash
git rev-parse HEAD
git diff --quiet
git diff --cached --quiet
git merge-base --is-ancestor \
  08c76418a8e1fef3c1aae83010658ff56a4faa57 HEAD
```

后两项 diff 检查不检查未跟踪文件；还应检查 `git status --short`，确认构建输入
来自提交。为冻结基线和候选使用独立 checkout、构建目录与运行数据目录。

候选 GPU 构建：

```bash
export HIPVS_ROOT=/opt/hipvs
export ROCM_ROOT=/opt/rocm
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_FROM_SOURCE=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  --make -j"$(nproc)"
```

如果 SDK 路径不同，传入 `-DCUVS_HIPVS_ROOT`、`-DCUVS_ROCM_ROOT` 以及必要的
`-DCUVS_HIPCC`。默认源码模式会忽略旧的 `CUVS_BRIDGE_LIB` 参数。
候选仍依赖运行时的 `libcuvs_c.so` 和 `libamdhip64.so`；应以 `ldd` / `readelf`
记录实际加载路径。完整编译命令最终应与组织方固定环境兼容。

另建 `OB_BUILD_CUVS_TRACE=ON` 的开发验证版本，运行本目录 README 中的 GPU
smoke。它覆盖 GPU build/serve、CPU VSAG 隔离、filter/delete/freshness 回退、
batch_knn、禁用 GPU 后的回退等功能。通过 trace 与 GPU 工具确认成功的 CAGRA
构建和查询；普通 SQL 成功并不能证明未发生 CPU fallback。

正式计时使用 trace 关闭的 release 版本。功能验证日志、profiler 采样和正式
计时分开保存，避免调试 I/O 或 profiler 开销污染成绩。

CPU 编译回归另行执行：

```bash
./build.sh release -DOB_BUILD_CUVS=OFF -DOB_BUILD_CUVS_TRACE=OFF --make -j"$(nproc)"
```

## 5. 可复现性能协议

正式性能对比对象是冻结 GPU 基线与候选。CPU VSAG 是另列的参考对照。
使用相同原始 FP32 数据、L2、ID 映射、SQL、索引参数、查询参数和资源配额。

每轮先启动干净的对应服务，导入原始向量表，确认目标索引不存在，且无旧图、
索引文件或进程内缓存。导入完成后再开始计时：

1. 记录 `t0`，发出创建向量索引的 SQL。
2. SQL 返回时记录 `t1`。
3. 立即执行预先固定的第一条查询，完整读取后记录 `t2`，随后校验返回 ID；
   非法结果使整轮无效。
4. 记录 `create_s=t1-t0`、`first_query_s=t2-t1`、`end_to_end_s=t2-t0`。
5. 在该索引上继续执行单条与批量查询，测量 Recall、QPS、P50/P95；同步保存
   峰值 RSS/VRAM。首次查询耗时只能计入构建，不能从构建测量中删去。

服务启动、编译、生成数据和原始数据导入不计入构建。不要只记录桥接函数内的
CAGRA kernel 时间，也不要把创建索引 SQL 返回当作计时终点。

每个数据集按 **baseline、candidate、candidate、baseline、baseline、candidate**
执行六轮，每轮重新清理索引与缓存并恢复相同数据状态。两版本各取三次有效
`end_to_end_s` 的中位数：

```text
build_speedup = median(baseline.end_to_end_s) / median(candidate.end_to_end_s)
```

CPU 对照使用相同 FP32 数据、L2 和明确记录的 HNSW 参数，设置 `lib=vsag`，
同样计时到第一次查询返回。CPU/GPU 对照和 GPU baseline/candidate 对照分开报告。

所有有效轮次同时满足：ID 合法、数量与唯一性正确；Recall@10 和 Recall@100
达到组织方最终公布的绝对下限且相比同轮冻结基线下降不超过 0.005；每个数据集
的综合查询吞吐达到冻结基线的 90%；真实执行 GPU CAGRA；无必需功能回归。
某轮失败时保留失败原始记录，不以挑选最快的成功轮次替代评测规则。

组织方尚未给出最终 Recall 绝对阈值、资源配额、多数据集聚合及连续计分的
完整公式。不能自行假设权重平均方式或用线性插值宣称正式得分。

### 本地开发测量脚本

新增 `tools/hipvs_bridge/tests/measure_index_build.py` 可对已启动的开发服务测量
一轮。它使用固定 seed 的公开合成数据，默认 N=10000、D=128、100 条查询；
FP32 使用 9 位有效数字的文本传入数据库，记录输入数据的 SHA-256，GT 对这些
FP32 值使用 float64 累加的精确平方 L2。参数固定为 m=16、ef_construction=200、
ef_search=200，仅用于开发复现；正式评测使用组织方指定的数据与参数。

先安装 `numpy`、`pymysql`，准备已有的开发数据库，并通过环境变量设置密码。
例如空密码的本地测试服务：

```bash
SEEKDB_PASSWORD='' python3 tools/hipvs_bridge/tests/measure_index_build.py \
  --mode baseline \
  --commit 08c76418a8e1fef3c1aae83010658ff56a4faa57 \
  --host 127.0.0.1 --port 2881 --database test > baseline-r1.json
```

切换到已重启的候选服务后，填写实际完整候选 SHA：

```bash
SEEKDB_PASSWORD='' python3 tools/hipvs_bridge/tests/measure_index_build.py \
  --mode candidate --commit "$(git rev-parse HEAD)" \
  --host 127.0.0.1 --port 2881 --database test > candidate-r1.json
```

CPU 对照改为 `--mode cpu`，SHA 填实际运行版本。GPU 模式拒绝 N<256，因为
冻结基线的累计建图路径在这个范围不会触发 CAGRA；CPU 模式要求 N>=100。
`--help` 无需安装数据库驱动即可运行。

脚本只创建全新随机名称的测试表；`--table` 指定的名称已存在时立即失败。
它不自动删除任何表；JSON 的 `retained_table` 给出本次新表及手工清理 SQL。
每轮的服务重启、旧索引与缓存清理、相同资源状态都由操作者完成。

JSON 保留 `build.create_s`、`build.first_query_s`、`build.end_to_end_s`，以及
top-10/top-100 的每条延迟、每条 Recall、QPS、P50/P95。此 QPS 为单请求串行
执行与读取的计时值之和所计算，不是批量或并发吞吐。脚本不测批量查询，不
进行官方评分，也不证明实际服务器二进制对应声明 SHA。JSON 中
`external_verification` 的各项必须通过其他证据补齐。

该脚本尚未连接真实 seekdb/AMD GPU；已验证的是其 host-side 计时与校验逻辑。

## 6. CPU/GPU 构建性能报告状态

以下空白是未测，不能解释为零耗时或零分。

| 指标 | CPU VSAG | 冻结 GPU 基线 | 候选 GPU |
| --- | --- | --- | --- |
| 完整 seekdb 构建与运行 | 待测 | 待测 | 待测 |
| 端到端构建三次原始耗时 | 待测 | 待测 | 待测 |
| 端到端构建中位数 | 待测 | 待测 | 待测 |
| 相对冻结 GPU 基线构建加速 | 仅参考 | 1.0×（定义值） | 待测 |
| Recall@10 / Recall@100 | 待测 | 待测 | 待测 |
| 单条 QPS、P50、P95 | 待测 | 待测 | 待测 |
| 批量 QPS、P50、P95 | 待测 | 待测 | 待测 |
| ID 正确性、GPU 路径证明 | 待测 / 不适用 | 待测 | 待测 |
| 峰值 RSS / VRAM | 待测 / 不适用 | 待测 | 待测 |

每条原始记录还应包含完整 SHA、祖先检查退出码、环境版本、数据集形状与
hash、索引/查询参数、轮次顺序及失败原因。Notebook 的约 12.65× 批量查询
示例不能填入这里作为构建成绩，也不是本候选的实测结果。

## 7. 下一轮构建优化的决策依据

源码显示，当前 HNSW DDL 逐行进入 Add，CPU VSAG mirror 仍被构建，GPU 图到
第一次查询才惰性构建。相关入口为 `ob_ddl_pipeline.cpp`、
`ob_direct_load_struct.cpp`、`ob_plugin_vector_index_adaptor.cpp` 及
`ob_vsag_adaptor.cpp`。GPU 输入缓冲使用 `std::vector::insert`；没有证据表明
它逐行全量重拷而导致 O(N²) 增长。

本轮线程和 workspace 复用直接作用于重复查询。冷启动首次查询仍需创建
线程、分配首次工作区和构建图，因此不能据此预测 2× 构建提升。

在 Radeon 上首先拆分测量：数据扫描/收集、CPU Add 与持久化、CPU/GPU 传输、
CAGRA build、首次 search。若主耗时在 CPU mirror，应先设计保留删除、过滤、
追加与重载语义的构建方案；若主耗时在 CAGRA 或传输，再研究对应算法、数据
布局与批处理。不要未经测量就删除 CPU Add、降低图质量或把首次查询移出计时。
