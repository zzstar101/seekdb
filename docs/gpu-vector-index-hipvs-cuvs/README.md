# hipVS/cuVS GPU vector backend

The frozen contest baseline documents Phase 1 validation on AMD `gfx1100` with
ROCm 7.2.4. The candidate changes add source-built bridge integration, reusable
query workspace and reusable large-stack workers. These changes have host-side
contract tests; **candidate ROCm compilation, GPU correctness and performance
are still unverified**. See [the candidate report](CONTEST_OPTIMIZATION.md) for
the exact scope and reproducible test procedure.

## Scope

The backend adds optional hipVS/cuVS acceleration behind seekdb's existing
vector adaptor. It does not replace VSAG globally.

- GPU support is compiled only with `OB_BUILD_CUVS=ON`; the default is `OFF`.
- A dense L2 HNSW/HGRAPH index opts in with `WITH (lib=cuvs)`.
- `lib=vsag` indexes keep their existing CPU behavior.
- Cosine and inner-product metrics are rejected for `lib=cuvs` at DDL time.
- Filters, deletes, stale GPU indexes, and unavailable GPUs fall back to VSAG.
- `dbms_vector.batch_knn` exposes one-call batched GPU ANN for explicit batch
  workloads.

The product integration lives in:

- [`ob_vsag_adaptor.cpp`](../../src/oblib/lib/vector/ob_vsag_adaptor.cpp):
  per-index marking, buffered vectors, lazy CAGRA build/search, freshness and
  safety fallback, trace points, and batch entry points.
- [`ob_plugin_vector_index_adaptor.cpp`](../../src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp):
  maps declarative `lib=cuvs` indexes to the adaptor handle.
- [`ob_vector_index_util.cpp`](../../src/observer/vector_index/ob_vector_index_util.cpp):
  validates the L2-only DDL contract.
- [`ob_dbms_vector_mysql.cpp`](../../src/pl/sys_package/ob_dbms_vector_mysql.cpp):
  implements `dbms_vector.batch_knn`.

## Bridge

seekdb links a small C ABI bridge instead of importing hipVS/cuVS C++ types into
the server. The maintained bridge source and build entry point are under
[`tools/hipvs_bridge`](../../tools/hipvs_bridge/). GPU builds now compile that
source into a PIC static archive and link it into seekdb. The hipVS C library
and HIP runtime remain shared dependencies. An older bridge `.so` in
`LD_LIBRARY_PATH` therefore cannot silently replace the candidate wrapper.

The bridge exports:

```text
seekdb_cuvs_build
seekdb_cuvs_search
seekdb_cuvs_free
seekdb_cuvs_cagra_knn
```

Build seekdb from the candidate sources (requires the hipVS development headers,
`libcuvs_c.so`, ROCm `hipcc` and `libamdhip64.so`):

```bash
export HIPVS_ROOT=/opt/hipvs
export ROCM_ROOT=/opt/rocm
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_FROM_SOURCE=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  --make -j"$(nproc)"
```

The paths may also be set using `-DCUVS_HIPVS_ROOT=...`,
`-DCUVS_ROCM_ROOT=...` and `-DCUVS_HIPCC=...`. Source mode is the default when
`OB_BUILD_CUVS=ON`; an existing `CUVS_BRIDGE_LIB` argument is ignored in that mode.
The archive is generated at
`build_release/hipvs_bridge/libseekdb_cuvs_bridge_source.a`.

For compatibility with the frozen baseline's external-wrapper workflow:

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_FROM_SOURCE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so \
  -DOB_BUILD_CUVS_TRACE=OFF \
  --make -j"$(nproc)"
```

Do not use a pre-existing wrapper to evaluate candidate source changes.

For a CPU-only build, leave the defaults or pass:

```bash
./build.sh release \
  -DOB_BUILD_CUVS=OFF \
  -DOB_BUILD_CUVS_TRACE=OFF \
  --make -j64
```

## SQL usage

Per-index GPU opt-in:

```sql
CREATE TABLE items(
  id BIGINT PRIMARY KEY,
  embedding VECTOR(128),
  VECTOR INDEX idx_embedding(embedding)
    WITH (distance=l2, type=hnsw, lib=cuvs)
);
```

Explicit batch ANN writes results to a caller-created output table:

```sql
CALL dbms_vector.batch_knn(
  "items",
  "query_vectors",
  10,
  "batch_results"
);
```

See [`examples/batch_knn.sql`](examples/batch_knn.sql) for the table contract.

## Validation

The self-contained GPU smoke runner generates deterministic vectors and ground
truth; it does not use private `/work/datasets` or `/work/bench` assets:

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh \
    --observer /path/to/trace-build/seekdb \
    --base-dir /tmp/seekdb-cuvs-smoke \
    --port 2981 \
    --render /dev/dri/renderD135 \
    --evidence /tmp/seekdb-cuvs-evidence
```

Build this validation binary with `-DOB_BUILD_CUVS_TRACE=ON`. For the frozen
baseline or explicit external-wrapper mode, also pass
`--bridge /path/to/libseekdb_cuvs_bridge.so`. Use a separate trace-disabled
release build for timing; the smoke runner is a correctness check, not the
contest performance harness. Choose disposable smoke directories because the
runner recreates its own `main`, `no-gpu` and generated data subdirectories.

The runner validates:

- cuVS L2 build/serve and same-process VSAG isolation;
- non-L2 DDL rejection;
- filter, delete, and freshness fallback;
- threshold-triggered CAGRA rebuild;
- one-call `batch_knn`, row cardinality, and recall;
- GPU device file descriptors and hidden-GPU fallback;
- bounded observer cleanup and closed ports.

The runner writes a machine-readable `summary.json` into its evidence directory.
Results for a specific candidate belong in that pull request's description or a
validation comment rather than in the source tree.
The no-GPU DDL contract remains covered by
[`vector_index_cuvs_ddl.test`](../../tools/deploy/mysql_test/test_suite/vector_index/t/vector_index_cuvs_ddl.test).

Host-side contract tests, which do not require ROCm or a GPU, are documented in
[`tools/hipvs_bridge/tests/README.md`](../../tools/hipvs_bridge/tests/README.md).

## Current limitations

- Phase 1 supports dense float32 L2 indexes only.
- GPU indexes are process-local and rebuilt lazily; persistence and cache reuse
  are follow-up work.
- The explicit batch API rebuilds its CAGRA index per call.
- GPU validation is manual because upstream CI has no compatible GPU runner.
- Multi-GPU, long-duration, and snapshot-reload coverage are out of scope for
  this phase.
