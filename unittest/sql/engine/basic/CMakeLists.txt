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
