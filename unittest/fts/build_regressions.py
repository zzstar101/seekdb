#!/usr/bin/env python3
"""Build the FTS native regressions against an existing seekdb build.

The default is a dry run. --execute compiles the regression test objects and
links them with the production archives and the installed Google Test libraries.
It neither rebuilds seekdb nor runs the tests. All output stays outside the source
and build trees.

Example, from the repository root:
  python3 unittest/fts/build_regressions.py --build build_release --execute
"""

import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


TEST_SOURCES = (
    "unittest/storage/test_fts_word_aggregation.cpp",
    "unittest/storage/test_fts_char_classification.cpp",
    "unittest/lib/charset/test_fts_strcmpsp_dispatch.cpp",
    "unittest/storage/test_fts_count_only_batch.cpp",
    "unittest/storage/test_fts_doc_id_comparison.cpp",
    "unittest/storage/test_fts_ik_dictionary_cache.cpp",
    "unittest/storage/test_fts_word_row_buffer.cpp",
)


class BuildError(RuntimeError):
    pass


def read_text(path):
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise BuildError("Cannot read {}: {}".format(path, error)) from error


def option_value_index(argv, option, description):
    positions = [i for i, value in enumerate(argv) if value == option]
    if len(positions) != 1 or positions[0] + 1 >= len(argv):
        raise BuildError("Expected one '{} <value>' in {}".format(option, description))
    return positions[0] + 1


def compile_template(build):
    path = build / "compile_commands.json"
    try:
        database = json.loads(read_text(path))
    except ValueError as error:
        raise BuildError("Invalid JSON in {}: {}".format(path, error)) from error
    if not isinstance(database, list):
        raise BuildError("{} must contain a compilation database array".format(path))
    entries = [entry for entry in database if isinstance(entry, dict)
               and isinstance(entry.get("file"), str)
               and Path(entry.get("file", "")).parts[-3:] == ("src", "observer", "main.cpp")]
    if len(entries) != 1:
        raise BuildError("Expected one src/observer/main.cpp entry in {}; found {}".format(path, len(entries)))
    entry = entries[0]
    if not isinstance(entry.get("directory"), str) or not entry["directory"]:
        raise BuildError("The main.cpp compilation entry has no working directory")
    cwd = Path(entry["directory"])
    cwd = (cwd if cwd.is_absolute() else build / cwd).resolve()
    if "arguments" in entry:
        argv = entry["arguments"]
        if not isinstance(argv, list) or not all(isinstance(arg, str) for arg in argv):
            raise BuildError("The main.cpp 'arguments' field must be a list of strings")
        argv = list(argv)
    elif isinstance(entry.get("command"), str):
        argv = shlex.split(entry["command"])
    else:
        raise BuildError("The main.cpp entry needs 'arguments' or 'command'")
    # Dependency output belongs to the original main object. Do not let a test
    # compilation overwrite its .d file or compilation-database fragment.
    cleaned = []
    index = 0
    while index < len(argv):
        arg = argv[index]
        if arg in ("-MF", "-MT", "-MQ", "-MJ"):
            if index + 1 >= len(argv):
                raise BuildError("Missing value after {} in the main.cpp command".format(arg))
            index += 2
        elif arg in ("-MD", "-MMD", "-MP", "-MG") or arg.startswith(("-MF", "-MT", "-MQ", "-MJ")):
            index += 1
        else:
            cleaned.append(arg)
            index += 1
    option_value_index(cleaned, "-c", "the main.cpp compilation command")
    option_value_index(cleaned, "-o", "the main.cpp compilation command")
    return cwd, cleaned


def find_gtest_libraries(link_argv, link_cwd, repo, gtest_prefix=None):
    search_dirs = [gtest_prefix / "lib", gtest_prefix / "lib64"] if gtest_prefix else []
    for index, arg in enumerate(link_argv):
        if arg == "-L" and index + 1 < len(link_argv):
            search_dirs.append(Path(link_argv[index + 1]))
        elif arg.startswith("-L") and len(arg) > 2:
            search_dirs.append(Path(arg[2:]))
    deps = repo / "deps/3rd/usr/local/oceanbase/deps/devel"
    search_dirs.extend((deps / "lib", deps / "lib64"))
    for directory in search_dirs:
        directory = (directory if directory.is_absolute() else link_cwd / directory).resolve()
        libraries = [directory / "libgtest_main.a", directory / "libgtest.a"]
        if all(library.is_file() for library in libraries):
            return libraries
    raise BuildError("Cannot find libgtest_main.a and libgtest.a in the production -L directories or repository dependencies")


def read_link_command(build):
    link_cwd = build / "src/observer"
    link_file = link_cwd / "CMakeFiles/seekdb.dir/link.txt"
    if link_file.is_file():
        return link_cwd, shlex.split(read_text(link_file))
    if not (build / "build.ninja").is_file():
        raise BuildError("Cannot find a Makefiles or Ninja seekdb link command in {}".format(build))
    try:
        commands = subprocess.check_output(
            ["ninja", "-C", str(build), "-t", "commands", "src/observer/seekdb"], text=True)
    except (OSError, subprocess.CalledProcessError) as error:
        raise BuildError("Cannot inspect Ninja link command: {}".format(error)) from error
    candidates = []
    for command in commands.splitlines():
        argv = shlex.split(command)
        if "-o" in argv and argv[argv.index("-o") + 1] == "src/observer/seekdb":
            # CMake wraps Ninja link steps with no-op shell commands. Execute
            # only the compiler argv; never pass the generated string to a shell.
            if argv[:2] == [":", "&&"]:
                argv = argv[2:]
            if argv[-2:] == ["&&", ":"]:
                argv = argv[:-2]
            if any(arg in ("&&", ";", "||", "|") for arg in argv):
                raise BuildError("Unsupported compound Ninja link command")
            candidates.append(argv)
    if len(candidates) != 1:
        raise BuildError("Expected one Ninja seekdb link command; found {}".format(len(candidates)))
    return build, candidates[0]


def make_plan(repo, build, out, gtest_prefix=None, extra_sources=()):
    for tree in (repo, build):
        if out == tree or tree in out.parents:
            raise BuildError("--out must be outside both the repository and build trees: {}".format(out))
    sources = [repo / relative for relative in (*TEST_SOURCES, *extra_sources)]
    for source in sources:
        if not source.is_file():
            raise BuildError("Missing native regression source: {}".format(source))
    compile_cwd, template = compile_template(build)
    objects = [out / (source.stem + ".o") for source in sources]
    compiles = []
    for source, obj in zip(sources, objects):
        argv = list(template)
        if gtest_prefix:
            argv.extend(["-isystem", str(gtest_prefix / "include")])
        argv[option_value_index(argv, "-c", "the compilation command")] = str(source)
        argv[option_value_index(argv, "-o", "the compilation command")] = str(obj)
        compiles.append({"source": str(source), "object": str(obj), "cwd": str(compile_cwd), "argv": argv})

    link_cwd, link_argv = read_link_command(build)
    main_suffix = "CMakeFiles/ob_main.dir/main.cpp.o"
    command_line_suffix = "CMakeFiles/ob_main.dir/ob_command_line_parser.cpp.o"
    main_args = [arg for arg in link_argv if arg.endswith(main_suffix)]
    command_line_args = [arg for arg in link_argv if arg.endswith(command_line_suffix)]
    if len(main_args) != 1 or len(command_line_args) != 1:
        raise BuildError("Expected one main.cpp.o and one ob_command_line_parser.cpp.o in the seekdb link command")
    libraries = find_gtest_libraries(link_argv, link_cwd, repo, gtest_prefix)
    replacement = [str(obj) for obj in objects] + [str(library) for library in libraries]
    linked = []
    for arg in link_argv:
        if arg == main_args[0]:
            linked.extend(replacement)
        elif arg != command_line_args[0]:
            linked.append(arg)
    binary = out / "fts_regression_tests"
    linked[option_value_index(linked, "-o", "the seekdb link command")] = str(binary)
    return {
        "repository": str(repo), "build": str(build), "binary": str(binary),
        "compile": compiles,
        "link": {"cwd": str(link_cwd), "argv": linked},
    }


def run_step(step, description, plan_path):
    try:
        subprocess.run(step["argv"], cwd=step["cwd"], check=True)
    except subprocess.CalledProcessError as error:
        raise BuildError("{} failed with exit code {}; commands are recorded in {}".format(description, error.returncode, plan_path)) from error
    except OSError as error:
        raise BuildError("Could not execute {}: {}; commands are recorded in {}".format(description, error, plan_path)) from error


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2], help="repository containing the regression sources")
    parser.add_argument("--build", type=Path, help="existing build directory (default: REPO/build_release)")
    parser.add_argument("--out", type=Path, default=Path(tempfile.gettempdir()) / "seekdb-fts-regressions", help="output directory outside the repository and build trees")
    parser.add_argument("--execute", action="store_true", help="compile and link; otherwise only check and show the plan")
    parser.add_argument("--gtest-prefix", type=Path, help="Google Test installation prefix, if outside production dependency paths")
    parser.add_argument("--test-source", action="append", default=[], help="additional native test source relative to the repository (repeatable)")
    args = parser.parse_args()
    repo = args.repo.resolve()
    build = (args.build or repo / "build_release").resolve()
    out = args.out.resolve()
    try:
        gtest_prefix = args.gtest_prefix.resolve() if args.gtest_prefix else None
        plan = make_plan(repo, build, out, gtest_prefix, args.test_source)
        print(json.dumps({
            "mode": "execute" if args.execute else "dry-run",
            "repository": str(repo), "build": str(build), "binary": plan["binary"],
            "sources": [step["source"] for step in plan["compile"]],
            "objects": [step["object"] for step in plan["compile"]],
            "plan_file": str(out / "build-plan.json") if args.execute else None,
        }, indent=2), flush=True)
        if args.execute:
            if not (build / "src/observer/seekdb").is_file():
                raise BuildError("Finish building the production seekdb target before using --execute")
            out.mkdir(parents=True, exist_ok=True)
            plan_path = out / "build-plan.json"
            plan_path.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
            for step in plan["compile"]:
                run_step(step, "Compiling " + Path(step["source"]).name, plan_path)
            run_step(plan["link"], "Linking fts_regression_tests", plan_path)
            print("Built " + plan["binary"], flush=True)
            print("Run: " + shlex.join([plan["binary"]]), flush=True)
    except (BuildError, OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
