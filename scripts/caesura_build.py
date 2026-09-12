#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
caesura_build.py — game-only desktop build & distribution packaging for the
Caesura (AmeKAG) creator CLI (backing store for "caesura build" / "caesura package").

Why this module exists
----------------------
The product task book (docs/plans/audit/Caesura-AmeKAG_产品化推进总任务书.md §7.1)
requires a *game-only package*: what a player downloads and double-clicks. Nothing
in the repo produced that shape before — CPack ships the whole developer tree
(scripts + demo + full 39 MB shared asset pool + projects/), and
scripts/package_game.mjs (Node CLI, t179) only produces the Web static site.

Design decision: this module NEVER configures or invokes a C++ toolchain.
------------------------------------------------------------------------
"caesura build" assembles a *runnable game directory around an engine binary that
already exists*. It does not run cmake. Reasons:
  1. Configuring MSVC/bgfx/SDL3/FFmpeg from a CLI wrapper cannot be made honest:
     the failure surface (missing generator, SDK, Steamworks, Live2D SDK, FFmpeg
     DLLs) is exactly the "CMake/bgfx/SDL concepts" the task book wants hidden,
     and a wrapper that shells out to cmake exposes them anyway — as a wall of
     compiler output.
  2. A creator iterating on a story rebuilds the *game*, not the engine. Coupling
     the two would make every package run a multi-minute C++ build.
  3. The engine is shipped to creators as a prebuilt binary (CPack ZIP / release
     artifact), so "an engine binary exists" is the normal state, not an edge case.
Therefore a missing engine binary is a first-class, actionable diagnostic (doctor
style: what was searched, what to do next), never a traceback.

Output shape (game-only)
------------------------
    <out>/
      CaesuraAmeKAG.exe          engine binary (+ every runtime lib beside it)
      SDL3.dll, av*.dll, ...     copied from the engine binary's directory
      scripts/                   engine Lua runtime (config.lua re-pointed)
      assets/                    engine-required assets (fonts/, lang/) + game assets
      projects/<game>/           the game itself (.ks scenes, entry.lua, assets)
      cache/ksc, saves, settings, logs
      BUILD-INFO.json            provenance manifest
      HOW-TO-PLAY.txt            player-facing launch note

projects/ is used as the in-package game root because scripts/sandbox.lua:314-316
allowlists exactly scripts/ | assets/ | tests/ | demo/ | projects/ for post-lockdown
io.open — a game placed anywhere else cannot perform cross-scene [jump] at runtime.
"""

import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

# The repo path itself can contain non-ASCII (this checkout does), and the
# Windows console default code page mangles it. Match the convention already
# used by scripts/verify_release_candidate.py:35-39.
for _stream in (sys.stdout, sys.stderr):
    if getattr(_stream, "encoding", "") != "utf-8" and hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8")
        except Exception:
            pass

ROOT = Path(__file__).resolve().parent.parent

# Engine-required assets: hardcoded in src/entry/Engine.cpp:366 and
# src/script/bindings/RenderBinding.cpp:945,967 (default font) and read by
# scripts/i18n.lua:62 (language packs). A game-only package without these
# renders no text at all, so they ship unconditionally.
ENGINE_REQUIRED_ASSETS = ("fonts", "lang")

PRUNE_DIR_NAMES = {"__pycache__", ".git", ".svn", "node_modules"}
PRUNE_SUFFIXES = {".pyc", ".pyo", ".bak"}

# Where a bare project NAME is looked up (in order).
PROJECT_SEARCH_DIRS = ("projects", "tools/project_templates", "tests/projects", "demo")

# Where an engine binary is looked for (in order), relative to ROOT.
ENGINE_SEARCH_DIRS = (
    "build/Release", "build/Debug", "build",
    "bin/Release", "bin/Debug", "bin",
    "build/RelWithDebInfo",
)


class BuildError(Exception):
    """A user-actionable failure. main() prints .args[0] and exits 1 — no traceback."""


# ----------------------------------------------------------------- helpers --

def _exe_name() -> str:
    return "CaesuraAmeKAG.exe" if os.name == "nt" else "CaesuraAmeKAG"


def _ignore_junk(_dir, names):
    return [n for n in names
            if n in PRUNE_DIR_NAMES or os.path.splitext(n)[1] in PRUNE_SUFFIXES]


def _copy_tree(src: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst, dirs_exist_ok=True, ignore=_ignore_junk)


def _copied_files(directory):
    """Mirror copytree's existing development-file filter for input manifests."""
    for current, directories, files in os.walk(directory):
        ignored = set(_ignore_junk(current, directories + files))
        directories[:] = [name for name in directories if name not in ignored]
        for name in files:
            path = Path(current) / name
            if name not in ignored and path.is_file(): yield path


def _rel(p: Path) -> str:
    try:
        return str(p.relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        return str(p).replace("\\", "/")


def _looks_like_caesura_output(out: Path) -> bool:
    """Best-effort marker: does this dir look like the residue of a 'caesura
    build' run that failed before BUILD-INFO.json was written? (The marker
    files are ones only assemble() creates; a crashed/killed build cannot run
    the t19 self-cleanup, so this distinguishes its residue from an unrelated
    user directory for the refusal message.)"""
    try:
        # Platform-agnostic engine-binary markers: residue carries the name
        # of the platform that BUILT it, and this wording chooser must not
        # flip its verdict based on which platform inspects the directory --
        # a planted CaesuraAmeKAG.exe marker read as "unrelated" on the
        # Linux/macOS guard (CI run 33184701644). Both branches refuse either
        # way; only the message differs, so matching generously is safe.
        _exe_markers = ("CaesuraAmeKAG.exe", "CaesuraAmeKAG")
        if any((out / n).is_file() for n in _exe_markers) \
                or (out / "HOW-TO-PLAY.txt").is_file():
            return True
        return any((out / "projects").glob("*/caesura-boot.lua"))
    except OSError:
        return False


def _prepare_out(out: Path) -> None:
    """S2 guard + ownership acquisition for an assemble() output directory.

    Refuses to delete a directory we did not create: -o accepts any path, and an
    unconditional rmtree would erase e.g. a pre-existing dist/ full of unrelated
    files. Every assemble() run writes BUILD-INFO.json, so its presence marks the
    directory as a previous build output; an empty directory is also safe to
    reuse. t19/A2: the refusal message now distinguishes a crashed build's
    residue (marker files, no BUILD-INFO) from an unrelated user directory.
    """
    if not out.exists():
        return
    if any(out.iterdir()) and not (out / "BUILD-INFO.json").exists():
        if _looks_like_caesura_output(out):
            raise BuildError(
                "Refusing to overwrite %s: a previous 'caesura build' into this "
                "directory FAILED before BUILD-INFO.json was written, leaving "
                "this partial output behind (a crashed/killed build cannot clean "
                "itself up). Delete it, or pick a new -o, then re-run." % _rel(out))
        raise BuildError(
            "Refusing to overwrite %s: it exists, is not empty, and has no "
            "BUILD-INFO.json (not a previous 'caesura build' output). "
            "Pick a new or empty -o directory, or delete it yourself."
            % _rel(out))
    shutil.rmtree(out)


def _assemble_clean(project: Path, entry_scene: Path, engine: Path, out: Path,
                    shared_assets: bool, dev_mode: bool, quiet: bool = False, capabilities=None) -> dict:
    """t19/A2: run assemble() and self-clean the output WE own on failure.

    A run that fails mid-assemble (disk error, crash, Ctrl+C) must not leave a
    half-written directory that blocks the same -o forever: the S2 guarantee
    refuses any non-empty dir without BUILD-INFO.json, and a failure before that
    file was written creates exactly that state. So from the moment _prepare_out
    gave us ownership (cleared previous build output or confirmed a fresh path),
    any failure removes only what we created -- user directories are refused
    BEFORE ownership, and never touched.
    """
    took_ownership = False
    try:
        if capabilities is not None:
            if _file_sha256(engine) != capabilities["profile"]["binary_sha256"]:
                raise BuildError("Selected engine changed after capability validation.")
            _require_capability_inputs(capabilities, project)
        _prepare_out(out)
        took_ownership = True
        return assemble(project, entry_scene, engine, out,
                        shared_assets=shared_assets, dev_mode=dev_mode, quiet=quiet, capabilities=capabilities)
    finally:
        if took_ownership and not (out / "BUILD-INFO.json").exists():
            shutil.rmtree(out, ignore_errors=True)


def find_node() -> str:
    """Resolve Node EXPLICITLY, never the bare name semantics alone.

    Node is already an implicit hard dependency of web packaging (vite/wasmoon
    toolchain); with the package_game.mjs CLI (t179) Git Bash is no longer
    required. Resolution order: CAESURA_NODE override -> PATH -> common
    installer locations. A bare-path resolution is still validated to be an
    actual file (nvm/nvs shims can shadow the real node on PATH).
    """
    env = os.environ.get("CAESURA_NODE", "").strip()
    if env and Path(env).is_file():
        return env
    found = shutil.which("node")
    if found and Path(found).is_file():
        return found
    candidates = []
    if os.name == "nt":
        for root in (os.environ.get("ProgramW6432"), os.environ.get("ProgramFiles"),
                     os.environ.get("ProgramFiles(x86)"),
                     os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs")):
            if root:
                candidates.append(os.path.join(root, "nodejs", "node.exe"))
    else:
        candidates.extend(["/usr/local/bin/node", "/usr/bin/node"])
    for c in candidates:
        if Path(c).is_file():
            return c
    raise BuildError(
        "No Node.js found (web packaging requires node; set CAESURA_NODE)")


def find_lua() -> str:
    """Explicit CAESURA_LUA, then packaged/build-tree interpreters, then PATH.

    external/lua/lua[.exe] exists in the RELEASE PACKAGE (installed from the
    lua_cli target) but is gitignored in a checkout: a fresh clone only has
    the interpreter at build/lua/<config>/lua[.exe] after cmake --build --
    the same place CI looks (ci.yml recurses build/lua first). Probing only
    external/lua/ went red on every machine without a stale hand-built relic
    or a PATH lua, while looking green on dev machines that have the relic.
    """
    # Validation supplies the exact lua_cli product for this build/configuration.
    # A configured path is authoritative, including when it is invalid.
    if "CAESURA_LUA" in os.environ:
        explicit = os.environ["CAESURA_LUA"]
        path = Path(explicit).resolve()
        if not explicit or not path.is_file():
            raise BuildError("CAESURA_LUA does not point at a Lua interpreter: %s"
                             % (explicit or "<empty>"))
        return str(path)

    candidates = [ROOT / "external/lua/lua.exe", ROOT / "external/lua/lua"]
    for cfg in ("Release", "Debug", "RelWithDebInfo", "MinSizeRel", ""):
        base = (ROOT / "build" / "lua" / cfg) if cfg else (ROOT / "build" / "lua")
        candidates.append(base / "lua.exe")
        candidates.append(base / "lua")
    for c in candidates:
        if c.is_file():
            return str(c)
    for c in ("lua5.4", "lua"):
        if shutil.which(c):
            return c
    raise BuildError(
        "No Lua 5.4 interpreter found.\n"
        "  Searched: external/lua/lua[.exe], build/lua/<config>/lua[.exe], "
        "lua5.4, lua (PATH)\n"
        "  Fix: build the engine first (cmake --build build), install Lua 5.4, "
        "or run from an extracted release package."
    )


# --------------------------------------------------------- project resolve --

def resolve_project(spec: str) -> Path:
    """A path (absolute / cwd-relative / ROOT-relative) or a bare project name."""
    cands = []
    p = Path(spec)
    if p.is_absolute():
        cands.append(p)
    else:
        cands.append(Path.cwd() / p)
        cands.append(ROOT / p)
        for d in PROJECT_SEARCH_DIRS:
            cands.append(ROOT / d / spec)
    for c in cands:
        if c.is_dir():
            return c.resolve()
    searched = "\n".join("    " + _rel(c) for c in cands)
    raise BuildError(
        "Project not found: %s\n"
        "  Searched:\n%s\n"
        "  Fix: pass a project directory, or create one:\n"
        "    python scripts/caesura.py create my_game --template basic"
        % (spec, searched)
    )


def collect_scenes(project: Path):
    scenes = sorted(path for path in _copied_files(project) if path.match("*.ks"))
    if not scenes:
        raise BuildError(
            "No .ks scenes in project: %s\n"
            "  A Caesura game needs at least one KAG scene (story.ks).\n"
            "  Fix: add story.ks, or point at the right directory." % _rel(project)
        )
    return scenes


def pick_entry_scene(project: Path, scenes, requested):
    """Entry scene: --entry, else story.ks, else the shallowest scene."""
    if requested:
        for s in scenes:
            if s.name == requested or str(s).replace("\\", "/").endswith(requested):
                return s
        raise BuildError(
            "--entry scene not found in project: %s\n  Available: %s"
            % (requested, ", ".join(s.name for s in scenes))
        )
    for s in scenes:
        if s.name == "story.ks":
            return s
    return sorted(scenes, key=lambda s: (len(s.relative_to(project).parts), s.name))[0]


# ---------------------------------------------------------- engine binary --

def find_engine(explicit=None, config=None):
    """Locate the engine binary. Returns Path. Raises BuildError with guidance."""
    searched = []
    if explicit:
        p = Path(explicit)
        if not p.is_absolute():
            p = (Path.cwd() / p).resolve()
        if p.is_dir():
            p = p / _exe_name()
        searched.append(p)
        if p.is_file():
            return p
        raise BuildError(
            "--engine does not point at an engine binary: %s\n"
            "  Expected a file (or a directory containing %s)." % (p, _exe_name())
        )
    if "CAESURA_ENGINE" in os.environ:
        env = os.environ["CAESURA_ENGINE"]
        p = Path(env).resolve()
        if env and p.is_dir():
            p = p / _exe_name()
        if env and p.is_file():
            return p
        raise BuildError("CAESURA_ENGINE does not point at an engine binary: %s"
                         % (env or "<empty>"))
    dirs = list(ENGINE_SEARCH_DIRS)
    if config:
        dirs.insert(0, "build/%s" % config)
        dirs.insert(1, "bin/%s" % config)
    for d in dirs:
        p = ROOT / d / _exe_name()
        if p not in searched:
            searched.append(p)
        if p.is_file():
            return p
    raise BuildError(
        "Engine binary not found -- nothing to build a game around.\n"
        "  Searched (in order):\n%s\n"
        "  This command packages a game AROUND an existing engine binary;\n"
        "  it deliberately does not configure or run a C++ toolchain.\n"
        "  Fix, pick one:\n"
        "    1. Use a release build of the engine:\n"
        "         python scripts/caesura.py build <project> --engine <dir-with-%s>\n"
        "       or set CAESURA_ENGINE=<dir-with-%s>\n"
        "    2. Build the engine once from this checkout:\n"
        "         cmake -B build -DCAESURA_LIVE2D=OFF\n"
        "         cmake --build build --config Release --parallel\n"
        "  Check your toolchain first with:  python scripts/caesura.py doctor"
        % ("\n".join("    " + _rel(s) for s in searched), _exe_name(), _exe_name())
    )


def runtime_libs(engine: Path):
    """Shared libraries sitting beside the engine binary (SDL3, FFmpeg, Steam...)."""
    sufs = (".dll",) if os.name == "nt" else (".so", ".dylib")
    out = []
    for f in sorted(engine.parent.iterdir()):
        if f.is_file() and (f.suffix.lower() in sufs or ".so." in f.name):
            out.append(f)
    return out


# ------------------------------------------------------------------ gating --

def run_ks_check(scenes, lua: str) -> None:
    failures = []
    for s in scenes:
        res = subprocess.run([lua, "scripts/ks_check.lua", _rel(s)],
                             cwd=str(ROOT), capture_output=True, text=True)
        tail = (res.stdout or "") + (res.stderr or "")
        if res.returncode != 0:
            failures.append((s, tail.strip()))
    if failures:
        lines = ["Contract check (ks_check) failed -- refusing to package a broken game:"]
        for s, out in failures:
            lines.append("  %s" % _rel(s))
            for ln in out.splitlines()[:20]:
                lines.append("      " + ln)
        lines.append("  Fix the scene(s) above, or re-run with --skip-check to package anyway.")
        raise BuildError("\n".join(lines))


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capability_catalog_sha256() -> str:
    generated = ROOT / "scripts" / "capability_catalog.lua"
    match = re.search(r'^\s*sha256 = "([0-9a-f]{64})",',
                      generated.read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        raise BuildError("Runtime capability catalog is missing or invalid.")
    source = ROOT / "config" / "runtime-capabilities.json"
    if source.is_file():
        text = source.read_text(encoding="utf-8").replace("\r\n", "\n")
        if hashlib.sha256(text.encode("utf-8")).hexdigest() != match.group(1):
            raise BuildError("Runtime capability catalog is stale; run scripts/generate_runtime_capabilities.py.")
    return match.group(1)


def native_capability_profile(engine: Path) -> dict:
    """Query the selected binary itself, with its identity checked on both sides."""
    before = _file_sha256(engine)
    try:
        result = subprocess.run([str(engine), "--capabilities-json"], cwd=engine.parent,
                                capture_output=True, text=True, encoding="utf-8", timeout=15)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise BuildError("Selected engine capability query failed: " + type(error).__name__) from error
    if _file_sha256(engine) != before:
        raise BuildError("Selected engine changed during the capability query.")
    if result.returncode:
        raise BuildError("Selected engine cannot report capabilities; use a compatible U19-or-later engine binary.")
    try:
        def unique(pairs):
            record = {}
            for key, value in pairs:
                if key in record: raise ValueError("duplicate key")
                record[key] = value
            return record
        profile = json.loads(result.stdout, object_pairs_hook=unique)
        if not isinstance(profile, dict) or profile.get("target") != "native" or profile.get("scope") != "build":
            raise ValueError("not a native build profile")
    except (ValueError, TypeError) as error:
        raise BuildError("Selected engine returned an invalid capability profile.") from error
    profile["binary_sha256"] = before
    profile["binary"] = engine.name
    return profile


def _project_capability_files(project):
    files = {path.relative_to(project).as_posix(): _file_sha256(path)
             for path in _copied_files(project) if path.match("*.ks") or path.match("*.lua")}
    metadata = project / "caesura.project.json"
    if metadata.exists(): files["caesura.project.json"] = _file_sha256(metadata)
    return dict(sorted(files.items()))


def _runtime_capability_files(directory):
    return dict(sorted((path.relative_to(directory).as_posix(), _file_sha256(path))
                       for path in _copied_files(directory) if path.match("*.lua")))


def _capability_inputs(project, scenes):
    project = project.resolve()
    return {"scope": "preflight_inputs", "catalog_sha256": capability_catalog_sha256(),
            "checked_scenes": sorted(scene.resolve().relative_to(project).as_posix() for scene in scenes),
            "scene_inventory": sorted(scene.relative_to(project).as_posix() for scene in collect_scenes(project)),
            "project_files": _project_capability_files(project),
            "runtime_lua": _runtime_capability_files(ROOT / "scripts")}


def _require_capability_inputs(report, project):
    project = project.resolve()
    expected = report.get("inputs")
    if not isinstance(expected, dict) or expected.get("checked_scenes") != expected.get("scene_inventory"):
        raise BuildError("Packaging requires capability checks for the complete project scene set.")
    scenes = collect_scenes(project)
    if _capability_inputs(project, scenes) != expected:
        raise BuildError("Project or runtime inputs changed after capability validation.")


def run_capability_check(project: Path, scenes, target: str, *, engine=None,
                         skip_syntax=False, output=None) -> dict:
    """One real Lua checker owns declarations and statically identifiable calls."""
    project = project.resolve()
    scenes = list(scenes)
    inputs = _capability_inputs(project, scenes)
    expected_digest = capability_catalog_sha256()
    if target == "native":
        profile = native_capability_profile(engine or find_engine())
        if profile.get("catalog_sha256") != expected_digest:
            raise BuildError("Selected engine does not match the runtime capability catalog.")
    elif target == "web":
        profile = {"schema": 1, "target": "web", "platform": "browser", "scope": "build",
                   "catalog_sha256": expected_digest, "compiled": {}}
    else:
        raise BuildError("Capability target must be native or web.")
    with tempfile.TemporaryDirectory(prefix="caesura-capability-check-") as scratch:
        scratch = Path(scratch)
        profile_path = scratch / "profile.json"
        profile_path.write_text(json.dumps(profile), encoding="utf-8")
        metadata = project / "caesura.project.json"
        if not metadata.exists():
            metadata = scratch / "legacy-project.json"
            metadata.write_text("{}\n", encoding="utf-8")
        report_path = scratch / "report.json"
        command = [find_lua(), str(ROOT / "scripts" / "ks_check.lua"),
                   "--target", target, "--profile", str(profile_path),
                   "--project", str(metadata), "--json-output", str(report_path)]
        if skip_syntax: command.append("--capabilities-only")
        command.extend(str(scene.resolve()) for scene in scenes)
        try:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=120)
        except subprocess.TimeoutExpired as error:
            raise BuildError("Capability checker did not finish before its deadline.") from error
        text = (result.stdout or "") + (result.stderr or "")
        if not report_path.is_file():
            raise BuildError("Capability checker did not produce a report:\n" + text[-12000:])
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if _capability_inputs(project, scenes) != inputs:
            raise BuildError("Project or runtime inputs changed during capability validation.")
        report["profile"] = profile
        report["inputs"] = inputs
        for path in sorted(_copied_files(project)):
            if path.match("*.lua"):
                report["not_proven"].append({"reason": "unanalysed_lua_file",
                    "location": {"scene": str(path), "line": 0, "command": "lua"}})
        if output is not None:
            Path(output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if result.returncode != 0 or report.get("passed") is not True:
            raise BuildError("Required target capabilities are not satisfied:\n" + text[-12000:])
        details = "\n".join(line for line in text.splitlines() if not line.startswith("Target capabilities:"))
        if details.strip(): print(details.rstrip())
        print("Target capabilities: PASS; %d unproved dynamic span(s) or Lua file(s)" % len(report["not_proven"]))
        return report


def _packaged_capability_report(report, project):
    """Keep diagnostics useful without shipping the author's absolute paths."""
    result = json.loads(json.dumps(report))
    profile = result.get("profile", {})
    result["profile"] = {key: profile[key] for key in
        ("schema", "target", "platform", "scope", "catalog_sha256", "compiled", "binary_sha256") if key in profile}
    if isinstance(profile.get("binary"), str):
        result["profile"]["binary"] = re.split(r"[/\\]", profile["binary"])[-1]
    def visit(value):
        if isinstance(value, dict):
            if isinstance(value.get("scene"), str) and Path(value["scene"]).is_absolute():
                try: value["scene"] = Path(value["scene"]).resolve().relative_to(project.resolve()).as_posix()
                except ValueError: value["scene"] = "project"
            for child in value.values(): visit(child)
        elif isinstance(value, list):
            for child in value: visit(child)
    visit(result)
    return result


# -------------------------------------------------------------- assembling --

BOOT_TEMPLATE = '''-- ==========================================================================
--  GENERATED by scripts/caesura_build.py — do not edit by hand.
--  Game-only boot shim: runs the author's entry.lua when it can locate its
--  own story, and otherwise starts the packaged entry scene directly.
--  Loaded by src/main.cpp:1204 as (scripts/ + config.entry_script).
-- ==========================================================================

local kag_runner = require("kag_runner")
local layers     = require("layers")

local GAME_ROOT  = %(game_root)s
local STORY_PATH = %(story_path)s
local capabilities = require("capability_runtime")
local capability_ok, capability_error, capability_detail = capabilities.configure_project_file(GAME_ROOT .. "/caesura.project.json", true)
if not capability_ok then
    error("Project capability configuration rejected: " .. (capability_detail and capabilities.message(capability_detail) or capability_error), 0)
end

-- Published for [iscript] blocks and project entry scripts that want to know
-- where they were packaged to.
_G.CAESURA_GAME_ROOT  = GAME_ROOT
_G.CAESURA_STORY_PATH = STORY_PATH

local project_entry = %(project_entry)s
if project_entry and loadfile then
    local chunk, load_err = loadfile(project_entry)
    if chunk then
        local ok, err = pcall(chunk)
        if not ok then
            print("[caesura] project entry.lua raised: " .. tostring(err))
        end
    elseif load_err then
        print("[caesura] project entry.lua not loadable: " .. tostring(load_err))
    end
end

-- The author's entry.lua may have failed to find its story (its search paths
-- are repo-relative). Verify the runner really started; otherwise boot the
-- packaged scene ourselves so the package is never a black window.
if not kag_runner.get_ctx() then
    print("[caesura] booting packaged entry scene: " .. STORY_PATH)
    local started, why = kag_runner.start(STORY_PATH)
    if not started then
        print("[caesura] FATAL: cannot start " .. STORY_PATH .. " (" .. tostring(why) .. ")")
    end
end

-- ---------------------------------------------------------------------------
--  Frame hooks. Engine.cpp calls the GLOBALS engine_update(dt) (Engine.cpp:757)
--  and engine_render() (Engine.cpp:1372) every frame, and _KAG_onClick on
--  input. A project entry.lua is NOT required to define them (neither
--  tests/projects/first_vn/entry.lua nor tools/project_templates/*/entry.lua
--  do -- only demo/entry.lua and scripts/*_demo_entry.lua), and without them
--  the story loads but never advances: a black window with audio silence.
--  Install the standard KAG hooks for whatever the project left undefined.
-- ---------------------------------------------------------------------------

local toast = nil
do
    local ok, mod = pcall(require, "toast")
    if ok then toast = mod end
end

if type(_G.engine_update) ~= "function" then
    function _G.engine_update(dt)
        dt = dt or 0.016
        if toast then pcall(function() toast.update(dt) end) end
        kag_runner.update(dt)
    end
end

if type(_G.engine_render) ~= "function" then
    function _G.engine_render()
        layers.render()
        kag_runner.render()
    end
end

if type(_G._KAG_onClick) ~= "function" then
    function _G._KAG_onClick()
        kag_runner.on_click()
    end
end
'''


def _lua_str(s: str) -> str:
    return '"%s"' % s.replace("\\", "/").replace('"', '\\"')


def patch_runtime_config(config_lua: Path, entry_rel: str, dev_mode: bool) -> None:
    """Re-point config.entry_script (and dev_mode) in the PACKAGED scripts/config.lua.

    Byte-level line replacement on purpose: the file mixes UTF-8 with mojibake
    box-drawing comments (scripts/config.lua:1), so a decode/encode round trip
    would corrupt it. Only the two ASCII assignment lines are touched.
    """
    data = config_lua.read_bytes()
    new_entry = ('config.entry_script = "%s"  -- caesura build (game-only)' % entry_rel).encode("utf-8")
    data, n = re.subn(rb"(?m)^config\.entry_script\s*=.*$", new_entry.replace(b"\\", b"\\\\"), data, count=1)
    if n != 1:
        raise BuildError(
            "Cannot re-point config.entry_script in %s (expected exactly one "
            "assignment, found %d). The engine runtime layout changed; update "
            "scripts/caesura_build.py." % (_rel(config_lua), n)
        )
    flag = b"true" if dev_mode else b"false"
    data, n2 = re.subn(rb"(?m)^config\.dev_mode\s*=[^\r\n]*",
                       b"config.dev_mode = " + flag +
                       b"  -- caesura build (" + (b"dev" if dev_mode else b"release") + b")",
                       data, count=1)
    if n2 != 1:
        raise BuildError("Cannot set config.dev_mode in %s (found %d assignments)."
                         % (_rel(config_lua), n2))
    config_lua.write_bytes(data)


# Attributes that name an asset in a KAG token, per docs/api/command-contracts.md
# (storage= for bg/playbgm/playse/image, sprite= for [ch], file= for video/SMA).
ASSET_ATTR_RE = re.compile(r'\b(storage|sprite|file)\s*=\s*"([^"]+)"')


def scan_asset_refs(scenes):
    """Every distinct STATIC asset path the scenes reference (source order, deduped).

    Paths carrying a macro parameter (%name%) or a brace interpolation (${...})
    are resolved at runtime, not at build time -- e.g.
    demo/example_game/story.ks:16 has storage="assets/bg/%bg%" inside a
    [macro]. Reporting those as "missing" would be a false alarm, so they are
    excluded here and surfaced separately by scan_dynamic_asset_refs.
    """
    refs, seen = [], set()
    for s in scenes:
        try:
            text = s.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for _attr, ref in ASSET_ATTR_RE.findall(text):
            ref = ref.replace("\\", "/")
            if not ref or ref in seen or ".." in ref:
                continue
            if "%" in ref or "${" in ref:
                continue
            seen.add(ref)
            refs.append(ref)
    return refs


def scan_dynamic_asset_refs(scenes):
    """Asset references whose path is computed at runtime (macro arg / ${expr})."""
    out, seen = [], set()
    for s in scenes:
        try:
            text = s.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for _attr, ref in ASSET_ATTR_RE.findall(text):
            ref = ref.replace("\\", "/")
            if ("%" in ref or "${" in ref) and ref not in seen:
                seen.add(ref)
                out.append(ref)
    return out


def resolve_referenced_assets(refs, project: Path, out: Path):
    """Copy every referenced asset that is not in the package yet from the repo
    shared pool. Returns (copied, missing).

    Without this, a project that references the shared pool (every stock
    template does: tools/project_templates/basic/story.ks:57 uses
    assets/bg/hana.png, which does NOT exist under the template's own assets/)
    would ship with holes and render placeholders instead of art.
    """
    copied, missing = [], []
    for ref in refs:
        dst = out / ref
        if dst.exists():
            continue
        for src in (project / ref, ROOT / ref):
            if src.is_file():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
                copied.append(ref)
                break
        else:
            missing.append(ref)
    return copied, missing


# Developer-only files that live in scripts/ but are never loaded by the engine
# runtime (build/CI helpers). A player package has no use for them.
DEV_SCRIPT_SUFFIXES = {".py", ".sh", ".bat", ".mjs", ".vdf", ".bak", ".pyc", ".pyo"}


def prune_dev_scripts(scripts_dir: Path) -> int:
    removed = 0
    for p in sorted(scripts_dir.rglob("*")):
        if p.is_dir():
            continue
        if p.suffix.lower() in DEV_SCRIPT_SUFFIXES:
            p.unlink()
            removed += 1
    for d in sorted(scripts_dir.rglob("*"), reverse=True):
        if d.is_dir() and not any(d.iterdir()):
            d.rmdir()
    return removed


# Build-time scene precompile. Run INSIDE the package with the vendored Lua so
# cache/ksc/<scene>.ksc exists before the player's first launch.
#
# Why this is load-bearing, not an optimization: the engine caps Lua at
# 20,000,000 instructions per budget window (src/script/vm/LuaManager.cpp:63),
# reset once before the entry script (src/main.cpp:1207) and once per frame
# (src/entry/Engine.cpp:1023). A cold tokenize+compile of a large scene runs
# inside that single startup window, so a big story dies at boot with
# "Sandbox: instruction budget exceeded". Empirically reproduced with
# demo/example_game/story.ks (454 lines) -- the package booted only after the
# .ksc cache existed. flow.load_scene keys freshness on a content hash
# (scripts/flow.lua:47), so a precompiled cache stays valid until the scene is
# edited, and a stale one merely costs a recompile.
PRECOMPILE_LUA = '''-- generated by scripts/caesura_build.py (build-time scene precompile)
package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;" .. package.path
local report = assert(io.open("caesura-precompile.results", "w"))
local loaded, flow = pcall(require, "flow")
local compiled, compiler = pcall(require, "kag.compiler")
local function verify(scene)
    if not loaded or not compiled then return "SKIP", "package runtime unavailable" end
    local s, err = flow.load_scene(scene)
    if not s then return "FAIL", "scene load failed: " .. tostring(err) end
    if type(s.tokens) ~= "table" or type(s.tokens._compiled) ~= "table" then
        return "FAIL", "scene did not produce compiled tokens"
    end
    local compatible, reason = compiler.isCompatible(s.tokens)
    if not compatible then return "INCOMPATIBLE", "compiled tokens: " .. tostring(reason) end

    -- Successful source execution can hide a failed optional cache write.
    -- Read the actual file and deserialize with THIS packaged runtime; do not
    -- accept a successful load_scene or an in-memory compiler cache as proof.
    local path = s.path or scene
    local cache = "cache/ksc/" .. path:gsub("[/\\\\]+", "_"):gsub("%%.ks$", ".ksc")
    local file = io.open(cache, "rb")
    if not file then return "FAIL", "cache file unavailable" end
    local read, text = pcall(file.read, file, "*a")
    pcall(file.close, file)
    if not read or type(text) ~= "string" or text == "" then
        return "FAIL", "cache file empty or unreadable"
    end
    local chunk = load(text, "=package-cache", "t", {})
    if not chunk then return "FAIL", "cache file incomplete or malformed" end
    local decoded, data = pcall(chunk)
    if not decoded or type(data) ~= "table" then return "FAIL", "cache file malformed" end
    local tokens, why = compiler.deserialize(data)
    if not tokens then
        if why == "compiled-stream-shape" then return "FAIL", why end
        return "INCOMPATIBLE", "disk cache: " .. tostring(why)
    end
    compatible, reason = compiler.isCompatible(tokens)
    if not compatible then return "INCOMPATIBLE", "disk cache: " .. tostring(reason) end
    local hash = compiler.hashFile(path)
    if not hash then return "FAIL", "source hash unavailable" end
    if tokens._compiled._srcHash ~= hash then return "INCOMPATIBLE", "source-hash-mismatch" end
    return "OK", ""
end
local failed = 0
for index, scene in ipairs({%(scenes)s}) do
    local checked, status, reason = pcall(verify, scene)
    if not checked then status, reason = "FAIL", tostring(status) end
    reason = tostring(reason):gsub("[\\r\\n\\t]", " ")
    assert(report:write(tostring(index), "\\t", status, "\\t", reason, "\\n"))
    assert(report:flush())
    if status ~= "OK" then failed = failed + 1 end
end
assert(report:close())
os.exit(failed == 0 and 0 or 1)
'''


_PATH_SHAPED = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\|/)[^\s'\"]{2,}")


def neutralize_paths(text):
    """Strip anything path-shaped out of a message bound for BUILD-INFO.

    BUILD-INFO ships to players, so no field may name the build machine's
    directory layout (N3). Subprocess and timeout errors embed the command
    line they failed on, so scrub rather than trust the message text.
    """
    return _PATH_SHAPED.sub("<path>", text) if text else text


def precompile_scenes(out: Path, scene_rels):
    """Verify caches with the packaged runtime; explicit incompatibility is fatal."""
    try:
        lua = find_lua()
    except BuildError as e:
        if "CAESURA_LUA" in os.environ:
            raise
        return [], [], "skipped (%s)" % str(e).splitlines()[0]
    lua_abs = str(Path(lua).resolve()) if Path(lua).exists() else lua
    script = out / "caesura-precompile.lua"
    report = out / "caesura-precompile.results"
    report.unlink(missing_ok=True)
    script.write_text(
        PRECOMPILE_LUA % {"scenes": ", ".join(_lua_str(s) for s in scene_rels)},
        encoding="utf-8", newline="\n")
    result_text = ""
    try:
        res = subprocess.run([lua_abs, "caesura-precompile.lua"], cwd=str(out),
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", timeout=600)
        if report.is_file():
            result_text = report.read_text(encoding="utf-8")
    except (OSError, subprocess.SubprocessError) as exc:
        if "CAESURA_LUA" in os.environ:
            raise BuildError("CAESURA_LUA precompile execution failed: %s" % exc) from exc
        return [], [], "skipped (%s)" % exc
    finally:
        script.unlink(missing_ok=True)
        report.unlink(missing_ok=True)
    # (t24) the old compiler.lua Windows 'mkdir -p' defect was fixed in
    # b38ac5de (if not exist branch); the cleanup below was removed and the
    # absence of a stray '-p' directory is now locked by
    # test_no_stray_dash_p_directory_in_package instead.

    # A dedicated report carries one result per requested index. Runtime logs
    # are not evidence, and missing/duplicate results never mean success.
    records, invalid_report = {}, False
    incompatible = []
    for line in result_text.splitlines():
        fields = line.split("\t", 2)
        if len(fields) != 3 or not fields[0].isdigit():
            invalid_report = True
            continue
        index, status, reason = int(fields[0]), fields[1], fields[2]
        if status == "INCOMPATIBLE":
            scene = scene_rels[index - 1] if 1 <= index <= len(scene_rels) else "unknown scene"
            incompatible.append("%s %s" % (scene, reason))
        if not 1 <= index <= len(scene_rels) or index in records \
                or status not in {"OK", "FAIL", "SKIP", "INCOMPATIBLE"}:
            invalid_report = True
            continue
        records[index] = (status, reason)
    if incompatible:
        raise BuildError("Precompile found incompatible cache/runtime -- refusing to package:\n  "
                         + "\n  ".join(incompatible))
    ok_list, fail_list = [], []
    for index, scene in enumerate(scene_rels, 1):
        status, reason = records.get(index, ("FAIL", "scene cache was not verified"))
        if status == "OK" and not invalid_report:
            ok_list.append(scene)
        else:
            fail_list.append("%s %s" % (scene, neutralize_paths(
                "invalid precompile report" if invalid_report else reason)))
    note = "" if res.returncode == 0 else "lua exited %d" % res.returncode
    if records and len(records) == len(scene_rels) \
            and all(status == "SKIP" for status, _ in records.values()):
        note = "skipped (package runtime unavailable)"
    return ok_list, fail_list, note


def assemble(project: Path, entry_scene: Path, engine: Path, out: Path,
             shared_assets: bool, dev_mode: bool, quiet=False, capabilities=None) -> dict:
    def say(msg):
        if not quiet:
            print(msg)

    game_name = project.name
    _prepare_out(out)
    out.mkdir(parents=True)

    # 1. engine binary + every runtime lib beside it (no manual DLL copying
    #    is ever asked of the player — that is the whole point of game-only).
    shutil.copy2(engine, out / engine.name)
    if capabilities is not None and _file_sha256(out / engine.name) != capabilities["profile"]["binary_sha256"]:
        raise BuildError("Packaged engine differs from the capability-checked binary.")
    libs = runtime_libs(engine)
    for lib in libs:
        shutil.copy2(lib, out / lib.name)
    say("[build] engine: %s (+%d runtime lib%s)"
        % (engine.name, len(libs), "" if len(libs) == 1 else "s"))

    # 2. engine Lua runtime
    _copy_tree(ROOT / "scripts", out / "scripts")
    if capabilities is not None and _runtime_capability_files(out / "scripts") != capabilities["inputs"]["runtime_lua"]:
        raise BuildError("Copied runtime differs from the capability-checked inputs.")
    for junk in ("game_logic.lua",):
        p = out / "scripts" / junk
        if p.exists():
            p.unlink()
    pruned = prune_dev_scripts(out / "scripts")
    say("[build] runtime: scripts/ (%d dev-only file%s pruned)"
        % (pruned, "" if pruned == 1 else "s"))

    # 3. assets — engine-required, then the game's own, then (opt-in) the pool
    (out / "assets").mkdir(exist_ok=True)
    for sub in ENGINE_REQUIRED_ASSETS:
        src = ROOT / "assets" / sub
        if src.is_dir():
            _copy_tree(src, out / "assets" / sub)
    if shared_assets:
        _copy_tree(ROOT / "assets", out / "assets")
        say("[build] assets: engine-required + repo shared pool (--with-shared-assets)")
    else:
        say("[build] assets: engine-required (%s) + game assets"
            % ", ".join(ENGINE_REQUIRED_ASSETS))
    proj_assets = project / "assets"
    if proj_assets.is_dir():
        _copy_tree(proj_assets, out / "assets")

    # 4. the game itself, under the sandbox-allowlisted projects/ root
    game_root = "projects/%s" % game_name
    _copy_tree(project, out / "projects" / game_name)
    if capabilities is not None and _project_capability_files(out / "projects" / game_name) != capabilities["inputs"]["project_files"]:
        raise BuildError("Copied project differs from the capability-checked inputs.")
    story_rel = "%s/%s" % (game_root, entry_scene.relative_to(project).as_posix())
    say("[build] game: %s (entry scene %s)" % (game_root, story_rel))

    # 4b. assets the scenes actually reference but the project does not own
    #     (stock templates reference the repo shared pool by design).
    scenes = collect_scenes(project)
    refs = scan_asset_refs(scenes)
    copied, missing = resolve_referenced_assets(refs, project, out)
    dynamic = scan_dynamic_asset_refs(scenes)
    say("[build] referenced assets: %d static (%d pulled from repo pool, %d missing)%s"
        % (len(refs), len(copied), len(missing),
           ", %d runtime-computed" % len(dynamic) if dynamic else ""))
    if missing:
        for m in missing[:8]:
            say("[build]   WARN missing asset (engine will draw a placeholder): %s" % m)
    if dynamic and not shared_assets:
        # A macro/expression path cannot be resolved statically: whatever the
        # author passes at runtime must already be in the package.
        say("[build]   NOTE runtime-computed asset path(s) -- verify their targets ship: %s"
            % ", ".join(dynamic[:5]))

    # 5. generated boot shim + re-pointed runtime config
    project_entry = "%s/entry.lua" % game_root
    has_entry = (out / "projects" / game_name / "entry.lua").is_file()
    boot_rel = "%s/caesura-boot.lua" % game_root
    (out / boot_rel).write_text(BOOT_TEMPLATE % {
        "game_root": _lua_str(game_root),
        "story_path": _lua_str(story_rel),
        "project_entry": _lua_str(project_entry) if has_entry else "nil",
    }, encoding="utf-8", newline="\n")
    patch_runtime_config(out / "scripts" / "config.lua", "../" + boot_rel, dev_mode)
    say("[build] boot: scripts/config.lua -> ../%s (dev_mode=%s)"
        % (boot_rel, "true" if dev_mode else "false"))

    # 6. writable runtime dirs (the engine writes saves/settings/logs and the
    #    KAG compiler caches bytecode under cache/ksc)
    for d in ("cache/ksc", "saves", "settings", "logs"):
        (out / d).mkdir(parents=True, exist_ok=True)

    # 6b. precompile every scene into cache/ksc (see PRECOMPILE_LUA: a cold
    #     compile of a large scene blows the startup Lua instruction budget).
    scene_rels = ["%s/%s" % (game_root, p.relative_to(project).as_posix()) for p in scenes]
    pre_ok, pre_fail, pre_note = precompile_scenes(out, scene_rels)
    if pre_fail or pre_note:
        if pre_note.startswith("skipped"):
            # N5: honest SKIP -- no lua interpreter (or subprocess error). The
            # player pays a cold compile at first boot instead; say so, and
            # record it in BUILD-INFO rather than a single silent WARN line.
            say("[build] precompile: SKIPPED (%s) -- %d/%d scene(s) not pre-cached; "
                "the player pays a cold compile at boot"
                % (pre_note, len(pre_ok), len(scene_rels)))
        else:
            say("[build] precompile: %d/%d scene(s) cached%s"
                % (len(pre_ok), len(scene_rels), (" -- " + pre_note) if pre_note else ""))
        for f in pre_fail[:5]:
            say("[build]   WARN scene did not precompile: %s" % f)
        pre_status = "skipped" if pre_note.startswith("skipped") else "partial"
        pre_reason = neutralize_paths(
            pre_note or ("%d scene(s) failed" % len(pre_fail)))
    else:
        say("[build] precompile: %d/%d scene(s) cached into cache/ksc"
            % (len(pre_ok), len(scene_rels)))
        pre_status, pre_reason = "ok", ""

    # 7. provenance + player note
    # Only claim a checkout build when the binary actually came from this
    # tree: --engine may point at a CI artifact or another machine's build,
    # and a fixed label would state a falsehood about the package's origin.
    try:
        engine.resolve().relative_to(ROOT)
        engine_origin = "local checkout build"
    except ValueError:
        engine_origin = "prebuilt engine binary"
    info = {
        "kind": "caesura-game-only",
        "schema": 1,
        "game": game_name,
        "entry_scene": story_rel,
        "boot_script": boot_rel,
        "engine_binary": engine.name,
        # N3: provenance must stay neutral -- engine_source used to carry the
        # build machine's absolute checkout path into the player package.
        # No BUILD-INFO field may contain an absolute path (either slash form).
        "engine_origin": engine_origin,
        "engine_modified_utc": datetime.datetime.fromtimestamp(
            engine.stat().st_mtime, datetime.timezone.utc).isoformat(),
        "runtime_libs": [l.name for l in libs],
        "scenes": sorted(p.relative_to(project).as_posix() for p in scenes),
        "dev_mode": dev_mode,
        "shared_assets": shared_assets,
        "precompiled_scenes": pre_ok,
        "precompile_failures": pre_fail,
        "precompile": {"status": pre_status, "reason": pre_reason,
                       "scene_count": len(pre_ok)},
        "asset_refs": len(refs),
        "assets_pulled_from_repo": copied,
        "assets_missing": missing,
        "assets_runtime_computed": dynamic,
        "host": {"os": platform.system(), "machine": platform.machine(),
                 "python": platform.python_version()},
    }
    if capabilities is not None:
        info["capabilities"] = _packaged_capability_report(capabilities, project)
    (out / "BUILD-INFO.json").write_text(
        json.dumps(info, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    (out / "HOW-TO-PLAY.txt").write_text(
        "%s -- Caesura (AmeKAG)\n\n"
        "Double-click %s to play. Everything needed is in this folder;\n"
        "no installation, no separate runtime, no manual DLL copying.\n\n"
        "Saves are written to saves/, settings to settings/, logs to logs/.\n"
        "Build provenance: BUILD-INFO.json\n" % (game_name, engine.name),
        encoding="utf-8", newline="\n")
    return info


# ------------------------------------------------------------------ public --

def cmd_build(args) -> int:
    try:
        project = resolve_project(args.project)
        scenes = collect_scenes(project)
        entry_scene = pick_entry_scene(project, scenes, getattr(args, "entry", None))
        engine = find_engine(getattr(args, "engine", None), getattr(args, "config", None))
        out = Path(args.out) if args.out else (ROOT / "dist" / ("%s-game" % project.name))
        if not out.is_absolute():
            out = (Path.cwd() / out).resolve()
        capabilities = run_capability_check(project, scenes, "native", engine=engine,
                                             skip_syntax=args.skip_check)
        if not args.skip_check:
            print("[build] ks_check: %d scene(s) pass contracts" % len(scenes))
        else:
            print("[build] ks_check: SKIPPED (--skip-check)")
        info = _assemble_clean(project, entry_scene, engine, out,
                               shared_assets=args.with_shared_assets,
                               dev_mode=args.dev, capabilities=capabilities)
    except (BuildError, OSError) as e:
        # OSError: assembly I/O failure (disk full, source vanished, ...).
        # _assemble_clean already removed the partial output (t19/A2); report
        # the error plainly instead of a traceback.
        print("caesura build: %s" % e, file=sys.stderr)
        return 1
    print("")
    print("=" * 66)
    print("  GAME-ONLY BUILD COMPLETE -> %s" % out)
    print("    play:   %s" % (out / info["engine_binary"]))
    print("    scenes: %d   entry: %s" % (len(info["scenes"]), info["entry_scene"]))
    print("    package it:  python scripts/caesura.py package %s" % args.project)
    print("=" * 66)
    return 0


def _zip_dir(src: Path, zip_path: Path) -> int:
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    if zip_path.exists():
        zip_path.unlink()
    top = zip_path.name
    for suf in (".zip",):
        if top.endswith(suf):
            top = top[: -len(suf)]
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(src):
            dirs[:] = [d for d in dirs if d not in PRUNE_DIR_NAMES]
            for f in files:
                full = Path(root) / f
                zf.write(full, str(Path(top) / full.relative_to(src)))
    return zip_path.stat().st_size


def cmd_package(args) -> int:
    targets = ["windows", "web"] if args.target == "both" else [args.target]
    if args.target == "auto":
        targets = ["windows"] if os.name == "nt" else ["web"]
    produced = []
    try:
        project = resolve_project(args.project)
    except BuildError as e:
        print("caesura package: %s" % e, file=sys.stderr)
        return 1
    out_dir = Path(args.out) if args.out else (ROOT / "dist")
    if not out_dir.is_absolute():
        out_dir = (Path.cwd() / out_dir).resolve()

    for target in targets:
        if target == "windows":
            # Desktop game-only ZIP: build the directory, then archive it.
            stage = out_dir / ("%s-game" % project.name)
            build_args = type("A", (), {
                "project": str(project), "out": str(stage),
                "engine": getattr(args, "engine", None),
                "config": getattr(args, "config", None),
                "entry": getattr(args, "entry", None),
                "skip_check": args.skip_check,
                "with_shared_assets": args.with_shared_assets,
                "dev": args.dev,
            })()
            rc = cmd_build(build_args)
            if rc != 0:
                return rc
            tag = "win64" if os.name == "nt" else platform.system().lower()
            zip_path = out_dir / ("%s-%s.zip" % (project.name, tag))
            size = _zip_dir(stage, zip_path)
            produced.append((zip_path, size))
            print("[package] windows game-only ZIP: %s (%d bytes)" % (zip_path, size))
        elif target == "web":
            # Reuse the existing, verified Web pipeline verbatim.
            web_out = out_dir / ("%s-web" % project.name)
            zip_path = out_dir / ("%s-web.zip" % project.name)
            cmd = [find_node(), "scripts/package_game.mjs", _rel(project),
                   "--out", _rel(web_out), "--zip", _rel(zip_path)]
            if args.skip_check: cmd.append("--skip-check")
            # flush: the child writes straight to the inherited handles, so an
            # unflushed announcement would print AFTER its own output.
            print("[package] web: %s" % " ".join(cmd), flush=True)
            res = subprocess.run(cmd, cwd=str(ROOT))
            if res.returncode != 0:
                print("caesura package: web packaging failed (scripts/package_game.mjs "
                      "exited %d -- see its output above)." % res.returncode, file=sys.stderr)
                return res.returncode
            if zip_path.exists():
                produced.append((zip_path, zip_path.stat().st_size))
        else:
            print("caesura package: unknown target '%s' (windows|web|both|auto)" % target,
                  file=sys.stderr)
            return 1

    print("")
    print("=" * 66)
    print("  PACKAGE COMPLETE (%d artifact%s)" % (len(produced), "" if len(produced) == 1 else "s"))
    for p, s in produced:
        print("    %s  (%.2f MB)" % (p, s / (1024 * 1024)))
    print("=" * 66)
    return 0
