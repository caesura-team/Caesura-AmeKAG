#!/usr/bin/env python3
"""Inspect one explicit final Web package's static contracts; never run its game.

The Lua executable and this verifier's compiler/collector are declared host
validation tools. Package modules are not executed. Resource lookup is strictly
inside package_root; loaded validator modules must also exist there with matching
bytes. STATIC_PASS is limited to these static checks, with runtime=NOT_RUN.
Archive authentication, runtime environment, browsers, PWA/offline behavior,
nonliteral JS requests and dynamic author media belong to the caller's lanes.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
from html.parser import HTMLParser
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
from urllib.parse import unquote_to_bytes, urlsplit

from package_verification import inspect_inventory, PackageVerificationError

SCRIPT_ROOT = Path(__file__).resolve().parent
LUA_INSPECTOR = SCRIPT_ROOT / "validation/inspect_web_bundle.lua"
SCHEMA = "caesura.web-package-static.v1"
FONT = "assets/fonts/NotoSansCJKsc-Regular.otf"
MAX_TEXT_BYTES = 32 * 1024 * 1024


class _Invalid(RuntimeError):
    pass


def _need(condition, message):
    if not condition:
        raise _Invalid(message)


def _hash(path: Path) -> str:
    before = path.stat()
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    after = path.stat()
    fields = ("st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns")
    _need(all(getattr(before, k) == getattr(after, k) for k in fields), f"Input changed: {path}")
    return digest


def _source_snapshot(path: Path) -> tuple[str, str]:
    before = path.stat()
    _need(before.st_size <= MAX_TEXT_BYTES, f"Static text exceeds size limit: {path.name}")
    data = path.read_bytes()
    after = path.stat()
    fields = ("st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns")
    _need(len(data) <= MAX_TEXT_BYTES and all(getattr(before, k) == getattr(after, k) for k in fields),
          f"Input changed while reading static text: {path}")
    # The digest belongs to the exact bytes decoded here, not a second read of
    # a possibly newer revision. Lua loadfile's BOM handling is preserved.
    return data.decode("utf-8-sig"), hashlib.sha256(data).hexdigest()


def _text(path: Path) -> str:
    return _source_snapshot(path)[0]


def _relative(value: str) -> str:
    _need(isinstance(value, str) and value and not value.startswith("/")
          and "\\" not in value and ":" not in value
          and not any(ord(c) < 32 or ord(c) == 127 for c in value),
          f"Unsafe package path: {value!r}")
    _need(all(p not in ("", ".", "..") for p in value.split("/")), f"Unsafe package path: {value!r}")
    return value


def _file(root: Path, relative: str) -> Path:
    relative = _relative(relative)
    path = root / relative
    resolved = path.resolve(strict=False)
    _need(root in resolved.parents, f"Resource escapes package: {relative}")
    _need(path.is_file(), f"Missing required package file: {relative}")
    # Windows file lookup folds case; deployed HTTP paths may not. Verify every
    # logical component against directory entries, including relative symlinks.
    current = root
    for part in PurePosixPath(relative).parts:
        with os.scandir(current) as entries:
            exact = any(entry.name == part for entry in entries)
        _need(exact, f"Missing exact package path spelling: {relative}")
        current /= part
    return path


def _url_path(reference: str, origin: str, *, allow_root: bool = False) -> str | None:
    reference = reference.strip()
    if reference.startswith("#") or reference.startswith("data:"):
        return None
    url = urlsplit(reference)
    _need(not url.scheme and not url.netloc and not reference.startswith(("/", "\\")),
          f"Nonlocal or root-relative reference in {origin}: {reference}")
    decoded = unquote_to_bytes(url.path).decode("utf-8")
    _need(decoded and "\\" not in decoded and ":" not in decoded and not decoded.startswith("/"),
          f"Unsafe reference in {origin}: {reference}")
    parts = list(PurePosixPath(origin).parent.parts)
    for part in decoded.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            _need(parts, f"Reference escapes package in {origin}: {reference}")
            parts.pop()
        else:
            parts.append(part)
    if not parts and allow_root: return ""
    return _relative("/".join(parts))


def _quoted(text: str, offset: int) -> tuple[str, int, bool]:
    quote, result, cursor, dynamic = text[offset], [], offset + 1, False
    escapes = {"n": "\n", "r": "\r", "t": "\t", "b": "\b", "f": "\f", "v": "\v", "0": "\0"}
    while cursor < len(text):
        char = text[cursor]
        if char == quote:
            value = "".join(result)
            # Join valid surrogate-pair escapes without permitting bad UTF-8 paths.
            return value.encode("utf-16", "surrogatepass").decode("utf-16"), cursor + 1, dynamic
        if quote == "`" and text.startswith("${", cursor):
            dynamic = True
        if char != "\\":
            result.append(char)
            cursor += 1
            continue
        cursor += 1
        _need(cursor < len(text), "Unterminated JavaScript string")
        char = text[cursor]
        if char in "xu":
            length = 2 if char == "x" else 4
            if char == "u" and text[cursor+1:cursor+2] == "{":
                end = text.find("}", cursor+2)
                _need(end != -1, "Invalid JavaScript Unicode escape")
                result.append(chr(int(text[cursor+2:end], 16)))
                cursor = end + 1
            else:
                result.append(chr(int(text[cursor+1:cursor+1+length], 16)))
                cursor += length + 1
        elif char in "\r\n":
            cursor += 2 if text.startswith("\r\n", cursor) else 1
        else:
            result.append(escapes.get(char, char))
            cursor += 1
    raise _Invalid("Unterminated JavaScript string")


def _js_tokens(text: str) -> list[tuple[str, str]]:
    """Tokenize strings/comments enough to inspect static URL/import syntax.

    This is deliberately not JS evaluation or a claim about arbitrary request
    expressions. Template expressions are explicitly reported as unproved.
    """
    tokens, cursor = [], 0
    regex_prefix = {"(", "=", ":", ",", "[", "!", "?", "{", ";", "return", "=>", "&&", "||"}
    while cursor < len(text):
        char = text[cursor]
        if char.isspace():
            cursor += 1
        elif text.startswith("//", cursor):
            end = text.find("\n", cursor+2)
            cursor = len(text) if end < 0 else end + 1
        elif text.startswith("/*", cursor):
            end = text.find("*/", cursor+2)
            _need(end >= 0, "Unterminated JavaScript comment")
            cursor = end + 2
        elif char in "\"'`":
            value, cursor, dynamic = _quoted(text, cursor)
            tokens.append(("template" if dynamic else "string", value))
        elif char == "/" and (not tokens or tokens[-1][1] in regex_prefix):
            cursor += 1
            bracket = False
            while cursor < len(text):
                if text[cursor] == "\\":
                    cursor += 2
                    continue
                if text[cursor] == "[": bracket = True
                if text[cursor] == "]": bracket = False
                if text[cursor] == "/" and not bracket:
                    cursor += 1
                    while cursor < len(text) and text[cursor].isalpha(): cursor += 1
                    break
                cursor += 1
            tokens.append(("regex", "<regex>"))
        elif char.isalpha() or char in "_$":
            end = cursor + 1
            while end < len(text) and (text[end].isalnum() or text[end] in "_$"): end += 1
            tokens.append(("word", text[cursor:end]))
            cursor = end
        else:
            pair = text[cursor:cursor+2]
            if pair in ("=>", "&&", "||", "==", "!=", "?."):
                tokens.append(("punct", pair)); cursor += 2
            else:
                tokens.append(("punct", char)); cursor += 1
    return tokens


def _js_references(text: str, origin: str) -> tuple[list[tuple[str, str]], list[str], bool]:
    tokens = _js_tokens(text)
    values = [t[1] for t in tokens]
    references, unproved = [], []
    wasm_pin = False
    for index, (kind, value) in enumerate(tokens):
        def is_string(at):
            return at < len(tokens) and tokens[at][0] == "string"
        if kind == "word" and value == "import" and index + 1 < len(tokens):
            if is_string(index+1): references.append((tokens[index+1][1], origin))
            elif values[index+1] == "(":
                if is_string(index+2): references.append((tokens[index+2][1], origin))
                else: unproved.append("nonliteral import in " + origin)
        elif kind == "word" and value == "from" and is_string(index+1):
            references.append((tokens[index+1][1], origin))
        elif values[index:index+3] == ["new", "URL", "("] and is_string(index+3):
            tail = values[index+4:index+13]
            if tail[:6] == [",", "import", ".", "meta", ".", "url"]:
                references.append((tokens[index+3][1], origin))
            elif tail[:4] == [",", "document", ".", "baseURI"]:
                references.append((tokens[index+3][1], "index.html"))
                if values[max(0,index-4):index] == ["self", ".", "__CAESURA_WASM_FILE__", "="]:
                    wasm_pin = _url_path(tokens[index+3][1], "index.html") == "web-assets/glue.wasm"
        # Vite dependency tables store root-relative-to-document paths in arrays.
        elif kind == "string" and value.startswith("web-assets/") and re.search(r"\.(?:m?js|css|wasm)(?:[?#].*)?$", value):
            references.append((value, "index.html"))
        elif kind == "template":
            unproved.append("template expression in " + origin)
    return references, sorted(set(unproved)), wasm_pin


def _css_unescape(value: str) -> str:
    return re.sub(r"\\([0-9a-fA-F]{1,6})(?:\s)?|\\(.)",
                  lambda match: chr(int(match[1], 16)) if match[1] else match[2], value)


def _css_references(text: str) -> list[str]:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    refs = []
    for match in re.finditer(r"\burl\(\s*(?:\"((?:\\.|[^\"\\])*)\"|'((?:\\.|[^'\\])*)'|([^)]*))\s*\)", text, re.I):
        refs.append(_css_unescape(next(group for group in match.groups() if group is not None).strip()))
    for match in re.finditer(r"@import\s+(?:\"((?:\\.|[^\"\\])*)\"|'((?:\\.|[^'\\])*)')", text, re.I):
        refs.append(_css_unescape(next(group for group in match.groups() if group is not None)))
    return refs


class _Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.references, self.styles, self.scripts, self.module_scripts = [], [], [], []
        self.active = None
        self.manifests = []

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if tag == "base": raise _Invalid("index.html must not override the package base URL")
        if attrs.get("src"):
            self.references.append(attrs["src"])
            if tag == "script" and attrs.get("type") == "module": self.module_scripts.append(attrs["src"])
        if tag == "link" and attrs.get("href"):
            self.references.append(attrs["href"])
            if "manifest" in attrs.get("rel", "").lower().split():
                self.manifests.append(attrs["href"])
        if attrs.get("style"): self.styles.append(attrs["style"])
        if tag in ("style", "script"):
            self.active = tag

    def handle_endtag(self, tag):
        if tag == self.active: self.active = None

    def handle_data(self, data):
        if self.active == "style": self.styles.append(data)
        if self.active == "script": self.scripts.append(data)


def _web_manifest(root: Path, relative: str) -> dict:
    """Check declared manifest URLs at their actual base, without installing a PWA.

    https://www.w3.org/TR/appmanifest/#start_url-member and #scope-member:
    relative URLs use the manifest URL; absent scope defaults to start's parent.
    This package contract rejects broken declarations instead of relying on a
    browser silently ignoring them. Icons are required local files, not decoded.
    """
    data = json.loads(_text(_file(root, relative)))
    _need(isinstance(data, dict), "Invalid web manifest object: " + relative)

    def resolve(value, label, *, allow_root=False):
        _need(isinstance(value, str) and value.strip(), "Missing web manifest " + label + ": " + relative)
        resolved = _url_path(value, relative, allow_root=allow_root)
        _need(resolved is not None, "Nonlocal web manifest " + label + ": " + relative)
        return resolved

    start = resolve(data.get("start_url"), "start_url")
    _file(root, start)
    if "scope" in data:
        scope = resolve(data["scope"], "scope", allow_root=True)
        if scope and unquote_to_bytes(urlsplit(data["scope"].strip()).path).decode("utf-8").endswith("/"):
            scope += "/"
    else:
        parent = PurePosixPath(start).parent.as_posix()
        scope = "" if parent == "." else parent + "/"
    _need(start.startswith(scope), "Web manifest start_url lies outside declared scope: " + relative)
    icons = data.get("icons")
    _need(isinstance(icons, list) and icons, "Missing web manifest icons: " + relative)
    resolved_icons = []
    for item in icons:
        _need(isinstance(item, dict), "Invalid web manifest icon: " + relative)
        icon = resolve(item.get("src"), "icon src")
        _file(root, icon)
        resolved_icons.append(icon)
    return {"path": relative, "start_url": start, "scope": scope,
            "icons": resolved_icons, "pwa_installation": "NOT_RUN"}


def _manifest(root: Path) -> dict:
    lines = _text(_file(root, "MANIFEST.txt")).splitlines()
    prefix = "Caesura (AmeKAG) web package: "
    _need(lines and lines[0].startswith(prefix), "Invalid MANIFEST.txt header")
    game = _relative(lines[0][len(prefix):])
    _need("/" not in game, "MANIFEST game name must be one directory component")
    fields, files, reading_files = {}, {}, False
    for line in lines[1:]:
        if line == "files (size bytes, path):": reading_files = True; continue
        if line == "---": reading_files = False; continue
        if reading_files:
            size, separator, name = line.partition("\t")
            _need(separator and size.isdecimal(), "Malformed MANIFEST file record")
            name = _relative(name)
            _need(name not in files, "Duplicate MANIFEST file: " + name)
            files[name] = int(size)
        elif ": " in line:
            key, value = line.split(": ", 1)
            _need(key not in fields, "Duplicate MANIFEST field: " + key)
            fields[key] = value
    _need(files and "entry scene" in fields, "MANIFEST entry/file listing missing")
    for name, size in files.items():
        _need(_file(root, name).stat().st_size == size, "MANIFEST size mismatch: " + name)
    return {"game": game, "fields": fields, "files": files}


def _module_index(root: Path) -> dict[str, Path]:
    scripts = root / "scripts"
    _file(root, "scripts/kag/init.lua")
    modules = {}
    for path in sorted(scripts.rglob("*.lua")):
        relative = path.relative_to(scripts)
        if any(part.startswith(".") for part in relative.parts): continue
        name = relative.as_posix()[:-4].replace("/", ".")
        _need(name not in modules, "Ambiguous scripts/index.json module: " + name)
        modules[name] = path
    actual = json.loads(_text(_file(root, "scripts/index.json")))
    expected = {name: True for name in modules}
    _need(actual == expected and all(value is True for value in actual.values()), "scripts/index.json differs from package Lua files")
    return modules


def _frame(value: str) -> bytes:
    # Lua's Windows stdin is text mode. Normalize CRLF in source text like Lua's
    # normal text-file loader; framed bytes contain no CRLF sequences to shrink.
    data = value.replace("\r\n", "\n").encode("utf-8")
    _need(len(data) <= MAX_TEXT_BYTES, "Lua inspector input frame too large")
    return str(len(data)).encode("ascii") + b"\n" + data


def _inspect_bundle(root: Path, lua: Path, manifest: dict, package_modules: dict,
                    report: dict, timeout_seconds: float, instruction_limit: int) -> dict:
    module_sources, module_files, module_hashes = {}, {}, {}
    for path in sorted(SCRIPT_ROOT.rglob("*.lua")):
        relative = path.relative_to(SCRIPT_ROOT)
        if any(part.startswith(".") for part in relative.parts): continue
        name = relative.as_posix()[:-4].replace("/", ".")
        _need(name not in module_sources, "Ambiguous host validator module: " + name)
        module_sources[name], module_hashes[name] = _source_snapshot(path)
        module_files[name] = path
    demo = root / "demo" / manifest["game"]
    _need(demo.is_dir(), "Missing package demo template directory: demo/" + manifest["game"])
    scene_paths = sorted(p.relative_to(root).as_posix() for p in demo.rglob("*.ks") if p.is_file())
    _need(scene_paths and len(scene_paths) <= 2000, "Missing or excessive package demo scenes")
    request = {"module_count": len(module_sources), "paths": scene_paths,
               "entry": manifest["fields"]["entry scene"], "instruction_limit": instruction_limit}
    helper_source, helper_hash = _source_snapshot(LUA_INSPECTOR)
    lua_hash = _hash(lua)
    report["tools"] = {"lua": {"path": str(lua), "sha256": lua_hash, "role": "host_validation_tool"},
                       "inspector": {"path": str(LUA_INSPECTOR), "sha256": helper_hash},
                       "validator_modules": {},
                       "source_transport": "UTF-8; strip BOM and normalize CRLF to LF before in-memory loading"}
    frames = [_frame(module_sources["capability_json"]), _frame(json.dumps(request, ensure_ascii=False))]
    for name, source in module_sources.items(): frames.extend((_frame(name), _frame(source)))
    frames.extend((_frame(_text(_file(root, "cache/story/story.lua"))),
                   _frame(_text(_file(root, "CAPABILITIES.json")))))
    frames.extend(_frame(_text(_file(root, path))) for path in scene_paths)
    payload = b"".join(frames)
    _need(len(payload) <= 64 * 1024 * 1024, "Lua inspector input exceeds total size limit")
    environment = {key: value for key, value in os.environ.items() if key.upper() in {"SYSTEMROOT", "WINDIR"}}
    # An explicit tool path, -E and the in-memory loader prevent LUA_INIT/PATH,
    # author modules or the working directory from becoming runtime fallbacks.
    with tempfile.TemporaryDirectory(prefix="caesura-web-inspector-") as cwd:
        # A fixed ASCII script basename avoids command-line option parsing and
        # Windows command-line length/Unicode limits for the trusted helper.
        helper_copy = Path(cwd) / "inspector.lua"
        helper_copy.write_bytes(helper_source.encode("utf-8"))
        try:
            process = subprocess.run([str(lua), "-E", "inspector.lua"], input=payload,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=cwd, env=environment,
                                     timeout=timeout_seconds, shell=False,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        except subprocess.TimeoutExpired as error:
            report["lua_process"] = {"timed_out": True, "timeout_seconds": timeout_seconds,
                                     "stdout": (error.stdout or b"")[:65536].decode("utf-8", "replace"),
                                     "stderr": (error.stderr or b"")[:65536].decode("utf-8", "replace")}
            raise _Invalid("Lua static inspector external timeout; owned child killed and reaped") from error
    report["lua_process"] = {"returncode": process.returncode, "timed_out": False,
                             "stderr": process.stderr[:65536].decode("utf-8", "replace")}
    _need(_hash(lua) == lua_hash and _hash(LUA_INSPECTOR) == helper_hash, "Host inspector tool changed during validation")
    _need(len(process.stdout) <= 1024 * 1024, "Lua inspector output exceeds size limit")
    decoded = process.stdout.decode("utf-8")
    if process.returncode:
        raise _Invalid("Lua static inspector failed: " + decoded[:65536] + report["lua_process"]["stderr"])
    bundle = json.loads(decoded)
    _need(bundle.get("runtime") == "NOT_RUN", "Lua static report scope mismatch")
    for name in bundle["used_validator_modules"]:
        _need(name in module_files, "Unrecognized validator module: " + name)
        relative = module_files[name].relative_to(SCRIPT_ROOT).as_posix()
        actual_path = "scripts/" + relative
        _need(name in package_modules, "Missing package validator module: " + actual_path)
        _need(_hash(module_files[name]) == module_hashes[name], "Host validator module changed: " + name)
        _need(_hash(_file(root, actual_path)) == module_hashes[name], "Package validator module identity mismatch: " + actual_path)
        report["tools"]["validator_modules"][name] = {"host_path": str(module_files[name]),
            "package_path": actual_path, "sha256": module_hashes[name]}
    return bundle


def inspect_web_package(package_root: str | Path, lua_executable: str | Path, *,
                        timeout_seconds: float = 10, instruction_limit: int = 50_000_000) -> dict:
    """Return structured STATIC_PASS/STATIC_FAIL for the exact supplied directory."""
    report = {"schema": SCHEMA, "scope": "web_package_static_contract", "status": "STATIC_FAIL",
              "runtime": "NOT_RUN", "started_at": datetime.now(timezone.utc).isoformat(),
              "package_root": str(package_root), "checks": [], "errors": [],
              "boundaries": {"dynamic_author_media": "NOT_PROVEN", "capability_skipped_media": "NOT_VERIFIED",
                  "browser_audio_offline_pwa": "NOT_RUN", "arbitrary_js_requests": "NOT_PROVEN"}}
    before = None
    try:
        root = Path(package_root).resolve(strict=True)
        lua = Path(lua_executable).resolve(strict=True)
        _need(root.is_dir(), "Explicit package root is not a directory")
        _need(lua.is_file() and lua.suffix.lower() not in (".bat", ".cmd"), "Explicit Lua tool must be an executable file")
        _need(root not in lua.parents, "Lua must be declared as a host validation tool outside the package")
        _need(math.isfinite(timeout_seconds) and 0 < timeout_seconds <= 120, "Invalid inspector timeout")
        _need(isinstance(instruction_limit, int) and not isinstance(instruction_limit, bool)
              and 0 < instruction_limit <= 200_000_000, "Invalid instruction limit")
        report["package_root"] = str(root)
        before = inspect_inventory(root)
        report["input_inventory"] = before
        manifest = _manifest(root)
        actual_files = {e["path"] for e in before["entries"] if e["type"] != "directory"}
        _need(set(manifest["files"]) == actual_files - {"MANIFEST.txt", ".caesura-output.json"},
              "MANIFEST.txt file listing differs from the package entry set")
        report["checks"].append("manifest file paths and sizes")
        for name in ("cache/story/story.lua", "scripts/index.json", "web-assets/glue.wasm", FONT): _file(root, name)
        _need(_file(root, "web-assets/glue.wasm").read_bytes()[:8] == b"\0asm\1\0\0\0", "Invalid package web-assets/glue.wasm header")
        _need(_file(root, FONT).read_bytes()[:4] == b"OTTO", "Invalid package CJK font header: " + FONT)
        page = _Page()
        page.feed(_text(_file(root, "index.html")))
        _need(page.module_scripts, "index.html has no actual module script reference")
        references = [(value, "index.html") for value in page.references]
        for css in page.styles: references.extend((value, "index.html") for value in _css_references(css))
        pins, unproved = [], []
        for script in page.scripts:
            refs, dynamic, pinned = _js_references(script, "index.html")
            references.extend(refs); unproved.extend(dynamic); pins.append(pinned)
        _need(any(pins), "index.html does not pin __CAESURA_WASM_FILE__ to package web-assets/glue.wasm")
        seen, pending = set(), references[:]
        while pending:
            reference, origin = pending.pop()
            relative = _url_path(reference, origin)
            if relative is None: continue
            path = _file(root, relative)
            if relative in seen: continue
            seen.add(relative)
            if path.suffix.lower() in (".js", ".mjs"):
                refs, dynamic, _ = _js_references(_text(path), relative)
                pending.extend(refs); unproved.extend(dynamic)
            elif path.suffix.lower() == ".css":
                pending.extend((value, relative) for value in _css_references(_text(path)))
        _need(FONT in seen, "index.html/styles do not reference the packaged CJK font: " + FONT)
        _need(len(page.manifests) == 1, "index.html must reference one local web manifest")
        manifest_path = _url_path(page.manifests[0], "index.html")
        _need(manifest_path is not None, "Missing local web manifest reference")
        try:
            web_manifest = _web_manifest(root, manifest_path)
        except (OSError, ValueError, RuntimeError, TypeError) as error:
            raise _Invalid("Web manifest validation failed: " + str(error)) from error
        report["web_manifests"] = [web_manifest]
        seen.update([web_manifest["start_url"], *web_manifest["icons"]])
        report["checks"].append("web manifest start_url/scope/icons resolve within the actual package")
        report["references"] = sorted(seen)
        report["js_unproved"] = sorted(set(unproved))
        report["checks"].append("literal HTML/JS/CSS references, WASM and CJK font")
        package_modules = _module_index(root)
        report["checks"].append("script index matches actual package Lua files")
        bundle = _inspect_bundle(root, lua, manifest, package_modules, report, timeout_seconds, instruction_limit)
        report["bundle"] = bundle
        fields = manifest["fields"]
        for field, actual in (("scenes", len(bundle["scene_keys"])),
                              ("static media dependencies", len(bundle["dependencies"]["static"])),
                              ("dynamic media dependencies (not proven)", len(bundle["dependencies"]["dynamic"])),
                              ("capability-skipped media dependencies (not verified)", len(bundle["dependencies"]["skipped"]))):
            _need(fields.get(field) == str(actual), "MANIFEST count mismatch: " + field)
        for reference in bundle["dependencies"]["static"]: _file(root, reference)
        report["checks"].append("real compiler entry/source/asset dependency contracts and static media")
        report["status"] = "STATIC_PASS"
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        report["errors"].append(str(error))
    finally:
        if before is not None:
            try:
                report["input_stable"] = inspect_inventory(root) == before
                if not report["input_stable"]: report["errors"].append("Package input changed during static validation")
            except (OSError, ValueError, RuntimeError) as error:
                report["input_stable"] = False
                report["errors"].append("Cannot confirm unchanged package input: " + str(error))
        if report["errors"]: report["status"] = "STATIC_FAIL"
        report["finished_at"] = datetime.now(timezone.utc).isoformat()
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--lua", type=Path, required=True, help="Explicit host validation Lua executable")
    parser.add_argument("--report", type=Path, required=True, help="New report path outside the package")
    args = parser.parse_args()
    try:
        package = args.package_root.resolve(strict=True)
        report_path = args.report.parent.resolve(strict=True) / args.report.name
        _need(report_path != package and package not in report_path.parents, "Report must be outside package input")
        with report_path.open("x", encoding="utf-8", newline="\n") as stream:
            report = inspect_web_package(args.package_root, args.lua)
            json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
        print(json.dumps({"status": report["status"], "runtime": "NOT_RUN", "report": str(report_path)}, ensure_ascii=False))
        return 0 if report["status"] == "STATIC_PASS" else 1
    except (OSError, ValueError, RuntimeError) as error:
        print(json.dumps({"status": "STATIC_FAIL", "runtime": "NOT_RUN", "error": str(error)}, ensure_ascii=False))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
