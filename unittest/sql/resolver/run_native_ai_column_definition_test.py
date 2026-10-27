#!/usr/bin/env python3
"""Build only native AI definition validation tests against existing server archives."""
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "build_release"
OUT = BUILD / "native_ai_column_definition_tests"
OUT.mkdir(exist_ok=True)
entry = next(x for x in json.loads((BUILD / "compile_commands.json").read_text())
             if x["file"].endswith("/observer/main.cpp"))
args = entry.get("arguments") or shlex.split(entry["command"])
flags = []
i = 0
while i < len(args):
    arg = args[i]
    if arg in ("-o", "-c", "-MF", "-MT", "-MQ"):
        i += 2
        continue
    if arg in ("-MD", "-MMD", "-MP") or arg.startswith("-O") or arg == "-g":
        i += 1
        continue
    flags.append(arg)
    i += 1
obj = OUT / "test.o"
subprocess.run(flags + ["-O0", "-g0", "-include", "config_bridge.h", "-include",
                       "lib/utility/utility.h", "-c",
                       str(Path(__file__).with_name("test_native_ai_column_definition.cpp")), "-o", str(obj)],
               cwd=entry["directory"], check=True)
link = shlex.split((BUILD / "src/observer/CMakeFiles/seekdb.dir/link.txt").read_text())
link = [arg for arg in link if not arg.startswith("CMakeFiles/ob_main.dir/")]
exe = OUT / "test_native_ai_column_definition"
link[link.index("-o") + 1] = str(exe)
link.insert(1, str(obj))
subprocess.run(link, cwd=BUILD / "src/observer", check=True)
raise SystemExit(subprocess.run([str(exe)], cwd=OUT).returncode)
