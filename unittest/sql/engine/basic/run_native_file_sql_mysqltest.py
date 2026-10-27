#!/usr/bin/env python3
"""Run the public file-SQL test with local (not SSH) fixture deployment.

The SQL and expected result remain unmodified. Only the two cluster deployment
includes are replaced; fixtures and logs are retained under build_release.
"""
import argparse
import base64
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SUITE = ROOT / 'tools/deploy/mysql_test/test_suite/file_sql'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=12883)
    parser.add_argument('--mysqltest', default='mysqltest')
    parser.add_argument('--user', default='root')
    args = parser.parse_args()
    base = Path(tempfile.mkdtemp(prefix='file-sql-public-', dir=ROOT / 'build_release')).resolve()
    fixtures = base / 'data_for_mysqltest_file_sql'
    fixtures.mkdir()
    for source in (SUITE / 'data').iterdir():
        if source.is_file():
            if source.suffix == '.b64':
                (fixtures / source.stem).write_bytes(base64.b64decode(source.read_bytes()))
            else:
                shutil.copyfile(source, fixtures / source.name)
    source = (SUITE / 't/file_sql_basic.test').read_text()
    setup = '--source mysql_test/test_suite/file_sql/include/copy_file_sql_data.inc'
    cleanup = '--source mysql_test/test_suite/file_sql/include/remove_file_sql_data.inc'
    if source.count(setup) != 1 or source.count(cleanup) != 1:
        raise RuntimeError('unexpected public fixture deployment structure')
    adapted = source.replace(setup, 'let $OBSERVER_DIR = %s;' % base).replace(cleanup, '')
    test = base / 'file_sql_basic_local.test'
    test.write_text(adapted)
    print('Retained public-test artifacts: ' + str(base), flush=True)
    result = subprocess.run([args.mysqltest, '--host=127.0.0.1', '--port=%d' % args.port,
                             '--user=' + args.user, '--database=oceanbase',
                             '--test-file=' + str(test),
                             '--result-file=' + str(SUITE / 'r/mysql/file_sql_basic.result')],
                            cwd=base, timeout=120)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
