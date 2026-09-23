#!/usr/bin/env python3
"""Read-only native installation contract; no binary or package code is run.

The maintained source beside this tool supplies required Lua/CLI/template bytes.
An external caller locks platform and effective SDK/linkage requirements. Package
manifests cannot lower those requirements. Mac packages additionally require
bounded Mach-O load-command framing and closed package/system dependency paths.
Neither those paths nor header signatures prove executable validity, ABI,
actual library loading, signing, or runtime behavior.
"""
from __future__ import annotations

import argparse
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
from urllib.parse import unquote, urlsplit

from package_verification import inspect_inventory, PackageVerificationError
from macho_dependencies import inspect_package_closure

SOURCE = Path(__file__).resolve().parents[1]
TEMPLATES = ("basic", "blank", "kag3", "live2d", "showcase")
FFMPEG_DLLS = ("avcodec-62.dll", "avformat-62.dll", "avutil-60.dll",
               "swscale-9.dll", "swresample-6.dll")
CONFIG_KEYS = {"schema", "sdl_linkage", "sdl_libraries", "ffmpeg", "steam", "live2d",
               "ffmpeg_linkage", "ffmpeg_libraries", "steam_linkage", "steam_libraries",
               "openssl_linkage", "openssl_libraries"}


def _digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _relative(value: str) -> str:
    if (not isinstance(value, str) or not value or "\\" in value or ":" in value
            or any(ord(c) < 32 for c in value) or value.startswith("/")):
        raise ValueError(f"Expected a package-relative path: {value!r}")
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError(f"Expected a canonical package-relative path: {value!r}")
    return value


def _libraries(value, name: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ValueError(f"{name} must be an explicit list of relative paths")
    paths = [_relative(item) for item in value]
    if len(set(paths)) != len(paths):
        raise ValueError(f"{name} contains duplicate paths")
    return paths


def _configuration(platform: str, required) -> tuple[dict, list[str]]:
    if platform not in ("windows", "linux", "macos"):
        raise ValueError(f"Unsupported native platform: {platform!r}")
    if not isinstance(required, dict):
        raise ValueError("An external required configuration object is mandatory")
    # Round-trip into a private snapshot: no shared mutable caller state.
    required = json.loads(json.dumps(required, allow_nan=False))
    if set(required) - CONFIG_KEYS:
        raise ValueError(f"Unknown required configuration keys: {sorted(set(required) - CONFIG_KEYS)}")
    mandatory = {"schema", "sdl_linkage", "sdl_libraries", "ffmpeg", "steam", "live2d"}
    if mandatory - set(required):
        raise ValueError(f"Missing required configuration keys: {sorted(mandatory - set(required))}")
    if type(required["schema"]) is not int or required["schema"] != 1:
        raise ValueError("Required configuration schema must be integer 1")
    for feature in ("ffmpeg", "steam", "live2d"):
        if type(required[feature]) is not bool:
            raise ValueError(f"{feature} must be an explicit boolean")
    result = []
    for feature in ("sdl", "ffmpeg", "steam"):
        enabled = feature == "sdl" or required[feature]
        linkage = required.get(feature + "_linkage")
        libraries = required.get(feature + "_libraries")
        if not enabled:
            if linkage not in (None, "disabled") or libraries not in (None, []):
                raise ValueError(f"Disabled {feature} cannot declare runtime linkage/libraries")
            continue
        fixed = None
        if platform == "windows" and feature in ("ffmpeg", "steam"):
            fixed = list(FFMPEG_DLLS) if feature == "ffmpeg" else ["steam_api64.dll"]
            if linkage not in (None, "shared") or libraries not in (None, fixed):
                raise ValueError(f"Windows {feature} requires its current installed DLL set: {fixed}")
            result.extend(fixed)
            continue
        if linkage not in ("shared", "static"):
            raise ValueError(f"Enabled {feature} requires explicit static/shared {feature}_linkage")
        paths = _libraries(libraries, feature + "_libraries") if libraries is not None else []
        if linkage == "shared" and not paths:
            raise ValueError(f"Shared {feature} requires exact {feature}_libraries paths")
        if linkage == "static" and paths:
            raise ValueError(f"Static {feature} must not declare dynamic libraries")
        result.extend(paths)
    # Schema-1 legacy callers may omit both fields. Keep that absence exactly:
    # the actual Mach-O closure is still checked, and absence never means static.
    openssl_keys = {"openssl_linkage", "openssl_libraries"} & set(required)
    if openssl_keys:
        if platform != "macos" or len(openssl_keys) != 2:
            raise ValueError("OpenSSL linkage/libraries must be declared together for macOS")
        linkage = required["openssl_linkage"]
        if linkage not in ("static", "shared"):
            raise ValueError("OpenSSL requires explicit static/shared linkage")
        paths = _libraries(required["openssl_libraries"], "openssl_libraries")
        if any(not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", path) for path in paths):
            raise ValueError("OpenSSL runtime libraries must be plain filenames")
        if linkage == "static" and paths:
            raise ValueError("Static OpenSSL must not declare dynamic libraries")
        if linkage == "shared" and (len(paths) != 2 or len({p.casefold() for p in paths}) != 2):
            raise ValueError("Shared OpenSSL requires two distinct SSL/Crypto runtime filenames")
        result.extend(paths)
    return required, list(dict.fromkeys(result))


class _HtmlReferences(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.references = []
        self.has_base_href = False

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "base" and "href" in attrs:
            self.has_base_href = True
        if tag == "script" and attrs.get("src"):
            self.references.append(attrs["src"])
        if tag == "link" and attrs.get("href") and set(attrs.get("rel", "").split()) & {
                "stylesheet", "modulepreload", "preload", "icon"}:
            self.references.append(attrs["href"])


def _header(path: Path, platform: str) -> str:
    with path.open("rb") as stream:
        head = stream.read(64)
        if platform == "windows":
            if len(head) != 64 or head[:2] != b"MZ":
                raise ValueError("missing DOS/PE signature")
            offset = struct.unpack_from("<I", head, 60)[0]
            if offset < 64 or offset > path.stat().st_size - 24:
                raise ValueError("invalid PE signature offset")
            stream.seek(offset)
            if stream.read(4) != b"PE\0\0":
                raise ValueError("missing PE signature")
            return "PE"
        if platform == "linux":
            if (len(head) < 52 or head[:4] != b"\x7fELF" or head[4] not in (1, 2)
                    or head[5] not in (1, 2) or head[6] != 1
                    or (head[4] == 2 and len(head) < 64)):
                raise ValueError("missing or truncated ELF signature/identification")
            return "ELF"
        if len(head) < 28 or head[:4] not in (
                b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
            raise ValueError("missing or truncated Mach-O signature")
        return "Mach-O"


def _trusted_paths() -> list[str]:
    # CMake installs scripts/ and tools/project_templates/. Only runtime Lua and
    # the actual Python author CLI are mandatory here, not validation utilities.
    paths = [path.relative_to(SOURCE).as_posix() for path in (SOURCE / "scripts").rglob("*.lua")
             if "validation" not in path.relative_to(SOURCE / "scripts").parts]
    paths += ["scripts/caesura.py", "scripts/caesura_build.py", "demo/entry.lua",
              "demo/cjk_smoke.ks", "assets/fonts/NotoSansCJKsc-Regular.otf",
              "web-editor/dist/index.html", "tools/project_templates/manifest.json"]
    for template in TEMPLATES:
        paths += [f"tools/project_templates/{template}/{name}"
                  for name in ("caesura.project.json", "entry.lua", "story.ks")]
    return sorted(set(paths))


def inspect_native_package(package_root: str | Path, platform: str, required: dict | None) -> dict:
    """Inspect an explicit prepared root using caller-owned effective config.

    The API accepts a caller-owned dict; CLI additionally proves its config file
    is outside the package. This function does not authenticate the caller or
    turn a package-provided dict into trusted build configuration.
    """
    report = {"schema": "caesura.native-package-static.v1", "status": "STATIC_ONLY",
              "passed": False, "runtime": "NOT_RUN", "abi": "NOT_VERIFIED",
              "input_stable": False, "platform": platform, "binaries": {},
              "runtime_libraries": [], "required_files": [], "errors": [],
              "limitations": ["Binary signatures do not prove executable validity or ABI.",
                              "No dynamic loading, signing, author CLI, GPU, or editor runtime executed.",
                              "Editor HTML references checked; JavaScript execution/import graph is a runtime gate."]}
    errors = report["errors"]
    try:
        required, libraries = _configuration(platform, required)
        report["required_configuration"] = required
        package = Path(package_root).absolute()
        before = inspect_inventory(package)  # Reject root alias, traversal, bad links and collisions.
        package = package.resolve(strict=True)
        report["package_root"] = str(package)
        report["inventory_before_sha256"] = before["sha256"]
        records = {item["path"]: item for item in before["entries"]}

        def file(relative, *, binary=False):
            relative = _relative(relative)
            path = package / relative
            try:
                resolved = path.resolve(strict=True)
                if not resolved.is_relative_to(package) or not resolved.is_file():
                    raise ValueError("required file must resolve to a regular file inside the package")
                # Exact logical spelling comes from the portable inventory,
                # independent of host filesystem case insensitivity.
                current = PurePosixPath()
                for part in PurePosixPath(relative).parts:
                    current /= part
                    candidate = package / str(current)
                    parent = candidate.parent.resolve(strict=True)
                    logical = (parent / part).relative_to(package).as_posix()
                    if logical not in records:
                        raise ValueError("missing exact package path spelling")
                actual = records[resolved.relative_to(package).as_posix()]
                if actual["type"] != "file" or actual["size"] == 0:
                    raise ValueError("required file is empty or not regular")
                digest = _digest(resolved)
                if digest != actual["sha256"]:
                    raise ValueError("file changed after initial inventory")
                identity = {"relative_path": relative, "logical_path": str(path),
                            "resolved_path": str(resolved), "sha256": digest, "size": actual["size"]}
                if binary:
                    identity.update(header_format=_header(resolved, platform),
                                    executable_validity="NOT_VERIFIED")
                return identity
            except (OSError, ValueError, KeyError, RuntimeError) as error:
                errors.append(f"{relative}: {error}")
                return None

        for name, relative in (("engine", "CaesuraAmeKAG.exe" if platform == "windows" else "CaesuraAmeKAG"),
                               ("lua", "external/lua/lua.exe" if platform == "windows" else "external/lua/lua")):
            identity = file(relative, binary=True)
            if identity:
                report["binaries"][name] = identity
                if os.name != "nt" and platform != "windows" and not os.access(identity["resolved_path"], os.X_OK):
                    errors.append(f"{relative}: missing executable permission on this POSIX host")
        report["executable_permissions"] = "OBSERVED_ON_POSIX_HOST" if os.name != "nt" else "NOT_VERIFIED_ON_WINDOWS_HOST"
        for relative in libraries:
            identity = file(relative, binary=True)
            if identity:
                report["runtime_libraries"].append(identity)
        if platform == "macos":
            try:
                report["macho_dependency_closure"] = inspect_package_closure(
                    package, [package / "CaesuraAmeKAG", package / "external/lua/lua"],
                    [package / relative for relative in libraries])
            except (OSError, ValueError, RuntimeError) as error:
                report["macho_dependency_closure"] = {"status":"NOT_VERIFIED", "error":str(error)}
                errors.append(f"Mach-O dependency closure: {error}")
        source_before = {}
        for relative in _trusted_paths():
            source_before[relative] = _digest(SOURCE / relative)
            identity = file(relative)
            if identity:
                report["required_files"].append(identity)
                if identity["sha256"] != source_before[relative]:
                    errors.append(f"{relative}: differs from trusted installed source")
        report["trusted_source_files"] = source_before
        for relative in ("scripts", "assets", "demo", "projects", "tools/project_templates"):
            if records.get(relative, {}).get("type") != "directory":
                errors.append(f"{relative}: required installation directory missing")
        for template in TEMPLATES:
            relative = f"tools/project_templates/{template}/caesura.project.json"
            identity = file(relative)
            if not identity:
                continue
            try:
                value = json.loads(Path(identity["resolved_path"]).read_text(encoding="utf-8-sig"))
                if not isinstance(value, dict) or value.get("template") != template or not value.get("name"):
                    raise ValueError("manifest needs nonempty name and matching template")
            except (ValueError, OSError) as error:
                errors.append(f"{relative}: {error}")
        identity = file("editor/dist/index.html")
        if identity:
            parser = _HtmlReferences()
            parser.feed(Path(identity["resolved_path"]).read_text(encoding="utf-8-sig"))
            if parser.has_base_href:
                errors.append("editor/dist/index.html: base href cannot override packaged resource URLs")
            references = []
            for reference in parser.references:
                parsed = urlsplit(reference)
                try:
                    if parsed.scheme or parsed.netloc or parsed.path.startswith("/"):
                        raise ValueError("editor assets must be relative to its packaged dist")
                    relative = unquote(parsed.path)
                    if relative.startswith("./"):
                        relative = relative[2:]
                    references.append(_relative(relative))
                    file("editor/dist/" + relative)
                except ValueError as error:
                    errors.append(f"editor/dist/index.html reference {reference!r}: {error}")
            if not any(item.startswith("assets/") and item.endswith(".js") for item in references):
                errors.append("editor/dist/index.html: no referenced assets/*.js chunk")
        after = inspect_inventory(package)
        report["inventory_after_sha256"] = after["sha256"]
        report["input_stable"] = before["sha256"] == after["sha256"]
        if not report["input_stable"]:
            errors.append("Package input changed during static inspection")
        if _trusted_paths() != sorted(source_before) or any(_digest(SOURCE / path) != digest for path, digest in source_before.items()):
            errors.append("Trusted installed source changed during static inspection")
    except (OSError, ValueError, TypeError, RuntimeError, PackageVerificationError) as error:
        errors.append(str(error))
    report["passed"] = not errors
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("windows", "linux", "macos"))
    parser.add_argument("--required-config", required=True, type=Path)
    args = parser.parse_args()
    try:
        config_path = args.required_config.resolve(strict=True)
        package = args.package_root.resolve(strict=True)
        if config_path.is_relative_to(package):
            raise ValueError("Required configuration must be outside the package")
        config_bytes = config_path.read_bytes()
        required = json.loads(config_bytes.decode("utf-8-sig"))
        report = inspect_native_package(args.package_root, args.platform, required)
        report["configuration_file"] = {"path": str(config_path), "sha256": hashlib.sha256(config_bytes).hexdigest()}
        if config_path.read_bytes() != config_bytes or args.required_config.resolve(strict=True) != config_path:
            report["errors"].append("External configuration changed during inspection")
            report["passed"] = False
    except (OSError, ValueError, RuntimeError) as error:
        report = {"schema": "caesura.native-package-static.v1", "status": "STATIC_ONLY",
                  "passed": False, "runtime": "NOT_RUN", "abi": "NOT_VERIFIED", "errors": [str(error)]}
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
