cmake_minimum_required(VERSION 3.20)
project(file_scan_unit_tests LANGUAGES CXX)

# Standalone, dependency-free tests for the same scanner compiled into the SQL
# engine. This also runs on native macOS where the pretest framework is absent.
enable_testing()
get_filename_component(SEEKDB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
add_executable(test_file_scan_utils
  test_file_scan_utils.cpp
  "${SEEKDB_ROOT}/src/sql/engine/basic/file_scan_utils.cpp")
target_include_directories(test_file_scan_utils PRIVATE "${SEEKDB_ROOT}/src")
target_compile_features(test_file_scan_utils PRIVATE cxx_std_17)
target_compile_options(test_file_scan_utils PRIVATE -Wall -Wextra -Werror)
add_test(NAME file_scan_utils COMMAND test_file_scan_utils)

# Secure glob expansion for the glob() table function.
add_executable(test_file_sql_glob
  test_file_sql_glob.cpp
  "${SEEKDB_ROOT}/src/sql/engine/basic/file_sql_glob.cpp"
  "${SEEKDB_ROOT}/src/sql/engine/basic/file_scan_utils.cpp")
target_include_directories(test_file_sql_glob PRIVATE "${SEEKDB_ROOT}/src")
target_compile_features(test_file_sql_glob PRIVATE cxx_std_17)
target_compile_options(test_file_sql_glob PRIVATE -Wall -Wextra -Werror)
add_test(NAME file_sql_glob COMMAND test_file_sql_glob)

# Streaming CSV writer backing COPY ... TO. Commit renames a temp file into
# place, so an aborted statement leaves neither a target nor a partial file.
add_executable(test_file_sql_export
  test_file_sql_export.cpp
  "${SEEKDB_ROOT}/src/sql/engine/basic/file_sql_export.cpp"
  "${SEEKDB_ROOT}/src/sql/engine/basic/file_scan_utils.cpp")
target_include_directories(test_file_sql_export PRIVATE "${SEEKDB_ROOT}/src")
target_compile_features(test_file_sql_export PRIVATE cxx_std_17)
target_compile_options(test_file_sql_export PRIVATE -Wall -Wextra -Werror)
add_test(NAME file_sql_export COMMAND test_file_sql_export)

# Format readers: one object per line JSON and columnar Parquet both stream from
# the same secure path and share the column/cell interface the executor uses.
add_executable(test_json_file_reader
  test_json_file_reader.cpp
  "${SEEKDB_ROOT}/src/sql/engine/basic/json_file_reader.cpp"
  "${SEEKDB_ROOT}/src/sql/engine/basic/file_scan_utils.cpp")
target_include_directories(test_json_file_reader PRIVATE "${SEEKDB_ROOT}/src")
target_compile_features(test_json_file_reader PRIVATE cxx_std_17)
target_compile_options(test_json_file_reader PRIVATE -Wall -Wextra -Werror)
add_test(NAME json_file_reader COMMAND test_json_file_reader)

add_executable(test_parquet_file_reader
  test_parquet_file_reader.cpp
  "${SEEKDB_ROOT}/src/sql/engine/basic/parquet_file_reader.cpp"
  "${SEEKDB_ROOT}/src/sql/engine/basic/file_scan_utils.cpp")
target_include_directories(test_parquet_file_reader PRIVATE "${SEEKDB_ROOT}/src")
target_compile_features(test_parquet_file_reader PRIVATE cxx_std_17)
target_compile_options(test_parquet_file_reader PRIVATE -Wall -Wextra -Werror)
add_test(NAME parquet_file_reader COMMAND test_parquet_file_reader)
