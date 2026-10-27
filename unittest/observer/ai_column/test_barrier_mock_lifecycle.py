#!/usr/bin/env python3
"""The mock's stale PID file must never authorize signalling another process."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
PATH = ROOT / "tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_barrier_mock.py"
spec = importlib.util.spec_from_file_location("barrier_mock", PATH)
barrier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(barrier)


class LifecycleTest(unittest.TestCase):
    def test_closed_port_with_stale_pid_never_sends_signals(self):
        with patch.object(barrier, "read_pid", return_value=12345), \
             patch.object(barrier, "port_open", return_value=False), \
             patch.object(barrier.os, "kill") as kill, \
             patch.object(barrier.os, "unlink"):
            self.assertEqual(0, barrier.stop_server(19126))
            kill.assert_not_called()

    def test_unowned_listener_is_not_stopped(self):
        with patch.object(barrier, "read_pid", return_value=12345), \
             patch.object(barrier, "port_open", return_value=True), \
             patch.object(barrier, "control", return_value={"service": "other", "pid": 12345}) as control, \
             patch.object(barrier.os, "kill") as kill, \
             patch.object(barrier.os, "unlink") as unlink:
            self.assertEqual(2, barrier.stop_server(19126))
            control.assert_called_once_with(19126, {"op": "identity"}, timeout=5.0)
            kill.assert_not_called()
            unlink.assert_not_called()


if __name__ == "__main__":
    unittest.main()
