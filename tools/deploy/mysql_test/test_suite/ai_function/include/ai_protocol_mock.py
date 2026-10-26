#!/usr/bin/env python3
"""Protocol assertions and adversarial responses for kernel AI function tests."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def serve(port):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                case = self.path.strip('/').split('/')[0]
                if self.path.endswith('/completions'):
                    assert isinstance(body['model'], str) and body['model']
                    assert self.headers.get('Authorization', '').startswith('Bearer ')
                    assert self.headers.get('Content-Type') == 'application/json'
                    messages = body['messages']
                    content = next(m['content'] for m in messages if m['role'] == 'user')
                    assert isinstance(content, str)
                    if case == 'config':
                        assert body['temperature'] == 0.2
                        assert body['max_tokens'] == 16
                    value = 'mock:' + content
                    if case == 'empty':
                        value = ''
                    elif case == 'null':
                        value = None
                    elif case == 'number':
                        value = 42
                    response = {'choices': [{'message': {'content': value}}]}
                    if case == 'missing':
                        response = {'choices': []}
                else:
                    raise AssertionError('unrecognized protocol URL')
                payload = json.dumps(response, ensure_ascii=False).encode()
                self.send_response(200)
            except (AssertionError, KeyError, ValueError, StopIteration) as error:
                payload = json.dumps({'error': str(error)}).encode()
                self.send_response(400)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()


def main():
    command, port = sys.argv[1], int(sys.argv[2])
    state = Path(tempfile.gettempdir()) / ('seekdb_ai_protocol_mock_%d' % port)
    pid_file = state / 'pid'
    if command == 'serve':
        serve(port)
    elif command == 'start':
        state.mkdir(exist_ok=True)
        if pid_file.exists():
            raise RuntimeError('mock PID file already exists; stop previous test first')
        with (state / 'server.log').open('w') as output:
            proc = subprocess.Popen([sys.executable, __file__, 'serve', str(port)],
                                    stdin=subprocess.DEVNULL, stdout=output,
                                    stderr=subprocess.STDOUT, start_new_session=True)
        pid_file.write_text(str(proc.pid))
        for _ in range(100):
            if proc.poll() is not None:
                raise RuntimeError('mock exited during startup')
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.02)
        proc.terminate()
        raise RuntimeError('mock startup timed out')
    elif command == 'stop':
        if pid_file.exists():
            try:
                os.kill(int(pid_file.read_text()), signal.SIGTERM)
            except ProcessLookupError:
                pass
            pid_file.unlink()
    else:
        raise ValueError('expected start, stop or serve')


if __name__ == '__main__':
    main()
