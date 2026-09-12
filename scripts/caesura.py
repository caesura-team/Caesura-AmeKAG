#!/usr/bin/env python3
"""
Caesura (AmeKAG) — Creator Unified Command Line Interface (CLI)
Unified interface for project scaffolding, doctor diagnostics, story flow, and i18n.
"""

import sys, os, json, subprocess, shutil, argparse
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from caesura_build import cmd_build, cmd_package  # noqa: E402
import caesura_build

def find_lua():
    # Anchored at the CLI's own root (checkout or extracted package), not the
    # CWD, mirroring caesura_build.find_lua: external/lua/ ships only in the
    # release package (gitignored in a checkout), while a built checkout has
    # the lua_cli product at build/lua/<config>/lua[.exe] (where CI looks too).
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates = [
        os.path.join(root, 'external', 'lua', 'lua.exe'),
        os.path.join(root, 'external', 'lua', 'lua'),
    ]
    for cfg in ('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel', ''):
        base = os.path.join(root, 'build', 'lua', cfg) if cfg else os.path.join(root, 'build', 'lua')
        candidates.append(os.path.join(base, 'lua.exe'))
        candidates.append(os.path.join(base, 'lua'))
    for c in candidates:
        if os.path.exists(c) and (os.access(c, os.X_OK) or os.name == 'nt'):
            return c
    return 'lua'

def cmd_doctor(args):
    print("=== Caesura Environment Doctor ===\n")
    tools = [
        ("Python 3", [sys.executable, "--version"], "Required for creator toolchain"),
        ("Lua 5.4", [find_lua(), "-v"], "Core script runtime"),
        # On Windows npm is npm.cmd; a bare "npm" is not resolved by
        # subprocess without shell=True, so doctor reported it missing on every
        # Windows machine that had it (an explicit reproduction of the
        # FileNotFoundError). Resolve both forms.
        ("Node.js / npm",
         [shutil.which("npm") or shutil.which("npm.cmd") or "npm", "--version"],
         "Web player test & packaging"),
        ("CMake", ["cmake", "--version"], "C++ build system"),
        ("Git", ["git", "--version"], "Version control"),
    ]
    
    passed = 0
    for name, cmd, desc in tools:
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            if res.returncode == 0:
                first_line = (res.stdout or res.stderr).strip().splitlines()[0]
                print(f"  [OK] {name:<16} : {first_line}")
                passed += 1
            else:
                print(f"  [WARN] {name:<14} : Exited with code {res.returncode}")
        except Exception as e:
            print(f"  [FAIL] {name:<14} : Not found ({desc})")
            
    print(f"\nDoctor check complete: {passed}/{len(tools)} critical tools ready.")
    return 0

def find_templates_root():
    """Resolve tools/project_templates/ by anchoring on THIS SCRIPT (the CLI
    executable), mirroring main.cpp's SDL_GetBasePath webRoot logic:
      1. the directory next to the script -- a release package puts scripts/
         at the package root next to tools/, and a checkout does the same;
      2. fall back to a walk up from the CWD (in-tree / build-dir workflows).
    Returns an absolute path, or None when no templates tree is found.
    """
    seen = set()

    def probe(start):
        p = os.path.abspath(start)
        while p and p not in seen:
            seen.add(p)
            candidate = os.path.join(p, "tools", "project_templates")
            if os.path.isdir(candidate):
                return os.path.abspath(candidate)
            parent = os.path.dirname(p)
            if parent == p:
                break
            p = parent
        return None

    script_dir = os.path.dirname(os.path.abspath(__file__))
    found = probe(script_dir)
    if found is None:
        found = probe(os.getcwd())
    return found


def cmd_create(args):
    name = args.name
    template = args.template or "showcase"
    target_dir = args.out or name

    templates_root = find_templates_root()
    template_src = None
    if templates_root is not None:
        candidate = os.path.join(templates_root, template)
        if os.path.isdir(candidate):
            template_src = candidate
    if template_src is None and template == "showcase":
        # Legacy in-checkout fallback: demo/example_game doubles as the
        # showcase template when no project_templates tree is around.
        # N4: anchor on THIS SCRIPT, not the CWD -- a non-repo-root CWD must
        # not silently disable the fallback (the old CWD-relative
        # os.path.join('demo', 'example_game') could never find it from
        # anywhere but the repo root).
        legacy = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "demo", "example_game")
        if os.path.isdir(legacy):
            template_src = legacy
    if template_src is None:
        where = templates_root if templates_root else "tools/project_templates"
        print(f"Error: template '{template}' not found (searched: {where})",
              file=sys.stderr)
        return 1

    if os.path.exists(target_dir):
        print(f"Error: target directory '{target_dir}' already exists.", file=sys.stderr)
        return 1

    shutil.copytree(template_src, target_dir)

    # M1-L: post-process caesura.project.json metadata. Pure copytree left
    # name=template id and empty created/modified, so a Studio New Project
    # showed no trace of the chosen name. Missing/corrupt manifest degrades
    # to a WARN and continues -- create must not fail on a template without
    # one (the legacy demo/example_game fallback has no manifest).
    project_json = os.path.join(target_dir, "caesura.project.json")
    try:
        with open(project_json, "r", encoding="utf-8") as f:
            meta = json.load(f)
        meta["name"] = args.project_name or os.path.basename(os.path.abspath(target_dir))
        if args.description is not None:
            meta["description"] = args.description
        now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        meta["created"] = now
        meta["modified"] = now
        with open(project_json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(meta, f, indent=2, ensure_ascii=False)
            f.write("\n")
    except FileNotFoundError:
        print(f"[WARN] caesura.project.json not found after copy: {project_json}")
    except (json.JSONDecodeError, OSError) as warn:
        print(f"[WARN] caesura.project.json unreadable; left as-copied: {warn}")

    print(f"[OK] Project '{name}' created from template '{template}' at: {target_dir}")
    print(f"     (template from: {template_src})")
    return 0

def cmd_flow(args):
    lua = find_lua()
    cmd = [lua, "scripts/kag_semantic.lua", "flow", args.path, "--format", args.format]
    if args.lint:
        cmd.append("--lint")
    if args.out:
        cmd.extend(["-o", args.out])
    res = subprocess.run(cmd)
    return res.returncode

def cmd_i18n(args):
    target = args.path
    if args.extract:
        out_csv = args.out or os.path.join(target, "i18n", "locales.csv")
        os.makedirs(os.path.dirname(out_csv), exist_ok=True)
        res = subprocess.run([sys.executable, "scripts/extract_i18n.py", target, "--out", out_csv])
        return res.returncode
    elif args.lint:
        dict_dir = args.dir or (os.path.join(target, "i18n") if os.path.isdir(target) else "i18n")
        res = subprocess.run([sys.executable, "scripts/lint_i18n.py", "--dir", dict_dir])
        return res.returncode
    else:
        # Full extraction
        out_csv = args.out or os.path.join(target, "locales.csv")
        res = subprocess.run([sys.executable, "scripts/extract_i18n.py", target, "--out", out_csv])
        return res.returncode

def cmd_check(args):
    try:
        path = Path(args.path).resolve()
        if not args.target:
            if args.engine or args.json_output:
                raise caesura_build.BuildError("--engine and --json-output require --target for check.")
            res = subprocess.run([caesura_build.find_lua(), str(caesura_build.ROOT / "scripts/ks_check.lua"), str(path)])
            return res.returncode
        if path.is_dir():
            project, scenes = path, caesura_build.collect_scenes(path)
        else:
            project, scenes = path.parent, [path]
            for parent in path.parents:
                if (parent / "caesura.project.json").is_file():
                    project = parent
                    break
        engine = caesura_build.find_engine(args.engine) if args.target == "native" else None
        caesura_build.run_capability_check(project, scenes, args.target, engine=engine, output=args.json_output)
        return 0
    except (caesura_build.BuildError, OSError) as error:
        print("caesura check: " + str(error), file=sys.stderr)
        return 1

def cmd_patch(args):
    raw_args = args.patch_args
    if not raw_args:
        print("Usage: caesura patch <base.carc> <target.carc> <delta.carc>")
        print("       caesura patch create <base.carc> <target.carc> <delta.carc>")
        print("       caesura patch apply <base.carc> <delta.carc> <output.carc>")
        print("       caesura patch verify <delta.carc>")
        return 1

    first = raw_args[0]
    if first == "create" and len(raw_args) >= 4:
        cmd = [sys.executable, "scripts/carc_pack.py", "delta", raw_args[1], raw_args[2], raw_args[3]]
    elif first == "apply" and len(raw_args) >= 4:
        cmd = [sys.executable, "scripts/carc_pack.py", "apply", raw_args[1], raw_args[2], raw_args[3]]
    elif first == "verify" and len(raw_args) >= 2:
        cmd = [sys.executable, "scripts/carc_pack.py", "verify-delta", raw_args[1]]
    elif len(raw_args) == 3:
        # Default: <base.carc> <target.carc> <delta.carc>
        cmd = [sys.executable, "scripts/carc_pack.py", "delta", raw_args[0], raw_args[1], raw_args[2]]
    else:
        print(f"Error: invalid patch arguments: {' '.join(raw_args)}", file=sys.stderr)
        return 1

    res = subprocess.run(cmd)
    return res.returncode

def main():
    parser = argparse.ArgumentParser(prog="caesura", description="Caesura (AmeKAG) Creator CLI")
    subparsers = parser.add_subparsers(dest="command", required=True)
    
    # doctor
    p_doc = subparsers.add_parser("doctor", help="Inspect local development environment")
    p_doc.set_defaults(func=cmd_doctor)
    
    # create
    p_create = subparsers.add_parser("create", help="Create a new visual novel project")
    p_create.add_argument("name", help="Project name")
    p_create.add_argument("--template", choices=["showcase", "basic", "blank", "live2d", "kag3"], default="showcase")
    p_create.add_argument("-o", "--out", help="Target output directory")
    p_create.add_argument("--name", dest="project_name", default=None,
                         help="Display name for caesura.project.json (default: target directory basename)")
    p_create.add_argument("--description", default=None,
                         help="Optional project description stored in caesura.project.json")
    p_create.set_defaults(func=cmd_create)
    
    # flow
    p_flow = subparsers.add_parser("flow", help="Generate story flow graph & diagnostics")
    p_flow.add_argument("path", help="Path to .ks scene or project directory")
    p_flow.add_argument("--format", choices=["mermaid", "json", "markdown"], default="mermaid")
    p_flow.add_argument("--lint", action="store_true", help="Run broken jump / orphan diagnostics")
    p_flow.add_argument("-o", "--out", help="Output file path")
    p_flow.set_defaults(func=cmd_flow)
    
    # i18n
    p_i18n = subparsers.add_parser("i18n", help="Manage localization pipeline")
    p_i18n.add_argument("path", help="Path to .ks scene or project directory")
    p_i18n.add_argument("--extract", action="store_true", help="Extract translatables to CSV")
    p_i18n.add_argument("--lint", action="store_true", help="Audit translation coverage")
    p_i18n.add_argument("--dir", help="Directory containing <lang>.json dictionaries for linting")
    p_i18n.add_argument("-o", "--out", help="Output CSV path")
    p_i18n.set_defaults(func=cmd_i18n)
    
    # check
    p_check = subparsers.add_parser("check", help="Statically validate .ks contracts")
    p_check.add_argument("path", help="Path to .ks scene, or project directory with --target")
    p_check.add_argument("--target", choices=["native", "web"], help="Check declared and statically identifiable target capabilities")
    p_check.add_argument("--engine", help="Selected engine binary for native capability checks")
    p_check.add_argument("--json-output", help="Write the capability report to this file")
    p_check.set_defaults(func=cmd_check)
    
    # patch
    p_patch = subparsers.add_parser("patch", help="Differential CARC patch creation, application, and verification")
    p_patch.add_argument("patch_args", nargs="*", help="Arguments: <base.carc> <target.carc> <delta.carc> or create|apply|verify ...")
    p_patch.set_defaults(func=cmd_patch)

    # build — game-only desktop directory (see scripts/caesura_build.py)
    p_build = subparsers.add_parser(
        "build", help="Assemble a game-only, double-click-runnable desktop build")
    p_build.add_argument("project", help="Project directory or name (e.g. basic, tests/projects/first_vn)")
    p_build.add_argument("-o", "--out", help="Output directory (default dist/<project>-game)")
    p_build.add_argument("--engine", help="Engine binary or the directory containing it "
                                         "(default: search build/, bin/; env CAESURA_ENGINE)")
    p_build.add_argument("--config", choices=["Debug", "Release", "RelWithDebInfo"],
                         help="Prefer this build configuration when searching for the engine")
    p_build.add_argument("--entry", help="Entry scene (default story.ks)")
    p_build.add_argument("--skip-check", action="store_true",
                         help="Skip ordinary scene lint; required capability checks still run")
    p_build.add_argument("--with-shared-assets", action="store_true",
                         help="Also ship the repo shared asset pool (assets/), not just what the game needs")
    p_build.add_argument("--dev", action="store_true",
                         help="Keep config.dev_mode = true (default: release, strict sandbox)")
    p_build.set_defaults(func=cmd_build)

    # package — distributable archives (desktop ZIP / web bundle)
    p_pkg = subparsers.add_parser(
        "package", help="Package a project into distributable archives (desktop ZIP / web bundle)")
    p_pkg.add_argument("project", help="Project directory or name")
    p_pkg.add_argument("--target", choices=["windows", "web", "both", "auto"], default="windows",
                       help="windows = game-only desktop ZIP, web = static site via package_game.mjs")
    p_pkg.add_argument("-o", "--out", help="Output directory (default dist/)")
    p_pkg.add_argument("--engine", help="Engine binary or its directory (desktop target)")
    p_pkg.add_argument("--config", choices=["Debug", "Release", "RelWithDebInfo"],
                       help="Prefer this build configuration when searching for the engine")
    p_pkg.add_argument("--entry", help="Entry scene (default story.ks)")
    p_pkg.add_argument("--skip-check", action="store_true", help="Skip ordinary scene lint; required capability checks still run")
    p_pkg.add_argument("--with-shared-assets", action="store_true",
                       help="Also ship the repo shared asset pool (assets/)")
    p_pkg.add_argument("--dev", action="store_true", help="Keep config.dev_mode = true")
    p_pkg.set_defaults(func=cmd_package)

    args = parser.parse_args()
    sys.exit(args.func(args))

if __name__ == "__main__":
    main()
