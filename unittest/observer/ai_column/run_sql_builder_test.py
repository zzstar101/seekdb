#!/usr/bin/env python3
"""Build real AI task SQL builders using the configured kernel toolchain/libraries.

No source substitution or SQL engine mocks: link the production implementation
from the already-built native release libraries. Run after the kernel build,
not concurrently with an archive writer. An optional argument selects build dir.
"""
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build_release"
OUT = BUILD / "ai_task_sql_tests"
OUT.mkdir(exist_ok=True)
commands = json.loads((BUILD / "compile_commands.json").read_text())
entry = next(item for item in commands if item["file"].endswith("/observer/main.cpp"))
original = entry.get("arguments") or shlex.split(entry["command"])
flags = []
i = 0
while i < len(original):
    arg = original[i]
    if arg in ("-o", "-c", "-MF", "-MT", "-MQ"):
        i += 2
        continue
    if arg in ("-MD", "-MMD", "-MP") or arg.startswith("-O") or arg == "-g":
        i += 1
        continue
    flags.append(arg)
    i += 1
objects = []
for name, source in (
    ("test", Path(__file__).with_name("test_ai_column_task_sql.cpp")),
):
    obj = OUT / (name + ".o")
    subprocess.run(flags + ["-O0", "-g0", "-include", "config_bridge.h",
                           "-include", "lib/utility/utility.h", "-c", str(source), "-o", str(obj)],
                   cwd=entry["directory"], check=True)
    objects.append(str(obj))
link = shlex.split((BUILD / "src/observer/CMakeFiles/seekdb.dir/link.txt").read_text())
link = [arg for arg in link if not arg.startswith("CMakeFiles/ob_main.dir/")]
link[link.index("-o") + 1] = str(OUT / "test_ai_column_task_sql")
link[1:1] = objects
subprocess.run(link, cwd=BUILD / "src/observer", check=True)
result = subprocess.run([str(OUT / "test_ai_column_task_sql")], cwd=OUT)
sys.exit(result.returncode)
