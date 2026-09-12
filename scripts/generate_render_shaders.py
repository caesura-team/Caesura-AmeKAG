#!/usr/bin/env python3
"""Deterministic ordinary-render shader generation; compiler paths are explicit.

Always builds into a new --output directory. --publish updates only the declared
embedded outputs and four existing DXBC files after every compiler/byte check
passes. No engine/GPU/CMake invocation, downloader, or bin2c dependency.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
# The complete existing ordinary-render GL/Metal manifest, plus one new FS.
# S5 skinning and Vulkan arrays have separate loader contracts and are untouched.
PLATFORM_SHADERS = (
    "vs_sprite", "vs_fullscreen", "stretch_blt_vs", "affine_blt_vs",
    "fs_texture", "fs_modulated_texture", "fs_blend", "fs_transition", "fs_vfx",
    "fs_postfx_vignette", "fs_postfx_lut", "fs_postfx_blur", "fs_postfx_bloom",
    "fs_postfx_lut3d", "stretch_blt_fs", "affine_blt_fs",
)
DXBC_SHADERS = ("vs_fullscreen", "fs_modulated_texture", "fs_blend", "fs_transition", "fs_postfx_lut3d")
EXISTING_DXBC = frozenset(DXBC_SHADERS) - {"fs_modulated_texture"}
NEW_SHADER = "fs_modulated_texture"
SYMBOL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def utc() -> str:
    return datetime.now(timezone.utc).isoformat()


def write_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def normalize_source(data: bytes) -> bytes:
    """Explicit encoding conversion, recorded alongside the original byte hash."""
    return data.decode("utf-8-sig").replace("\r\n", "\n").encode("utf-8")


def emit_array(symbol: str, data: bytes) -> str:
    if not SYMBOL.fullmatch(symbol) or not data:
        raise ValueError("invalid symbol or empty shader")
    rows = ["    " + ", ".join(f"0x{value:02x}" for value in data[i:i + 12]) + ","
            for i in range(0, len(data), 12)]
    return (f"const uint8_t {symbol}[] = {{\n" + "\n".join(rows) + "\n};\n"
            + f"const size_t {symbol}_size = sizeof({symbol});\n")


def read_array(text: str, symbol: str) -> bytes:
    """Strictly reject malformed/sign-expanded tokens, duplicate symbols and size drift."""
    if not SYMBOL.fullmatch(symbol):
        raise ValueError("invalid symbol")
    pattern = (r"const\s+uint8_t\s+" + re.escape(symbol)
               + r"\s*\[\s*(\d*)\s*\]\s*=\s*(?:/\*.*?\*/\s*)?\{(.*?)\}\s*;")
    matches = list(re.finditer(pattern, text, re.S))
    if len(matches) != 1:
        raise ValueError("expected one byte array: " + symbol)
    declared, body = matches[0].groups()
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)
    tokens = [token.strip() for token in body.split(",")]
    if tokens and tokens[-1] == "":
        tokens.pop()
    if not tokens or any(not re.fullmatch(r"0x[0-9A-Fa-f]{2}", token) for token in tokens):
        raise ValueError("invalid unsigned byte token: " + symbol)
    data = bytes(int(token[2:], 16) for token in tokens)
    if declared and int(declared) != len(data):
        raise ValueError("declared byte count differs: " + symbol)
    sizes = re.findall(r"const\s+size_t\s+" + re.escape(symbol)
                       + r"_size\s*=\s*([^;]+);", text)
    if len(sizes) != 1:
        raise ValueError("expected one size symbol: " + symbol)
    size = sizes[0].strip()
    if size != f"sizeof({symbol})" and (not size.isdecimal() or int(size) != len(data)):
        raise ValueError("size definition differs: " + symbol)
    return data


def replace_array(text: str, symbol: str, data: bytes, *, allow_new: bool = False) -> str:
    pattern = (r"const\s+uint8_t\s+" + re.escape(symbol)
               + r"\s*\[\s*\d*\s*\]\s*=\s*\{.*?\}\s*;\s*const\s+size_t\s+"
               + re.escape(symbol) + r"_size\s*=\s*[^;]+;")
    matches = list(re.finditer(pattern, text, re.S))
    if len(matches) == 1:
        match = matches[0]
        result = text[:match.start()] + emit_array(symbol, data).rstrip() + text[match.end():]
    elif not matches and allow_new:
        marker = "} // namespace Caesura"
        if text.count(marker) != 1:
            raise ValueError("ambiguous namespace boundary")
        result = text.replace(marker, emit_array(symbol, data) + "\n" + marker)
    else:
        raise ValueError("missing/duplicate target array: " + symbol)
    if read_array(result, symbol) != data:
        raise ValueError("generated array did not round-trip: " + symbol)
    return result


def platform_source(previous: str, prefix: str, binaries: dict[str, bytes]) -> str:
    expected = {f"kEmbedded{prefix}_{name}" for name in PLATFORM_SHADERS}
    required_previous = expected - {f"kEmbedded{prefix}_{NEW_SHADER}"}
    found = set(re.findall(r"const\s+uint8_t\s+(kEmbedded" + prefix + r"_\w+)\s*\[", previous))
    if not required_previous.issubset(found) or not found.issubset(expected):
        raise ValueError(f"{prefix} existing shader manifest changed; refusing to omit programs")
    if set(binaries) != set(PLATFORM_SHADERS):
        raise ValueError(f"{prefix} compiled shader manifest incomplete")
    text = ("// Generated by scripts/generate_render_shaders.py; do not hand-edit bytes.\n"
            "// Complete ordinary-render shader manifest; inputs are shaders/glsl/*.sc.\n"
            '#include "EmbeddedShaders.h"\n\nnamespace Caesura {\n\n')
    for name in PLATFORM_SHADERS:
        symbol = f"kEmbedded{prefix}_{name}"
        text += emit_array(symbol, binaries[name]) + "\n"
    text += "} // namespace Caesura\n"
    for name, data in binaries.items():
        if read_array(text, f"kEmbedded{prefix}_{name}") != data:
            raise ValueError("generated platform bytes differ")
    return text


def inspect_binary(data: bytes, backend: str, stage: str) -> dict:
    if backend == "dxbc":
        if len(data) < 32 or data[:4] != b"DXBC":
            raise ValueError("missing raw DXBC header")
        version, size, count = struct.unpack_from("<III", data, 20)
        if size != len(data) or 32 + count * 4 > size:
            raise ValueError("invalid DXBC size")
        chunks = []
        for i in range(count):
            offset = struct.unpack_from("<I", data, 32 + i * 4)[0]
            if offset + 8 > size:
                raise ValueError("invalid DXBC chunk offset")
            length = struct.unpack_from("<I", data, offset + 4)[0]
            if offset + 8 + length > size:
                raise ValueError("truncated DXBC chunk")
            chunks.append(data[offset:offset + 4].decode("ascii"))
        return {"kind": "raw-DXBC", "version": version, "chunks": chunks}
    if len(data) < 18 or data[:3] != (b"VSH" if stage == "vertex" else b"FSH") or data[3] != 11:
        raise ValueError("expected matching bgfx v11 shader container")
    hash_in, hash_out, count = struct.unpack_from("<IIH", data, 4)
    cursor, uniforms = 14, []
    for _ in range(count):
        if cursor >= len(data):
            raise ValueError("missing uniform record")
        length = data[cursor]
        cursor += 1
        if cursor + length + 10 > len(data):
            raise ValueError("truncated uniform record")
        name = data[cursor:cursor + length].decode("utf-8")
        cursor += length
        kind, number, register, registers, tex_info, tex_format = struct.unpack_from("<BBHHHH", data, cursor)
        cursor += 10
        uniforms.append({"name": name, "type": kind, "num": number, "register": register,
                         "register_count": registers, "texture_info": tex_info, "texture_format": tex_format})
    if cursor + 4 > len(data):
        raise ValueError("missing shader code length")
    length = struct.unpack_from("<I", data, cursor)[0]
    cursor += 4
    if not length or cursor + length >= len(data) or data[cursor + length] != 0:
        raise ValueError("invalid shader payload/terminator")
    code = data[cursor:cursor + length]
    if code[:3] in (b"VSH", b"FSH"):
        raise ValueError("nested bgfx wrapper")
    return {"kind": "bgfx-container", "version": 11, "hash_in": f"{hash_in:08x}",
            "hash_out": f"{hash_out:08x}", "uniforms": uniforms,
            "code_offset": cursor, "code_bytes": length, "code_sha256": sha(code)}


def write_atomic(path: Path, data: bytes) -> None:
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as file:
            temporary = Path(file.name)
            file.write(data)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def generate(args) -> int:
    root = args.root.resolve(strict=True)
    output = args.output.resolve()
    # A new artifact directory is mandatory. Never mix old binaries into a run.
    output.mkdir(parents=True, exist_ok=False)
    tool_paths = {name: getattr(args, name).resolve(strict=True) for name in ("fxc", "shaderc")}
    if any(not path.is_file() for path in tool_paths.values()):
        raise ValueError("compiler path must be an existing file")
    sources = [f"shaders/dx11/{name}.hlsl" for name in DXBC_SHADERS]
    sources += [f"shaders/glsl/{name}.sc" for name in PLATFORM_SHADERS]
    sources += ["shaders/varying.def", "external/bgfx/bgfx/src/bgfx_shader.sh"]
    target_paths = [root / "src/render/EmbeddedShaders.cpp", root / "src/render/EmbeddedShaders_GL.cpp",
                    root / "src/render/EmbeddedShaders_Metal.cpp"]
    originals = {path: path.read_bytes() for path in target_paths}
    inputs = output / "inputs"
    records = {}
    for name in sources:
        raw = (root / name).read_bytes()
        normalized = normalize_source(raw)
        path = inputs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(normalized)
        records[name] = {"source_sha256": sha(raw), "normalized_sha256": sha(normalized),
                         "removed_utf8_bom": raw.startswith(b"\xef\xbb\xbf"), "bytes": len(raw)}
    report = {"schema": 1, "status": "RUNNING", "started_utc": utc(), "root": str(root),
              "output": str(output), "publish_requested": args.publish, "published": False,
              "generator_sha256": sha(Path(__file__).read_bytes()), "sources": records,
              "tools": {name: {"path": str(path), "sha256": sha(path.read_bytes())}
                        for name, path in tool_paths.items()}, "commands": [], "artifacts": {}, "pairs": []}

    def persist():
        write_json(output / "receipt.json", report)

    def run(label, command, accepted_exit_codes=(0,)):
        directory = output / "commands" / label
        directory.mkdir(parents=True)
        record = {"name": label, "argv": command, "cwd": str(inputs), "started_utc": utc(), "timeout_seconds": 60,
                  "accepted_exit_codes": list(accepted_exit_codes)}
        report["commands"].append(record)
        persist()
        started = time.monotonic()
        with (directory / "stdout.log").open("wb") as stdout, (directory / "stderr.log").open("wb") as stderr:
            child = subprocess.Popen(command, cwd=inputs, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            record["pid"] = child.pid
            persist()
            try:
                record["exit_code"] = child.wait(timeout=60)
            except subprocess.TimeoutExpired:
                record["timed_out"] = True
                child.kill()
                record["exit_code"] = child.wait(timeout=10)
            except BaseException:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=10)
                raise
        record["elapsed_seconds"] = round(time.monotonic() - started, 6)
        record["finished_utc"] = utc()
        record["stdout_sha256"] = sha((directory / "stdout.log").read_bytes())
        record["stderr_sha256"] = sha((directory / "stderr.log").read_bytes())
        persist()
        if record.get("timed_out") or record["exit_code"] not in accepted_exit_codes:
            raise RuntimeError("compiler command failed: " + label)
        return record

    try:
        persist()
        for name, flag in (("fxc", "/?"), ("shaderc", "--version")):
            # FXC's documented help invocation prints its version but exits 1.
            # This exception applies only to /?; every actual compile still
            # requires exit 0. Verify the banner instead of accepting arbitrary errors.
            run("version-" + name, [str(tool_paths[name]), flag], (0, 1) if name == "fxc" else (0,))
            directory = output / "commands" / ("version-" + name)
            version = "".join((directory / stream).read_text(encoding="utf-8", errors="replace")
                              for stream in ("stdout.log", "stderr.log"))
            expected = "Direct3D Shader Compiler" if name == "fxc" else "bgfx shader compiler tool, version"
            if expected not in version:
                raise ValueError("unexpected compiler version output: " + name)
            report["tools"][name]["version_output"] = version
        binaries = {backend: {} for backend in ("dxbc", "gl", "metal")}
        metadata = {backend: {} for backend in binaries}
        for backend in binaries:
            destination = output / "compiled" / backend
            destination.mkdir(parents=True)
            for name in (DXBC_SHADERS if backend == "dxbc" else PLATFORM_SHADERS):
                stage = "vertex" if name.startswith("vs_") or name.endswith("_vs") else "fragment"
                target = destination / (name + (".dxbc" if backend == "dxbc" else ".bin"))
                relative_target = os.path.relpath(target, inputs)
                if backend == "dxbc":
                    command = [str(tool_paths["fxc"]), "/nologo", "/T", "vs_4_0" if stage == "vertex" else "ps_4_0",
                               "/E", "main", "/O3", "/Fo", relative_target, f"shaders/dx11/{name}.hlsl"]
                else:
                    command = [str(tool_paths["shaderc"]), "-f", f"shaders/glsl/{name}.sc", "-o", relative_target,
                               "--type", stage, "--platform", "linux" if backend == "gl" else "osx",
                               "--profile", "430" if backend == "gl" else "metal", "--varyingdef", "shaders/varying.def",
                               "-i", "external/bgfx/bgfx/src", "--depends"]
                record = run(backend + "-" + name, command)
                data = target.read_bytes()
                parsed = inspect_binary(data, backend, stage)
                record.update(output_sha256=sha(data), output_bytes=len(data), binary=parsed)
                binaries[backend][name], metadata[backend][name] = data, parsed
                if backend != "dxbc":
                    offset, length = parsed["code_offset"], parsed["code_bytes"]
                    (destination / (name + ".payload.txt")).write_bytes(data[offset:offset + length])
                else:
                    run("reflection-" + name, [str(tool_paths["fxc"]), "/nologo", "/dumpbin", relative_target])
                persist()
        for backend in ("gl", "metal"):
            for name in PLATFORM_SHADERS:
                if name.startswith("vs_") or name.endswith("_vs"):
                    continue
                vertex = ("stretch_blt_vs" if name == "stretch_blt_fs" else "affine_blt_vs" if name == "affine_blt_fs"
                          else "vs_sprite" if name in ("fs_texture", NEW_SHADER) else "vs_fullscreen")
                match = metadata[backend][vertex]["hash_out"] == metadata[backend][name]["hash_in"]
                report["pairs"].append({"backend": backend, "vertex": vertex, "fragment": name, "matched": match})
                if not match:
                    raise ValueError("shader varying hash mismatch: " + backend + "/" + name)
        generated = {}
        dxbc_text = originals[target_paths[0]].decode("utf-8")
        for name, data in binaries["dxbc"].items():
            dxbc_text = replace_array(dxbc_text, "kEmbeddedDXBC_" + name, data, allow_new=name == NEW_SHADER)
        generated[target_paths[0]] = dxbc_text.encode("utf-8")
        for path, backend, prefix in ((target_paths[1], "gl", "GL"), (target_paths[2], "metal", "Metal")):
            generated[path] = platform_source(originals[path].decode("utf-8"), prefix, binaries[backend]).encode("utf-8")
        for name in EXISTING_DXBC:
            path = root / "shaders/dx11" / (name + ".dxbc")
            originals[path] = path.read_bytes()  # Existing tracked intermediate only.
            generated[path] = binaries["dxbc"][name]
        for path, data in generated.items():
            relative_path = path.relative_to(root)
            candidate = output / "generated" / relative_path
            candidate.parent.mkdir(parents=True, exist_ok=True)
            candidate.write_bytes(data)
            report["artifacts"][relative_path.as_posix()] = {"before_sha256": sha(originals[path]), "sha256": sha(data),
                                                           "bytes": len(data)}
        if any(sha((root / name).read_bytes()) != record["source_sha256"] for name, record in records.items()):
            raise ValueError("shader source changed during generation")
        if any(sha(path.read_bytes()) != report["tools"][name]["sha256"] for name, path in tool_paths.items()):
            raise ValueError("compiler changed during generation")
        if any(path.read_bytes() != previous for path, previous in originals.items()):
            raise ValueError("embedded output changed during generation")
        if args.publish:
            # Compiler failures, missing programs and invalid bytes cannot reach
            # this publication boundary. Preserve unrelated DXBC/SPIR-V blocks.
            for path, data in generated.items():
                write_atomic(path, data)
            report["published"] = True
        report["status"] = "PASS"
    except Exception as error:
        report["status"] = "FAIL"
        report["error"] = str(error)
    report["finished_utc"] = utc()
    persist()
    print(json.dumps({"status": report["status"], "published": report["published"], "receipt": str(output / "receipt.json")},
                     ensure_ascii=False))
    return 0 if report["status"] == "PASS" else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fxc", type=Path, required=True)
    parser.add_argument("--shaderc", type=Path, required=True)
    parser.add_argument("--publish", action="store_true")
    return generate(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
