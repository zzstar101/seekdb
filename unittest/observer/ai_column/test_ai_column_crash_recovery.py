#!/usr/bin/env python3
"""Native crash/recovery integration; owns every process it terminates.

Run with --seekdb build_release/src/observer/seekdb. A fresh runtime is retained
under build_release for diagnosis. No existing database, listener, or runtime is
stopped or overwritten. Requires the mysql client and three unused local ports.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
MOCK = ROOT / 'tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_barrier_mock.py'


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def wait_until(predicate, description, seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError('timeout: ' + description)


class RecoveryTest:
    def __init__(self, args):
        self.args = args
        self.server = self.mock = self.locker = None
        self.logs = []
        self.base = Path(tempfile.mkdtemp(prefix='ai-column-recovery-', dir=ROOT / 'build_release'))
        self.mysql = [args.mysql, '--protocol=TCP', '--host=127.0.0.1',
                      '--port=' + str(args.sql_port), '--user=root',
                      '--database=oceanbase', '--batch', '--skip-column-names', '--unbuffered']
        self.task_where = ("table_id=(select t.table_id from oceanbase.__all_table t "
                           "join oceanbase.__all_database d on d.database_id=t.database_id "
                           "where d.database_name='ai_recovery' and t.table_name='docs')")

    def log_file(self, name):
        log = (self.base / name).open('ab')
        self.logs.append(log)
        return log

    def sql(self, sql, allow_failure=False):
        result = subprocess.run(self.mysql + ['--execute=' + sql], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
        if result.returncode and not allow_failure:
            raise AssertionError(result.stderr + '\nSQL: ' + sql)
        return result.stdout.strip() if result.returncode == 0 else None

    def ctl(self, op, **kwargs):
        request = urllib.request.Request(
            'http://127.0.0.1:%d/__barrier_ctl' % self.args.mock_port,
            json.dumps(dict(op=op, **kwargs)).encode(), {'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=35) as response:
            body = json.load(response)
        check(body.get('ok'), 'barrier control failed: ' + str(body))
        return body

    def start_database(self):
        check(self.server is None, 'a database process is already owned')
        command = [str(Path(self.args.seekdb).resolve()), '--nodaemon', '--base-dir', str(self.base),
                   '--port', str(self.args.sql_port), '--parameter', 'rpc_port=%d' % self.args.rpc_port,
                   '--parameter', 'memory_budget=2G', '--parameter', 'datafile_size=1G',
                   '--parameter', 'datafile_maxsize=2G', '--parameter', 'log_disk_size=2G']
        self.server = subprocess.Popen(command, cwd=ROOT, stdout=self.log_file('server.stdout'),
                                       stderr=subprocess.STDOUT)

        def ready():
            check(self.server.poll() is None, 'owned database exited; inspect ' + str(self.base))
            return self.sql('select 1', allow_failure=True) == '1'
        wait_until(ready, 'database startup', 120)

    def crash_database(self):
        check(self.server is not None and self.server.poll() is None, 'no live owned database')
        self.server.kill()  # Deliberately bypass graceful shutdown; never target an external PID.
        self.server.wait(timeout=30)
        self.server = None
        if self.locker is not None:
            self.locker.communicate(timeout=30)
            self.locker = None

    def tasks(self, condition):
        return self.sql('select count(*) from oceanbase.__all_ai_column_task where '
                        + self.task_where + ' and ' + condition)

    def lock_row(self, row_id):
        self.locker = subprocess.Popen(self.mysql, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=self.log_file('locker.stderr'), text=True, bufsize=1)
        self.locker.stdin.write('start transaction; select id from ai_recovery.docs where id=%d '
                                "for update; select 'LOCKED';\n" % row_id)
        self.locker.stdin.flush()
        # The client emits one row for each statement, including the explicit
        # lock-acquired marker. Use OS readiness rather than an unbounded read.
        selector = selectors.DefaultSelector()
        selector.register(self.locker.stdout, selectors.EVENT_READ)
        try:
            deadline = time.monotonic() + 20
            output = b''
            while time.monotonic() < deadline:
                check(self.locker.poll() is None, 'row-lock session exited early')
                if selector.select(timeout=1):
                    output += os.read(self.locker.stdout.fileno(), 4096)
                    if b'LOCKED\n' in output:
                        return
            raise AssertionError('row lock was not acquired')
        finally:
            selector.close()

    def stage_first_hold_second(self, row_id, content):
        # Queue eligibility timestamps, not declaration order, select the first
        # sibling. Hold both possible requests and release the actual first one.
        # This establishes the crash precondition without assuming A runs first.
        tokens = ['A:' + content, 'B:' + content]
        self.ctl('arm', tokens=tokens)
        self.sql("insert into ai_recovery.docs(id,content) values(%d,'%s')" % (row_id, content))
        wait_until(lambda: self.tasks('state=2') == '1', 'first sibling claimed', 30)
        first = self.sql("select json_unquote(json_extract(prompt,'$.prompt')) "
                         'from oceanbase.__all_ai_column_task where ' + self.task_where + ' and state=2')
        check(first in tokens, 'unexpected first task: ' + first)
        second = next(token for token in tokens if token != first)
        self.ctl('await', token=first, event='started', count=1, timeout_ms=30000)
        self.ctl('release', tokens=[first])
        self.ctl('await', token=second, event='started', count=1, timeout_ms=30000)
        check(self.tasks('state=4') == '1', 'first sibling must be durably staged')
        return first, second

    def run(self):
        # Refuse all occupied ports before launching anything; no listener takeover.
        for port in (self.args.sql_port, self.args.rpc_port, self.args.mock_port):
            with socket.socket() as probe:
                try:
                    probe.bind(('127.0.0.1', port))
                except OSError as error:
                    raise RuntimeError('refusing occupied local port %d' % port) from error
        self.mock = subprocess.Popen([sys.executable, str(MOCK), 'serve', str(self.args.mock_port)],
                                     cwd=ROOT, stdout=self.log_file('mock.stdout'), stderr=subprocess.STDOUT)
        def mock_ready():
            check(self.mock.poll() is None, 'owned mock exited')
            try:
                return self.ctl('identity')['pid'] == self.mock.pid
            except (OSError, urllib.error.URLError):
                return False
        wait_until(mock_ready, 'mock startup', 10)
        self.ctl('set_hold_timeout', timeout_ms=120000)
        self.start_database()
        endpoint = json.dumps(dict(ai_model_name='recovery_model',
            url='http://127.0.0.1:%d/v1/chat/completions' % self.args.mock_port,
            access_key='synthetic-recovery-key', provider='openai', request_model_name='mock-complete'))
        self.sql("create database ai_recovery; "
                 "call DBMS_AI_SERVICE.CREATE_AI_MODEL('recovery_model',"
                 "'{\"type\":\"completion\",\"model_name\":\"mock-complete\"}'); "
                 "call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('recovery_ep','%s'); "
                 "create table ai_recovery.docs(id bigint primary key, content varchar(30), "
                 "a varchar(200) AI COLUMN (AI_COMPLETE('recovery_model',concat('A:',content))) "
                 "WITH(sync_mode='async',max_retries=0), "
                 "b varchar(200) AI COLUMN (AI_COMPLETE('recovery_model',concat('B:',content))) "
                 "WITH(sync_mode='async',max_retries=0));" % endpoint)

        # First crash: one sibling is durably staged, the other has reached
        # upstream but its response is lost. Only the latter may be inferred again.
        first, lost = self.stage_first_hold_second(1, 'lost')
        check(self.tasks('state=2') == '1', 'second sibling must be running before the first crash')
        check(self.sql('select a is null and b is null from ai_recovery.docs where id=1') == '1',
              'a partially completed group was published')
        self.crash_database()
        self.ctl('release', tokens=[lost])
        self.start_database()
        wait_until(lambda: self.sql("select ifnull(a='mock:A:lost' and b='mock:B:lost',0) "
                                   'from ai_recovery.docs where id=1') == '1', 'lease-expiry recovery', 75)
        check(self.tasks('1=1') == '0', 'recovered tasks were not removed')
        print('lost_response_recovery=ok', flush=True)

        # Second crash: hold the business row lock after the first result is
        # staged, then release the second. Both results become durable while
        # publication remains blocked, and restart must not infer either again.
        _, second = self.stage_first_hold_second(2, 'staged')
        self.lock_row(2)
        self.ctl('release', tokens=[second])
        wait_until(lambda: self.tasks('state=4') == '2', 'both results durably staged', 20)
        self.crash_database()
        self.start_database()
        wait_until(lambda: self.sql("select ifnull(a='mock:A:staged' and b='mock:B:staged',0) "
                                   'from ai_recovery.docs where id=2') == '1', 'terminal-task recovery', 30)
        check(self.tasks('1=1') == '0', 'terminal recovery did not drain tasks')
        counts = Counter(event['user'] for event in self.ctl('events')['events'])
        check(counts == Counter({first: 1, lost: 2, 'A:staged': 1, 'B:staged': 1}),
              'unexpected upstream invocation counts: ' + str(counts))
        print('staged_group_recovery=ok\nupstream_invocation_counts=ok', flush=True)

    def close(self):
        for process in (self.locker, self.server, self.mock):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=30)
        for log in self.logs:
            log.close()
        print('Retained recovery artifacts: ' + str(self.base), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seekdb', required=True)
    parser.add_argument('--mysql', default='mysql')
    parser.add_argument('--sql-port', type=int, default=12885)
    parser.add_argument('--rpc-port', type=int, default=12886)
    parser.add_argument('--mock-port', type=int, default=19127)
    args = parser.parse_args()
    check(Path(args.seekdb).is_file(), 'seekdb executable not found')
    check(len({args.sql_port, args.rpc_port, args.mock_port}) == 3, 'ports must be distinct')
    test = RecoveryTest(args)
    try:
        test.run()
    finally:
        test.close()


if __name__ == '__main__':
    main()
