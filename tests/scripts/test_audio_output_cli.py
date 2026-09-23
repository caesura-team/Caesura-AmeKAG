"""Exercise audio output selection in the actual Engine CLI, without a GPU.

Usage: python test_audio_output_cli.py --engine ENGINE [--output NEW_DIRECTORY]
Without --output, retains a fresh exclusive directory in the OS temporary root.
This proves CLI/backend selection only. Real mixer progression is exercised by
the C++ suite; packaged GPU/demo execution and physical audio are not claimed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import uuid


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve(strict=True)
    output = (args.output if args.output is not None else
              Path(tempfile.gettempdir()) / ("caesura-audio-output-cli-" + str(uuid.uuid4()))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f"Evidence directory: {output}", flush=True)
    engine_sha = sha256(engine)
    cases = [
        ("default_headless", [], None),
        ("explicit_software", ["--audio-output", "software"], None),
        ("explicit_device", ["--audio-output", "device"], None),
        ("missing", ["--audio-output"], "--audio-output requires a value"),
        ("empty", ["--audio-output", ""], "--audio-output requires a value"),
        ("next_flag", ["--audio-output", "--frames", "1"], "--audio-output requires a value"),
        ("unknown", ["--audio-output", "automatic"], "Invalid --audio-output value"),
        ("case_sensitive", ["--audio-output", "Software"], "Invalid --audio-output value"),
        ("manual_not_cli", ["--audio-output", "manual"], "Invalid --audio-output value"),
        ("conflicting", ["--audio-output", "software", "--audio-output", "device"],
         "Conflicting --audio-output options"),
    ]
    records = []
    environment = os.environ.copy()
    environment.pop("CAESURA_RESOURCE_ROOT", None)
    for name, options, error in cases:
        directory = output / name
        launch = directory / "launch space"
        resource = directory / "resource space"
        launch.mkdir(parents=True)
        (resource / "assets").mkdir(parents=True)
        (resource / "scripts" / "kag").mkdir(parents=True)
        (resource / "scripts" / "config.lua").write_text(
            "config = {}\nprint('U22_AUDIO_CONFIG_LOADED')\n", encoding="utf-8")
        (resource / "scripts" / "kag" / "init.lua").write_text(
            "-- Isolated CLI transport fixture; no substituted audio implementation.\n", encoding="utf-8")
        command = [str(engine), "--headless", "--resource-root", str(resource), *options]
        record = {"name": name, "argv": command, "cwd": str(launch), "passed": False,
                  "physical_device": "NOT_RUN"}
        try:
            completed = subprocess.run(command, cwd=launch, env=environment,
                                       input=b'{"id":101,"method":"ping"}\n',
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            stdout, stderr = completed.stdout, completed.stderr
            record["exit_code"] = completed.returncode
        except subprocess.TimeoutExpired as caught:
            stdout, stderr = caught.stdout or b"", caught.stderr or b""
            record["error"] = "CLI process exceeded 30 seconds and was killed by its owning subprocess.run"
        (directory / "stdout.log").write_bytes(stdout)
        (directory / "stderr.log").write_bytes(stderr)
        record["stdout_sha256"] = sha256(directory / "stdout.log")
        record["stderr_sha256"] = sha256(directory / "stderr.log")
        text = (stdout + stderr).decode("utf-8", errors="replace")
        if "error" not in record:
            if error is not None:
                record["passed"] = (completed.returncode != 0 and error in text
                                    and "U22_AUDIO_CONFIG_LOADED" not in text
                                    and "[Audio] Output mode:" not in text)
            else:
                responses = []
                for line in stdout.decode("utf-8", errors="replace").splitlines():
                    try:
                        responses.append(json.loads(line))
                    except ValueError:
                        pass
                ping_ok = any(isinstance(item, dict) and item.get("id") == 101
                              and item.get("result") == "ok" for item in responses)
                stats_prefix = "[Audio] Software mix stats: "
                stats_lines = [line[len(stats_prefix):] for line in text.splitlines()
                               if line.startswith(stats_prefix)]
                stats_ok = False
                if len(stats_lines) == 1:
                    try:
                        stats = json.loads(stats_lines[0])
                        stats_ok = (type(stats["frames"]) is int and stats["frames"] >= 0
                                    and type(stats["samples"]) is int and stats["samples"] == 2 * stats["frames"]
                                    and stats["nonzero_samples"] == 0 and stats["nonfinite_samples"] == 0
                                    and stats["saturated"] is False and stats["sample_rate"] == 48000
                                    and stats["channels"] == 2 and stats["physical_device"] == "NOT_RUN"
                                    and math.isfinite(stats["peak"]) and stats["peak"] == 0
                                    and math.isfinite(stats["absolute_energy"]) and stats["absolute_energy"] == 0)
                    except (ValueError, KeyError, TypeError):
                        pass
                selected = {
                    "default_headless": "[BackendRegistry] Using NullAudioBackend." in text
                                        and "[Audio] Output mode:" not in text and not stats_lines,
                    "explicit_software": "[Audio] Output mode: software; physical_device=NOT_RUN" in text
                                         and "[Audio] SoLoud initialized: 3 buses" in text
                                         and "Using NullAudioBackend" not in text and stats_ok,
                    # Device may initialize or fail on this host. Either result must
                    # show the explicit Device attempt and never select Software.
                    "explicit_device": "[Audio] Output mode: device; physical_device=NOT_VERIFIED" in text
                                       and "[Audio] Output mode: software" not in text and not stats_lines,
                }[name]
                record["passed"] = (completed.returncode == 0 and ping_ok and selected
                                    and "U22_AUDIO_CONFIG_LOADED" in text)
        records.append(record)
        print(f"{'PASS' if record['passed'] else 'FAIL'} {name}")
    stable = sha256(engine) == engine_sha
    report = {"schema": "caesura.audio-output-cli-test.v1", "engine": str(engine),
              "engine_sha256": engine_sha, "engine_stable": stable, "cases": records,
              "physical_device": "NOT_RUN", "packaged_gpu_demo": "NOT_RUN",
              "passed": stable and all(record["passed"] for record in records)}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{sum(record['passed'] for record in records)}/{len(records)} passed; physical audio NOT_RUN")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
