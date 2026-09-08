#!/usr/bin/env python3
"""Run native Caesura checks on EAS macOS workers; never manufacture release receipts."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import uuid

UPLOAD_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(UPLOAD_ROOT / "scripts"))
from validation_process import run_owned_command

SOURCE_URL = "https://github.com/caesura-team/Caesura-AmeKAG.git"
SDL_SHA = "b5c3eab6b447111d3c7879bb547b80fb4abd9063"  # release-3.2.4
OPENSSL_SHA = "fb7fab9fa6f4869eaa8fbb97e0d593159f03ffe4"  # openssl-3.3.2
# Match the effective engine minimum recorded by the Xcode 26.6 native builds.
IOS_DEPLOYMENT_TARGET = "14.0"


def now():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class Lane:
    def __init__(self, name, source_sha):
        self.name, self.source_sha = name, source_sha.lower()
        self.root = (UPLOAD_ROOT / "artifacts/eas" / name).resolve()
        self.evidence = self.root / "evidence"
        self.evidence.mkdir(parents=True, exist_ok=False)
        self.source = self.root / "source"
        self.simulator = None
        self.receipt = {
            "schema_version": 1, "provider": "expo-eas-workflows", "lane": name,
            "started_at": now(), "status": "RUNNING", "source_url": SOURCE_URL,
            "requested_source_sha": source_sha, "commands": [],
            "uploaded_driver_sha256": sha256(__file__),
            "uploaded_process_helper_sha256": sha256(UPLOAD_ROOT / "scripts/validation_process.py"),
            "uploaded_workflow_sha256": sha256(UPLOAD_ROOT / ".eas/workflows/u2-apple-validation.yml"),
            "unverified": ["physical Apple devices", "signing/provisioning/TestFlight",
                           "UIKit app lifecycle", "real Metal rendering", "real audio output"],
        }
        if name != "macos":
            self.receipt["unverified"] += ["iOS Lua suites", "iOS CTest integration"]
        if name == "ios-device":
            self.receipt["unverified"] += ["iOS device runtime (compile only)"]
        self.save()

    def save(self):
        (self.evidence / "eas-execution.json").write_text(
            json.dumps(self.receipt, indent=2) + "\n", encoding="utf-8")

    def source_snapshot(self):
        spec = importlib.util.spec_from_file_location("pinned_runner", self.source / "scripts/run_validation.py")
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        profile = json.loads((self.source / "scripts/validation_profiles.json").read_text(encoding="utf-8-sig"))
        return {**runner._source_identity(self.source),
                "fixture_sha256": runner.fingerprint_paths(self.source, profile["fixture_paths"])}

    def run(self, name, argv, *, cwd=None, timeout=600, allow_failure=False, env=None):
        argv = [str(part) for part in argv]
        cwd = Path(cwd or self.source).resolve()
        stdout, stderr = self.evidence / f"{name}.stdout.log", self.evidence / f"{name}.stderr.log"
        row = {"id": name, "argv": argv, "cwd": str(cwd), "started_at": now()}
        self.receipt["commands"].append(row)
        self.save()
        print(f"[{self.name}] {name}", flush=True)
        previous = {key: os.environ.get(key) for key in (env or {})}
        try:
            os.environ.update(env or {})
            with stdout.open("wb") as out, stderr.open("wb") as err:
                try:
                    row["exit_code"] = run_owned_command(argv, cwd, out, err, timeout)
                except subprocess.TimeoutExpired:
                    row.update(exit_code=124, error="timeout")
                except (OSError, ValueError) as error:
                    row.update(exit_code=127, error=str(error))
                    err.write((str(error) + "\n").encode())
        finally:
            for key, value in previous.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
            row.update(finished_at=now(), stdout=stdout.name, stderr=stderr.name)
            row["stdout_sha256"], row["stderr_sha256"] = sha256(stdout), sha256(stderr)
            self.save()
        if row["exit_code"]:
            print(stderr.read_text(errors="replace")[-4000:], flush=True)
            print(stdout.read_text(errors="replace")[-4000:], flush=True)
            if not allow_failure:
                raise RuntimeError(f"{name} failed with exit code {row['exit_code']}")
        return row["exit_code"], stdout.read_text(encoding="utf-8", errors="replace").strip()

    def checkout(self):
        if not re.fullmatch(r"[0-9a-fA-F]{40}", self.source_sha):
            raise ValueError("CAESURA_SOURCE_SHA must be an exact 40-character commit SHA")
        self.run("source-clone", ["git", "clone", "--filter=blob:none", "--no-checkout",
                 SOURCE_URL, self.source], cwd=self.root, timeout=1200)
        self.run("source-fetch", ["git", "fetch", "origin", self.source_sha], timeout=1200)
        self.run("source-checkout", ["git", "checkout", "--detach", self.source_sha], timeout=1200)
        if (self.source / ".gitmodules").is_file():
            self.run("source-submodules", ["git", "submodule", "update", "--init", "--recursive"], timeout=1200)
        _, actual = self.run("source-head", ["git", "rev-parse", "HEAD"])
        _, dirty = self.run("source-clean-before", ["git", "status", "--porcelain"])
        if actual != self.source_sha.lower() or dirty:
            raise RuntimeError("Source checkout is not the requested clean commit")
        self.receipt.update(source_sha=actual, clean_before=True, source_before=self.source_snapshot())
        self.save()

    def prepare(self):
        if platform.system() != "Darwin" or platform.machine() != "arm64":
            raise RuntimeError("This lane requires an Apple Silicon macOS worker")
        self.checkout()
        self.run("host", ["sw_vers"])
        self.run("xcode", ["xcodebuild", "-version"])
        self.run("sdk-inventory", ["xcodebuild", "-showsdks"])
        host_dependencies = ["cmake", "pkg-config", "python@3.12"]
        if self.name == "macos":
            host_dependencies += ["openssl@3", "sdl3"]
        # iOS SDL/OpenSSL are separate pinned target builds, never host bottles.
        self.run("brew-dependencies", ["brew", "install", *host_dependencies], timeout=1200)
        self.run("brew-versions", ["brew", "list", "--versions", *host_dependencies])
        self.run("cmake-version", ["cmake", "--version"])
        self.run("clang-version", ["clang", "--version"])
        venv = self.root / "python"
        _, python_prefix = self.run("python-prefix", ["brew", "--prefix", "python@3.12"])
        self.run("python-venv", [Path(python_prefix) / "bin/python3.12", "-m", "venv", venv])
        self.python = venv / "bin/python"
        self.run("python-dependencies", [self.python, "-m", "pip", "install", "PyYAML==6.0.2"], timeout=300)
        self.run("python-version", [self.python, "--version"])

    def macos(self):
        _, openssl = self.run("macos-openssl-prefix", ["brew", "--prefix", "openssl@3"])
        build = self.source / "build/presets/macos-foundation"
        self.run("configure", ["cmake", "--preset", "macos-foundation", "-DBX_CONFIG_DEBUG=1",
                 f"-DCMAKE_PREFIX_PATH={openssl}", f"-DPython3_EXECUTABLE={self.python}"], timeout=600)
        profile = self.source / "scripts/validation_profiles.json"
        raw = self.evidence / "native-run"
        runner_exit, _ = self.run("native-executor", [self.python, "scripts/run_validation.py",
            "--profile", profile, "--profile-name", "macos-debug", "--build-dir", build,
            "--configuration", "Debug", "--run-dir", raw], timeout=7200, allow_failure=True)
        if not (raw / "run.json").is_file():
            raise RuntimeError("Native executor did not produce a receipt")
        receipt = json.loads((raw / "run.json").read_text(encoding="utf-8"))
        bundle = self.evidence / "native-evidence" / receipt["source_sha"] / receipt["run_id"] / "macos-debug"
        collect_exit, _ = self.run("native-collector", [self.python, "scripts/collect_validation_evidence.py",
            "--profile", profile, "--profile-name", "macos-debug", "--run", raw / "run.json",
            "--output", bundle], timeout=600, allow_failure=True)
        verify_exit, _ = self.run("native-verifier", [self.python, "scripts/verify_release_candidate.py",
            "--artifacts-dir", bundle, "--profile", profile, "--profile-name", "macos-debug",
            "--expected-run", raw / "run.json", "--commit", self.source_sha,
            "--repo-root", self.source, "--check"], timeout=300, allow_failure=True)
        self.receipt["native_profile"] = {"name": "macos-debug", "runner_exit": runner_exit,
            "collector_exit": collect_exit, "verifier_exit": verify_exit,
            "receipt": "native-run/run.json", "evidence": bundle.relative_to(self.evidence).as_posix()}
        if any([runner_exit, collect_exit, verify_exit]):
            raise RuntimeError("The macOS native validation profile did not pass all gates")
        shutil.copyfile(build / "CMakeCache.txt", self.evidence / "CMakeCache.txt")

    def dependency_checkout(self, name, url, commit):
        path = self.root / name
        self.run(f"{name}-clone", ["git", "clone", "--filter=blob:none", "--no-checkout",
                 "--depth=1", url, path], timeout=900)
        self.run(f"{name}-fetch", ["git", "fetch", "--depth=1", "origin", commit], cwd=path, timeout=900)
        self.run(f"{name}-checkout", ["git", "checkout", "--detach", commit], cwd=path, timeout=900)
        _, actual = self.run(f"{name}-identity", ["git", "rev-parse", "HEAD"], cwd=path)
        if actual != commit:
            raise RuntimeError(f"{name} dependency does not match its pinned commit")
        return path

    def ios(self):
        simulator = self.name == "ios-simulator"
        sdk = "iphonesimulator" if simulator else "iphoneos"
        target_triple = f"arm64-apple-ios{IOS_DEPLOYMENT_TARGET}" + ("-simulator" if simulator else "")
        self.run("simulator-inventory", ["xcrun", "simctl", "list", "--json"], allow_failure=not simulator)
        sdl = self.dependency_checkout("sdl", "https://github.com/libsdl-org/SDL.git", SDL_SHA)
        ssl = self.dependency_checkout("openssl", "https://github.com/openssl/openssl.git", OPENSSL_SHA)
        sdl_install, ssl_install = self.root / "sdl-install", self.root / "openssl-install"
        flags = ["-G", "Xcode", "-DCMAKE_SYSTEM_NAME=iOS", f"-DCMAKE_OSX_SYSROOT={sdk}",
                 "-DCMAKE_OSX_ARCHITECTURES=arm64", f"-DCMAKE_OSX_DEPLOYMENT_TARGET={IOS_DEPLOYMENT_TARGET}",
                 "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO",
                 "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED=NO"]
        self.run("sdl-configure", ["cmake", "-S", sdl, "-B", sdl / "build", *flags,
            "-DSDL_STATIC=ON", "-DSDL_SHARED=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
            "-DSDL_DOCS=OFF", f"-DCMAKE_INSTALL_PREFIX={sdl_install}"], timeout=600)
        self.run("sdl-build", ["cmake", "--build", sdl / "build", "--config", "Debug", "--parallel", "3",
            "--", "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"], timeout=1800)
        self.run("sdl-install", ["cmake", "--install", sdl / "build", "--config", "Debug"])
        ssl_target = "iossimulator-arm64-xcrun" if simulator else "ios64-xcrun"
        self.run("openssl-configure", ["perl", "Configure", ssl_target, f"--prefix={ssl_install}",
            "--libdir=lib", f"--target={target_triple}", "no-shared", "no-tests", "no-apps", "no-docs"], cwd=ssl)
        self.run("openssl-build", ["make", "-j3"], cwd=ssl, timeout=1800)
        self.run("openssl-install", ["make", "install_sw"], cwd=ssl)
        self.run("metal-source-audit", [self.python, "scripts/verify_metal_shaders.py"])
        build = self.source / f"build/eas-{self.name}"
        self.run("configure", ["cmake", "-S", self.source, "-B", build, *flags,
            "-DCAESURA_LIVE2D=OFF", "-DCAESURA_ENABLE_FFMPEG=OFF", "-DCAESURA_HAS_STEAM=OFF",
            "-DCAESURA_REQUIRE_TEST_PREREQUISITES=ON", f"-DPython3_EXECUTABLE={self.python}",
            f"-DSDL3_DIR={sdl_install}/lib/cmake/SDL3", f"-DOPENSSL_ROOT_DIR={ssl_install}",
            "-DCMAKE_XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER=com.caesura.amekag"], timeout=900)
        self.run("build", ["cmake", "--build", build, "--config", "Debug", "--parallel", "3",
            "--", "CODE_SIGNING_ALLOWED=NO", "CODE_SIGNING_REQUIRED=NO"], timeout=2400)
        shutil.copyfile(build / "CMakeCache.txt", self.evidence / "CMakeCache.txt")
        binaries = {}
        for name in ("CaesuraAmeKAG", "CaesuraTests"):
            found = [p for p in build.rglob(name) if p.is_file() and ".dSYM" not in str(p)]
            if len(found) != 1:
                raise RuntimeError(f"Expected one newly built {name}, found {len(found)}")
            binary = found[0].resolve()
            self.identify_binary(name, binary, "IOSSIMULATOR" if simulator else "IOS")
            binaries[name] = binary
        # Store the actual .app products and the fixture trees beside the C++ binary.
        with tarfile.open(self.evidence / "native-products.tar.gz", "w:gz") as archive:
            for name, binary in binaries.items():
                archive.add(binary.parent, arcname=name)
        self.receipt["compile"] = {"status": "PASS", "sdk": sdk, "configuration": "Debug",
            "deployment_target": IOS_DEPLOYMENT_TARGET, "openssl_target_triple": target_triple,
            "code_signing_allowed": False, "sdl_sha": SDL_SHA, "openssl_sha": OPENSSL_SHA}
        self.save()
        if simulator:
            fixture_script = build / "tests/sync_caesura_test_assets_Debug.cmake"
            fixture_text = fixture_script.read_text(encoding="utf-8")
            match = re.search(r"set\(CAESURA_FIXTURE_TEST_OUTPUT \[\[(.*?)\]\]\)", fixture_text)
            if not match:
                raise RuntimeError("CMake did not declare the actual test fixture output directory")
            test_cwd = Path(match[1]).resolve()
            if not test_cwd.is_relative_to(build.resolve()) or not test_cwd.is_dir():
                raise RuntimeError("CMake test fixture directory is not inside the newly built tree")
            self.receipt["test_fixture"] = {"script": str(fixture_script),
                "script_sha256": sha256(fixture_script), "directory": str(test_cwd)}
            self.simulator_tests(binaries["CaesuraTests"], test_cwd)

    def identify_binary(self, name, binary, expected_platform):
        self.run(f"{name}-file", ["file", binary])
        _, arch = self.run(f"{name}-arch", ["xcrun", "lipo", "-archs", binary])
        _, build = self.run(f"{name}-platform", ["xcrun", "vtool", "-show-build", binary])
        if arch != "arm64" or not re.search(rf"\bplatform\s+{expected_platform}\b", build):
            raise RuntimeError(f"{name} is not an arm64 {expected_platform} binary")
        minimum = re.search(r"\bminos\s+([0-9.]+)\b", build)
        if not minimum or tuple(int(n) for n in minimum[1].split(".")[:2]) != tuple(int(n) for n in IOS_DEPLOYMENT_TARGET.split(".")):
            raise RuntimeError(f"{name} does not preserve the iOS {IOS_DEPLOYMENT_TARGET} deployment target")
        self.receipt.setdefault("binaries", {})[name] = {
            "path": str(binary), "sha256": sha256(binary), "architecture": arch,
            "platform": expected_platform, "minimum_os": minimum[1]}
        self.save()

    def simulator_tests(self, binary, test_cwd):
        _, inventory = self.run("simulator-runtimes", ["xcrun", "simctl", "list", "--json"])
        inventory = json.loads(inventory)
        runtimes = [r for r in inventory["runtimes"] if r.get("isAvailable")
                    and r["identifier"].startswith("com.apple.CoreSimulator.SimRuntime.iOS-")]
        device_types = [d for d in inventory["devicetypes"] if d.get("productFamily") == "iPhone"]
        if not runtimes or not device_types:
            raise RuntimeError("No available iOS runtime/iPhone simulator device type")
        runtime = max(runtimes, key=lambda r: tuple(int(v) for v in r["version"].split(".")))
        # Pick an existing available device's type when possible to avoid unsupported combinations.
        compatible = [d for d in inventory["devices"].get(runtime["identifier"], [])
                      if d.get("isAvailable") and d.get("deviceTypeIdentifier")
                      in {t["identifier"] for t in device_types}]
        if not compatible:
            raise RuntimeError("The selected iOS runtime has no known compatible iPhone device type")
        device_type = compatible[0]["deviceTypeIdentifier"]
        _, udid = self.run("simulator-create", ["xcrun", "simctl", "create",
            f"Caesura-U2-{uuid.uuid4().hex[:10]}", device_type, runtime["identifier"]])
        if not re.fullmatch(r"[0-9A-Fa-f-]{36}", udid):
            raise RuntimeError("simctl create did not return a device UDID")
        self.simulator = udid
        self.receipt["simulator"] = {"udid": udid, "runtime": runtime, "device_type": device_type,
                                     "test_cwd": str(test_cwd)}
        self.save()
        self.run("simulator-boot", ["xcrun", "simctl", "boot", udid])
        self.run("simulator-bootstatus", ["xcrun", "simctl", "bootstatus", udid, "-b"], timeout=600)
        self.run("simulator-spawn-help", ["xcrun", "simctl", "help", "spawn"])
        _, sdk = self.run("simulator-sdk", ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"])
        probe_source = Path(__file__).with_name("simulator_cwd_probe.c")
        probe = self.root / "simulator-cwd-probe"
        self.receipt["uploaded_cwd_probe_sha256"] = sha256(probe_source)
        self.run("cwd-probe-compile", ["xcrun", "--sdk", "iphonesimulator", "clang", "-target",
            f"arm64-apple-ios{IOS_DEPLOYMENT_TARGET}-simulator", "-isysroot", sdk, probe_source, "-o", probe])
        self.identify_binary("cwd-probe", probe, "IOSSIMULATOR")
        self.run("simulator-cwd-proof", ["xcrun", "simctl", "spawn", udid, probe, test_cwd], cwd=test_cwd)
        before = sha256(binary)
        code, text = self.run("simulator-cpp", ["xcrun", "simctl", "spawn", udid, binary, "--no-colors"],
                              cwd=test_cwd, timeout=900, allow_failure=True)
        module_spec = importlib.util.spec_from_file_location("pinned_collector",
            self.source / "scripts/collect_validation_evidence.py")
        collector = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(collector)
        # Use the existing conservative native Apple discovery floor; never lower it after a failure.
        profile_path = self.source / "scripts/validation_profiles.json"
        profile = json.loads(profile_path.read_text(encoding="utf-8-sig"))
        shutil.copyfile(profile_path, self.evidence / "simulator-discovery-profile.json")
        minimum = next(c["min_discovered"] for c in profile["profiles"]["macos-debug"]["checks"] if c["id"] == "cpp")
        counts, _ = collector.parse_doctest(text)
        passed = code == 0 and counts["discovered"] >= minimum and counts["failed"] == 0 and counts["skipped"] == 0 and sha256(binary) == before
        self.receipt["simulator_cpp"] = {"status": "PASS" if passed else "FAIL", "exit_code": code,
            "counts": counts, "minimum_discovered": minimum, "minimum_source": "macos-debug/cpp",
            "discovery_profile_sha256": sha256(profile_path),
            "binary_sha256_before": before, "binary_sha256_after": sha256(binary),
            "scope": "unfiltered iOS Simulator C++ executable; no app UI or physical device evidence"}
        self.save()
        if not passed:
            raise RuntimeError("iOS Simulator C++ suite failed its exit/count/skip/binary-identity gate")

    def finish(self, error):
        if self.simulator:
            self.run("simulator-shutdown", ["xcrun", "simctl", "shutdown", self.simulator], allow_failure=True)
            code, _ = self.run("simulator-delete", ["xcrun", "simctl", "delete", self.simulator], allow_failure=True)
            if code and error is None:
                error = RuntimeError("Failed to delete this job's simulator")
        if self.receipt.get("clean_before"):
            code, dirty = self.run("source-clean-after", ["git", "status", "--porcelain"], allow_failure=True)
            self.receipt["clean_after"] = code == 0 and not dirty
            if (code or dirty) and error is None:
                error = RuntimeError("Source tree changed during validation")
            try:
                self.receipt["source_after"] = self.source_snapshot()
                self.receipt["source_changed"] = self.receipt["source_before"] != self.receipt["source_after"]
                if self.receipt["source_changed"] and error is None:
                    error = RuntimeError("Source/fixture identity changed during validation")
            except (OSError, ValueError, subprocess.SubprocessError) as snapshot_error:
                self.receipt["source_snapshot_error"] = str(snapshot_error)
                if error is None:
                    error = snapshot_error
        # Keep configure/build diagnostics even when no runnable binary was produced.
        build = self.source / ("build/presets/macos-foundation" if self.name == "macos" else f"build/eas-{self.name}")
        for name in ("CMakeCache.txt", "CMakeFiles/CMakeConfigureLog.yaml"):
            if (build / name).is_file():
                shutil.copyfile(build / name, self.evidence / Path(name).name)
        self.receipt.update(status="FAIL" if error else "PASS", finished_at=now())
        if error:
            self.receipt["error"] = str(error)
        self.save()
        print(json.dumps({"status": self.receipt["status"], "evidence": str(self.evidence),
                          "error": str(error) if error else None}), flush=True)
        return 1 if error else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lane", choices=["macos", "ios-device", "ios-simulator"])
    args = parser.parse_args()
    lane = Lane(args.lane, os.environ.get("CAESURA_SOURCE_SHA", ""))
    error = None
    try:
        lane.prepare()
        if args.lane == "macos":
            lane.macos()
        else:
            lane.ios()
    except (Exception, KeyboardInterrupt) as caught:
        error = caught
        print(f"Native validation failed: {caught}", file=sys.stderr, flush=True)
    return lane.finish(error)


if __name__ == "__main__":
    raise SystemExit(main())
