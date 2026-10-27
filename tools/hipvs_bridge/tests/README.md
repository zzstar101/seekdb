# Host contract tests without a GPU

Run from the repository root:

```bash
tools/hipvs_bridge/tests/run_mock_tests.sh
SANITIZE=1 tools/hipvs_bridge/tests/run_mock_tests.sh
bash tools/hipvs_bridge/tests/worker_test.sh
python3 tools/hipvs_bridge/tests/adaptor_job_test.py
```

The runner compiles the actual `seekdb_cuvs_bridge.c` as both C11 and C++11,
with warnings treated as errors. The C++ mode covers the language mode used by
the HIP compiler. `SANITIZE=1` enables AddressSanitizer and UndefinedBehaviorSanitizer;
LeakSanitizer needs permission to inspect its own process in `/proc`.

`worker_test.sh` exercises the real persistent worker, including thread creation
failures, exception reporting, repeated dispatch and cleanup. `adaptor_job_test.py`
extracts the production adaptor callbacks and injects host allocation failure to
check that the previous graph, ID mapping and generation remain intact. The
adaptor test also supports `SANITIZE=1`.

These tests substitute a small, test-only cuVS/runtime API. Copies, build and
search remain queued until the resource stream is synchronized. Allocation and
deallocation do not implicitly complete queued operations. The mock search
computes exact FP32 distances only to check buffer contents and output ordering;
it does **not** simulate CAGRA or validate GPU Recall/performance.

Coverage includes:

- A completed build no longer references the caller's host input.
- 100 repeated same-size searches allocate no additional device workspace or
  search parameters; smaller requests reuse it and larger requests grow it.
- Each allocation is freed using its original capacity, not the most recent
  request's dimensions, and separate handles own separate allocations.
- Build/setup failures and each of the three initial/growth allocation failures
  release resources or retain a workspace that can be safely retried.
- Copies use the resource stream; successful return includes completed results.
- Copy and search submission failures drain queued work before returning.
- A synchronization or device release failure prevents subsequent reuse of the
  handle; allocation failures can be retried.
- Device release failures before and after deallocation receive at most one
  release attempt. The former intentionally leaves an isolated allocation;
  test teardown simulates process-exit cleanup for that allocation. This is
  **not** a claim that fatal backend release errors are leak-free.
- Invalid pointers/shapes, top-k beyond the dataset and byte-count overflow are
  rejected before backend work.

Actual ROCm/hipVS compilation, GPU execution, Recall@10/100, device memory peaks
and the contest's alternating cold-start measurements remain required on the
AMD test server.

## Source build and link integration

```bash
python3 tools/hipvs_bridge/tests/build_integration_test.py
# Or select an installed CMake explicitly:
python3 tools/hipvs_bridge/tests/build_integration_test.py --cmake /path/to/cmake
```

This Linux test requires CMake 3.20+, `make`, `g++`, `readelf` and `nm`. It copies
the actual production CMake module, bridge and build script into a temporary
project and supplies test-only SDK stubs. It verifies PIC compilation, propagation
from an object library into a shared consumer, explicit hipVS/HIP linking,
header-triggered rebuilds, isolation from an incompatible older wrapper in the
loader path, CPU-only builds without SDK discovery, and explicit external-wrapper
compatibility. It does not establish real HIP ABI compatibility or compile the
complete seekdb server.

## Development SQL measurement

```bash
python3 tools/hipvs_bridge/tests/test_measure_index_build.py
python3 tools/hipvs_bridge/tests/measure_index_build.py --help
```

The first command tests timing boundaries and ID checks against a mock database;
no third-party dependencies are required. Actual measurements need `numpy`,
`pymysql`, a prepared development database and an already started seekdb service.
The script creates a new table, imports deterministic FP32 vectors outside the
timer, and times index DDL through the first query's complete result fetch. It
then measures single-query Recall@10/100, QPS and latencies. It retains the table
for manual cleanup and never replaces an existing table.

See [the candidate report](../../../docs/gpu-vector-index-hipvs-cuvs/CONTEST_OPTIMIZATION.md)
for the alternating cold-run procedure. The script does not restart the server,
prove GPU execution or binary identity, measure batch throughput, enforce the
contest gates, or calculate a score. These remain separate validation tasks.
