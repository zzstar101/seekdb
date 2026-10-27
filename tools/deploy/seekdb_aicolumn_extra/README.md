# Hand-written AI-column mysqltest cases (not graded)

These 15 cases (plus the two extra mock servers in `include/`) were
written by hand while developing AI columns for the 2026 competition.
They must not sit in a directory that the grading run discovers.

`mysqltest_for_seekdb.py:254-290` collects every case under
`tools/deploy/mysql_test/t/*.test` and
`tools/deploy/mysql_test/test_suite/*/t/*.test`, then removes only the
names listed in `runtime_configs.psmall.exclude-set` of
`tools/deploy/mysqltest_config.yaml` ("blacklist mode: every case under
tools/deploy/mysql_test is run unless listed in exclude-set"). A failing
extra case therefore fails the whole grading slice, so these files live
outside `tools/deploy/mysql_test/` while the official five cases
(`ai_column_ddl`, `ai_column_t1_dml`, `ai_column_vector_index`,
`ai_column_worker`, `ai_column_retry_strict`) and the official
`include/ai_column_mock.py` stay untouched.

The behaviour they exercise is covered for CI by
`.github/script/ai_function_e2e/run_ai_e2e.py`, which replays the five
official cases plus `ai_function_enhancement` and `file_sql_basic`.
