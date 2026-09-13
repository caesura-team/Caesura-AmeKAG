"""Exercise --export-replay through the ordinary GPU executable.

Run explicitly with --engine and a new --output directory. Every invocation
keeps its author fixtures, executable/source hashes, owned PID, raw streams,
and assertions. This requires a working GPU; it never substitutes headless.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
CASES = (
    "ordinary", "playback", "empty", "missing_file", "module_error",
    "missing_load", "load_error", "missing_set_mode", "set_mode_error",
    "set_mode_false",
    "module_proxy", "module_proxy_error_load", "module_proxy_error_set_mode",
)
FAULTS = {
    "module_error": '''package.loaded.replay = nil
package.preload.replay = function() error("U21_MODULE_FAILURE") end''',
    "missing_load": "package.loaded.replay = {set_mode = replay.set_mode}",
    "load_error": '''package.loaded.replay = {
    load = function() error("U21_LOAD_FAILURE") end,
    set_mode = replay.set_mode,
}''',
    "missing_set_mode": "package.loaded.replay = {load = replay.load}",
    "set_mode_error": '''package.loaded.replay = {
    load = replay.load,
    set_mode = function() error("U21_MODE_FAILURE") end,
}''',
    "set_mode_false": '''package.loaded.replay = {
    load = replay.load,
    set_mode = function() return false end,
}''',
    "module_proxy": '''package.loaded.replay = setmetatable({}, {
    __index = function(_, key) return replay[key] end,
})''',
    "module_proxy_error_load": '''package.loaded.replay = setmetatable({}, {
    __index = function(_, key)
        print("U21_PROXY_LOOKUP:" .. key); io.stdout:flush()
        if key == "load" then error("U21_PROXY_LOAD_FAILURE") end
        return replay[key]
    end,
})''',
    "module_proxy_error_set_mode": '''package.loaded.replay = setmetatable({}, {
    __index = function(_, key)
        print("U21_PROXY_LOOKUP:" .. key); io.stdout:flush()
        if key == "set_mode" then error("U21_PROXY_MODE_FAILURE") end
        return replay[key]
    end,
})''',
}
ENTRY = '''local runner = require("kag_runner")
local replay = require("replay")
local layers = require("layers")
_G._KAG_onClick = runner.on_click
assert(runner.start("projects/replay_cli/entry.ks"))
for _ = 1, 8 do runner.update(0) end
local ctx = assert(runner.get_ctx())
assert(ctx.waiting_input and ctx.f.after_page == nil, "U21_FIXTURE_NOT_WAITING")
print("U21_CLI_READY")
local frame = 0
function _G.engine_update(dt)
    frame = frame + 1
    if frame == 1 then
        print("U21_CLI_START:" .. replay.get_mode() .. ":" .. replay.event_count())
        local file, detail = io.open("events.json", "r")
        assert(file == nil and detail == "io.open path not allowlisted",
            "U21_SANDBOX_BOUNDARY_CHANGED")
        print("U21_CLI_SANDBOX_LOCKED")
        if %s then _G._KAG_onClick() end
    end
    runner.update(dt)
    if frame == 8 then
        print("U21_CLI_RESULT:" .. replay.get_mode() .. ":" .. replay.event_count()
            .. ":" .. replay.clicks_fired() .. ":" .. tostring(ctx.f.after_page)
            .. ":" .. tostring(ctx.f.after_second_page))
    end
end
function _G.engine_render()
    layers.render()
    runner.render()
end
%s
'''


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def run_case(engine: Path, output: Path, case: str, backend: str, timeout: float):
    evidence = output / case
    fixture = evidence / "game"
    fixture.mkdir(parents=True)
    shutil.copytree(ROOT / "scripts", fixture / "scripts")
    (fixture / "assets" / "fonts").mkdir(parents=True)
    font = ROOT / "assets" / "fonts" / "NotoSansCJKsc-Regular.otf"
    shutil.copy2(font, fixture / "assets" / "fonts" / font.name)
    project = fixture / "projects" / "replay_cli"
    project.mkdir(parents=True)
    (project / "entry.ks").write_text(
        '[p]\n[set var="f.after_page" value=1]\n[p]\n'
        '[set var="f.after_second_page" value=1]\n[end]\n', encoding="utf-8")
    (fixture / "scripts" / "replay_cli_entry.lua").write_text(
        ENTRY % ("true" if case == "ordinary" else "false", FAULTS.get(case, "")), encoding="utf-8")
    config_path = fixture / "scripts" / "config.lua"
    config = config_path.read_text(encoding="utf-8")
    config = config.replace('config.entry_script = "../demo/entry.lua"',
                            'config.entry_script = "replay_cli_entry.lua"')
    config = config.replace("config.dev_mode = true", "config.dev_mode = false")
    # The fixture contains no audio assets or audio commands. Keep config's
    # ordinary initialization; no mock backend or production test hook.
    config_path.write_text(config, encoding="utf-8")
    if case != "missing_file":
        (fixture / "events.json").write_text(
            "[]" if case == "empty" else '[{"t":0,"type":"click","x":100,"y":100}]',
            encoding="utf-8")
    command = [str(engine), "--resource-root", str(fixture), "--backend", backend,
               "--resolution", "320x240", "--frames", "8"]
    if case != "ordinary":
        command += ["--export-replay", "events.json", "--export-dir", "frames"]
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    environment = dict(os.environ)
    for key in ("CAESURA_RESOURCE_ROOT", "LUA_PATH", "LUA_CPATH", "LUA_INIT", "LUA_INIT_5_4"):
        environment.pop(key, None)
    fixture_hashes = {p.relative_to(fixture).as_posix(): digest(p)
                      for p in sorted(fixture.rglob("*")) if p.is_file()}
    record = {"case": case, "command": command, "cwd": str(fixture),
              "engine_sha256": digest(engine), "fixtures": fixture_hashes,
              "started_utc": datetime.now(timezone.utc).isoformat(), "status": "RUNNING"}
    write_json(evidence / "process.json", record)
    old_error_mode = None
    if os.name == "nt":
        # Children inherit process error mode. Suppress OS crash dialogs for
        # this owned launch, restoring the harness immediately afterwards.
        # No registry or machine-wide error-reporting setting is changed.
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetErrorMode.argtypes = []
        kernel32.GetErrorMode.restype = ctypes.c_uint
        kernel32.SetErrorMode.argtypes = [ctypes.c_uint]
        kernel32.SetErrorMode.restype = ctypes.c_uint
        old_error_mode = kernel32.GetErrorMode()
        kernel32.SetErrorMode(old_error_mode | 0x0001 | 0x0002 | 0x8000)
    try:
        process = subprocess.Popen(command, cwd=fixture, env=environment, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, startupinfo=startup,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    finally:
        if old_error_mode is not None:
            kernel32.SetErrorMode(old_error_mode)
    record["pid"] = process.pid
    write_json(evidence / "process.json", record)
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.kill()  # This exact owned process only; reap before returning.
        stdout, stderr = process.communicate(timeout=10)
    (evidence / "stdout.log").write_bytes(stdout)
    (evidence / "stderr.log").write_bytes(stderr)
    text = (stdout + stderr).decode("utf-8", errors="replace")
    failures = []
    if timed_out:
        failures.append("engine exceeded external timeout")
    if "U21_CLI_READY" not in text:
        failures.append("author fixture did not reach its real page wait")
    if "[HEADLESS MODE]" in text or "Failed to initialize engine" in text:
        failures.append("ordinary GPU engine did not initialize")
    positive = case in ("ordinary", "playback", "empty", "module_proxy")
    expected_exit = 0 if positive else 1
    if process.returncode != expected_exit:
        failures.append(f"expected exit {expected_exit}, got {process.returncode}")
    if positive:
        expected = {
            "ordinary": "U21_CLI_RESULT:off:0:0:1:nil",
            "playback": "U21_CLI_RESULT:playback:1:1:1:nil",
            "empty": "U21_CLI_RESULT:playback:0:0:nil:nil",
            "module_proxy": "U21_CLI_RESULT:playback:1:1:1:nil",
        }[case]
        for marker in (expected, "U21_CLI_SANDBOX_LOCKED"):
            if text.count(marker) != 1:
                failures.append("expected exactly one marker: " + marker)
        if case != "ordinary" and not list((fixture / "frames").glob("frame_*.png")):
            failures.append("export mode produced no rendered frame")
    else:
        if "[main] Export mode:" in text:
            failures.append("failed replay initialization claimed Export mode success")
        if "U21_CLI_START:" in text:
            failures.append("failed replay initialization entered the game loop")
        if "[main] replay" not in text:
            failures.append("failed replay initialization had no replay diagnostic")
    record.update(status="FAIL" if failures else "PASS", failures=failures,
                  exit_code=process.returncode, timed_out=timed_out,
                  finished_utc=datetime.now(timezone.utc).isoformat(),
                  stdout_sha256=digest(evidence / "stdout.log"),
                  stderr_sha256=digest(evidence / "stderr.log"),
                  frames=len(list((fixture / "frames").glob("frame_*.png"))))
    write_json(evidence / "process.json", record)
    print(f"{record['status']} {case}: " + ("; ".join(failures) if failures else "all checks passed"), flush=True)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--backend", default="dx11" if os.name == "nt" else "opengl")
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=CASES)
    options = parser.parse_args()
    engine = options.engine.resolve(strict=True)
    output = options.output.resolve()
    if output.exists():
        parser.error("--output must be a new evidence directory; existing results are never overwritten")
    output.mkdir(parents=True)
    sources = [ROOT / "src/main.cpp", ROOT / "scripts/replay.lua", ROOT / "scripts/kag_runner.lua",
               ROOT / "scripts/sandbox.lua", Path(__file__).resolve()]
    source_hashes = {path.relative_to(ROOT).as_posix(): digest(path) for path in sources}
    engine_hash = digest(engine)
    write_json(output / "source.json", source_hashes)
    results = [run_case(engine, output, case, options.backend, options.timeout) for case in options.cases]
    unchanged = source_hashes == {path.relative_to(ROOT).as_posix(): digest(path) for path in sources}
    binary_unchanged = digest(engine) == engine_hash and all(
        item["engine_sha256"] == engine_hash for item in results)
    result = {"passed": sum(item["status"] == "PASS" for item in results),
              "failed": sum(item["status"] != "PASS" for item in results),
              "sources_unchanged": unchanged, "binary_unchanged": binary_unchanged,
              "engine_sha256": engine_hash, "cases": results}
    write_json(output / "result.json", result)
    return 0 if result["failed"] == 0 and unchanged and binary_unchanged else 1


if __name__ == "__main__":
    sys.exit(main())
