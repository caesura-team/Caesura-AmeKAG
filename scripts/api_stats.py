#!/usr/bin/env python3
"""api_stats.py — authoritative API census for the Caesura engine.

Scans the source tree (interfaces, Lua bindings, KAG commands, RPC
endpoints) and emits a Markdown statistics document. Every number is
derived from the code, never hand-maintained.

Round 70: the census used to also report live test-run counts (C++
cases/assertions, Lua tests passed). Those are environment-dependent
(FFmpeg presence, GPU/audio availability, runner load) and made the
CI docs-freshness guard flaky; test health is enforced by the
dedicated gates (ctest, Lua suites, check_test_coverage) instead.

Usage: python scripts/api_stats.py [--json]
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def scan_interfaces():
    """src/<module>/api/I*.h — pure-virtual method census."""
    modules = {}
    total_methods = 0
    for mod in sorted(os.listdir(os.path.join(ROOT, "src"))):
        api_dir = os.path.join(ROOT, "src", mod, "api")
        if not os.path.isdir(api_dir):
            continue
        ifaces = []
        for fn in sorted(os.listdir(api_dir)):
            if not (fn.startswith("I") and fn.endswith(".h")):
                continue
            path = os.path.join(api_dir, fn)
            text = open(path, encoding="utf-8", errors="replace").read()
            # Pure-virtual methods end in ") = 0;" or ") const = 0;".
            # The optional const qualifier (member-const methods) was previously
            # missed, systematically undercounting interfaces with const getters.
            methods = len(re.findall(r"\)\s*(?:const\s*)?=\s*0\s*;", text))
            ifaces.append({"file": fn, "methods": methods})
        if ifaces:
            modules[mod] = ifaces
            total_methods += sum(i["methods"] for i in ifaces)
    return modules, total_methods


def scan_lua_bindings():
    """src/script/bindings/*.cpp luaL_Reg tables + registration prints."""
    bindings = {}
    bind_dir = os.path.join(ROOT, "src", "script", "bindings")
    for fn in sorted(os.listdir(bind_dir)):
        if not fn.endswith(".cpp"):
            continue
        text = open(os.path.join(bind_dir, fn), encoding="utf-8", errors="replace").read()
        # luaL_Reg array entries: { "name", func },
        regs = re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*[A-Za-z_][A-Za-z0-9_]*\s*\}', text)
        # Explicit global-table registration also publishes callable APIs.
        # Requiring lua_getglobal excludes metatable __gc/data field writes.
        direct = re.findall(
            r'lua_getglobal\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\)\s*;\s*'
            r'lua_pushcfunction\(\s*\1\s*,\s*[A-Za-z_][A-Za-z0-9_]*\s*\)\s*;\s*'
            r'lua_setfield\(\s*\1\s*,\s*-2\s*,\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\)', text)
        regs = list(dict.fromkeys(regs + [name for _, _, name in direct]))
        globals_ = re.findall(r'lua_setglobal\([^,]+,\s*"([A-Za-z_][A-Za-z0-9_]*)"\)', text)
        globals_ = list(dict.fromkeys(globals_ + [target for _, target, _ in direct]))
        if regs:
            bindings[fn] = {"apis": regs, "globals": globals_}
    return bindings


def scan_kag_commands():
    """scripts/kag/commands/ + command-contracts.md census."""
    cmd_dir = os.path.join(ROOT, "scripts", "kag", "commands")
    files = sorted(os.listdir(cmd_dir)) if os.path.isdir(cmd_dir) else []
    contracts = os.path.join(ROOT, "docs", "api", "command-contracts.md")
    contract_count = 0
    if os.path.isfile(contracts):
        text = open(contracts, encoding="utf-8").read()
        contract_count = len(re.findall(r"^#{2,3} .*`\[", text, re.M)) or \
            len(re.findall(r"`\[[a-z_]+", text))
    return {"files": files, "contract_commands": contract_count}


def scan_rpc():
    """EditorServer endpoints (svr.Get/Post) + stdin dispatcher methods."""
    path = os.path.join(ROOT, "src", "rpc", "EditorServer.cpp")
    endpoints = []
    if os.path.isfile(path):
        text = open(path, encoding="utf-8", errors="replace").read()
        endpoints = re.findall(r'svr\.(Get|Post)\("([^"]+)"', text)
    # stdin JSON-RPC: RpcServer.cpp uses a method == "..." if-chain.
    stdin_methods = []
    rpc_path = os.path.join(ROOT, "src", "rpc", "RpcServer.cpp")
    if os.path.isfile(rpc_path):
        text = open(rpc_path, encoding="utf-8", errors="replace").read()
        stdin_methods = sorted(set(re.findall(r'method\s*==\s*"([a-zA-Z_][a-zA-Z0-9_]*)"', text)))
    return {"http_endpoints": endpoints, "stdin_methods": stdin_methods}


def count_lua_scripts():
    scripts_dir = os.path.join(ROOT, "scripts")
    count = 0
    for root, _, files in os.walk(scripts_dir):
        if "check" in root or "demo" in root:
            continue
        count += sum(1 for f in files if f.endswith(".lua"))
    return count


def main():
    interfaces, total_iface_methods = scan_interfaces()
    bindings = scan_lua_bindings()
    kag = scan_kag_commands()
    rpc = scan_rpc()
    lua_scripts = count_lua_scripts()

    stats = {
        "modules": sorted(interfaces.keys()),
        "interface_files": sum(len(v) for v in interfaces.values()),
        "interface_methods": total_iface_methods,
        "bindings": bindings,
        "binding_apis": sum(len(v["apis"]) for v in bindings.values()),
        "kag_command_files": len(kag["files"]),
        "kag_contract_commands": kag["contract_commands"],
        "rpc_http_endpoints": len(rpc["http_endpoints"]),
        "rpc_stdin_methods": len(rpc["stdin_methods"]),
        "lua_scripts": lua_scripts,
            }

    if "--json" in sys.argv:
        print(json.dumps(stats, indent=2, ensure_ascii=False))
        return

    lines = []
    A = lines.append
    A("# Caesura (AmeKAG) — API Statistics")
    A("")
    A("> Auto-generated by `scripts/api_stats.py`. Every number is derived from the")
    A("> source tree or a live test run — never hand-maintained. Regenerate with:")
    A("> `python scripts/api_stats.py`")
    A("")
    A("## 1. Surface summary")
    A("")
    A("| Metric | Count |")
    A("|--------|-------|")
    A(f"| Module libraries (src/) | {len(stats['modules'])} |")
    A(f"| API interface headers (src/*/api/I*.h) | {stats['interface_files']} |")
    A(f"| Pure-virtual interface methods | {stats['interface_methods']} |")
    A(f"| Lua binding functions (luaL_Reg entries) | {stats['binding_apis']} |")
    A(f"| KAG command handler files | {stats['kag_command_files']} |")
    A(f"| KAG contract commands (command-contracts.md) | {stats['kag_contract_commands']} |")
    A(f"| RPC HTTP endpoints (EditorServer) | {stats['rpc_http_endpoints']} |")
    A(f"| RPC stdin JSON-RPC methods | {stats['rpc_stdin_methods']} |")
    A(f"| Lua runtime scripts (scripts/, excl. demo/check) | {stats['lua_scripts']} |")
    A("")
    A("## 2. C++ interfaces by module")
    A("")
    A("| Module | Interface file | Pure-virtual methods |")
    A("|--------|----------------|----------------------|")
    for mod in stats["modules"]:
        for iface in interfaces[mod]:
            A(f"| {mod} | {iface['file']} | {iface['methods']} |")
    A("")
    A("## 3. Lua bindings by source file")
    A("")
    A("| Binding file | API count | Registered globals |")
    A("|--------------|-----------|--------------------|")
    for fn, info in bindings.items():
        A(f"| {fn} | {len(info['apis'])} | {', '.join(info['globals']) or '—'} |")
    A("")
    A("## 4. RPC surface")
    A("")
    A("### 4.1 HTTP endpoints (EditorServer)")
    A("")
    A("| Method | Path |")
    A("|--------|------|")
    for m, p in sorted(rpc["http_endpoints"]):
        A(f"| {m} | {p} |")
    A("")
    A("### 4.2 stdin JSON-RPC methods")
    A("")
    for m in rpc["stdin_methods"]:
        A(f"- `{m}`")
    A("")
    A("## 5. KAG command handlers")
    A("")
    A("| File |")
    A("|------|")
    for f in kag["files"]:
        A(f"| {f} |")

    out = os.path.join(ROOT, "docs", "api", "api-stats.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Wrote {out} ({len(lines)} lines)")


if __name__ == "__main__":
    main()
