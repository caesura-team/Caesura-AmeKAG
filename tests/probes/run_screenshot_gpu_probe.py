#!/usr/bin/env python3
"""Run each real D3D11 probe scenario once in its own bounded, hidden process.

The executable owns GPU validation. This wrapper preserves its observations,
logs, process outcome, fixture identity, and binary identity without retrying or
turning a missing/aborted/timed-out probe into a passing result.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime, timezone


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def snapshot(executable: Path, resources: Path) -> dict[str, str | None]:
    files = {
        "executable": executable,
        "font": resources / "assets/fonts/NotoSansCJKsc-Regular.otf",
        "probe_source": resources / "tests/probes/screenshot_gpu_probe.cpp",
        "renderer_source": resources / "src/render/BgfxRenderDevice.cpp",
        "engine_source": resources / "src/entry/Engine.cpp",
        "renderer_interface": resources / "src/render/api/IRenderDevice.h",
        "screenshot_queue_source": resources / "src/render/ScreenshotQueue.cpp",
        "screenshot_queue_header": resources / "src/render/ScreenshotQueue.h",
        "device_core_source": resources / "src/render/BgfxDeviceCore.cpp",
        "device_core_header": resources / "src/render/BgfxDeviceCore.h",
        "draw_effects_source": resources / "src/render/BgfxDraw_Effects.cpp",
        "callback_source": resources / "src/render/BgfxDebugCallback.cpp",
        "callback_header": resources / "src/render/BgfxDebugCallback.h",
        "text_renderer_source": resources / "src/render/TextRenderer.cpp",
        "text_renderer_font_source": resources / "src/render/TextRendererFont.cpp",
        "text_renderer_header": resources / "src/render/TextRenderer.h",
        "texture_manager_source": resources / "src/render/TextureManager.cpp",
        "image_decoder_source": resources / "src/resource/ImageDecoder.cpp",
        "runner_source": Path(__file__).resolve(),
    }
    return {name: sha256(path) if path.is_file() else None for name, path in files.items()}


def run_scenario(executable: Path, resources: Path, output: Path, scenario: str, timeout: float) -> dict:
    scenario_output = output / scenario
    scenario_output.mkdir()
    command = [str(executable), "--scenario", scenario, "--resource-root", str(resources),
               "--output-dir", str(scenario_output)]
    record = {"scenario": scenario, "command": command, "cwd": str(resources),
              "started_utc": utc_now(), "timeout_seconds": timeout, "status": "RUNNING"}
    started = time.monotonic()
    with (scenario_output / "stdout.log").open("wb") as stdout, (scenario_output / "stderr.log").open("wb") as stderr:
        child = subprocess.Popen(command, cwd=resources, stdin=subprocess.DEVNULL,
                                 stdout=stdout, stderr=stderr,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        record["pid"] = child.pid
        write_json(scenario_output / "process.json", record)
        try:
            record["exit_code"] = child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            # Only the exact owned child is killed. No process-name matching,
            # machine-wide cleanup, or a second run with a relaxed timeout.
            record["timed_out"] = True
            child.kill()
            record["exit_code"] = child.wait(timeout=10)
        except BaseException:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
            raise
    record["elapsed_seconds"] = round(time.monotonic() - started, 3)
    record["finished_utc"] = utc_now()
    result_path = scenario_output / "result.json"
    observation = None
    try:
        observation = json.loads(result_path.read_text(encoding="utf-8"))
        record["probe_status"] = observation.get("status")
        record["probe_error"] = observation.get("error")
        checks = observation.get("checks", [])
        record["checks_observed"] = len(checks)
        record["failed_checks"] = [check.get("name") for check in checks if check.get("passed") is not True]
    except (OSError, ValueError, TypeError, AttributeError) as error:
        record["observation_error"] = str(error)
    passed = (not record.get("timed_out") and record["exit_code"] == 0
              and isinstance(observation, dict) and observation.get("status") == "PASS"
              and observation.get("scenario") == scenario
              and observation.get("shutdown_completed") is True
              and len(observation.get("checks", [])) >= 10
              and not record.get("failed_checks"))
    if scenario == "fill":
        # Preserve raw logs and make explicit resource diagnostics fail this
        # ownership observation, even if the driver happened to retain pixels.
        # Other scenarios keep their original result policy.
        diagnostics = []
        markers = ("invalid handle.", "already destroyed", "invalid texture handle")
        for name in ("stdout.log", "stderr.log"):
            for line in (scenario_output / name).read_text(encoding="utf-8", errors="replace").splitlines():
                if any(marker in line.lower() for marker in markers):
                    diagnostics.append({"log": name, "line": line})
        record["resource_diagnostics"] = diagnostics
        passed = passed and not diagnostics
    if record.get("timed_out"):
        record["status"] = "TIMEOUT"
    elif passed:
        record["status"] = "PASS"
    elif record["exit_code"] not in (0, 1):
        record["status"] = "PROCESS_ERROR"
    else:
        record["status"] = "FAIL"
    record["artifacts_sha256"] = {path.name: sha256(path)
                                  for path in scenario_output.iterdir()
                                  if path.is_file() and path.name != "process.json"}
    write_json(scenario_output / "process.json", record)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--resource-root", required=True, type=Path,
                        help="Checkout/fixture root containing assets and scripts; used as the actual child CWD")
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="A new directory; an existing directory is rejected to preserve earlier evidence")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="Per-scenario wall-clock limit in seconds (default 60, maximum 120)")
    parser.add_argument("--scenario", choices=("renderer", "rpc", "fill", "present-recovery"),
                        help="Run only this scenario; default remains renderer then rpc (one child each)")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This actual D3D11 probe requires Windows")
    executable = args.exe.resolve(strict=True)
    resources = args.resource_root.resolve(strict=True)
    output = args.output_dir.resolve()
    if not executable.is_file() or not resources.is_dir():
        parser.error("The executable or resource root is invalid")
    if not 1.0 <= args.timeout <= 120.0:
        parser.error("--timeout must be between 1 and 120 seconds")
    if not (resources / "assets/fonts/NotoSansCJKsc-Regular.otf").is_file():
        parser.error("The required real font fixture is missing")
    output.mkdir(parents=True, exist_ok=False)
    scenarios = (args.scenario,) if args.scenario else ("renderer", "rpc")
    report = {"schema": 1, "status": "RUNNING", "started_utc": utc_now(),
              "executable": str(executable), "resource_root": str(resources),
              "identity_before": snapshot(executable, resources), "scenarios": [],
              "boundary": ("U16 explicit physical-size recovery, real 800x450 drawable and 640x360 logical canvas; not OS removal or a real DPI transition"
                           if args.scenario == "present-recovery" else
                           "U16 production fillViewport/cache ownership, one RTT, S/A/two normal frames/same A/B/shutdown"
                           if args.scenario == "fill" else
                           "D3D11 screenshot/recovery and actual Engine captureFrameForRpc; not OS removal or post-core restoration failure"),
              "identity_boundary": "Only enumerated files are observed; whole-worktree/build identity is the integration gate's responsibility"}
    write_json(output / "run.json", report)
    try:
        for scenario in scenarios:
            report["scenarios"].append(run_scenario(executable, resources, output, scenario, args.timeout))
            write_json(output / "run.json", report)
        report["identity_after"] = snapshot(executable, resources)
        report["observed_inputs_stable"] = report["identity_before"] == report["identity_after"]
        passed = report["observed_inputs_stable"] and all(
            scenario["status"] == "PASS" for scenario in report["scenarios"])
        report["status"] = "PASS" if passed else "FAIL"
    except Exception as error:
        report["status"] = "RUNNER_ERROR"
        report["error"] = str(error)
    report["finished_utc"] = utc_now()
    write_json(output / "run.json", report)
    print(json.dumps({"status": report["status"], "report": str(output / "run.json"),
                      "scenarios": [{"scenario": item["scenario"], "status": item["status"]}
                                    for item in report["scenarios"]]}, ensure_ascii=False))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
