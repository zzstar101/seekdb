# Hand-written file-SQL mysqltest cases (not graded)

These 9 cases were written by hand while developing file SQL for the
2026 competition. **They have never been executed against a running
seekdb**: their `r/mysql/*.result` files were typed by hand instead of
being produced by mysqltest, so a mismatch there says nothing about the
implementation.

They deliberately live outside `tools/deploy/mysql_test/`.
`.github/script/seekdb/mysqltest_for_seekdb.py` discovers cases under
`mysql_test/t/*.test` and `mysql_test/test_suite/*/t/*.test` and
`tools/deploy/mysqltest_config.yaml` runs every discovered case that is
not listed in `runtime_configs.psmall.exclude-set`. A case that stays
under those roots is therefore executed by the grading run, and a single
failing case makes the whole slice report failure. Only the official
`file_sql.file_sql_basic` case may sit in the discovered tree.

To run one of these by hand against a seekdb instance that was started
with `sdb.py start` and initialised with `init.sql`/`init_user.sql`
(user `admin`/`admin`, database `test`):

    export OBMYSQL_PORT=2881 OBMYSQL_MS0=127.0.0.1 OBMYSQL_PWD=admin
    export OBMYSQL_USR=admin OBSERVER_DIR=/path/to/base-dir TENANT=mysql
    mysqltest --host=127.0.0.1 --port=2881 --user=admin --password=admin \
        --database=test --test-file=t/file_sql_csv_regression.test \
        --result-file=r/mysql/file_sql_csv_regression.result \
        --logdir=/tmp/mysqltest --tmpdir=/tmp/mysqltest

Expect the `.result` files to need regeneration after the first real
run; treat the current ones as expectations, not as recorded output.
