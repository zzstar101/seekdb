#!/usr/bin/env python3
"""Compile current retry implementation, then link real kernel archives; no server."""
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "build_release"
OUT = BUILD / "native_ai_retry_request_tests"
OUT.mkdir(exist_ok=True)
commands = json.loads((BUILD / "compile_commands.json").read_text())
main = next(x for x in commands if x["file"].endswith("/observer/main.cpp"))
# All functions of the original Unity member must precede the archive together,
# otherwise the archive may pull in the old retry implementation as a duplicate.
unity = next(x for x in commands if "unity_ob_server_ob_server_ai_service_0_" in x["file"])
def compile_source(entry, source, output):
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
    subprocess.run(flags + ["-O0", "-g0", "-include", "config_bridge.h", "-include",
                           "lib/utility/utility.h", "-c", str(source), "-o", str(output)],
                   cwd=entry["directory"], check=True)
obj = OUT / "test.o"
implementation = OUT / "ai_service_current.o"
compile_source(main, Path(__file__).with_name("test_native_retry_request_id.cpp"), obj)
compile_source(unity, unity["file"], implementation)
link = shlex.split((BUILD / "src/observer/CMakeFiles/seekdb.dir/link.txt").read_text())
link = [arg for arg in link if not arg.startswith("CMakeFiles/ob_main.dir/")]
exe = OUT / "test_native_retry_request_id"
link[link.index("-o") + 1] = str(exe)
link[1:1] = [str(obj), str(implementation)]
subprocess.run(link, cwd=BUILD / "src/observer", check=True)
raise SystemExit(subprocess.run([str(exe)], cwd=OUT).returncode)
