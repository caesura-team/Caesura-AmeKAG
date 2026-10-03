#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_test_coverage.py — prevent orphan-test regressions (round 14).

Verifies that every test_*.lua in tests/scripts/ is registered in the main
runner (run_lua_tests.lua), the isolated orphan runner (run_orphan_tests.lua),
a direct lua_cli CTest command, or an explicit Lua entry in the owned CLI wrapper,
and that every test_*.cpp in tests/cpp/ is registered in tests/CMakeLists.txt.

Exit code 1 lists any unregistered test — the "silent green" failure mode.
Usage: python tests/scripts/check_test_coverage.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPTS = os.path.join(ROOT, "tests", "scripts")
CPP = os.path.join(ROOT, "tests", "cpp")
CMAKE = os.path.join(ROOT, "tests", "CMakeLists.txt")


def read(p):
    with io.open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def owned_cli_wrapper_tests(command):
    """Recognize only the maintained wrapper's complete, unambiguous CTest argv.

    This deliberately supports a restricted CMake token grammar, not expansion
    or shell interpretation. A quoted argument remains one token, so text inside
    another argument cannot invent flags. Registration requires all explicit
    inputs although standalone wrapper callers may use its sibling-test default.
    """
    token = r'"[^"\r\n]*"|[^\s"]+'
    if not re.fullmatch(r'\s*(?:' + token + r')(?:\s+(?:' + token + r'))*\s*', command):
        return set()
    argv = [value[1:-1] if value.startswith('"') else value
            for value in re.findall(token, command)]
    if (len(argv) != 12 or argv[0] != "NAME"
            or not re.fullmatch(r"[A-Za-z0-9_]+", argv[1])
            or argv[2:5] != ["COMMAND", "${Python3_EXECUTABLE}", "-B"]):
        return set()
    wrapper = "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_cli_asset_directory.py"
    if argv[5] != wrapper:
        return set()
    required = {
        "--lua": "$<TARGET_FILE:lua_cli>",
        "--scripts": "${CMAKE_SOURCE_DIR}/scripts",
        "--test": wrapper[:-3] + ".lua",
    }
    seen = set()
    for index in range(6, len(argv), 2):
        flag, value = argv[index:index + 2]
        if flag not in required or flag in seen or value != required[flag]:
            return set()
        seen.add(flag)
    if seen != set(required):
        return set()
    return {required["--test"].rsplit("/", 1)[1][:-4]}


def main():
    problems = []

    # --- Lua side ---------------------------------------------------------
    lua_tests = sorted(
        fn[:-4] for fn in os.listdir(SCRIPTS)
        if fn.startswith("test_") and fn.endswith(".lua")
    )
    registered = set()
    for runner in ("run_lua_tests.lua", "run_orphan_tests.lua"):
        src = read(os.path.join(SCRIPTS, runner))
        registered |= set(re.findall(r'"([a-z0-9_]+)"', src))
    # "replay" is a preload require, not a test — exclude it from the check.
    registered.discard("replay")
    cmake = read(CMAKE)
    commands = re.sub(r"#\[(=*)\[.*?\]\1\]", "", cmake, flags=re.S)
    commands = re.sub(r"(?m)#.*$", "", commands)
    for command in re.findall(r"add_test\s*\((.*?)\)", commands, re.S):
        registered |= set(re.findall(
            r"COMMAND\s+\$<TARGET_FILE:lua_cli>\s+"
            r"\$\{CMAKE_CURRENT_SOURCE_DIR\}/scripts/(test_[a-z0-9_]+)\.lua(?=\s|$)",
            command))
        registered |= owned_cli_wrapper_tests(command)
    for t in lua_tests:
        if t not in registered:
            problems.append(f"Lua test not registered: tests/scripts/{t}.lua")

    # --- C++ side ---------------------------------------------------------
    cpp_tests = sorted(
        fn for fn in os.listdir(CPP)
        if fn.startswith("test_") and fn.endswith(".cpp")
    )
    listed = set(re.findall(r"cpp/(test_[a-z0-9_]+.cpp)", cmake))
    for f in cpp_tests:
        if f not in listed:
            problems.append(f"C++ test not in CMakeLists file list: tests/cpp/{f}")

    # --- Editor command-highlight drift ------------------------------------
    # The editor's Monaco KAG_COMMANDS table must cover every schema contract
    # command; a missing command renders it as tag.invalid (round 19).
    editor_lang = os.path.join(ROOT, "editor", "src", "ide", "kagLanguage.ts")
    doc = os.path.join(ROOT, "docs", "api", "command-contracts.md")
    if os.path.exists(editor_lang) and os.path.exists(doc):
        lang_src = read(editor_lang)
        # Round 82: KAG_COMMANDS is now re-exported from lib/commandLint.ts
        # (KNOWN_COMMANDS is the single source of truth). Parse the literal
        # array in commandLint.ts; fall back to the old KAG_COMMANDS literal
        # for robustness.
        lint_path = os.path.join(ROOT, "editor", "src", "lib", "commandLint.ts")
        editor_cmds = set()
        for probe_path, probe_name in ((lint_path, "KNOWN_COMMANDS"),
                                       (editor_lang, "KAG_COMMANDS")):
            if not os.path.exists(probe_path):
                continue
            probe_src = read(probe_path)
            m = re.search(re.escape(probe_name) + r"[^=]*=\s*\[(.*?)\]", probe_src, re.S)
            if m:
                editor_cmds = set(re.findall(chr(39) + "([a-z0-9_]+)" + chr(39), m.group(1)))
                break
        doc_src = read(doc)
        doc_cmds = set(re.findall("^### " + chr(96) + chr(92) + "[([a-z0-9_]+)" + chr(92) + "]", doc_src, re.M))
        for c in sorted(doc_cmds):
            if c not in editor_cmds:
                problems.append(f"Editor highlight missing schema command: {c}")
        # TEMP-DEBUG

    if problems:
        print(f"TEST COVERAGE: {len(problems)} problem(s) found:")
        for p in problems:
            print("  -", p)
        print("New tests must be registered in the runner / CMakeLists to run.")
        sys.exit(1)
    print(f"TEST COVERAGE OK: {len(lua_tests)} lua + {len(cpp_tests)} cpp tests all registered.")


if __name__ == "__main__":
    main()
