#!/usr/bin/env python3
"""Transform one locked CPack TGZ into one explicit AppImage, without runtime claims."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys

# The owned launcher uses -I -S. Admit only this explicit maintained script
# directory, rather than inheriting the caller's Python import environment.
if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from package_verification import (_component, _expected, _new_attempt, _plain_root, _reparse,
                                  _sha256_file, inspect_inventory, prepare_package, verify_stable)
from validation_process import run_owned_command
from verify_native_package import inspect_native_package

ROOT = Path(__file__).resolve().parents[1]
CONTROLLER_PYTHON = Path(sys.executable).resolve(strict=True)
SCHEMA = "caesura.appimage-build.v1"
APPRUN = '''#!/bin/sh
# Preserve the installed root for Engine resource lookup and packaged author CLI.
APPDIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)" || exit 1
cd "$APPDIR" || exit 1
exec "$APPDIR/CaesuraAmeKAG" "$@"
'''


def _host_platform():
    return "linux" if sys.platform.startswith("linux") else sys.platform


def _save(path: Path, value: dict):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True)
        stream.write("\n")


def _path(value, *, existing=True):
    path = Path(value)
    if not path.is_absolute():
        raise ValueError(f"Explicit absolute path required: {value}")
    if not existing and os.path.lexists(path):
        raise ValueError(f"Refusing existing output: {path}")
    for parent in path.parents:
        if parent.is_symlink() or _reparse(parent):
            raise ValueError(f"Path ancestors cannot be symlinks/reparse points: {parent}")
    return _plain_root(path) if existing else path.parent.resolve(strict=True) / path.name


def _locked(path, expected):
    actual = _sha256_file(path)
    if actual != _expected(expected, "build input"):
        raise ValueError(f"Prelocked SHA256 mismatch: {path}")
    return {"path": str(path), "sha256_before": actual}


class _OutputParent:
    """Retain the originally selected directory through final publication."""

    def __init__(self, path: Path, expected_identity: tuple[int, int]):
        self.path, self.identity = path, expected_identity
        self.fd = self.handle = None
        try:
            if os.name == "nt":
                # Windows only exercises protocol fixtures; its production
                # AppImage lane remains refused. This real directory handle
                # omits SHARE_DELETE so a fixture cannot redirect the parent.
                self.handle, self.windows_identity = self._windows_open()
            else:
                if os.open not in os.supports_dir_fd or not all(hasattr(os, name) for name in ("O_DIRECTORY", "O_NOFOLLOW")):
                    raise ValueError("Cannot bind the output directory for relative exclusive publication")
                self.fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0))
            self.verify()
        except BaseException:
            self.close()
            raise

    def _windows_open(self):
        import ctypes
        from ctypes import wintypes
        class FileInformation(ctypes.Structure):
            _fields_ = [("attributes", wintypes.DWORD), ("creation", wintypes.FILETIME),
                ("access", wintypes.FILETIME), ("write", wintypes.FILETIME),
                ("volume", wintypes.DWORD), ("size_high", wintypes.DWORD),
                ("size_low", wintypes.DWORD), ("links", wintypes.DWORD),
                ("index_high", wintypes.DWORD), ("index_low", wintypes.DWORD)]
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.kernel.CreateFileW.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.GetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.POINTER(FileInformation)]
        self.kernel.GetFileInformationByHandle.restype = wintypes.BOOL
        handle = self.kernel.CreateFileW(str(self.path), 0x80, 0x3, None, 3, 0x02200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise OSError(ctypes.get_last_error(), "Cannot bind the Windows output directory")
        try:
            info = FileInformation()
            if not self.kernel.GetFileInformationByHandle(handle, ctypes.byref(info)):
                raise OSError(ctypes.get_last_error(), "Cannot identify the Windows output directory")
            if not info.attributes & 0x10 or info.attributes & 0x400:
                raise ValueError("Output parent must be a plain directory")
            return handle, (info.volume, info.index_high, info.index_low)
        except BaseException:
            self.kernel.CloseHandle(handle)
            raise

    def verify(self):
        if _path(self.path) != self.path:
            raise ValueError("Output parent no longer resolves to its original path")
        current = self.path.stat()
        if (current.st_dev, current.st_ino) != self.identity:
            raise ValueError("Output parent identity changed after selection")
        if self.fd is not None:
            bound = os.fstat(self.fd)
            if not stat.S_ISDIR(bound.st_mode) or (bound.st_dev, bound.st_ino) != self.identity:
                raise ValueError("Output directory descriptor does not match its original identity")
        else:
            handle, identity = self._windows_open()
            try:
                if identity != self.windows_identity:
                    raise ValueError("Output directory handle identity changed")
            finally:
                self.kernel.CloseHandle(handle)

    def create(self, name: str):
        _component(name)
        self.verify()
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        if self.fd is not None:
            return os.open(name, flags | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)
        return os.open(self.path / name, flags | getattr(os, "O_BINARY", 0), 0o600)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        if self.handle is not None:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def _architecture(engine: Path):
    with engine.open("rb") as stream:
        header = stream.read(64)
    if len(header) < 64 or header[:4] != b"\x7fELF" or header[4] not in (1, 2) or header[5] not in (1, 2):
        raise ValueError("AppImage input Engine needs an ELF architecture header")
    machine = struct.unpack_from("<H" if header[5] == 1 else ">H", header, 18)[0]
    architecture = {62: "x86_64", 183: "aarch64", 3: "i686", 40: "armhf"}.get(machine)
    if architecture is None:
        raise ValueError(f"Unsupported AppImage Engine ELF machine: {machine}")
    return architecture


def _add_file(root: Path, relative: str, data: bytes, mode: int):
    target = root / relative
    for parent in target.parents:
        if parent == root.parent:
            break
        if parent.exists() and (parent.is_symlink() or _reparse(parent) or not parent.is_dir()):
            raise ValueError(f"AppDir addition has an unsafe parent: {relative}")
    if os.path.lexists(target):
        raise ValueError(f"AppDir addition would overwrite an installed path: {relative}")
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("xb") as stream:
        stream.write(data)
    target.chmod(mode)


def _tool_launcher(request_path: Path):
    request = json.loads(request_path.read_text(encoding="utf-8"))
    result = {"status": "LAUNCH_FAILED", "actual_exit_code": None, "pid": None}
    process = None
    try:
        if _sha256_file(Path(request["argv"][0])) != request["tool_sha256"]:
            raise ValueError("Tool changed before launch")
        process = subprocess.Popen(request["argv"], cwd=request["cwd"], env=request["env"],
            stdin=subprocess.DEVNULL, shell=False,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        result["pid"] = process.pid
        result["actual_exit_code"] = process.wait()
        result["status"] = "EXITED"
        return 0
    except (Exception, KeyboardInterrupt) as error:
        result["error"] = f"{type(error).__name__}: {error}"
        return 125
    finally:
        # Outer owned runner also owns a self-extracting tool's exec/children.
        if process is not None:
            if process.poll() is None:
                process.kill()
            result["actual_exit_code"] = process.wait()
        _save(Path(request["result_path"]), result)


def _invoke_tool(work: Path, tool: Path, tool_sha256: str, argv: list[str], environment: dict,
                 timeout: float) -> dict:
    command = work / "command"
    command.mkdir(mode=0o700)
    request_path, result_path = command / "request.json", command / "result.json"
    _save(request_path, {"argv": argv, "tool_sha256": tool_sha256, "cwd": str(work / "cwd"),
                         "env": environment, "result_path": str(result_path)})
    report = {"argv": argv, "status": "FAILED", "owned_tree_cleanup": "NOT_COMPLETED",
              "stdout_path": str(command / "stdout.log"), "stderr_path": str(command / "stderr.log")}
    try:
        with (command / "stdout.log").open("xb") as out, (command / "stderr.log").open("xb") as err:
            report["launcher_exit_code"] = run_owned_command(
                [str(CONTROLLER_PYTHON), "-I", "-S", str(Path(__file__).resolve()),
                 "--tool-launcher", str(request_path)], work / "cwd", out, err, timeout)
        report["owned_tree_cleanup"] = "COMPLETE"
        result = json.loads(result_path.read_text(encoding="utf-8"))
        report["tool_result"] = result
        if report["launcher_exit_code"] != 0 or result.get("status") != "EXITED" or result.get("actual_exit_code") != 0:
            raise ValueError("appimagetool did not exit successfully")
        report["status"] = "EXITED"
    except subprocess.TimeoutExpired as error:
        report.update(status="TIMED_OUT", owned_tree_cleanup="COMPLETE", error=str(error))
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = f"{type(error).__name__}: {error}"
    _save(command / "run.json", report)
    return report


def _stable(report):
    for item in report["inputs"].values():
        item["sha256_after"] = _sha256_file(Path(item["path"]))
        if item["sha256_after"] != item["sha256_before"]:
            raise ValueError(f"AppImage build input/tool changed: {item['path']}")
    if "source_preparation" in report:
        verify_stable(report["source_preparation"])
    if "appdir_inventory" in report and inspect_inventory(report["appdir"]) != report["appdir_inventory"]:
        raise ValueError("AppDir changed during appimagetool execution")
    work = Path(report["work_dir"])
    for relative, digest in report["evidence"].items():
        if _sha256_file(work / relative) != digest:
            raise ValueError(f"Completed build-stage evidence changed: {relative}")


def _evidence(report, paths):
    work = Path(report["work_dir"])
    for path in paths:
        if path.is_symlink() or _reparse(path):
            raise ValueError("Build evidence cannot be a symlink/reparse point")
        if path.is_dir():
            continue
        relative = path.relative_to(work).as_posix()
        digest = _sha256_file(path)
        if relative in report["evidence"] and report["evidence"][relative] != digest:
            raise ValueError(f"Completed build-stage evidence changed: {relative}")
        report["evidence"][relative] = digest


def build_appimage(*, tgz_path, expected_sha256, requirements_path, requirements_sha256,
                   appimagetool_path, appimagetool_sha256, runtime_file_path, runtime_sha256,
                   work_dir, output_path,
                   timeout_seconds=300) -> dict:
    # Reused/unsafe destinations are refused before allocating any work.
    output = _path(output_path, existing=False)
    selected_parent = output.parent.stat()
    parent_identity = (selected_parent.st_dev, selected_parent.st_ino)
    work_given = Path(work_dir).absolute()
    for parent in work_given.parents:
        if os.path.lexists(parent / ".git"):
            raise ValueError("AppImage work must be outside every repository")
    _, work = _new_attempt(Path(tgz_path), work_given)
    report = {"schema": SCHEMA, "status": "APPIMAGE_BUILD_FAIL", "accepted": False,
              "runtime": "NOT_RUN", "started_at": datetime.now(timezone.utc).isoformat(),
              "work_dir": str(work), "requested_output": str(output), "inputs": {},
              "evidence": {}, "errors": [], "host_platform": _host_platform(),
              "cleanup": "OWNED_WORK_RETAINED"}
    output_parent = None
    try:
        if _host_platform() != "linux":
            raise ValueError("Actual Linux host required for AppImage construction; NOT_RUN")
        if isinstance(timeout_seconds, bool) or not math.isfinite(timeout_seconds) or timeout_seconds <= 0:
            raise ValueError("Tool timeout must be positive and finite")
        output_parent = _OutputParent(output.parent, parent_identity)
        report["output_parent"] = {"path": str(output.parent), "device": parent_identity[0],
            "inode": parent_identity[1], "binding": "directory_fd" if output_parent.fd is not None else "windows_directory_handle"}
        for name, path, digest in (("tgz", tgz_path, expected_sha256),
                ("requirements", requirements_path, requirements_sha256),
                ("appimagetool", appimagetool_path, appimagetool_sha256),
                ("runtime", runtime_file_path, runtime_sha256)):
            selected = _path(path)
            if selected.is_relative_to(work) or selected == output:
                raise ValueError("Build input/tool must stay outside the new work and output")
            report["inputs"][name] = _locked(selected, digest)
        paths = [entry["path"] for entry in report["inputs"].values()]
        if len(set(paths)) != len(paths):
            raise ValueError("Archive, external requirements, appimagetool and runtime must be distinct inputs")
        metadata = json.loads(Path(report["inputs"]["requirements"]["path"]).read_text(encoding="utf-8-sig"))
        if metadata.get("schema") != "caesura.package-build.v1" or metadata.get("platform") != "linux":
            raise ValueError("Expected external Linux build requirements")
        if metadata.get("configuration") not in {"Release", "Debug", "RelWithDebInfo", "MinSizeRel"}:
            raise ValueError("Build requirements need an explicit configuration")
        if metadata.get("engine_relative_path") != "CaesuraAmeKAG" or metadata.get("lua_relative_path") != "external/lua/lua":
            raise ValueError("AppImage preserves root Engine and authoritative packaged Lua")
        stem = metadata.get("archive_basename")
        if not isinstance(stem, str):
            raise ValueError("External requirements need an exact archive_basename")
        _component(stem)
        artifacts = metadata.get("artifacts", {})
        if artifacts.get("tgz") != stem + ".tar.gz" or artifacts.get("appimage") != stem + ".AppImage":
            raise ValueError("Required artifact names do not match archive_basename")
        tgz = Path(report["inputs"]["tgz"]["path"])
        if tgz.name != artifacts["tgz"] or output.name != artifacts["appimage"]:
            raise ValueError("Explicit TGZ/output names differ from external build requirements")
        report["requirements"] = metadata
        source = prepare_package(tgz, work / "source", expected_sha256=expected_sha256)
        report["source_preparation"] = source
        _evidence(report, [work / "source/preparation.json"])
        package = Path(source["package_path"]) / stem
        if not package.is_dir() or package.is_symlink() or _reparse(package):
            raise ValueError("Exact required archive root is missing or not a plain directory")
        if any(item["path"].split("/", 1)[0] != stem for item in source["inventory"]["entries"]):
            raise ValueError("TGZ has entries outside its explicitly required archive root")
        static = inspect_native_package(package, "linux", metadata.get("required_configuration"))
        _save(work / "native-static.json", static)
        _evidence(report, [work / "native-static.json"])
        if not static.get("passed"):
            raise ValueError("Input native static checks failed: " + "; ".join(static.get("errors", [])))
        payload = prepare_package(package, work / "appdir", expected_inventory_sha256=inspect_inventory(package)["sha256"])
        report["appdir_preparation"] = payload
        _evidence(report, [work / "appdir/preparation.json"])
        appdir = Path(payload["package_path"])
        report["appdir"] = str(appdir)
        architecture = _architecture(appdir / "CaesuraAmeKAG")
        report["architecture"] = architecture
        for name in ("caesura-amekag.desktop", "caesura-amekag.png"):
            asset = ROOT / "tools/appimage" / name
            report["inputs"][name] = {"path": str(asset), "sha256_before": _sha256_file(asset)}
        desktop = (ROOT / "tools/appimage/caesura-amekag.desktop").read_bytes()
        icon = (ROOT / "tools/appimage/caesura-amekag.png").read_bytes()
        _add_file(appdir, "AppRun", APPRUN.encode("utf-8"), 0o755)
        for relative, data in (("caesura-amekag.desktop", desktop),
                ("usr/share/applications/caesura-amekag.desktop", desktop),
                ("caesura-amekag.png", icon),
                (".DirIcon", icon),
                ("usr/share/icons/hicolor/256x256/apps/caesura-amekag.png", icon)):
            _add_file(appdir, relative, data, 0o644)
        report["appdir_inventory"] = inspect_inventory(appdir)
        for name in ("home", "temp", "cwd"):
            (work / name).mkdir(mode=0o700)
        tool = Path(report["inputs"]["appimagetool"]["path"])
        locked_tool = work / "locked-appimagetool"
        shutil.copyfile(tool, locked_tool)
        shutil.copymode(tool, locked_tool)
        report["inputs"]["locked_tool"] = _locked(locked_tool, appimagetool_sha256)
        if not os.access(locked_tool, os.X_OK):
            raise ValueError("Locked appimagetool is not executable")
        runtime = Path(report["inputs"]["runtime"]["path"])
        locked_runtime = work / "locked-runtime"
        shutil.copyfile(runtime, locked_runtime)
        report["inputs"]["locked_runtime"] = _locked(locked_runtime, runtime_sha256)
        with locked_runtime.open("rb") as stream:
            runtime_header = stream.read(11)
        if runtime_header[8:11] != b"AI\x02" or _architecture(locked_runtime) != architecture:
            raise ValueError("Explicit type-2 runtime architecture differs from Engine")
        for path in (Path(__file__).resolve(), CONTROLLER_PYTHON, ROOT / "scripts/validation_process.py"):
            report["inputs"][str(path)] = {"path": str(path), "sha256_before": _sha256_file(path)}
        environment = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "HOME": str(work / "home"),
            "TMP": str(work / "temp"), "TEMP": str(work / "temp"), "TMPDIR": str(work / "temp"),
            "XDG_CACHE_HOME": str(work / "home/.cache"), "XDG_CONFIG_HOME": str(work / "home/.config"),
            "LC_ALL": "C", "LANG": "C", "ARCH": architecture, "APPIMAGE_EXTRACT_AND_RUN": "1"}
        generated = work / "generated.AppImage"
        _stable(report)
        tool_run = _invoke_tool(work, locked_tool, appimagetool_sha256.lower(),
                               [str(locked_tool), str(appdir), str(generated),
                                "--runtime-file", str(locked_runtime)], environment, timeout_seconds)
        report["tool_run"] = tool_run
        _evidence(report, (work / "command").rglob("*"))
        if tool_run["status"] != "EXITED":
            raise ValueError("appimagetool failed; retained work is not published")
        generated_digest = _sha256_file(generated)
        with generated.open("rb") as stream:
            header = stream.read(11)
        if header[:4] != b"\x7fELF" or header[8:11] != b"AI\x02":
            raise ValueError("Generated output lacks supported ELF/type-2 AppImage header")
        if _architecture(generated) != architecture or not os.access(generated, os.X_OK):
            raise ValueError("Generated AppImage architecture/execute permission differs from Engine")
        report["generated"] = {"path": str(generated), "sha256": generated_digest}
        _stable(report)
        # Exclusive creation refuses another producer; never overwrite/delete.
        with generated.open("rb") as source_stream, os.fdopen(output_parent.create(output.name), "wb") as target:
            shutil.copyfileobj(source_stream, target)
            target.flush()
            if os.name != "nt":
                os.fchmod(target.fileno(), stat.S_IMODE(generated.stat().st_mode) & 0o777)
            os.fsync(target.fileno())
        output_parent.verify()
        output_digest = _sha256_file(output)
        report["output"] = {"path": str(output), "sha256": output_digest}
        if output_digest != generated_digest or _sha256_file(generated) != generated_digest:
            raise ValueError("Output changed while publishing transformed bytes")
        _stable(report)
        report["status"] = "APPIMAGE_BUILT"
    except (Exception, KeyboardInterrupt) as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        try:
            _stable(report)
            if output_parent is not None:
                output_parent.verify()
            if "output" in report and _sha256_file(Path(report["output"]["path"])) != report["output"]["sha256"]:
                raise ValueError("Final AppImage changed before build receipt")
        except (Exception, KeyboardInterrupt) as error:
            report["errors"].append(f"Final stability: {type(error).__name__}: {error}")
        if report["errors"]:
            report["status"] = "APPIMAGE_BUILD_FAIL"
        report["finished_at"] = datetime.now(timezone.utc).isoformat()
        try:
            _save(work / "appimage-build.json", report)
        finally:
            if output_parent is not None:
                output_parent.close()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("tgz", "sha256", "requirements", "requirements-sha256", "appimagetool",
                   "appimagetool-sha256", "runtime-file", "runtime-sha256", "work", "output"):
        parser.add_argument("--" + option, required=True)
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    try:
        report = build_appimage(tgz_path=args.tgz, expected_sha256=args.sha256,
            requirements_path=args.requirements, requirements_sha256=args.requirements_sha256,
            appimagetool_path=args.appimagetool, appimagetool_sha256=args.appimagetool_sha256,
            runtime_file_path=args.runtime_file, runtime_sha256=args.runtime_sha256,
            work_dir=args.work, output_path=args.output, timeout_seconds=args.timeout)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0 if report["status"] == "APPIMAGE_BUILT" else 1
    except Exception as error:
        print(f"AppImage build refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--tool-launcher":
        raise SystemExit(_tool_launcher(Path(sys.argv[2])))
    raise SystemExit(main())
