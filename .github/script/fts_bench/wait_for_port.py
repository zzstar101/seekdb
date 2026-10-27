#!/usr/bin/env python3
"""Block until a TCP port accepts connections, or fail after a timeout.

The benchmark workflow starts ``seekdb`` with ``--nodaemon`` and needs to know
when the server is actually serving before it drives it with the mysql CLI.
Polling the port is cheaper and less brittle than parsing the server log.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time


def wait_for_port(host: str, port: int, timeout: float, interval: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=min(interval, 2.0)):
                return True
        except OSError:
            time.sleep(interval)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--interval", type=float, default=2.0)
    args = parser.parse_args()

    if wait_for_port(args.host, args.port, args.timeout, args.interval):
        print(f"{args.host}:{args.port} is accepting connections")
        return 0
    print(f"{args.host}:{args.port} did not accept connections within {args.timeout:.0f}s")
    return 1


if __name__ == "__main__":
    sys.exit(main())
