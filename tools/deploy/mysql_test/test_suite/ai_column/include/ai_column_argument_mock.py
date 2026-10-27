#!/usr/bin/env python3
"""Deterministic mock oracle for the AI column argument-contract mysqltest.

It records every model request (prompt text, system prompt, temperature and
max_tokens) so the test can assert what the worker really sent, not only what it
published.  Port 19229 belongs to this suite file alone.
"""

import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


def state_dir(port):
    return "/tmp/seekdb_ai_column_argument_mock_%d" % port


def state_path(port, name):
    return os.path.join(state_dir(port), name)


def read_events(port):
    events = []
    path = state_path(port, "events.jsonl")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as stream:
            for line in stream:
                line = line.strip()
                if line:
                    events.append(json.loads(line))
    return events


def stop_server(port):
    pid_path = state_path(port, "server.pid")
    if not os.path.exists(pid_path):
        return
    try:
        with open(pid_path, "r", encoding="ascii") as stream:
            pid = int(stream.read().strip())
        os.kill(pid, signal.SIGTERM)
        for _ in range(50):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.02)
    except (ValueError, ProcessLookupError):
        pass
    try:
        os.unlink(pid_path)
    except FileNotFoundError:
        pass


def start_server(port):
    os.makedirs(state_dir(port), exist_ok=True)
    stop_server(port)
    for name in ("events.jsonl", "server.out"):
        with open(state_path(port, name), "w", encoding="utf-8"):
            pass
    output = open(state_path(port, "server.out"), "a", encoding="utf-8")
    process = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "serve", str(port)],
        stdin=subprocess.DEVNULL,
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    with open(state_path(port, "server.pid"), "w", encoding="ascii") as stream:
        stream.write(str(process.pid))
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                return
        except OSError:
            if process.poll() is not None:
                break
            time.sleep(0.02)
    raise RuntimeError("AI column argument mock did not start on port %d" % port)


def serve(port):
    lock = threading.Lock()
    log_path = state_path(port, "events.jsonl")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(size)
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {}
            parsed = urlparse(self.path)
            messages = body.get("messages", [])
            system_text = ""
            user_text = ""
            for message in messages:
                if message.get("role") == "system":
                    system_text = str(message.get("content", ""))
                elif message.get("role") == "user":
                    user_text = str(message.get("content", ""))
            if parsed.path.endswith("/embeddings"):
                input_value = body.get("input", "")
                if isinstance(input_value, list):
                    user_text = "|".join(str(value) for value in input_value)
                else:
                    user_text = str(input_value)
            time.sleep(0.02)
            if parsed.path.endswith("/embeddings"):
                values = body.get("input", [])
                if not isinstance(values, list):
                    values = [values]
                status = 200
                response = {
                    "data": [
                        {"index": index, "embedding": [float(index + 1), 0.25, 0.5, 0.75]}
                        for index, _ in enumerate(values)
                    ]
                }
            else:
                status = 200
                reply = "mock:" + user_text
                if body.get("temperature") is not None:
                    # Surface the config the worker really sent in the answer, so
                    # the SQL assertion can see which config a task snapshotted.
                    reply = reply + "@" + str(body.get("temperature"))
                response = {
                    "choices": [{"message": {"role": "assistant", "content": reply}}]
                }
            event = {
                "path": parsed.path,
                "status": status,
                "user": user_text,
                "system": system_text,
                "temperature": body.get("temperature"),
                "max_tokens": body.get("max_tokens"),
                "authorization": self.headers.get("Authorization", ""),
            }
            with lock:
                with open(log_path, "a", encoding="utf-8") as stream:
                    stream.write(json.dumps(event, ensure_ascii=False) + "\n")
            payload = json.dumps(response, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


LOOKALIKE = '{"template":"{0}","args":["lookalike"]}'


def assert_events(port, assertion):
    events = read_events(port)
    completions = [e for e in events if not e["path"].endswith("/embeddings")]
    embeddings = [e for e in events if e["path"].endswith("/embeddings")]

    def complete_users():
        return [e["user"] for e in completions]

    if assertion == "null-prompt-not-sent":
        # A NULL prompt argument is a terminal task failure: it must not produce a
        # request at all, and it must not abort the business INSERT/UPDATE.
        ok = not any(user.startswith("Classify:") for user in complete_users())
    elif assertion == "prompt-object-rendered":
        # AI_PROMPT(...) is a genuine JSON prompt object: validated and rendered,
        # including the unicode template and the repeated {0} placeholder.
        ok = any(user == "分类: 苹果 ; 苹果" for user in complete_users())
    elif assertion == "lookalike-verbatim":
        # A VARCHAR whose text looks like a prompt object is a plain string: it is
        # sent to the model verbatim, never expanded.
        ok = any(user == LOOKALIKE for user in complete_users())
        ok = ok and not any(user == "lookalike" for user in complete_users())
    elif assertion == "config-applied":
        ok = any(
            e["user"] == "hot-cfg" and e["temperature"] == 0.2 and e["max_tokens"] == 8
            for e in completions
        )
    elif assertion == "null-config-omitted":
        cold = [e for e in completions if e["user"] == "cold-cfg"]
        ok = len(cold) > 0 and all(e["temperature"] is None for e in cold)
    elif assertion == "config-dependency":
        # The config came from a json column: the value used is the one committed
        # when the task was enqueued, and updating that column re-enqueues.
        temperatures = {e["temperature"] for e in completions if e["user"] == "dep-cfg"}
        ok = 0.9 in temperatures and 0.1 in temperatures
    elif assertion == "embedding-none":
        # The NULL input row must not produce a vector request at all: SQL NULL
        # propagation is decided before the model is called.
        ok = len(embeddings) == 0
    elif assertion == "embedding-once":
        ok = len([e for e in embeddings if e["user"] == "vec-one"]) == 1
    else:
        raise ValueError("unknown assertion: " + assertion)
    if not ok:
        raise AssertionError("assertion %s failed: %r" % (assertion, events))
    print("mock_%s=ok" % assertion.replace("-", "_"))


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: ai_column_argument_mock.py MODE PORT [ASSERTION]")
    mode = sys.argv[1]
    port = int(sys.argv[2])
    if mode == "start":
        start_server(port)
    elif mode == "stop":
        stop_server(port)
    elif mode == "serve":
        serve(port)
    elif mode == "reset":
        with open(state_path(port, "events.jsonl"), "w", encoding="utf-8"):
            pass
    elif mode == "assert":
        assert_events(port, sys.argv[3])
    else:
        raise SystemExit("unknown mode: " + mode)


if __name__ == "__main__":
    main()
