# Build the candidate bridge from its own sources.  Keeping this wrapper in a
# static archive prevents an older libseekdb_cuvs_bridge.so in LD_LIBRARY_PATH
# from silently replacing the candidate implementation at runtime.
function(seekdb_link_cuvs_bridge target)
  if(NOT OB_BUILD_CUVS)
    return()
  endif()

  if(NOT OB_BUILD_CUVS_FROM_SOURCE)
    if(NOT EXISTS "${CUVS_BRIDGE_LIB}")
      message(FATAL_ERROR
        "OB_BUILD_CUVS_FROM_SOURCE=OFF requires an existing CUVS_BRIDGE_LIB")
    endif()
    target_link_libraries(${target} PUBLIC "${CUVS_BRIDGE_LIB}")
    return()
  endif()

  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The source-built hipVS bridge requires Linux")
  endif()

  set(_hipvs_root "$ENV{HIPVS_ROOT}")
  if(NOT _hipvs_root)
    set(_hipvs_root "/opt/hipvs")
  endif()
  set(_rocm_root "$ENV{ROCM_ROOT}")
  if(NOT _rocm_root)
    set(_rocm_root "/opt/rocm")
  endif()
  set(CUVS_HIPVS_ROOT "${_hipvs_root}" CACHE PATH "hipVS development installation")
  set(CUVS_ROCM_ROOT "${_rocm_root}" CACHE PATH "ROCm development installation")
  find_program(CUVS_HIPCC NAMES hipcc
    HINTS "${CUVS_ROCM_ROOT}/bin" REQUIRED)
  find_library(CUVS_C_LIBRARY NAMES cuvs_c
    HINTS "${CUVS_HIPVS_ROOT}/lib" "${CUVS_HIPVS_ROOT}/lib64"
    NO_DEFAULT_PATH REQUIRED)
  find_library(CUVS_HIP_RUNTIME_LIBRARY NAMES amdhip64
    HINTS "${CUVS_ROCM_ROOT}/lib" "${CUVS_ROCM_ROOT}/lib64"
    NO_DEFAULT_PATH REQUIRED)
  if(NOT EXISTS "${CUVS_HIPVS_ROOT}/include/cuvs/core/c_api.h")
    message(FATAL_ERROR "hipVS development headers are missing in CUVS_HIPVS_ROOT")
  endif()

  get_filename_component(_hipvs_lib_dir "${CUVS_C_LIBRARY}" DIRECTORY)
  set(_bridge_dir "${CMAKE_BINARY_DIR}/hipvs_bridge")
  set(_bridge_archive "${_bridge_dir}/libseekdb_cuvs_bridge_source.a")
  set(_bridge_source_dir "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge")
  add_custom_command(
    OUTPUT "${_bridge_archive}"
    BYPRODUCTS "${_bridge_dir}/libseekdb_cuvs_bridge_source.o"
    COMMAND "${CMAKE_COMMAND}" -E env
      "HIPVS_ROOT=${CUVS_HIPVS_ROOT}"
      "HIPVS_LIB_DIR=${_hipvs_lib_dir}"
      "ROCM_ROOT=${CUVS_ROCM_ROOT}"
      "HIPCC=${CUVS_HIPCC}"
      "AR=${CMAKE_AR}"
      bash "${_bridge_source_dir}/build.sh" "${_bridge_archive}"
    DEPENDS
      "${_bridge_source_dir}/build.sh"
      "${_bridge_source_dir}/seekdb_cuvs_bridge.c"
      "${_bridge_source_dir}/seekdb_cuvs_bridge.h"
    COMMENT "Building the hipVS bridge from candidate sources"
    VERBATIM)
  add_custom_target(seekdb_cuvs_bridge_source_build DEPENDS "${_bridge_archive}")
  add_library(seekdb_cuvs_bridge_source STATIC IMPORTED GLOBAL)
  set_target_properties(seekdb_cuvs_bridge_source PROPERTIES
    IMPORTED_LOCATION "${_bridge_archive}"
    INTERFACE_LINK_LIBRARIES "${CUVS_C_LIBRARY};${CUVS_HIP_RUNTIME_LIBRARY}")
  add_dependencies(seekdb_cuvs_bridge_source seekdb_cuvs_bridge_source_build)
  target_link_libraries(${target} PUBLIC seekdb_cuvs_bridge_source)
  message(STATUS "cuVS bridge: candidate sources (static); external CUVS_BRIDGE_LIB is unused")
endfunction()
