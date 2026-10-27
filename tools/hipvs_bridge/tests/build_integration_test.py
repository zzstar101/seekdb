#!/usr/bin/env python3
"""Exercise the production bridge build/link rules with a CPU-only fake SDK.

This validates CMake dependency propagation and source selection, not HIP ABI,
GPU execution or a complete seekdb build. Requires Linux, cmake, make and g++.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def run(*args, env=None, expected=0):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    if result.returncode != expected:
        raise RuntimeError(f"command failed ({result.returncode}): {args}\n{result.stdout}")
    return result.stdout


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)


STUB_CUVS = r'''
#include <cuvs/neighbors/cagra.h>
extern "C" {
const char *cuvsGetLastErrorText(void) { return "link fixture"; }
cuvsError_t cuvsResourcesCreate(cuvsResources_t *) { return CUVS_ERROR; }
cuvsError_t cuvsResourcesDestroy(cuvsResources_t) { return CUVS_SUCCESS; }
cuvsError_t cuvsStreamGet(cuvsResources_t, cudaStream_t *) { return CUVS_ERROR; }
cuvsError_t cuvsStreamSync(cuvsResources_t) { return CUVS_SUCCESS; }
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void **, size_t) { return CUVS_ERROR; }
cuvsError_t cuvsRMMFree(cuvsResources_t, void *, size_t) { return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *) { return CUVS_ERROR; }
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t) { return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *) { return CUVS_ERROR; }
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t) { return CUVS_SUCCESS; }
cuvsError_t cuvsCagraBuild(cuvsResources_t, cuvsCagraIndexParams_t,
                         DLManagedTensor *, cuvsCagraIndex_t) { return CUVS_ERROR; }
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *) { return CUVS_ERROR; }
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t) { return CUVS_SUCCESS; }
cuvsError_t cuvsCagraSearch(cuvsResources_t, cuvsCagraSearchParams_t, cuvsCagraIndex_t,
    DLManagedTensor *, DLManagedTensor *, DLManagedTensor *, cuvsFilter) {
  return CUVS_ERROR;
}
}
'''

STUB_HIP = r'''
#include <cuda_runtime.h>
extern "C" cudaError_t cudaMemcpyAsync(void *, const void *, size_t,
                                      cudaMemcpyKind, cudaStream_t) {
  return cudaErrorUnknown;
}
'''

CMAKE_PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(bridge_link_contract LANGUAGES CXX)
option(OB_BUILD_CUVS "Build bridge" ON)
option(OB_BUILD_CUVS_FROM_SOURCE "Build candidate source" ON)
add_library(oblib_lib OBJECT consumer.cpp)
set_target_properties(oblib_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(OB_BUILD_CUVS)
  target_compile_definitions(oblib_lib PRIVATE OB_BUILD_CUVS)
endif()
include(cmake/Cuvs.cmake)
seekdb_link_cuvs_bridge(oblib_lib)
add_library(fixture_oceanbase SHARED anchor.cpp)
target_link_libraries(fixture_oceanbase PRIVATE oblib_lib)
target_link_options(fixture_oceanbase PRIVATE -Wl,--no-undefined)
add_executable(probe main.cpp)
target_link_libraries(probe PRIVATE fixture_oceanbase)
'''

CONSUMER = r'''
#include "tools/hipvs_bridge/seekdb_cuvs_bridge.h"
extern "C" int consumer_check() {
#ifdef OB_BUILD_CUVS
  // Actual candidate input validation differs deliberately from the old fixture.
  return seekdb_cuvs_build(nullptr, 0, 0) == nullptr ? 0 : 1;
#else
  return 0;
#endif
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default=os.environ.get("CMAKE", "cmake"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="seekdb-bridge-link-") as directory:
        temp = Path(directory)
        source, hipvs, rocm = temp / "source", temp / "hipvs", temp / "rocm"
        for name in ("cmake/Cuvs.cmake", "tools/hipvs_bridge/build.sh",
                     "tools/hipvs_bridge/seekdb_cuvs_bridge.c",
                     "tools/hipvs_bridge/seekdb_cuvs_bridge.h"):
            destination = source / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, destination)
        shutil.copytree(HERE / "mock_include", hipvs / "include")
        write(source / "CMakeLists.txt", CMAKE_PROJECT)
        write(source / "consumer.cpp", CONSUMER)
        write(source / "anchor.cpp", 'extern "C" int fixture_anchor() { return 0; }\n')
        write(source / "main.cpp", 'extern "C" int consumer_check();\nint main() { return consumer_check(); }\n')
        write(temp / "cuvs.cpp", STUB_CUVS)
        write(temp / "hip.cpp", STUB_HIP)
        (hipvs / "lib").mkdir()
        (rocm / "lib").mkdir(parents=True)
        for source_name, destination, soname in (
            ("cuvs.cpp", hipvs / "lib/libcuvs_c.so", "libcuvs_c.so"),
            ("hip.cpp", rocm / "lib/libamdhip64.so", "libamdhip64.so"),
        ):
            run(args.cxx, "-fPIC", "-shared", f"-I{hipvs / 'include'}",
                str(temp / source_name), f"-Wl,-soname,{soname}", "-o", str(destination))
        # A fake compiler driver forwards the actual production compile command.
        hipcc = rocm / "bin/hipcc"
        write(hipcc, '#!/usr/bin/env bash\nset -euo pipefail\nexec "$TEST_CXX" "$@"\n')
        hipcc.chmod(0o755)
        legacy = temp / "legacy/libseekdb_cuvs_bridge.so"
        write(temp / "legacy.cpp", 'extern "C" void *seekdb_cuvs_build(const float *, long, long) { return (void *)1; }\n')
        legacy.parent.mkdir()
        run(args.cxx, "-fPIC", "-shared", str(temp / "legacy.cpp"),
            "-Wl,-soname,libseekdb_cuvs_bridge.so", "-o", str(legacy))
        env = dict(os.environ, TEST_CXX=args.cxx)
        # Deliberately put an incompatible older bridge first in the loader path.
        env["LD_LIBRARY_PATH"] = ":".join(map(str, (legacy.parent, hipvs / "lib", rocm / "lib")))
        build = temp / "build-source"
        run(args.cmake, "-S", str(source), "-B", str(build), "-G", "Unix Makefiles",
            f"-DCMAKE_CXX_COMPILER={args.cxx}", f"-DCUVS_HIPVS_ROOT={hipvs}",
            f"-DCUVS_ROCM_ROOT={rocm}", f"-DCUVS_BRIDGE_LIB={legacy}", env=env)
        run(args.cmake, "--build", str(build), "--parallel", "2", env=env)
        run(str(build / "probe"), env=env)
        library = build / "libfixture_oceanbase.so"
        dynamic = run("readelf", "-d", str(library))
        assert "libseekdb_cuvs_bridge.so" not in dynamic, dynamic
        assert "libcuvs_c.so" in dynamic and "libamdhip64.so" in dynamic, dynamic
        assert " T seekdb_cuvs_build" in run("nm", "-D", str(library))
        archive = build / "hipvs_bridge/libseekdb_cuvs_bridge_source.a"
        before = archive.stat().st_mtime_ns
        header = source / "tools/hipvs_bridge/seekdb_cuvs_bridge.h"
        header.write_text(header.read_text() + "\n/* incremental rebuild probe */\n")
        run(args.cmake, "--build", str(build), "--parallel", "2", env=env)
        assert archive.stat().st_mtime_ns > before, "header edit did not rebuild bridge"
        run(str(build / "probe"), env=env)
        print("PASS: candidate source archive, object-to-shared dependency propagation, PIC, stale-library isolation, rebuild")

        cpu = temp / "build-cpu"
        run(args.cmake, "-S", str(source), "-B", str(cpu), "-G", "Unix Makefiles",
            "-DOB_BUILD_CUVS=OFF", "-DCUVS_HIPVS_ROOT=/missing-sdk",
            "-DCUVS_ROCM_ROOT=/missing-sdk", env=env)
        run(args.cmake, "--build", str(cpu), "--parallel", "2", env=env)
        run(str(cpu / "probe"), env=env)
        assert "CUVS_HIPCC:" not in (cpu / "CMakeCache.txt").read_text()
        assert not (cpu / "hipvs_bridge").exists()
        print("PASS: CPU-only consumer neither discovers nor builds HIP dependencies")

        external = temp / "build-external"
        run(args.cmake, "-S", str(source), "-B", str(external), "-G", "Unix Makefiles",
            "-DOB_BUILD_CUVS_FROM_SOURCE=OFF", f"-DCUVS_BRIDGE_LIB={legacy}", env=env)
        run(args.cmake, "--build", str(external), "--parallel", "2", env=env)
        run(str(external / "probe"), env=env, expected=1)
        assert "libseekdb_cuvs_bridge.so" in run("readelf", "-d", str(external / "libfixture_oceanbase.so"))
        print("PASS: explicit external bridge mode retains legacy behavior")


if __name__ == "__main__":
    main()
