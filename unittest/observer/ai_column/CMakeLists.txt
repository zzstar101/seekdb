cmake_minimum_required(VERSION 3.20)
project(ai_column_runtime_unit_tests LANGUAGES CXX)

# Standalone tests for the AI column runtime (typed results use bundled RapidJSON).
# The task state
# machine, the STATUS pseudo column names and the retry policy are compiled into
# the server from src/observer/ai_service/ai_column, but they carry no
# OceanBase dependency, so they can be exercised here on native macOS too, where
# the pretest framework is absent.  The end to end behaviour (queue rows written
# inside the user transaction, asynchronous model calls, atomic publish) is
# covered by tools/deploy/mysql_test/test_suite/ai_column.
enable_testing()
get_filename_component(SEEKDB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(test_ai_column_task_defs test_ai_column_task_defs.cpp)
target_include_directories(test_ai_column_task_defs PRIVATE "${SEEKDB_ROOT}/src")
target_compile_features(test_ai_column_task_defs PRIVATE cxx_std_17)
target_compile_options(test_ai_column_task_defs PRIVATE -Wall -Wextra -Werror)
add_test(NAME ai_column_task_defs COMMAND test_ai_column_task_defs)

find_path(RAPIDJSON_INCLUDE_DIR rapidjson/reader.h
          HINTS "${SEEKDB_ROOT}/deps/3rd/usr/local/oceanbase/deps/devel/include"
          REQUIRED)
add_executable(test_ai_column_typed_result test_ai_column_typed_result.cpp)
target_include_directories(test_ai_column_typed_result PRIVATE "${SEEKDB_ROOT}/src")
target_include_directories(test_ai_column_typed_result SYSTEM PRIVATE "${RAPIDJSON_INCLUDE_DIR}")
target_compile_features(test_ai_column_typed_result PRIVATE cxx_std_17)
target_compile_options(test_ai_column_typed_result PRIVATE -Wall -Wextra -Werror)
add_test(NAME ai_column_typed_result COMMAND test_ai_column_typed_result)

# The worker itself carries the OceanBase dependency, so it can not be linked
# here.  Its structural invariants - one precise delete fence, staging vs
# publishing separation, the model call outside the publish transaction, every
# result materialized inside its own SMART_VAR read scope, declared members
# defined - are checked by a dependency free python3 script over the sources.
# `run_sql_builder_test.py` covers the generated statements on top of that and
# needs the real build flags, so it stays a manual check.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME ai_column_worker_invariants
           COMMAND ${Python3_EXECUTABLE}
                   "${CMAKE_CURRENT_LIST_DIR}/test_ai_column_worker_invariants.py")
endif()
