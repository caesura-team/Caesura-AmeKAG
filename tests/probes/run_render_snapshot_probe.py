#!/usr/bin/env python3
"""One bounded U27 ownership observation; see the frozen request/build contract.

This controller never builds or retries. The maintained package runtime owns
the sole native child and its receipt. No alternate termination path lives here.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from datetime import datetime, timezone


BUDGETS = {"warm_frames": 8, "recovery_frames": 16, "png_wait_ms": 3000,
           "process_timeout_seconds": 90, "resource_fields": 11}
RESOURCE_FIELDS = {"dynamicIndexBuffers", "dynamicVertexBuffers", "frameBuffers", "indexBuffers",
                   "occlusionQueries", "programs", "shaders", "textures", "uniforms",
                   "vertexBuffers", "vertexLayouts"}
SOURCE_REQUIRED = {
    "CMakeLists.txt", "tests/CMakeLists.txt", "src/entry/Engine.cpp", "src/entry/Engine.h",
    "src/render/api/IRenderDevice.h", "src/render/api/ITextureManager.h",
    "src/render/BgfxRenderDevice.h", "src/render/BgfxRenderDevice.cpp",
    "src/render/BgfxDeviceCore.h", "src/render/BgfxDeviceCore.cpp",
    "src/render/BgfxDebugCallback.h", "src/render/BgfxDebugCallback.cpp",
    "src/render/ScreenshotQueue.h", "src/render/ScreenshotQueue.cpp",
    "src/render/TextureManager.cpp", "src/render/BgfxShaderManager.cpp",
    "src/render/EmbeddedShaders.h", "src/render/EmbeddedShaders.cpp", "src/render/EmbeddedShaders_S5.cpp",
    "src/render/TextRenderer.cpp", "src/render/TextRendererFont.cpp", "src/render/TextRenderer.h",
    "src/resource/ImageDecoder.cpp", "tests/probes/screenshot_gpu_probe.cpp",
    "tests/probes/run_screenshot_gpu_probe.py", "tests/probes/run_render_snapshot_probe.py",
    "scripts/package_runtime.py", "scripts/package_verification.py", "scripts/validation_process.py",
    "scripts/validation_sanitizer.py",
    "scripts/run_validation.py", "external/bgfx/bgfx/include/bgfx/bgfx.h",
    "external/bgfx/bgfx/src/bgfx.cpp", "external/bgfx/bgfx/src/bgfx_p.h",
    "external/bgfx/bgfx/src/renderer_d3d11.cpp",
}
CHECKS = {
    "ownership_ready", "warm_exactly_eight_advances", "baseline_ready_empty", "getter_preserves_observation",
    "extra_resources_created", "extra_allocator_growth", "retained_resources_reject_baseline",
    "extra_engine_handles_released", "resource_vector_returns_within_sixteen_frames",
    "retained_png_rejects_baseline", "normal_recovery_succeeds", "normal_recovery_fresh_context",
    "normal_recovery_actual_d3d11", "old_terminal_retained_across_recovery", "old_context_baseline_rejected",
    "old_fixture_rtt_invalid", "manager_fixture_restored", "ttf_fixture_restored",
    "empty_new_context_rejects_old_baseline", "capture_generation_changes_independently",
    "recovered_font_pixels_equal", "invalid_size_pending_admission", "invalid_size_ticket_waiting_not_submitted",
    "invalid_size_recovery_returns_false", "invalid_size_context_unavailable",
    "invalid_size_shutdown_tracking_complete", "invalid_size_rejects_baseline", "invalid_size_rejects_new_ticket",
    "invalid_size_waiting_ticket_cancelled", "invalid_size_ticket_consumed_once", "invalid_size_retained_queue_empty",
    "invalid_size_frame_does_not_advance_submission",
}
for _name in ("warm", "held", "fresh"):
    CHECKS.update(_name + suffix for suffix in (
        "_admission", "_terminal_png_retained", "_wait_without_frame_pump", "_take_transfers_png", "_take_once",
        "_png_ticket_and_dimensions", "_png_decoded_dimensions", "_png_background_pixels",
        "_png_manager_texture_pixels", "_png_rtt_pixels", "_png_ttf_visible", "_png_u16_ttf_opaque_coverage"))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def file_lock(path):
    path = Path(path).resolve(strict=True)
    require(path.is_file(), f"Not a required file: {path}")
    return {"path": str(path), "sha256": digest(path), "size": path.stat().st_size}


def verify_file(lock):
    require(isinstance(lock, dict) and set(lock) == {"path", "sha256", "size"}, "Invalid file lock schema")
    require(Path(lock["path"]).is_absolute(), "File locks require absolute paths")
    require(file_lock(lock["path"]) == lock, f"File lock changed: {lock['path']}")
    return Path(lock["path"])


def source_locks(root, expected):
    require(SOURCE_REQUIRED <= set(expected), "Required source/runner/embedded shader locks missing")
    actual = {}
    for name, sha in expected.items():
        path = (root / name).resolve(strict=True)
        require(not Path(name).is_absolute() and path.is_relative_to(root), "Source lock escapes checkout")
        require(isinstance(sha, str) and len(sha) == 64 and digest(path) == sha, f"Source changed: {name}")
        actual[name] = sha
    return actual


def clean_receipt(receipt, executable):
    require(receipt.get("status") == "EXITED" and receipt.get("actual_exit_code") == 0,
            "Owned command did not exit successfully")
    require(receipt.get("owned_tree_cleanup") == "COMPLETE" and receipt.get("launcher_exit_code") == 0,
            "Owned command cleanup/launcher receipt incomplete")
    require(all(receipt.get(key) is False for key in ("timed_out", "forced_kill", "stop_requested")),
            "Timeout, forced termination or stop cannot pass")
    process = receipt.get("process", {})
    require(type(process.get("pid")) is int and process["pid"] > 0 and str(process.get("created", "")).isdigit()
            and process.get("source") == "windows:GetProcessTimes/QueryFullProcessImageNameW",
            "Missing actual Windows creation/image identity")
    require(Path(process.get("executable", "")).resolve() == executable, "Observed executable identity differs")


def validate_build(request, root, exe, cache):
    record = read_json(verify_file(request["build_record"]))
    require(record.get("schema") == 1, "Unknown build record schema")
    require(record["source_before"] == request["source_identity"] == record["source_after"], "Build/source mismatch")
    require(record["locks_before"] == request["source_locks"] == record["locks_after"], "Build input locks mismatch")
    require(record["cache_before"] == request["cmake_cache"] == record["cache_after"], "Build cache changed")
    require(record["binary_after"] == request["runtime_locks"]["executable"], "Built binary lock mismatch")
    receipt = read_json(verify_file(record["receipt_file"]))
    verify_file(record["stdout"])
    verify_file(record["stderr"])
    argv = receipt.get("argv", [])
    require(len(argv) in (7, 9), "Unexpected probe build argv")
    require(argv[1:7] == ["--build", str(cache.parent), "--config", "Debug", "--target", "CaesuraScreenshotGpuProbe"]
            and (len(argv) == 7 or (argv[7] == "--parallel" and argv[8].isdigit() and int(argv[8]) > 0)),
            "Build target/configuration must identify the probe")
    require(Path(receipt["cwd"]).resolve() == root, "Build cwd differs from locked source")
    clean_receipt(receipt, Path(argv[0]).resolve(strict=True))
    require(Path(exe).is_relative_to(cache.parent), "Probe binary is outside the bound build directory")
    return record


def ready(value):
    return all(value.get(key) is True for key in ("supported", "context_initialized", "rendering_available",
        "resource_counts_available", "ownership_complete", "readback_tracking_supported")) and (
        value["backend_kind"] == 2 and value["context_generation"] > 0 and value["screenshots"]["supported"] is True)


def empty(value):
    queue = value["screenshots"]
    return all(queue[key] == 0 for key in ("waiting", "submitted", "terminal", "reserved_bytes", "png_bytes")) and value["readbacks_outstanding"] == 0


def accepts(baseline, value):
    return ready(baseline) and ready(value) and empty(value) and (
        baseline["context_generation"] == value["context_generation"] and baseline["resources"] == value["resources"])


def validate_observation(report, receipt, locks, output):
    require(report.get("schema") == 1 and report.get("scenario") == "ownership" and report.get("status") == "PASS"
            and report.get("shutdown_completed") is True and report.get("failed_checks") == 0, "Probe result is not a completed PASS")
    require(report["ownership_contract"] == BUDGETS, "Probe budget drift")
    require(report["pid"] == receipt["process"]["pid"], "Receipt/probe PID mismatch")
    require(report["host"]["renderer"] == "Direct3D 11" and report["host"]["shader_ready"] is True, "Actual D3D11 missing")
    for role in ("executable", "sdl", "d3d11"):
        require(Path(report["runtime_modules"][role]).resolve() == Path(locks[role]["path"]), f"Loaded {role} differs from lock")
    checks = report["checks"]
    names = [entry["name"] for entry in checks]
    require(len(names) == len(set(names)) and set(names) == CHECKS and all(entry["passed"] is True for entry in checks),
            f"Exact named checks differ: missing={sorted(CHECKS-set(names))}, extra={sorted(set(names)-CHECKS)}")
    samples = report["ownership_samples"]
    stages = [item["stage"] for item in samples]
    require(len(stages) == len(set(stages)), "Duplicate snapshot stages")
    by_stage = {item["stage"]: item for item in samples}
    expected_stages = ["initial"] + [f"warm_{i}" for i in range(1, 9)] + [
        "warm_terminal", "warm_before_take", "warm_after_take", "baseline", "baseline_reread", "extra_retained", "after_extra_destroy"]
    release_frames = report["release_frames"]
    require(type(release_frames) is int and 1 <= release_frames <= 16, "Release frame budget exceeded")
    expected_stages += [f"release_{i}" for i in range(1, release_frames + 1)] + [
        "held_terminal", "before_recover_held", "after_recover_held", "held_before_take", "held_after_take", "new_context_empty",
        "fresh_terminal", "fresh_before_take", "fresh_after_take", "before_invalid_size", "after_invalid_size",
        "invalid_size_taken", "invalid_size_noop_frame"]
    require(stages == expected_stages, "Missing/reordered raw snapshot stage")
    for item in samples:
        value = item["snapshot"]
        require(set(value["resources"]) == RESOURCE_FIELDS and all(type(v) is int and 0 <= v <= 2**64-1 for v in value["resources"].values()),
                "Eleven exact uint64 resource fields required")
    baseline = by_stage["baseline"]["snapshot"]
    require(ready(baseline) and empty(baseline) and baseline == by_stage["baseline_reread"]["snapshot"], "Baseline/getter observation mismatch")
    retained = by_stage["extra_retained"]["snapshot"]
    require(retained["resources"]["textures"] >= baseline["resources"]["textures"] + 2
            and retained["resources"]["frameBuffers"] >= baseline["resources"]["frameBuffers"] + 1 and not accepts(baseline, retained),
            "Retained real-resource negative control did not reject")
    require(by_stage["warm_8"]["owner_advances"] == 8 and by_stage["baseline"]["owner_advances"] == 8, "Warm budget differs")
    initial_frame = by_stage["initial"]["snapshot"]["capture_submission_frame"]
    require(by_stage["initial"]["owner_advances"] == 0, "Initial owner call count differs")
    for i in range(1, 9):
        item = by_stage[f"warm_{i}"]
        require(item["owner_advances"] == i and item["snapshot"]["capture_submission_frame"] == initial_frame + i,
                "Warm calls differ from actual submission frame progression")
    for i in range(1, release_frames + 1):
        item = by_stage[f"release_{i}"]
        require(item["owner_advances"] == 8 + i, "Missing explicit release frame")
        require(item["snapshot"]["capture_submission_frame"] == baseline["capture_submission_frame"] + i,
                "Release call differs from actual submission frame progression")
        require(accepts(baseline, item["snapshot"]) == (i == release_frames), "Release sequence must stop at first exact recovery")
    for name in ("warm", "held", "fresh"):
        wait = report["ownership_waits"][name]
        require(wait["owner_advances_before"] == wait["owner_advances_after"] and wait["snapshots"], "PNG wait pumped or lacks observations")
        require(type(wait["wait_ms"]) is int and 0 <= wait["wait_ms"] <= 3000, "PNG wait exceeded frozen deadline")
        require(all(s["capture_submission_frame"] == wait["submission_frame_before"] for s in wait["snapshots"]), "PNG wait advanced submission")
        terminal = by_stage[name + "_terminal"]["snapshot"]
        before = by_stage[name + "_before_take"]["snapshot"]
        after = by_stage[name + "_after_take"]["snapshot"]
        capture = report["captures"][name]
        require(ready(terminal) and terminal["screenshots"]["terminal"] == 1 and terminal["screenshots"]["png_bytes"] > 0
                and terminal["readbacks_outstanding"] == 0, "PNG retained terminal missing")
        require(before["screenshots"]["png_bytes"] == capture["png_bytes"] > 0 and empty(after)
                and capture["status"] == "Completed" and capture["width"] == 640 and capture["height"] == 360,
                "PNG ownership transfer mismatch")
        png = output / "probe" / (name + "_png.png")
        require(png.is_file() and png.stat().st_size == capture["png_bytes"] and png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", "Original PNG evidence missing")
    before = by_stage["before_recover_held"]["snapshot"]
    after = by_stage["after_recover_held"]["snapshot"]
    require(ready(after) and after["context_generation"] > before["context_generation"]
            and after["screenshots"] == before["screenshots"] and not accepts(baseline, after), "Recovery context/terminal mismatch")
    require(empty(by_stage["new_context_empty"]["snapshot"]) and not accepts(baseline, by_stage["new_context_empty"]["snapshot"]), "Old baseline accepted new context")
    captures = report["captures"]
    require(captures["warm"]["frame_id"] == initial_frame + 1
            and captures["held"]["frame_id"] == initial_frame + 9 + release_frames
            and captures["fresh"]["frame_id"] == initial_frame + 10 + release_frames,
            "Actual capture submissions differ from fixed owner frame sequence")
    require(captures["fresh"]["generation"] > captures["held"]["generation"]
            and captures["fresh"]["request_id"] != captures["held"]["request_id"], "Capture generation did not change")
    failed = by_stage["after_invalid_size"]["snapshot"]
    require(all(failed[key] is False for key in ("context_initialized", "rendering_available", "resource_counts_available"))
            and failed["context_generation"] == after["context_generation"] and not accepts(baseline, failed), "Invalid-size boundary mismatch")
    require(set(report["not_measured"]) == {"unanswered_native_request_retirement", "post_core_font_restore_failure",
            "native_os_device_removal", "other_backends_or_platforms", "one_hour_soak"}, "Unmeasured boundaries changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    require(not output.exists(), "Output directory must be fresh")
    output.mkdir(parents=True)
    record = {"schema": 1, "status": "FAIL", "started_utc": datetime.now(timezone.utc).isoformat(),
              "budgets": BUDGETS, "runtime_attempted": False}
    request = None
    source_identity = None
    locked_build_files = []
    try:
        require(os.name == "nt", "This probe requires Windows D3D11")
        request_lock = file_lock(args.request)
        record["request_lock"] = request_lock
        request = read_json(args.request)
        require(request.get("schema") == 1 and request["budgets"] == BUDGETS, "Unknown request/budget contract")
        root = Path(request["source_root"]).resolve(strict=True)
        require((root / ".git").exists(), "Missing checkout identity")
        require(Path(__file__).resolve() == root / "tests/probes/run_render_snapshot_probe.py", "Run the applied, source-locked driver")
        record["locks_before"] = source_locks(root, request["source_locks"])
        runtime = request["runtime_locks"]
        require({"executable", "sdl", "d3d11", "font"} <= set(runtime), "Runtime locks incomplete")
        for lock in runtime.values():
            verify_file(lock)
        exe = Path(runtime["executable"]["path"])
        require(exe.name == "CaesuraScreenshotGpuProbe.exe", "Wrong probe executable")
        require(Path(runtime["font"]["path"]) == root / "assets/fonts/NotoSansCJKsc-Regular.otf", "Wrong font fixture")
        require(Path(runtime["sdl"]["path"]) == exe.parent / "SDL3.dll", "Wrong SDL runtime")
        locked_paths = {Path(lock["path"]) for lock in runtime.values()}
        require(all(path.resolve() in locked_paths for path in exe.parent.glob("*.dll")), "Executable-adjacent DLL is unlocked")
        cache = verify_file(request["cmake_cache"])
        require(cache.name == "CMakeCache.txt", "Expected the actual CMake cache")
        sys.path.insert(0, str(root / "scripts"))
        from package_runtime import run_runtime_command
        from run_validation import _source_identity, _read_cmake_cache
        source_identity = _source_identity
        record["source_before"] = source_identity(root)
        require(record["source_before"] == request["source_identity"], "Current source differs from frozen request")
        cache_values = _read_cmake_cache(cache.parent)
        require(Path(cache_values["CMAKE_HOME_DIRECTORY"]).resolve() == root and "Debug" in cache_values.get("CMAKE_CONFIGURATION_TYPES", "").split(";"), "Build cache source/config mismatch")
        build = validate_build(request, root, exe, cache)
        locked_build_files = [request["cmake_cache"], request["build_record"], build["receipt_file"], build["stdout"], build["stderr"]]
        record["build_record"] = request["build_record"]
        record["runtime_before"] = runtime
        (output / "probe").mkdir()
        (output / "private-home").mkdir()
        (output / "private-temp").mkdir()
        system = Path(os.environ["SystemRoot"]).resolve(strict=True)
        env = {"SystemRoot": str(system), "WINDIR": str(system),
               "PATH": str(exe.parent) + os.pathsep + str(system / "System32"),
               "TEMP": str(output / "private-temp"), "TMP": str(output / "private-temp"),
               "USERPROFILE": str(output / "private-home"), "APPDATA": str(output / "private-home"),
               "LOCALAPPDATA": str(output / "private-home")}
        argv = [str(exe), "--scenario", "ownership", "--resource-root", str(root), "--output-dir", str(output / "probe")]
        record["argv"] = argv
        write_json(output / "request.json", request)
        record["runtime_attempted"] = True
        with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
            receipt = run_runtime_command(argv, root, env, output / "owned-process", stdout, stderr, 90)
        record["receipt"] = receipt
        clean_receipt(receipt, exe)
        require(receipt["argv"] == argv and Path(receipt["cwd"]).resolve() == root, "Runtime argv/cwd mismatch")
        observation = read_json(output / "probe/result.json")
        validate_observation(observation, receipt, runtime, output)
        record["named_checks"] = sorted(CHECKS)
        record["status"] = "PASS"
    except Exception as error:
        record["error"] = f"{type(error).__name__}: {error}"
    finally:
        try:
            if request is not None:
                verify_file(record["request_lock"])
                root = Path(request["source_root"]).resolve(strict=True)
                record["locks_after"] = source_locks(root, request["source_locks"])
                for lock in request["runtime_locks"].values():
                    verify_file(lock)
                for lock in locked_build_files:
                    verify_file(lock)
                record["runtime_after"] = request["runtime_locks"]
                if source_identity is not None:
                    record["source_after"] = source_identity(root)
                    require(record["source_after"] == request["source_identity"], "Source changed during observation")
            if (output / "owned-process/run.json").is_file():
                durable = read_json(output / "owned-process/run.json")
                if record["status"] == "PASS":
                    require(durable == record["receipt"], "Durable runtime receipt differs from observed receipt")
                record["receipt"] = durable
        except Exception as error:
            record["status"] = "FAIL"
            record["postcheck_error"] = f"{type(error).__name__}: {error}"
        record["finished_utc"] = datetime.now(timezone.utc).isoformat()
        record["evidence"] = [file_lock(path) for path in sorted(output.rglob("*")) if path.is_file() and path.name != "report.json"]
        write_json(output / "report.json", record)
    print(json.dumps({"status": record["status"], "report": str(output / "report.json")}, ensure_ascii=False))
    return 0 if record["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
