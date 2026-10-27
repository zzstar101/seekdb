# Hand-written AI-function mysqltest cases (not graded)

These 6 cases and their `include/` helpers were written by hand while
developing the AI functions for the 2026 competition. They stay here so
the code history is kept, but they must not live in a directory that the
grading run discovers.

`mysqltest_for_seekdb.py:254-290` collects every case under
`tools/deploy/mysql_test/t/*.test` and
`tools/deploy/mysql_test/test_suite/*/t/*.test`, then removes only the
names listed in `runtime_configs.psmall.exclude-set` of
`tools/deploy/mysqltest_config.yaml` ("blacklist mode: every case under
tools/deploy/mysql_test is run unless listed in exclude-set"). A failing
extra case therefore fails the whole grading slice, so these files live
outside `tools/deploy/mysql_test/` and the official suite stays
byte-for-byte identical to `5bce9e06d`.

The behaviour of these cases is covered for CI by
`.github/script/ai_function_e2e/run_ai_e2e.py`, which replays the
official `ai_function_enhancement` expectations against a mock server.
