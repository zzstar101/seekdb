#!/usr/bin/env python3
"""Run the production package-loader task against deterministic dependencies.

This targeted C++ test requires only a compiler, not a built seekdb server. It
executes the real task implementation and header, with shims for its external
SQL/admin-job, PL loader, timer, global-context and clock dependencies. It does
not replace end-to-end SQL or bootstrap testing.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[3])
    parser.add_argument("--implementation", type=Path,
                        help="Alternate production .cpp for before/after verification")
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    source = (args.implementation or source_root /
              "src/observer/ob_system_package_load_task.cpp").resolve()
    if not source.is_file():
        parser.error(f"Production implementation not found: {source}")

    with tempfile.TemporaryDirectory(prefix="system-package-task-test-") as tmp:
        tmp = Path(tmp)
        # The production class header remains unmodified and is read from src/.
        # Dependencies are defined once in the test translation unit; these
        # include shims prevent dragging in the entire storage/SQL runtime.
        for header in (
            "common/ob_timeout_ctx.h",
            "lib/task/ob_timer.h",
            "lib/utility/ob_macro_utils.h",
            "share/ob_define.h",
            "pl/ob_pl_package_manager.h",
            "rootserver/ob_admin_job_table_operator.h",
            "share/ob_server_struct.h",
            "share/rc/ob_server_runtime.h",
        ):
            path = tmp / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n", encoding="utf-8")
        binary = tmp / "test_system_package_load_task"
        command = shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(tmp), "-I", str(source_root / "src"),
            f'-DSYSTEM_PACKAGE_LOAD_TASK_SOURCE="{source}"',
            str(Path(__file__).with_name("test_system_package_load_task.cpp")),
            "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        return subprocess.run([str(binary)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
