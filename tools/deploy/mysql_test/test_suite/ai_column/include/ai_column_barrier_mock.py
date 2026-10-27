#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Deterministic model endpoint barrier for the AI column partial refresh test.

The mock answers OpenAI style /v1/chat/completions requests with the
deterministic content "mock:<prompt text>" and, unlike a plain mock, it can
*hold* a request until the test releases it.  That is how
tools/deploy/mysql_test/test_suite/ai_column/t/ai_column_partial_refresh.test
produces its ordering: the test updates one row while the model request of
another column of the very same request group is still in flight, without a
single timing dependent --sleep.

Ordering primitives (the whole state lives in the server process behind one
threading.Condition, so counting, waiting and releasing are thread safe):

  arm    <port> <token>...      hold every request whose prompt contains <token>
  release<port> <token>...      release the requests currently held by <token>
  mark   <port> <phase>         name the current ordering phase
  await  <port> <token> <started|completed> <count> <timeout_ms>
                                block until <token> was seen <count> times
  assert <port> <oracle>        evaluate an ordering oracle, exit 1 when violated
  set_hold_timeout <port> <ms>  cap how long a request may be held (20000 ms)
  state  <port>                 counters and phases (debug aid)
  events <port>                 every recorded request (debug aid)
  start  <port>                 (re)start the server, truncating the event log
  stop   <port>                 shut the server down
  selftest [<port>]             exercise the control protocol end to end
  serve  <port>                 internal, runs the server in the foreground

Every model request is recorded as one event

  {seq, path, user, token, hold, phase_at_start, phase_at_end, status}

where hold is "none" (never held), "released" (held and released on request) or
"timeout" (the hold was not released in time; the mock still answers so that a
stuck barrier shows up as a violated oracle instead of a hanging test).  The
phase is the value of the last `mark`, which lets an oracle prove *when* a
response was produced relative to a statement of the test.

The command line client only talks to the control endpoint /__barrier_ctl, so
the SQL test drives the barrier from plain `--exec` lines and never needs a
second connection or a background shell job.

Exit codes: 0 ok, 1 oracle violated, 2 transport or usage error, 3 await timed
out.
"""

import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CTL_PATH = "/__barrier_ctl"
COMPLETION_SUFFIX = "/chat/completions"

DEFAULT_HOLD_TIMEOUT_MS = 20000
START_TIMEOUT_SEC = 30.0
STOP_TIMEOUT_SEC = 10.0
CONTROL_TIMEOUT_SEC = 60.0
# A test that dies between start and stop leaves the server behind; the next
# start reclaims the port, and this idle watchdog eventually reclaims the
# process even when no further test run happens.
IDLE_SHUTDOWN_SEC = 900.0

EVENT_FILE = "events.jsonl"
PID_FILE = "server.pid"
OUT_FILE = "server.out"

USAGE = (
    "usage: ai_column_barrier_mock.py "
    "start|stop|serve <port> | arm|release <port> <token>... | "
    "mark <port> <phase> | await <port> <token> <started|completed> <count> "
    "<timeout_ms> | assert <port> <oracle> | set_hold_timeout <port> <ms> | "
    "state|events <port> | selftest [<port>]\n"
)


# ---------------------------------------------------------------------------
# filesystem helpers
# ---------------------------------------------------------------------------

def state_dir(port):
    return "/tmp/seekdb_ai_column_barrier_%d" % port


def state_path(port, name):
    return os.path.join(state_dir(port), name)


def read_events(port):
    events = []
    path = state_path(port, EVENT_FILE)
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as stream:
            for line in stream:
                line = line.strip()
                if line:
                    try:
                        events.append(json.loads(line))
                    except ValueError:
                        pass
    return events


def read_pid(port):
    path = state_path(port, PID_FILE)
    if not os.path.exists(path):
        return 0
    try:
        with open(path, "r", encoding="ascii") as stream:
            return int(stream.read().strip())
    except (ValueError, OSError):
        return 0


def write_pid(port, pid):
    with open(state_path(port, PID_FILE), "w", encoding="ascii") as stream:
        stream.write(str(pid))


def log(message):
    sys.stderr.write("ai_column_barrier_mock: %s\n" % message)
    sys.stderr.flush()


def port_open(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
            return True
    except OSError:
        return False


def wait_port(port, expected_open, timeout_sec):
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        if port_open(port) == expected_open:
            return True
        time.sleep(0.02)
    return port_open(port) == expected_open


# ---------------------------------------------------------------------------
# control client
# ---------------------------------------------------------------------------

def control(port, payload, timeout=CONTROL_TIMEOUT_SEC):
    url = "http://127.0.0.1:%d%s" % (port, CTL_PATH)
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.URLError as error:
        log("control endpoint %s is not reachable on port %d (%s)" % (CTL_PATH, port, error))
        raise SystemExit(2)
    except (ValueError, OSError) as error:
        log("bad control response from port %d (%s)" % (port, error))
        raise SystemExit(2)


# ---------------------------------------------------------------------------
# server side state
# ---------------------------------------------------------------------------

class Barrier(object):
    """Thread safe ordering state of one barrier server."""

    def __init__(self):
        self.cond = threading.Condition()
        self.phase = "init"
        self.armed = []
        self.arrivals = []
        self.events = []
        self.hold_timeout_ms = DEFAULT_HOLD_TIMEOUT_MS
        self.last_activity = time.time()
        self.event_path = None

    # -- helpers, every one of them runs with self.cond held ---------------

    def _matching(self, token, event):
        return [event] if token in event["user"] else []

    def _seen(self, token, event_kind):
        if event_kind == "started":
            candidates = self.arrivals
        else:
            candidates = self.events
        return [item for item in candidates if token in item["user"]]

    def _held_token(self, user):
        for token in self.armed:
            if token in user:
                return token
        return None

    # -- model request lifecycle ------------------------------------------

    def begin(self, path, user):
        """Register a model request and block while one of its tokens is armed."""
        with self.cond:
            self.last_activity = time.time()
            token = self._held_token(user)
            record = {
                "seq": len(self.arrivals) + 1,
                "path": path,
                "user": user,
                "token": token,
                "hold": "none" if token is None else "held",
                "phase_at_start": self.phase,
                "phase_at_end": self.phase,
                "status": 0,
            }
            self.arrivals.append(record)
            self.cond.notify_all()
            if token is None:
                return record
            deadline = time.time() + self.hold_timeout_ms / 1000.0
            while token in self.armed and record["hold"] == "held":
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    record["hold"] = "timeout"
                    log("hold of token %r timed out after %d ms"
                        % (token, self.hold_timeout_ms))
                    break
                self.cond.wait(remaining)
            if record["hold"] == "held":
                record["hold"] = "released"
            return record

    def finish(self, record, status):
        """Record the answered model request."""
        with self.cond:
            self.last_activity = time.time()
            record["status"] = status
            record["phase_at_end"] = self.phase
            self.events.append(dict(record))
            self.cond.notify_all()
            self._append_event(record)

    def _append_event(self, record):
        if not self.event_path:
            return
        try:
            with open(self.event_path, "a", encoding="utf-8") as stream:
                stream.write(json.dumps(record, ensure_ascii=False) + "\n")
        except OSError:
            pass

    # -- control operations ------------------------------------------------

    def op_arm(self, tokens):
        with self.cond:
            self.last_activity = time.time()
            for token in tokens:
                if token not in self.armed:
                    self.armed.append(token)
            self.cond.notify_all()
            return {"ok": True, "armed": list(self.armed)}

    def op_release(self, tokens):
        with self.cond:
            self.last_activity = time.time()
            for token in tokens:
                while token in self.armed:
                    self.armed.remove(token)
            waiting = [
                arrival for arrival in self.arrivals
                if arrival["token"] in tokens and arrival["hold"] == "held"
            ]
            self.cond.notify_all()
            return {"ok": True, "armed": list(self.armed), "released": len(waiting)}

    def op_mark(self, phase):
        with self.cond:
            self.last_activity = time.time()
            self.phase = phase
            self.cond.notify_all()
            return {"ok": True, "phase": phase}

    def op_await(self, token, event_kind, count, timeout_ms):
        deadline = time.time() + timeout_ms / 1000.0
        with self.cond:
            while True:
                seen = len(self._seen(token, event_kind))
                if seen >= count:
                    return {"ok": True, "token": token, "event": event_kind, "seen": seen}
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    return {"ok": False, "token": token, "event": event_kind, "seen": seen}
                self.cond.wait(remaining)

    def op_set_hold_timeout(self, timeout_ms):
        with self.cond:
            self.last_activity = time.time()
            self.hold_timeout_ms = int(timeout_ms)
            self.cond.notify_all()
            return {"ok": True, "hold_timeout_ms": self.hold_timeout_ms}

    def op_state(self):
        with self.cond:
            users = {}
            for event in self.events:
                users[event["user"]] = users.get(event["user"], 0) + 1
            return {
                "ok": True,
                "phase": self.phase,
                "armed": list(self.armed),
                "hold_timeout_ms": self.hold_timeout_ms,
                "arrivals": len(self.arrivals),
                "completed": len(self.events),
                "held_now": len([a for a in self.arrivals if a["hold"] == "held"]),
                "users": users,
            }

    def op_events(self):
        with self.cond:
            return {"ok": True, "events": [dict(event) for event in self.events]}

    def op_assert(self, name):
        with self.cond:
            if name in ORACLES:
                problems = self._check_scenario(ORACLES[name])
            elif name == "partial-refresh-requests":
                problems = self._check_request_set()
            else:
                return {"ok": False, "name": name,
                        "problems": ["unknown oracle %r" % name]}
            return {"ok": not problems, "name": name, "problems": problems}

    # -- oracles -----------------------------------------------------------

    def _check_scenario(self, spec):
        """Verify the ordering of one partial refresh scenario.

        * the stale request was started before the update, was really held and
          was answered only after the update phase began,
        * the request of the untouched column was started in either order and
          was never held,
        * the freshly requested prompt was started after the update phase.
        """
        problems = []
        for role, expected in (("stale", spec["stale"]),
                               ("untouched", spec["untouched"]),
                               ("fresh", spec["fresh"])):
            matching = [
                event for event in self.events if expected["user"] in event["user"]
            ]
            if len(matching) != 1:
                problems.append(
                    "%s %s: expected exactly 1 model request, saw %d"
                    % (role, expected["user"], len(matching))
                )
                continue
            event = matching[0]
            if event["status"] != 200:
                problems.append("%s %s: status %s, expected 200"
                                % (role, expected["user"], event["status"]))
            if event["hold"] not in expected["hold"]:
                problems.append("%s %s: hold %r, expected one of %r"
                                % (role, expected["user"], event["hold"],
                                   expected["hold"]))
            if event["phase_at_start"] not in expected["start"]:
                problems.append("%s %s: started in phase %r, expected one of %r"
                                % (role, expected["user"], event["phase_at_start"],
                                   expected["start"]))
            if event["phase_at_end"] not in expected["end"]:
                problems.append("%s %s: answered in phase %r, expected one of %r"
                                % (role, expected["user"], event["phase_at_end"],
                                   expected["end"]))
        still_held = [event["user"] for event in self.arrivals
                      if event["hold"] == "held"]
        if still_held:
            problems.append("model requests are still held: %r" % (still_held,))
        unfinished = [event["user"] for event in self.arrivals
                      if not any(done["seq"] == event["seq"] for done in self.events)]
        if unfinished:
            problems.append("model requests were never answered: %r" % (unfinished,))
        return problems

    def _check_request_set(self):
        """Verify that the test produced exactly the expected model requests."""
        problems = []
        users = {}
        for event in self.events:
            users[event["user"]] = users.get(event["user"], 0) + 1
        for user, count in sorted(REQUEST_SET.items()):
            if users.get(user, 0) != count:
                problems.append("%s: expected %d model request(s), saw %d"
                                % (user, count, users.get(user, 0)))
        for user, count in sorted(users.items()):
            if user not in REQUEST_SET:
                problems.append("%s: unexpected model request (%d time(s))"
                                % (user, count))
        unfinished = len(self.arrivals) - len(self.events)
        if unfinished:
            problems.append("%d model request(s) were never answered" % unfinished)
        return problems


# The ordering oracle of every partial refresh scenario.  "hold" lists the
# accepted hold outcomes, "start"/"end" list the accepted phases.
ORACLES = {
    "partial-refresh-scenario1": {
        "name": "group1",
        "stale": {
            "user": "A:a0",
            "hold": ["released"],
            "start": ["group1_armed"],
            "end": ["group1_after_update"],
        },
        "untouched": {
            "user": "B:b0",
            "hold": ["none"],
            "start": ["group1_armed", "group1_after_update"],
            "end": ["group1_armed", "group1_after_update"],
        },
        "fresh": {
            "user": "A:a1",
            "hold": ["none"],
            "start": ["group1_after_update"],
            "end": ["group1_after_update"],
        },
    },
    "partial-refresh-scenario2": {
        "name": "group2",
        "stale": {
            "user": "B:b2",
            "hold": ["released"],
            "start": ["group2_armed"],
            "end": ["group2_after_update"],
        },
        "untouched": {
            "user": "A:a2",
            "hold": ["none"],
            "start": ["group2_armed", "group2_after_update"],
            "end": ["group2_armed", "group2_after_update"],
        },
        "fresh": {
            "user": "B:b1",
            "hold": ["none"],
            "start": ["group2_after_update"],
            "end": ["group2_after_update"],
        },
    },
}

# The exact model requests the whole test is allowed to produce: one per prompt,
# no retry and no re-execution of a staged task.
REQUEST_SET = {
    "A:a0": 1,
    "B:b0": 1,
    "A:a1": 1,
    "A:a2": 1,
    "B:b2": 1,
    "B:b1": 1,
}


def extract_messages(payload):
    system_text = ""
    user_text = ""
    for message in payload.get("messages", []) or []:
        if not isinstance(message, dict):
            continue
        content = message.get("content")
        if isinstance(content, list):
            content = "".join(
                str(part.get("text", ""))
                for part in content
                if isinstance(part, dict)
            )
        content = "" if content is None else str(content)
        if message.get("role") == "system":
            system_text = content
        elif message.get("role") == "user":
            user_text = content
    return system_text, user_text


def completion_body(user_text, model):
    return {
        "id": "ai-column-barrier",
        "object": "chat.completion",
        "created": 0,
        "model": model,
        "choices": [
            {"index": 0,
             "message": {"role": "assistant", "content": "mock:" + user_text},
             "finish_reason": "stop"}
        ],
        "usage": {"prompt_tokens": 8, "completion_tokens": 4, "total_tokens": 12},
    }


# ---------------------------------------------------------------------------
# server
# ---------------------------------------------------------------------------

def serve(port):
    barrier = Barrier()
    barrier.event_path = state_path(port, EVENT_FILE)
    server_holder = {}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def _read_body(self):
            size = int(self.headers.get("Content-Length", "0") or "0")
            raw = self.rfile.read(size) if size > 0 else b""
            try:
                return json.loads(raw.decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                return {}

        def _send_json(self, status, payload):
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            try:
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return False
            return True

        def do_POST(self):
            body = self._read_body()
            if self.path.startswith(CTL_PATH):
                self._handle_control(body)
            elif self.path.endswith(COMPLETION_SUFFIX):
                self._handle_completion(body)
            else:
                self._send_json(404, {"ok": False, "error": "unknown path " + self.path})

        def _handle_completion(self, body):
            _, user_text = extract_messages(body)
            record = barrier.begin(self.path, user_text)
            model = str(body.get("model") or "mock-complete")
            sent = self._send_json(200, completion_body(user_text, model))
            barrier.finish(record, 200 if sent else 0)

        def _handle_control(self, body):
            op = str(body.get("op", ""))
            if op == "arm":
                reply = barrier.op_arm([str(token) for token in body.get("tokens", [])])
            elif op == "release":
                reply = barrier.op_release([str(token) for token in body.get("tokens", [])])
            elif op == "mark":
                reply = barrier.op_mark(str(body.get("phase", "")))
            elif op == "await":
                reply = barrier.op_await(
                    str(body.get("token", "")),
                    str(body.get("event", "started")),
                    int(body.get("count", 1)),
                    int(body.get("timeout_ms", 30000)),
                )
            elif op == "set_hold_timeout":
                reply = barrier.op_set_hold_timeout(int(body.get("timeout_ms", 0)))
            elif op == "state":
                reply = barrier.op_state()
            elif op == "events":
                reply = barrier.op_events()
            elif op == "assert":
                reply = barrier.op_assert(str(body.get("name", "")))
            elif op == "identity":
                reply = {"ok": True, "service": "seekdb-ai-column-barrier", "pid": os.getpid()}
            elif op == "shutdown":
                self._send_json(200, {"ok": True})
                server = server_holder["server"]
                threading.Thread(target=server.shutdown, daemon=True).start()
                return
            else:
                reply = {"ok": False, "error": "unknown control op %r" % op}
            self._send_json(200, reply)

    class Server(ThreadingHTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    try:
        httpd = Server(("127.0.0.1", port), Handler)
    except OSError as error:
        log("can not listen on 127.0.0.1:%d (%s)" % (port, error))
        return 2
    server_holder["server"] = httpd
    barrier.last_activity = time.time()

    def watchdog():
        while True:
            time.sleep(1.0)
            if time.time() - barrier.last_activity > IDLE_SHUTDOWN_SEC:
                log("idle for %d s, shutting down" % IDLE_SHUTDOWN_SEC)
                httpd.shutdown()
                return

    threading.Thread(target=watchdog, daemon=True).start()
    httpd.serve_forever(poll_interval=0.05)
    httpd.server_close()
    return 0


# ---------------------------------------------------------------------------
# lifecycle
# ---------------------------------------------------------------------------

def stop_server(port):
    pid = read_pid(port)
    if port_open(port):
        try:
            identity = control(port, {"op": "identity"}, timeout=5.0)
            if (pid <= 0 or identity.get("service") != "seekdb-ai-column-barrier"
                    or identity.get("pid") != pid):
                log("refusing to stop an unowned listener on port %d" % port)
                return 2
            control(port, {"op": "shutdown"}, timeout=5.0)
        except SystemExit:
            return 2
        if not wait_port(port, False, STOP_TIMEOUT_SEC):
            log("mock on port %d did not stop gracefully" % port)
            return 2
    # A PID can be recycled after a crash. Never send OS signals from a PID file.
    # The controlled shutdown above terminates our server, including held handlers.
    try:
        os.unlink(state_path(port, PID_FILE))
    except OSError:
        pass
    return 0


def start_server(port):
    os.makedirs(state_dir(port), exist_ok=True)
    if stop_server(port) != 0:
        return 2
    for name in (EVENT_FILE, OUT_FILE):
        with open(state_path(port, name), "w", encoding="utf-8"):
            pass
    output = open(state_path(port, OUT_FILE), "a", encoding="utf-8")
    process = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "serve", str(port)],
        stdin=subprocess.DEVNULL,
        stdout=output,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    write_pid(port, process.pid)
    deadline = time.time() + START_TIMEOUT_SEC
    while time.time() < deadline:
        if process.poll() is not None:
            log("server exited with %s, see %s"
                % (process.returncode, state_path(port, OUT_FILE)))
            return 2
        if port_open(port):
            return 0
        time.sleep(0.02)
    log("server did not open port %d in %d s" % (port, START_TIMEOUT_SEC))
    return 2


# ---------------------------------------------------------------------------
# self test of the control protocol
# ---------------------------------------------------------------------------

def post_completion(port, user_text, system_text=" Declared SQL result: .", timeout=60.0):
    payload = {
        "model": "mock-complete",
        "messages": [
            {"role": "system", "content": system_text},
            {"role": "user", "content": user_text},
        ],
    }
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        "http://127.0.0.1:%d%s" % (port, COMPLETION_SUFFIX),
        data=data,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = json.loads(response.read().decode("utf-8"))
    return body["choices"][0]["message"]["content"]


def selftest(port):
    """Exercise the control protocol with the ordering of the SQL test."""
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)
        print("%s: %s" % ("ok  " if condition else "FAIL", message))

    def oracle(name):
        return control(port, {"op": "assert", "name": name})

    started = time.time()
    code = start_server(port)
    check(code == 0, "server starts on port %d" % port)
    if code != 0:
        return 1
    try:
        check(control(port, {"op": "mark", "phase": "group1_armed"})["ok"] is True,
              "mark accepts a phase")
        check(control(port, {"op": "arm", "tokens": ["A:a0"]})["armed"] == ["A:a0"],
              "arm holds a token")

        answered = {}
        worker = threading.Thread(
            target=lambda: answered.update(
                text=post_completion(port, "A:a0"),
                elapsed=time.time() - started,
            )
        )
        worker.start()
        held = control(port, {"op": "await", "token": "A:a0", "event": "started",
                              "count": 1, "timeout_ms": 10000})
        check(held["ok"] is True, "await sees the held request while it is blocked")
        check(worker.is_alive(), "the armed request stays blocked until it is released")
        check(post_completion(port, "B:b0") == "mock:B:b0",
              "an unarmed request is answered immediately")
        check(worker.is_alive(), "the armed request is still blocked after other traffic")

        control(port, {"op": "mark", "phase": "group1_after_update"})
        released = control(port, {"op": "release", "tokens": ["A:a0"]})
        check(released["released"] == 1, "release reports the request it unblocks")
        worker.join(30.0)
        check(not worker.is_alive(), "the released request is answered")
        check(answered.get("text") == "mock:A:a0", "the released answer is deterministic")
        check(answered.get("elapsed", 0.0) > 0.2, "the released answer was really delayed")
        check(post_completion(port, "A:a1") == "mock:A:a1",
              "the fresh request is answered immediately")

        state = control(port, {"op": "state"})
        check(state["completed"] == 3, "three requests are recorded")
        check(state["armed"] == [], "nothing is armed any more")

        result = oracle("partial-refresh-scenario1")
        check(result["ok"] is True,
              "scenario1 oracle passes on a correct ordering: %r" % (result["problems"],))

        # The oracle must be able to fail: a second request for the same prompt
        # is a violation, and so is a request that was held until the timeout.
        post_completion(port, "A:a1")
        result = oracle("partial-refresh-scenario1")
        check(result["ok"] is False, "scenario1 oracle rejects a duplicated request")
        check(any("A:a1" in problem for problem in result["problems"]),
              "the failure names the duplicated prompt: %r" % (result["problems"],))

        result = oracle("partial-refresh-requests")
        check(result["ok"] is False, "the request set oracle rejects the extra request")

        control(port, {"op": "set_hold_timeout", "timeout_ms": 300})
        control(port, {"op": "arm", "tokens": ["Z:z"]})
        started_at = time.time()
        text = post_completion(port, "Z:z")
        elapsed = time.time() - started_at
        check(text == "mock:Z:z", "a request that is never released is still answered")
        check(0.25 <= elapsed <= 5.0, "the hold timeout bounds the wait (%.2f s)" % elapsed)
        events = control(port, {"op": "events"})["events"]
        timed_out = [event for event in events if event["user"] == "Z:z"]
        check(len(timed_out) == 1 and timed_out[0]["hold"] == "timeout",
              "the event of a timed out hold is reported as a violation")

        result = oracle("partial-refresh-scenario2")
        check(result["ok"] is False, "scenario2 oracle fails while its prompts are missing")

        # A restarted server must not inherit any state of the previous run.
        check(start_server(port) == 0, "start reclaims the port of a running server")
        state = control(port, {"op": "state"})
        check(state["arrivals"] == 0 and state["completed"] == 0,
              "a restarted server starts with empty counters")
        check(state["armed"] == [] and state["phase"] == "init",
              "a restarted server starts with an empty barrier")
    finally:
        stop_server(port)
    check(not port_open(port), "the server is stopped")
    if failures:
        for failure in failures:
            log("selftest failure: " + failure)
        return 1
    print("mock_barrier_selftest=ok")
    return 0


# ---------------------------------------------------------------------------
# command line
# ---------------------------------------------------------------------------

def main(argv):
    if len(argv) < 2:
        sys.stderr.write(USAGE)
        return 2
    mode = argv[1]
    if mode == "selftest":
        port = int(argv[2]) if len(argv) > 2 else 19126
        return selftest(port)
    if len(argv) < 3:
        sys.stderr.write(USAGE)
        return 2
    try:
        port = int(argv[2])
    except ValueError:
        sys.stderr.write(USAGE)
        return 2
    tokens = argv[3:]
    if mode == "serve":
        return serve(port)
    if mode == "start":
        return start_server(port)
    if mode == "stop":
        return stop_server(port)
    if mode == "arm":
        control(port, {"op": "arm", "tokens": tokens})
        return 0
    if mode == "release":
        control(port, {"op": "release", "tokens": tokens})
        return 0
    if mode == "mark":
        control(port, {"op": "mark", "phase": argv[3]})
        return 0
    if mode == "set_hold_timeout":
        control(port, {"op": "set_hold_timeout", "timeout_ms": int(argv[3])})
        return 0
    if mode == "await":
        if len(tokens) != 4:
            sys.stderr.write(USAGE)
            return 2
        token, event_kind, count, timeout_ms = tokens
        reply = control(
            port,
            {"op": "await", "token": token, "event": event_kind,
             "count": int(count), "timeout_ms": int(timeout_ms)},
            timeout=int(timeout_ms) / 1000.0 + CONTROL_TIMEOUT_SEC,
        )
        if reply.get("ok") is True:
            return 0
        log("request %r was not %s %s time(s) within %s ms"
            % (token, event_kind, count, timeout_ms))
        return 3
    if mode == "assert":
        if len(tokens) != 1:
            sys.stderr.write(USAGE)
            return 2
        reply = control(port, {"op": "assert", "name": tokens[0]})
        if reply.get("ok") is True:
            print("mock_%s=ok" % tokens[0].replace("-", "_"))
            return 0
        log("oracle %s failed:" % tokens[0])
        for problem in reply.get("problems", []):
            log("  - " + problem)
        return 1
    if mode == "state":
        print(json.dumps(control(port, {"op": "state"}), indent=2, sort_keys=True))
        return 0
    if mode == "events":
        print(json.dumps(control(port, {"op": "events"}), indent=2, sort_keys=True))
        return 0
    sys.stderr.write(USAGE)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
