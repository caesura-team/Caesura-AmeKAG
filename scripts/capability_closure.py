#!/usr/bin/env python3
"""Capability Closure scanner v5+v6 (029 artifact A; t103 MUST-FIX + overrides; t134 phantom-binding
validation; t185 status adjudication: an overrides entry may carry 'status' to authoritatively
set the final Status column -- the machine grade stays recorded as status_machine for comparison).

For every KAG command contract in docs/api/command-contracts.md, grade the
four closure dimensions that can be measured statically:

  Declared   -- the command has a contract entry (docs/api/command-contracts.md)
  Dispatched -- a handler is registered into the KAG dispatch table
                (scripts/kag/commands/*.lua export tables + scripts/kag.lua
                explicit mappings + scripts/kag/sma.lua sma_commands)
  Consumed   -- the handler body CALLS an effect surface in a call context
                (backend.foo( / layers.foo( / kag.foo( / ctx.tf field/assign).
                V2 (t103): comments and string literals are stripped BEFORE
                the call scan, and a bare mention (no parenthesis call or
                field access) no longer counts -- the v1 substring heuristic
                produced false positives from require("kag.xxx") literals and
                trailing comment blocks (assert / endbutton / waitclick).
                 V5 (t134): backend.<name> hits are validated against the REAL
                 binding surface (dynamically extracted from
                 src/script/bindings/*.cpp luaL_Reg + scripts/backend.lua shim
                 defs + backend_factory.lua cmd dispatch + kag.lua KAG defs).
                 A hit whose name is NOT on that surface is a PHANTOM BINDING:
                 dropped from Consumed evidence and listed in the
                 幻影绑定（v5） section with file:line.
  Tested     -- HEURISTIC reference count in tests/scripts/*.lua and
                web/*.test.js (NOT assertion-level coverage)

Observable / Platform Tested / Packaged are manual placeholders (?) unless
overridden in docs/design/capability-closure-overrides.json (human layer).

Outputs:
  build/capability-closure.json             raw data
  docs/design/capability-closure-matrix.md  generated markdown (do not edit)

Idempotent: deterministic given unchanged inputs (generated_at = newest input
mtime; no runtime data in the output).

Usage:
  python scripts/capability_closure.py
"""

import hashlib
import json
import re
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CONTRACTS = REPO / "docs" / "api" / "command-contracts.md"
KAG_LUA = REPO / "scripts" / "kag.lua"
SMA_LUA = REPO / "scripts" / "kag" / "sma.lua"
COMMANDS_DIR = REPO / "scripts" / "kag" / "commands"
TESTS_LUA = REPO / "tests" / "scripts"
TESTS_WEB = REPO / "web"
OVERRIDES = REPO / "docs" / "design" / "capability-closure-overrides.json"
OUT_JSON = REPO / "build" / "capability-closure.json"
OUT_MD = REPO / "docs" / "design" / "capability-closure-matrix.md"
BINDINGS_DIR = REPO / "src" / "script" / "bindings"
BACKEND_LUA_PATH = REPO / "scripts" / "backend.lua"
FACTORY_PATH = REPO / "scripts" / "backend_factory.lua"
WEB_BRIDGE = REPO / "web" / "bridge.js"

NL = chr(10)
TABLE_FILE = {
    "AudioCommands": "scripts/kag/commands/audio.lua",
    "CharacterCommands": "scripts/kag/commands/character.lua",
    "LayerCommands": "scripts/kag/commands/layer.lua",
    "LayoutCommands": "scripts/kag/commands/layout.lua",
    "MathCommands": "scripts/kag/commands/math.lua",
    "ResourceCommands": "scripts/kag/commands/resource.lua",
    "SaveCommands": "scripts/kag/commands/save.lua",
    "SystemCommands": "scripts/kag/commands/system.lua",
    "TextCommands": "scripts/kag/commands/text.lua",
    "TransCommands": "scripts/kag/commands/transition.lua",
    "TweenCommands": "scripts/kag/commands/tween.lua",
    "VFXCommands": "scripts/kag/commands/vfx.lua",
    "VideoCommands": "scripts/kag/commands/video.lua",
    "sma_commands": "scripts/kag/sma.lua",
}

# t103 MUST-FIX 1 regression locks: these commands must grade PARTIAL under
# the v2 call-context criteria because their bodies only use local modules
# (kag.expr / palette / state) -- v1 flagged them CLOSED via comment-block and
# string-literal contamination. If the code later gains a real effect call,
# the guard must be updated deliberately (t103: stay honest, never widen the
# heuristic to preserve a number).
GUARD_PARTIAL = {
    "assert": "body: kag.expr evaluate + error (no effect surface call)",
    "endbutton": "body: kag.expr evaluateTranslated + ctx state (no effect surface call)",
}

# =============================================================================
# t192 (v7): machine-grade adjudication categories. Sources are the batch3
# read-only audits (t181/t188/t189/t190) -- these lists are their INPUTS; an
# implementer must not add/remove names without a new audit verdict.
# Values carry the audit reference + consumption anchor per entry.
# =============================================================================

# PAIRING_GROUPS: same effect surface across commands/aliases; any member
# reaching Consumed (call-form) -> whole (declared) group grades CLOSED.
PAIRING_GROUPS = {
    # t181 button / t189 endbutton: one choice engine (button registers,
    # endbutton renders + hit-tests via _KAG_onClick; text.lua:1340/:1375+)
    "button": ["endbutton"],
    # t181 select: sel = button alias (text.lua:1488), endselect = endbutton
    # alias (:1491) -- shared implementation; "sel" stays EXTRA (no contract)
    "select": ["sel", "endselect"],
    # t188 delay: alias shares SystemCommands.wait (kag.lua:337-347)
    "delay": ["wait"],
}

# EXEMPT_PURE: zero backend.*/engine.* call-form + registered handler +
# semantic tests == CLOSED (pure Lua state/execution, or a dedicated
# non-backend namespace; the audits verified the effect chains).
EXEMPT_PURE = {
    # t183/t185 adjudicated group (status field retained as adjudication history)
    "i18n": "t183/t185 (system.lua:691-712 -> i18n.set_language + relocalize_page)",
    "add": "t183/t185 (math.lua:83-119 binop -> ctx scope write)",
    "sub": "t183/t185 (math.lua:83-119)",
    "mul": "t183/t185 (math.lua:83-119)",
    "div": "t183/t185 (math.lua:83-119)",
    "mod": "t183/t185 (math.lua:83-119)",
    "dec": "t183/t185 (math.lua:129-141)",
    "ending": "t183/t185 (system.lua:365-375 -> save.lua:176/418 + title_menu:30-53)",
    "saveplace": "t183/t185 (save.lua:550-552 -> system.lua:313-361 -> _pendingJump)",
    # t188 batch3a: pure Lua state/execution
    "emb": "t188 emb (system.lua:92-178 sandbox.execute/load + ctx mutation sync)",
    "eval": "t188 eval (live=scheduler.lua:1099 inline)",
    "set": "t188 set (system.lua:453-465 resolve_var/infer_value scope write)",
    "inc": "t188 inc (system.lua:471-484 nil-safe increment)",
    "assert": "t188 assert (system.lua:506-526 exprLang.evaluate + error->handle_error)",
    "random": "t188 random (system.lua:528-545 integer-floor scope write)",
    "skip": "t188 skip (text.lua:1082-1094 ctx.skip_mode -> kag_runner:481-530)",
    "unlock": "t188 unlock (system.lua:399-413 -> gallery.lua:51-103 + save persistence)",
    # t190 batch3c: proprietary sma.* namespace surface (SmaBinding.cpp:197-211
    # luaL_Reg; composition root Engine.cpp:605-612) -- dedicated-namespace
    # effect surface, NOT backend.*
    "sma_play": "t190 sma_play (sma.lua:712-721 -> binding().create_mesh @sma.lua:382-383)",
    "sma_anim": "t190 sma_anim (sma.lua:723-731 -> sma.update re-skin @:528-533; per-frame pump caveat)",
    "sma_ik": "t190 sma_ik (sma.lua:733-739 -> 2-bone constraint -> update_mesh)",
    "sma_variant": "t190 sma_variant (sma.lua:741-745 -> binding().destroy_mesh/create_mesh immediate)",
    "sma_stop": "t190 sma_stop (sma.lua:747-749 -> binding().destroy_mesh @:452-453)",
}

# EXEMPT_CONSUMED: zero (or all-real) backend.* call-form inside the handler;
# the effect is consumed at runner/scheduler/layers/module APIs and semantic
# tests exist == CLOSED.
EXEMPT_CONSUMED = {
    "wait": "t188 wait (system.lua:50-84 Operation/CancelToken + scheduler-dt yield loop)",
    "waitclick": "t188 waitclick (kag.lua:312-318 waiting_input -> runner click flow)",
    "waitforclick": "t188 waitforclick (kag.lua:388-396 waiting_input loop -> runner)",
    "auto": "t189 auto (text.lua:1112 ctx.auto_mode -> kag_runner:525-536 auto-advance)",
    "cps": "t189 cps (text.lua:1219 -> apply_text_cps:1197 ctx.text_speed -> kag_runner:482)",
    "pt": "t189 pt (text.lua:1153 ctx.text_speed -> same read point)",
    "textspeed": "t189 textspeed (text.lua:1215 ctx.text_speed=floor(1000/cps) -> kag_runner:482)",
    "nameplate": "t189 nameplate (text.lua:418 -> _renderNameplate:431-454 layers+render_text)",
    "voice_off": "t189 voice_off (text.lua:1126 ctx.voice_muted -> audio.lua:243 gate + save:168/375)",
    "voice_wait": "t189 voice_wait (kag.lua:297 -> audio.lua:277-301 wait loop + click-skip)",
    "br": "t189 br (kag.lua:212 -> KAG.l real line break)",
    "s": "t189 s (kag.lua:303 -> System.wait(ms=250))",
    "shake": "t189 shake (kag.lua:417 -> vfx.lua:82 -> node.shake.offset -> layers.lua:585-593)",
    "quake": "t189 quake (kag.lua:421 -> vfx.lua:28-70 -> node.quake.offset -> layers.lua:585-593)",
    "notify": "t189 notify (system.lua:648-675 -> toast.show toast.lua:18-41 real UI)",
    "replay": "t188 replay (system.lua:579-604 -> replay module + kag_runner tick :442-448/:740)",
    "rollback": "t188 rollback (system.lua:389-397 -> kag_runner.rollback:710 snapshot chain)",
    "saveload": "t188 saveload (save.lua:495-523 -> saveload_menu -> SaveCommands.save/load C++)",
}


def apply_v7(records):
    """t192: machine-grade categories. Runs AFTER guard_partial so the t103
    regression locks keep checking the PRE-v7 machine grade (their premise --
    assert/endbutton have no CONSUMED call-form -- stays true; the CLOSED
    grade comes from the audited category, not the heuristic). Never
    downgrades; raw machine grade stays in status_machine / status_v5."""
    by = {r["name"]: r for r in records}
    flips = []
    # Phase 1: EXEMPT categories. List membership IS the audit evidence of
    # handler + semantic tests -- the scanner tested_count is a heuristic
    # that misses direct-handler tests like sma.commands.sma_anim(...).
    for name in sorted(set(EXEMPT_PURE) | set(EXEMPT_CONSUMED)):
        r = by.get(name)
        if not r or not r["declared"]:
            continue
        cat = "EXEMPT_PURE" if name in EXEMPT_PURE else "EXEMPT_CONSUMED"
        if not r.get("v7_cat"):
            r["v7_cat"] = cat
        if r["status"] != "CLOSED" and r["dispatched"]:
            r["status"] = "CLOSED"
            flips.append(name)
    # Phase 2: PAIRING -- once any group member is CLOSED (machine, EXEMPT,
    # or adjudicated override), the whole declared group grades CLOSED.
    for lead, members in PAIRING_GROUPS.items():
        group = [lead] + list(members)
        if any(by.get(g, {}).get("status") == "CLOSED" for g in group if g in by):
            for g in group:
                r = by.get(g)
                if r and r["declared"] and not r.get("v7_cat"):
                    r["v7_cat"] = "PAIRING"
                if r and r["declared"] and r["status"] != "CLOSED":
                    r["status"] = "CLOSED"
                    flips.append(g)
    return flips


def guard_categories():
    """t192: category-list guards (exit 3 -- same code as guard_partial)."""
    bad = []
    if len(PAIRING_GROUPS) != 3:
        bad.append("PAIRING_GROUPS=%d (expect 3 groups)" % len(PAIRING_GROUPS))
    if len(EXEMPT_PURE) < 21:
        bad.append("EXEMPT_PURE=%d (expect >=21)" % len(EXEMPT_PURE))
    if len(EXEMPT_CONSUMED) < 18:
        bad.append("EXEMPT_CONSUMED=%d (expect >=18)" % len(EXEMPT_CONSUMED))
    if bad:
        print("[closure] ERROR: t192 v7 category guards violated: " + "; ".join(bad))
        sys.exit(3)


def source_mtime(paths):
    return max(int(p.stat().st_mtime) for p in paths if p.exists())



def source_fingerprint():
    h = hashlib.sha256()
    paths = [CONTRACTS, KAG_LUA, SMA_LUA, OVERRIDES,
             BACKEND_LUA_PATH, FACTORY_PATH]
    paths += sorted(BINDINGS_DIR.glob("*.cpp"))
    paths += sorted(COMMANDS_DIR.glob("*.lua"))
    if TESTS_LUA.is_dir():
        paths += sorted(TESTS_LUA.rglob("*.lua"))
    if TESTS_WEB.is_dir():
        paths += sorted(TESTS_WEB.glob("*.test.js"))
    for p in paths:
        if not p.exists():
            continue
        # as_posix() keeps the path bytes identical on Windows (backslash
        # native strings) and POSIX; EOL-normalize content because CRLF vs LF
        # is a checkout artifact (autocrlf), not content. Together the
        # fingerprint is identical across machines -- round 32 CI: Windows
        # hashed 'docs\design\...' + CRLF while the Linux runner hashed
        # 'docs/design/...' + LF, silently differing.
        h.update(p.relative_to(REPO).as_posix().encode("utf-8"))
        h.update(chr(0).encode("utf-8"))
        h.update(p.read_bytes().replace(b"\r\n", b"\n"))
    return h.hexdigest()[:16]


# Ground truth from five manual audit batches (t110/t113/t116/t117/t119):
# human VERIFIED (47) and human 真 PARTIAL (21). v4 flips are governed by these.
HUMAN_VERIFIED = {
    "i18n", "eval", "textspeed", "button", "notify", "delay", "select",
    "layout", "layout_slot", "endselect", "skip", "emb", "tween", "vibrate",
    "rollback", "preload", "pt", "history", "saveload", "ruby", "s", "auto",
    "flash", "gallery", "loadplace", "nameplate", "play", "playstop",
    "sma_play", "replay", "bgm", "blur", "l", "sma_stop", "sma_anim", "sma_ik",
    "sma_variant", "voice", "voice_wait", "br", "r", "chapter", "music",
    "postprocess", "fadeout", "shake", "quake",
}
HUMAN_PARTIAL = {
    "set", "wait", "unlock", "ending", "random", "saveplace", "waitforclick",
    "hr", "add", "sub", "mul", "div", "mod", "dec", "inc",
    "assert", "endbutton", "waitclick", "cps", "palette", "voice_off",
}


# t132: captain adjudication of v4 suspect flips (the flip keeper keeps the
# v3 grade; ADJUDICATED renders the ruling instead of "待裁决").
ADJUDICATED = {
    "palette": {
        "verdict": "维持 PARTIAL",
        "reason": "v5 已核真（t134）：渗透命中的 set_palette/load_image/is_valid 均为幻影绑定——三者不在原生绑定面（bindings/*.cpp luaL_Reg 156 键、backend.lua shim 68 def、backend_factory 62 cmd、kag.lua KAG 20 def 的并集）；web/jsBackend 提供同名项（bridge.js:325-327）但原生引擎无。仅 destroy_texture 为真实绑定（shim:188→RenderBinding:981）。apply/clear 经 lut_available() 守卫降级为可见 no-op（palette.lua:14-24）——全链半程：纹理销毁真实/LUT 应用未接线，按裁决维持 PARTIAL。"
    },
}



# string literals (require("kag.xxx") used to count as a kag. mention).
# ---------------------------------------------------------------------------


def strip_lua(code):
    out = []
    i = 0
    n = len(code)
    while i < n:
        c = code[i]
        nxt = code[i + 1] if i + 1 < n else ""
        if c == "-" and nxt == "-":
            # line comment, or long-bracket comment --[[ .. ]] / --[=[ .. ]=]
            j = i + 2
            level = 0
            if j < n and code[j] == "[":
                k = j + 1
                while k < n and code[k] == "=":
                    level += 1
                    k += 1
                if k < n and code[k] == "[":
                    close = "]" + ("=" * level) + "]"
                    e = code.find(close, k + 1)
                    e = e + len(close) if e >= 0 else n
                    out.extend([" "] * (e - i))
                    i = e
                    continue
            e = code.find(chr(10), i)
            e = e if e >= 0 else n
            out.extend([" "] * (e - i))
            i = e
        elif c == '"' or c == "'":
            j = i + 1
            while j < n:
                if code[j] == chr(92):
                    j += 2
                    continue
                if code[j] == c or code[j] == chr(10):
                    break
                j += 1
            e = (j + 1) if j < n else n
            out.extend([" "] * (e - i))
            i = e
        elif c == "[" and nxt == "[":
            j = i + 2
            level = 0
            while j < n and code[j] == "=":
                level += 1
                j += 1
            if j < n and code[j] == "[":
                close = "]" + ("=" * level) + "]"
                e = code.find(close, j + 1)
                e = e + len(close) if e >= 0 else n
                out.extend([" "] * (e - i))
                i = e
                continue
            out.append(c)
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def has_call(clean, tok):
    """True when an effect token appears in CALL context: tok.<ident>( ."""
    pos = 0
    n = len(clean)
    while True:
        p = clean.find(tok, pos)
        if p < 0:
            return False
        q = p + len(tok)
        s = q
        while s < n and (clean[s].isalnum() or clean[s] == "_"):
            s += 1
        if s == q:
            pos = p + 1
            continue
        while s < n and clean[s] in (" ", chr(9)):
            s += 1
        if s < n and clean[s] == "(":
            return True
        pos = p + 1


def has_ctx_tf_use(clean):
    """ctx.tf field access or assignment (ctx.tf. / ctx.tf[ / ctx.tf =)."""
    pos = 0
    while True:
        p = clean.find("ctx.tf", pos)
        if p < 0:
            return False
        q = p + len("ctx.tf")
        if q < len(clean) and clean[q] in ".=[":
            return True
        pos = p + 1


def call_hits(code):
    """Effect-surface call-context hits for a raw code slice (strips first)."""
    clean = strip_lua(code)
    hits = []
    for tok in ("backend.", "layers.", "kag."):
        if has_call(clean, tok):
            hits.append(tok)
    if has_ctx_tf_use(clean):
        hits.append("ctx.tf")
    return hits


def parse_contracts():
    names = []
    for line in open(CONTRACTS, encoding="utf-8").read().splitlines():
        if line.startswith("### `[") and line.endswith("]`"):
            names.append(line[6:-2])
    return names


def split_def(line, table):
    prefix = "function " + table + "."
    if not line.startswith(prefix):
        return None
    rest = line[len(prefix):]
    name = rest.split("(")[0].strip()
    return name


def full_path(file):
    """REPO-relative full evidence path (t103 MUST-FIX 2)."""
    if file == "kag.lua":
        return "scripts/kag.lua"
    if file == "kag/sma.lua":
        return "scripts/kag/sma.lua"
    return "scripts/kag/commands/" + file




def collect_subtable_names():
    """Table-like names in command files (name N with 'function N.' defs),
    excluding per-file export tables. Used to tell C:subtable-key assigns
    (TransCommands.Bezier = Bezier) from B:api-helper-export ones.
    """
    names = set()
    for f in sorted(COMMANDS_DIR.glob("*.lua")):
        txt = f.read_text(encoding="utf-8")
        for line in txt.splitlines():
            if line.startswith("function ") and "." in line:
                rest = line[len("function "):]
                nm = rest.split(".")[0].strip()
                if nm and nm.isidentifier():
                    names.add(nm)
    return names

def parse_command_modules():
    handlers = {}
    private = []
    for f in sorted(COMMANDS_DIR.glob("*.lua")):
        txt = f.read_text(encoding="utf-8")
        lines = txt.splitlines()
        table = None
        for line in lines:
            if line.startswith("return "):
                possible = line[7:].strip()
                if possible and possible.isidentifier():
                    table = possible
        if not table:
            continue
        defs = [i for i, l in enumerate(lines) if split_def(l, table)]
        for k, i in enumerate(defs):
            name = split_def(lines[i], table)
            body_end = defs[k + 1] if k + 1 < len(defs) else len(lines)
            body = NL.join(lines[i:body_end])
            line_no = i + 1
            if name.startswith("_"):
                private.append((name, f.name, line_no))
                continue
            handlers[name] = {"file": full_path(f.name), "line": line_no,
                              "body": body, "kind": "def"}
        for i, l in enumerate(lines):
            if not l.startswith(table + "."):
                continue
            if l.split("(", 1)[0].strip().startswith("function"):
                continue
            rest = l[len(table) + 1:]
            name = rest.split("=", 1)[0].strip()
            if not name or not name.isidentifier() or name.startswith("_"):
                continue
            if "=" not in l or name in handlers:
                continue
            rhs = l.split("=", 1)[1].strip()
            handlers[name] = {"file": full_path(f.name), "line": i + 1,
                              "body": rhs, "kind": "assign"}
    return handlers, private


def parse_sma_commands():
    txt = SMA_LUA.read_text(encoding="utf-8")
    lines = txt.splitlines()
    handlers = {}
    defs = [i for i, l in enumerate(lines) if split_def(l, "sma_commands")]
    for k, i in enumerate(defs):
        name = split_def(lines[i], "sma_commands")
        body_end = defs[k + 1] if k + 1 < len(defs) else len(lines)
        body = NL.join(lines[i:body_end])
        handlers[name] = {"file": full_path("kag/sma.lua"), "line": i + 1,
                          "body": body, "kind": "def"}
    return handlers


def parse_kag_lua_mappings():
    txt = KAG_LUA.read_text(encoding="utf-8")
    lines = txt.splitlines()
    out = {}
    defs = {}
    for idx, line in enumerate(lines):
        m = None
        if line.startswith("function KAG.") and "(" in line:
            rest = line[len("function KAG."):]
            name = rest.split("(")[0].strip()
            if name and name.isidentifier():
                m = ("def", name)
        if m is None and "=" in line:
            if line.startswith("KAG["):
                rhs = line.split("=", 1)[1].strip()
                inner = line[4:].split("]", 1)[0].strip().strip("\"'")
                m = ("asg", inner, rhs)
            elif line.startswith("KAG."):
                rhs = line.split("=", 1)[1].strip()
                rest = line[4:].split("=")[0].strip()
                n = ""
                for ch in rest:
                    if ch.isalnum() or ch == "_":
                        n += ch
                    else:
                        break
                m = ("asg", n, rhs)
        if m is None:
            continue
        if m[0] == "def":
            name = m[1]
            if name.startswith("_"):
                continue
            body_lines = []
            j = idx
            while j < len(lines):
                l = lines[j]
                if j > idx and (l.startswith("function KAG.") or l.startswith("KAG.")
                                or l.startswith("local ") or l.startswith("end}")):
                    break
                body_lines.append(l)
                j += 1
            defs[name] = {"file": full_path("kag.lua"), "line": idx + 1,
                          "body": NL.join(body_lines), "kind": "def"}
        else:
            name, rhs = m[1], m[2]
            if name == "name" and rhs == "handler":
                continue
            # t132: table-constructor assignment (rhs starts with {) is a
            # KAG-table DATA export, not a dispatchable command -- skip (the
            # scheduler dispatches functions; a table value would nil-call).
            # gesture_defaults (kag.lua:586) is the canonical runtime-gesture
            # defaults export, NOT a [gesture_defaults] command.
            if rhs.startswith("{"):
                continue
            out[name] = {"line": idx + 1, "rhs": rhs}
    return out, defs


SUBTABLE_NAMES = set()


def classify_source(file_, kind, rhs):
    """t103 三分层 EXTRA 类型：
    A=user-command-missing-contract（KAG.* 直接 API/别名；owner=contracts 生成链，入册与否=产品决策待议）
    B=api-helper-export（命令模块导出辅助/API 助手，非用户命令面）
    C=subtable-key（pairs() 注册的表引用真噪声，非命令面）。
    """
    if file_ == "scripts/kag.lua":
        return "A:user-command-missing-contract"
    if kind == "assign" and rhs and rhs.isidentifier() and rhs in SUBTABLE_NAMES:
        return "C:subtable-key"
    return "B:api-helper-export"








# ---------------------------------------------------------------------------
# v4: one-hop call penetration (t126). For a handler body we also scan:
#   (a) same-file local functions the body CALLS (layout->apply_container,
#       tween->resolve_layer/step_tween, nameplate->_renderNameplate),
#   (b) module-table delegation foo.bar( where the module was require()d
#       (toast.show / VFX.flash / HistoryUI.show) -- resolve the module file
#       and scan the called function body once. Function body not found ->
#       not counted (conservative). Stripping/call-context discipline = v3.
# ---------------------------------------------------------------------------

V4_FILES_CACHE = {}


def v4_file_index(path):
    global V4_FILES_CACHE
    if path in V4_FILES_CACHE:
        return V4_FILES_CACHE[path]
    fp = REPO / path
    info = {"lines": [], "locals": {}, "modules": {}, "fns": {}, "local_lines": {}}
    if fp.exists():
        info["lines"] = fp.read_text(encoding="utf-8").splitlines()
    V4_FILES_CACHE[path] = info
    return info


def v4_index_file(path):
    """Fill locals/modules/fns for one file from its lines."""
    global V4_FILES_CACHE
    info = v4_file_index(path)
    if info.get("_done"):
        return info
    lines = info["lines"]
    bounds = []
    defs = []
    for i, l in enumerate(lines):
        s = l.strip()
        if s.startswith("local function ") and "(" in s:
            name = s[len("local function "):].split("(")[0].strip()
            defs.append((i, name))
            bounds.append(i)
        elif s.startswith("local ") and "= function" in s:
            nm = s[len("local "):].split("=")[0].strip()
            if nm.isidentifier():
                defs.append((i, nm))
                bounds.append(i)
        elif s.startswith("function ") and "(" in s:
            bounds.append(i)
    for k, (i, name) in enumerate(defs):
        end = len(lines)
        for b in bounds:
            if b > i:
                end = b
                break
        info["locals"][name] = NL.join(lines[i:end])
        info["local_lines"][name] = i + 1
    for i, l in enumerate(lines):
        s = l.strip()
        if s.startswith("local ") and "require(" in s and "=" in s:
            alias = s[len("local "):].split("=")[0].strip()
            if not alias.isidentifier():
                continue
            rest = s.split("require(", 1)[1]
            p = rest.find(chr(34))
            q = rest.find(chr(34), p + 1) if p >= 0 else -1
            if p >= 0 and q > p:
                info["modules"][alias] = rest[p + 1:q]
    for i, l in enumerate(lines):
        s = l.strip()
        if s.startswith("function ") and "." in s and "(" in s:
            rest = s[len("function "):]
            table = rest.split(".")[0].strip()
            nm = rest.split("(")[0].strip()
            if "." in nm:
                nm = nm.split(".", 1)[1]
            if table and nm and table.isidentifier() and nm.isidentifier():
                info["fns"][(table, nm)] = i
    info["_done"] = True
    return info


def v4_module_path(mod):
    rel = mod.replace(".", "/")
    candidates = ["scripts/" + rel + ".lua", "scripts/kag/" + rel + ".lua"]
    for c in candidates:
        if (REPO / c).exists():
            return c
    return None


def v4_local_call(clean, info):
    called = []
    for name in info.get("locals", {}):
        if has_ident_call(clean, name):
            called.append(name)
    return called


def has_ident_call(clean, name):
    pos = 0
    n = len(clean)
    while True:
        p = clean.find(name, pos)
        if p < 0:
            return False
        after = p + len(name)
        s = after
        while s < n and clean[s] in (" ", chr(9)):
            s += 1
        if s < n and clean[s] == "(":
            return True
        pos = p + 1


def v4_module_calls(clean, info):
    out = []
    for alias, mod in info.get("modules", {}).items():
        try:
            pos = 0
            while True:
                p = clean.find(alias + ".", pos)
                if p < 0:
                    break
                q = p + len(alias) + 1
                s = q
                while s < len(clean) and (clean[s].isalnum() or clean[s] == "_"):
                    s += 1
                fn = clean[q:s]
                t = s
                while t < len(clean) and clean[t] in (" ", chr(9)):
                    t += 1
                if fn and t < len(clean) and clean[t] == "(":
                    out.append((alias, fn, mod))
                pos = p + 1
        except Exception:
            pass
    return out


def v4_penetrated_landings(body, file_path):
    """One-hop penetration target bodies as (slice_text, src_file, base_line).

    Same traversal as the v4 hit scan: same-file local functions the body
    calls + require()d module functions. Base line is the 1-based line OF
    the slice's first line in its source file (needed for v5 file:line
    phantom evidence).
    """
    clean = strip_lua(body)
    out = []
    info = v4_index_file(file_path)
    for name in v4_local_call(clean, info):
        lb = info["locals"].get(name) or ""
        out.append((lb, file_path, info.get("local_lines", {}).get(name, 1)))
    for alias, fn, mod in v4_module_calls(clean, info):
        mp = v4_module_path(mod)
        if not mp:
            continue
        minfo = v4_index_file(mp)
        idx = minfo["fns"].get((alias, fn))
        if idx is None:
            key_candidates = [k for k in minfo["fns"] if k[1] == fn]
            if key_candidates:
                idx = minfo["fns"][key_candidates[0]]
        if idx is None:
            continue
        body2 = StringUtils_file_slice(minfo, idx)
        out.append((body2, mp, idx + 1))
    return out


def v4_penetrated_hits(body, file_path):
    hits = []
    for slice_text, _src, _base in v4_penetrated_landings(body, file_path):
        hits.extend(call_hits(slice_text))
    seen = set()
    out = []
    for h in hits:
        if h not in seen:
            seen.add(h)
            out.append(h)
    return out



def StringUtils_file_slice(info, idx):
    lines = info["lines"]
    end = len(lines)
    for j in range(idx + 1, len(lines)):
        s = lines[j].strip()
        if (s.startswith("function ") and "(" in s) or (s.startswith("local function ") and "(" in s) or (s.startswith("local ") and "= function" in s):
            end = j
            break
    return NL.join(lines[idx:end])


# ---------------------------------------------------------------------------
# v5: phantom-binding validation (t134). The REAL binding surface for
# backend.<name> is extracted DYNAMICALLY from the native registration
# sites -- NOT a single fixed list:
#   (a) src/script/bindings/*.cpp luaL_Reg entries { "name", lua_X }
#       (KAGBinding flat table, plus Render/DevCore/Save/VFX/SMA/Steam/
#        MiniGame/AI/Debug/Engine subtables reached via the shim's
#        render_or_guard / devcore_or_guard routing),
#   (b) scripts/backend.lua 'function Backend.name' shim defs (the Lua-side
#       API surface -- e.g. audio_play/audio_stop/load_texture are shim
#       functions, NOT luaL_Reg keys),
#   (c) scripts/backend_factory.lua dispatch cmd strings (the BackendFactory
#       proxy registry -- legacy _CAESURA_BACKEND-style routing),
#   (d) scripts/kag.lua 'function KAG.name' defs (fallback chain in
#       backend.lua's resolve(): _CAESURA_BACKEND -> KAG -> kag module).
# A backend.X consumption hit whose X is NOT on (a)|(b)|(c)|(d) is a PHANTOM
# BINDING: dropped from Consumed evidence and listed in the 幻影绑定 section
# with file:line evidence.
# ---------------------------------------------------------------------------

V5_SURFACE_CACHE = None


def extract_v5_surface():
    """Return (surface_set, meta) -- meta describes extraction pattern+sizes."""
    global V5_SURFACE_CACHE
    if V5_SURFACE_CACHE is not None:
        return V5_SURFACE_CACHE
    cpp_names = set()
    cpp_files = 0
    if BINDINGS_DIR.is_dir():
        for p in sorted(BINDINGS_DIR.glob("*.cpp")):
            cpp_files += 1
            txt = p.read_text(encoding="utf-8")
            for m in re.finditer(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*lua_', txt):
                cpp_names.add(m.group(1))
    shim_names = set()
    if BACKEND_LUA_PATH.exists():
        txt = BACKEND_LUA_PATH.read_text(encoding="utf-8")
        for m in re.finditer(r'^function Backend\.([A-Za-z_][A-Za-z0-9_]*)', txt, re.M):
            shim_names.add(m.group(1))
    factory_cmds = set()
    if FACTORY_PATH.exists():
        txt = FACTORY_PATH.read_text(encoding="utf-8")
        for m in re.finditer(r'elseif cmd == "([A-Za-z0-9_]+)"', txt):
            factory_cmds.add(m.group(1))
        for m in re.finditer(r'if cmd == "([A-Za-z0-9_]+)"', txt):
            factory_cmds.add(m.group(1))
    kag_names = set()
    if KAG_LUA.exists():
        txt = KAG_LUA.read_text(encoding="utf-8")
        for m in re.finditer(r'^function KAG\.([A-Za-z_][A-Za-z0-9_]*)', txt, re.M):
            kag_names.add(m.group(1))
    surface = cpp_names | shim_names | factory_cmds | kag_names
    meta = {
        "pattern": ("union of: bindings/*.cpp luaL_Reg { name, lua_X }; "
                    "backend.lua ^function Backend.X; backend_factory.lua cmd==X; "
                    "kag.lua ^function KAG.X"),
        "files": cpp_files,
        "cpp": len(cpp_names),
        "shim": len(shim_names),
        "factory": len(factory_cmds),
        "kag": len(kag_names),
        "union": len(surface),
    }
    V5_SURFACE_CACHE = (surface, meta)
    return V5_SURFACE_CACHE


def js_backend_surface():
    """Optional cross-ref (NOT used in judgment): field names of the web
    jsBackend object in web/bridge.js (the browser-side backend shim).
    """
    out = set()
    if WEB_BRIDGE.exists():
        txt = WEB_BRIDGE.read_text(encoding="utf-8")
        # line-anchored object fields (4-space indent) AND inline entries of
        # share-a-line literals (create_lut_texture: ..., render_frame: ...).
        for m in re.finditer(r'^\s{4}([A-Za-z_][A-Za-z0-9_]*):\s*\(', txt, re.M):
            out.add(m.group(1))
        for m in re.finditer(r'\b([A-Za-z_][A-Za-z0-9_]*):\s*\(', txt):
            out.add(m.group(1))
    return out


def backend_name_hits(clean):
    """[(name, offset)] for backend.<name>( call-context hits in stripped code."""
    out = []
    pos = 0
    n = len(clean)
    while True:
        p = clean.find("backend.", pos)
        if p < 0:
            return out
        q = p + len("backend.")
        s = q
        while s < n and (clean[s].isalnum() or clean[s] == "_"):
            s += 1
        if s == q:
            pos = p + 1
            continue
        name = clean[q:s]
        t = s
        while t < n and clean[t] in (" ", chr(9)):
            t += 1
        if t < n and clean[t] == "(":
            out.append((name, p))
        pos = p + 1


def clean_line_offset(clean, pos):
    """1-based line of a position in a body slice (slice line 1 = 1)."""
    return clean[:pos].count(chr(10)) + 1


def v5_phantom_evidence(rec):
    """Collect backend.<name> call-context hits (direct + v4 landings) with
    file:line, and split into REAL (name on surface) vs PHANTOM hits.
    Returns (real_names set, phantom list of {name,file,line}).
    """
    surface, _ = extract_v5_surface()
    real = set()
    phantom = []
    seen = set()
    direct = strip_lua(rec["body"])
    for name, p in backend_name_hits(direct):
        line = rec["line"] + clean_line_offset(direct, p) - 1
        if name in surface:
            real.add(name)
        else:
            key = (name, rec["file"], line)
            if key not in seen:
                seen.add(key)
                phantom.append({"name": name, "file": rec["file"], "line": line})
    for slice_text, src_file, base_line in v4_penetrated_landings(rec["body"], rec["file"]):
        cl = strip_lua(slice_text)
        for name, p in backend_name_hits(cl):
            line = base_line + clean_line_offset(cl, p) - 1
            if name in surface:
                real.add(name)
            else:
                key = (name, src_file, line)
                if key not in seen:
                    seen.add(key)
                    phantom.append({"name": name, "file": src_file, "line": line})
    phantom.sort(key=lambda x: (x["file"], x["line"], x["name"]))
    return real, phantom


def consumed_for(rec, body):
    """v3 direct hits + v4 one-hop penetration."""
    hits = call_hits(body)
    pent = v4_penetrated_hits(body, rec["file"])
    for h in pent:
        if h not in hits:
            hits.append(h)
    return hits


def fill_v5(rec):
    """Compute v5 phantom-filtered consumed + phantom evidence for a record."""
    real, phant = v5_phantom_evidence(rec)
    hits5 = [h for h in (rec.get("hits") or []) if h != "backend."]
    if real:
        hits5.append("backend.")
    rec["consumed_v5"] = bool(hits5)
    rec["hits_v5"] = hits5
    rec["phantom_real"] = sorted(real)
    rec["phantom_hits"] = phant


def resolve_consumed(name, index, depth):
    rec = index[name]
    if rec.get("consumed") is not None or depth > 8:
        return rec.get("consumed") or False
    if rec["kind"] == "def":
        hits = consumed_for(rec, rec["body"])
        v3 = call_hits(rec["body"])
        rec["consumed"] = bool(hits)
        rec["consumed_v3"] = bool(v3)
        rec["hits"] = hits
        rec["hits_v3"] = v3
        fill_v5(rec)
        return rec["consumed"]
    rhs = rec["body"]
    if rhs.startswith("function"):
        hits = consumed_for(rec, rhs)
        v3 = call_hits(rhs)
        rec["consumed"] = bool(hits)
        rec["consumed_v3"] = bool(v3)
        rec["hits"] = hits
        rec["hits_v3"] = v3
        fill_v5(rec)
        return rec["consumed"]
    rec["alias_rhs"] = rhs
    for branch in rhs.split(" or "):
        parts = branch.strip().split(".")
        if len(parts) == 2 and parts[1].isidentifier() and parts[0].isidentifier():
            table, target = parts
            if target in index and (table == "KAG" or table in TABLE_FILE):
                ok = resolve_consumed(target, index, depth + 1)
                rec["consumed"] = ok
                rec["consumed_v3"] = ok
                t5 = index[target].get("consumed_v5")
                rec["consumed_v5"] = ok if t5 is None else t5
                rec["hits_v5"] = list(index[target].get("hits_v5") or [])
                rec["phantom_hits"] = list(index[target].get("phantom_hits") or [])
                rec["phantom_real"] = list(index[target].get("phantom_real") or [])
                rec["alias_target"] = table + "." + target
                return ok
    hits = consumed_for(rec, rhs)
    v3 = call_hits(rhs)
    rec["consumed"] = bool(hits)
    rec["consumed_v3"] = bool(v3)
    rec["hits"] = hits
    rec["hits_v3"] = v3
    fill_v5(rec)
    return rec["consumed"]


def grade_status(declared, dispatched, consumed):
    if declared and not dispatched:
        return "UNWIRED"
    if declared and not consumed:
        return "PARTIAL"
    if declared:
        return "CLOSED"
    if dispatched:
        return "EXTRA"
    return "NONE"


def count_in(p, name):
    txt = p.read_text(encoding="utf-8", errors="replace")
    n = 0
    needle = "[" + name
    kag_needle = "kag." + name
    pos = txt.find(needle)
    while pos >= 0:
        after = txt[pos + len(needle):pos + len(needle) + 1]
        if after in (" ", "]", "=", chr(92)) or after == "" or after == chr(10):
            n += 1
        pos = txt.find(needle, pos + 1)
    pos = txt.find(kag_needle)
    while pos >= 0:
        after = txt[pos + len(kag_needle):pos + len(kag_needle) + 1]
        if after in ("", ".", " ", "(", "[", chr(10)) or (after.isalnum() is False):
            n += 1
        pos = txt.find(kag_needle, pos + 1)
    return n


def tested_count(name):
    total = 0
    files = []
    if TESTS_LUA.is_dir():
        for p in sorted(TESTS_LUA.rglob("*.lua")):
            n = count_in(p, name)
            if n:
                total += n
                files.append(str(p.relative_to(REPO)))
    if TESTS_WEB.is_dir():
        for p in sorted(TESTS_WEB.glob("*.test.js")):
            n = count_in(p, name)
            if n:
                total += n
                files.append(str(p.relative_to(REPO)))
    return total, files


# v6: an overrides entry may carry a 'status' field that AUTHORITATIVELY
# adjudicates the final Status column (machine grade preserved as
# status_machine for comparison). Unknown values are rejected loudly.
VALID_OVERRIDE_STATUS = {"CLOSED", "PARTIAL", "EXTRA", "UNWIRED", "EXPERIMENTAL"}

# t197 (032 5): platform_tested / packaged protocol value domain. Legal
# platform_tested = missing / "-" / comma-separated subset of PLATFORM_ENUMS
# (canonical form: deduped + sorted); legal packaged = missing / "-" / non-empty
# string <= MAX_PACKAGED_LEN. Values outside the domain are migrated to "-" by
# migrate_protocol_fields (no evidence = unverified) and any survivor is
# rejected loudly (exit 2) -- same governance pattern as VALID_OVERRIDE_STATUS.
PLATFORM_ENUMS = frozenset({"win", "linux", "macos", "web", "android", "ios"})
MAX_PACKAGED_LEN = 120
# t197 B3: legacy placeholders ("?" / "未知" / "unknown") = no evidence. They are
# migrated to "-" even when they would satisfy the packaged string domain.
LEGACY_PLACEHOLDER_VALUES = frozenset({"?", "未知", "unknown"})


def normalize_platform_tested(raw):
    """Return the canonical protocol string ('-' = no evidence) or None when the
    value is outside the legal domain."""
    if raw is None or raw == "":
        return "-"
    if not isinstance(raw, str):
        return None
    if raw == "-":
        return "-"
    parts = [p.strip() for p in raw.split(",")]
    if not parts or any(p not in PLATFORM_ENUMS for p in parts):
        return None
    return ",".join(sorted(set(parts)))


def normalize_packaged(raw):
    if raw is None or raw == "":
        return "-"
    if not isinstance(raw, str):
        return None
    if raw == "-":
        return "-"
    v = raw.strip()
    if not v or len(v) > MAX_PACKAGED_LEN:
        return None
    return v


def migrate_protocol_fields(data, overrides_path):
    """t197 B3: survey + migrate non-protocol platform_tested/packaged values to
    '-' (no evidence = unverified). Rewrites the overrides JSON only when a
    change was made. Returns (bad_before_pt, bad_before_pk, changed_pt,
    changed_pk): bad_before_* counts values outside the protocol domain
    (migrated to '-'); changed_* counts all rewrites (incl. canonical
    dedupe/sort). After migration the bad counts must be 0."""
    cmds = data.get("commands", {})
    bad_before_pt = bad_before_pk = 0
    changed_pt = changed_pk = 0
    for k, v in cmds.items():
        if not isinstance(v, dict):
            continue
        for key in ("platform_tested", "packaged"):
            if key not in v:
                continue
            old = v[key]
            norm = (normalize_platform_tested(old) if key == "platform_tested"
                    else normalize_packaged(old))
            if old in LEGACY_PLACEHOLDER_VALUES or norm is None:
                if key == "platform_tested":
                    bad_before_pt += 1
                    changed_pt += 1
                else:
                    bad_before_pk += 1
                    changed_pk += 1
                v[key] = "-"
            elif norm != old:
                v[key] = norm
                if key == "platform_tested":
                    changed_pt += 1
                else:
                    changed_pk += 1
    if changed_pt or changed_pk:
        overrides_path.parent.mkdir(parents=True, exist_ok=True)
        with open(overrides_path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, ensure_ascii=False, indent=2)
            fh.write(NL)
    return bad_before_pt, bad_before_pk, changed_pt, changed_pk


def validate_protocol_fields(commands):
    """t197 B1/B2: return [(name, field, value)] for values outside the
    protocol domain (the scanner exits 2 when non-empty)."""
    bad = []
    for k, v in commands.items():
        if not isinstance(v, dict):
            continue
        pt = v.get("platform_tested", "-")
        if normalize_platform_tested(pt) is None:
            bad.append((k, "platform_tested", pt))
        pk = v.get("packaged", "-")
        if normalize_packaged(pk) is None:
            bad.append((k, "packaged", pk))
    return bad


def platform_reported(raw):
    return normalize_platform_tested(raw) not in (None, "-")


def packaged_reported(raw):
    return normalize_packaged(raw) not in (None, "-")


def load_overrides(known_names):
    if not OVERRIDES.exists():
        return {}, []
    data = json.loads(OVERRIDES.read_text(encoding="utf-8"))
    commands = data.get("commands", {})
    oos = data.get("out_of_scope", [])
    bad = [k for k in commands if k not in known_names]
    if bad:
        print("[closure] ERROR: overrides reference unknown commands: " + ", ".join(bad))
        sys.exit(2)
    bad_status = [
        (k, v.get("status")) for k, v in commands.items()
        if isinstance(v, dict) and v.get("status")
        and v.get("status") not in VALID_OVERRIDE_STATUS
    ]
    if bad_status:
        print("[closure] ERROR: overrides 'status' values must be one of "
              + ", ".join(sorted(VALID_OVERRIDE_STATUS)) + ": "
              + ", ".join(k + "=" + str(s) for k, s in bad_status))
        sys.exit(2)
    # t197 B3: migrate non-protocol platform_tested/packaged to '-' (no
    # evidence = unverified); prints survey/migration counts.
    before = migrate_protocol_fields(data, OVERRIDES)
    print("[closure] t197 overrides protocol migration: platform_tested bad="
          + str(before[0]) + "->0 (changed " + str(before[2])
          + ") · packaged bad=" + str(before[1]) + "->0 (changed " + str(before[3]) + ")")
    # t197 B1/B2: any survivor outside the protocol domain is a hard error.
    bad_values = validate_protocol_fields(commands)
    if bad_values:
        print("[closure] ERROR: overrides protocol values illegal ("
              + ", ".join(k + "." + f + "=" + repr(v) for k, f, v in bad_values)
              + "); platform_tested: '-' or comma-separated subset of "
              + ",".join(sorted(PLATFORM_ENUMS))
              + "; packaged: '-' or non-empty string <=" + str(MAX_PACKAGED_LEN) + " chars")
        sys.exit(2)
    return commands, oos


def build_records(overrides_map):
    declared_names = set(parse_contracts())
    handlers, private = parse_command_modules()
    handlers.update(parse_sma_commands())
    kag_map, kag_defs = parse_kag_lua_mappings()
    handlers.update(kag_defs)
    index = {}
    for name, info in handlers.items():
        index[name] = {"kind": info.get("kind", "def"),
                       "file": info["file"],
                       "line": info["line"],
                       "body": info.get("body", ""),
                       "consumed": None, "hits": [],
                       "consumed_v3": None, "hits_v3": [],
                       "consumed_v5": None, "hits_v5": [],
                       "phantom_hits": [], "phantom_real": [],
                       "rhs": info.get("body", "")}
    for name, mp in kag_map.items():
        index[name] = {"kind": "assign", "file": "scripts/kag.lua",
                       "line": mp["line"], "body": mp["rhs"],
                       "consumed": None, "hits": [],
                       "consumed_v3": None, "hits_v3": [],
                       "consumed_v5": None, "hits_v5": [],
                       "phantom_hits": [], "phantom_real": [],
                       "rhs": mp["rhs"]}
    for name in sorted(index):
        resolve_consumed(name, index, 0)
    contract_lines = {}
    for ln, l in enumerate(open(CONTRACTS, encoding="utf-8").read().splitlines(), 1):
        if l.startswith("### `[") and l.endswith("]`"):
            contract_lines[l[6:-2]] = ln
    records = []
    suspected = []
    suspected_v5 = []
    flips_v45 = []
    for name in sorted(set(declared_names) | set(index)):
        declared = name in declared_names
        rec = index.get(name)
        if rec:
            dispatched = True
            consumed = rec.get("consumed") or False
            consumed_v3 = rec.get("consumed_v3") or False
            consumed_v5 = rec.get("consumed_v5") or False
            hits = rec.get("hits") or []
            hits_v5 = rec.get("hits_v5") or []
            phantom_hits = rec.get("phantom_hits") or []
            phantom_real = rec.get("phantom_real") or []
            evidence = rec["file"] + ":" + str(rec["line"])
            alias = rec.get("alias_target")
            source_type = classify_source(rec["file"], rec["kind"], rec.get("rhs", ""))
        else:
            dispatched, consumed = False, False
            consumed_v3 = False
            consumed_v5 = False
            hits = []
            hits_v5 = []
            phantom_hits = []
            phantom_real = []
            evidence = None
            alias = None
            source_type = None
        status_v3 = grade_status(declared, dispatched, consumed_v3)
        status_v4 = grade_status(declared, dispatched, consumed)
        status_v5 = grade_status(declared, dispatched, consumed_v5)
        # v5 phantom-downgrade governance: any human-VERIFIED name that the
        # phantom filter now downgrades is HIGHLY suspicious (v4 evidence
        # destroyed) -- conservatively keep human grade + captain adjudication.
        # Non-verified machine downgrades are accepted (phantom evidence was
        # never real; this is the t134 fix direction).
        flip_v5 = None
        if status_v5 == "PARTIAL" and status_v4 == "CLOSED":
            if name in HUMAN_VERIFIED:
                flip_v5 = "suspect-keep-v4"
                suspected_v5.append({
                    "name": name, "v4": status_v4, "v5": status_v5,
                    "phantom": phantom_hits})
                status_v5 = status_v4
            else:
                flip_v5 = "phantom-filtered"
                flips_v45.append({
                    "name": name, "v4": status_v4, "v5": status_v5,
                    "phantom": phantom_hits})
        status = status_v5
        flip = None
        if status_v5 == "CLOSED" and status_v3 == "PARTIAL":
            if name in HUMAN_VERIFIED:
                flip = "accepted-human-verified"
            else:
                # conservative governance: keep v3 grade, flag for captain
                status = status_v3
                flip = "suspect-keep-v3"
                suspected.append({"name": name, "v3": status_v3, "v4": status_v4})
        ov = overrides_map.get(name)
        tcount, tfiles = tested_count(name)
        rec_out = {
            "name": name, "declared": declared, "dispatched": dispatched,
            "consumed": consumed, "consumed_v3": consumed_v3, "consumed_hits": hits,
            "consumed_v5": consumed_v5, "hits_v5": hits_v5,
            "phantom_hits": phantom_hits, "phantom_real": phantom_real,
            "tested_count": tcount, "tested_files": tfiles,
            "status": status, "status_v3": status_v3, "status_v4": status_v4,
            "status_v5": status_v5,
            "evidence": evidence, "alias": alias,
            "contract_line": contract_lines.get(name),
            "source_type": source_type, "override": ov or None, "flip": flip,
            "flip_v5": flip_v5,
        }
        if ov and ov.get("status"):
            rec_out["status_machine"] = rec_out["status"]
            rec_out["status"] = ov["status"]
        else:
            rec_out["status_machine"] = rec_out["status"]
        records.append(rec_out)
    surface, surface_meta = extract_v5_surface()
    return records, private, len(declared_names), suspected, suspected_v5, flips_v45, surface_meta


def guard_partial(records):
    by_name = {r["name"]: r for r in records}
    bad = []
    for name, why in GUARD_PARTIAL.items():
        row = by_name.get(name)
        if not row or row["status"] != "PARTIAL":
            bad.append(name + " -> " + (row["status"] if row else "missing"))
    if bad:
        print("[closure] ERROR: t103 regression locks violated: " + ", ".join(bad))
        sys.exit(3)


def guard_phantom(records):
    """t134 self-check: palette must auto-classify its set_palette hit as
    phantom (ADJUDICATED ruling requires the phantom evidence to exist);
    if the phantom filter changes any human-VERIFIED grade, flag loudly.
    """
    bad = []
    by_name = {r["name"]: r for r in records}
    pal = by_name.get("palette")
    if not pal:
        bad.append("palette record missing")
    else:
        # t214: palette's legacy phantom names (set_palette/load_image/is_valid)
        # were REPLACED by the real-name surface (load_texture/is_valid_handle/
        # set_postfx). The sentinel now asserts the OLD phantom names are ABSENT
        # (fix landed); a resurrection would fail loudly.
        for p in (pal.get("phantom_hits") or []):
            if p.get("name") in ("set_palette", "load_image", "is_valid"):
                bad.append("palette resurrected phantom: " + str(p.get("name"))
                          + " @ " + str(p.get("file")))
    v5_susp = [r for r in records
               if r.get("flip_v5") == "suspect-keep-v4"]
    for r in v5_susp:
        print("[closure] v5-SUSPECT(keep v4 human grade): " + r["name"]
              + " phantom=" + str([p["name"] + "@" + p["file"] + ":" + str(p["line"]) for p in (r.get("phantom_hits") or [])][:6]))
    if bad:
        print("[closure] ERROR v5 phantom self-check: " + "; ".join(bad))
        sys.exit(5)
    print("[closure] v5 phantom self-check OK")


def render_markdown(records, private, declared_total, oos, generated_at, fp, suspected, suspected_v5, flips_v45, surface_meta):
    by_status = {}
    for r in records:
        by_status.setdefault(r["status"], []).append(r)
    L = []
    L.append("# Capability Closure Matrix (auto-generated)")
    L.append("")
    L.append("> 由 python scripts/capability_closure.py 生成；勿手动编辑。")
    L.append("> 生成时间（输入源最新 mtime）：" + generated_at)
    L.append("> 生成命令：python scripts/capability_closure.py")
    L.append("> 源指纹（输入内容 sha256 前 16 hex）：" + fp)
    L.append("> 输出确定性：同源指纹同字节（generated_at 为输入源最新 mtime；跨机 checkout 的 mtime 差异属 by-design，确定性以指纹为准）")
    L.append("")
    L.append("## 概述")
    L.append("")
    n_disp = sum(1 for r in records if r["dispatched"])
    n_cons = sum(1 for r in records if r["dispatched"] and r["consumed"])
    n_test = sum(1 for r in records if r["tested_count"] > 0)
    n_unw = len(by_status.get("UNWIRED", []))
    n_part = len(by_status.get("PARTIAL", []))
    n_closed = len(by_status.get("CLOSED", []))
    n_extra = len(by_status.get("EXTRA", []))
    n_exp = len(by_status.get("EXPERIMENTAL", []))
    n_exp_decl = sum(1 for r in by_status.get("EXPERIMENTAL", []) if r["declared"])
    n_exp_extra = n_exp - n_exp_decl
    L.append("- 合约总数（Declared，docs/api/command-contracts.md）：**" + str(declared_total) + "**")
    L.append("- 已注册（Dispatched）：**" + str(n_disp) + "**")
    L.append("- 触达效果面（Consumed，调用形上下文+一跳穿透 v4）：**" + str(n_cons) + "**")
    L.append("- 测试引用（Tested，启发式计数）：**" + str(n_test) + "**")
    L.append("- UNWIRED：" + str(n_unw) + " · PARTIAL：" + str(n_part)
             + " · CLOSED：" + str(n_closed) + " · EXTRA：" + str(n_extra) + " · EXPERIMENTAL(人工)：" + str(n_exp))
    # t197 (032 5): four-layer closure productization. The legacy stats line
    # above is kept byte-stable (generate_plan_status.py regex depends on it).
    n_plat4 = sum(1 for r in records
                  if platform_reported((r.get("override") or {}).get("platform_tested")))
    n_pkg4 = sum(1 for r in records
                 if packaged_reported((r.get("override") or {}).get("packaged")))
    L.append("- **结构扫描与人工声明**：Structural Closed=" + str(n_closed)
             + " · Test references=" + str(n_test)
             + " · Platform declarations=" + str(n_plat4) + " · Package declarations=" + str(n_pkg4))
    L.append("  - 列注记：测试引用只表示源码中发现引用；未读取运行日志，不代表执行、通过或覆盖率。"
             "Platform/Package/Observable 为人工声明，原始平台、包和设备证据未由本扫描器重验。")
    all_phantom = []
    seen_ph = set()
    for r in records:
        for p in (r.get("phantom_hits") or []):
            k = (p["name"], p["file"], p["line"])
            if k not in seen_ph:
                seen_ph.add(k)
                all_phantom.append(p)
    all_phantom.sort(key=lambda x: (x["file"], x["line"], x["name"]))
    n_phant = len(all_phantom)
    L.append("- **幻影绑定（v5）**：**" + str(n_phant) + "** 处 backend.<name> 调用命中")
    L.append("  - 提取模式：" + str(surface_meta.get("pattern", "")))
    L.append("  - 清单大小：" + str(surface_meta.get("union", 0)) + " 个可解析名（cpp="
             + str(surface_meta.get("cpp", 0)) + " · shim=" + str(surface_meta.get("shim", 0))
             + " · factory=" + str(surface_meta.get("factory", 0))
             + " · kag=" + str(surface_meta.get("kag", 0))
             + "，绑定文件 " + str(surface_meta.get("files", 0)) + " 个）")
    js_surface = js_backend_surface()
    js_only = sorted(set(p["name"] for p in all_phantom) - js_surface)
    js_both = sorted(set(p["name"] for p in all_phantom) & js_surface)
    if js_only or js_both:
        L.append("  - web/jsBackend 交叉核对（仅报告，不参与判定）：幻影名在 web/bridge.js 亦有=" + (",".join(js_both) or "（无）"))
        L.append("    · 原生+js 均无=" + (",".join(js_only) or "（无）"))
    L.append("- **恒等式：**" + str(declared_total) + " = CLOSED(" + str(n_closed)
             + ") + PARTIAL(" + str(n_part) + ") + UNWIRED(" + str(n_unw) + ") + EXPERIMENTAL(在册 " + str(n_exp_decl) + ")；"
             + str(n_disp) + " = " + str(declared_total) + "(Declared) + EXTRA(" + str(n_extra) + ") + EXPERIMENTAL(合约外 " + str(n_exp_extra) + ")**")
    L.append("")
    L.append("**范围声明（t103 MUST-FIX 3）**：本矩阵的 " + str(declared_total)
             + " = 声明式 KAG 命令合约闭包（docs/api/command-contracts.md 全量条目）。下列能力**不在 "
             + str(declared_total) + " 内**：")
    L.append("- 原生手势链：SwipeDown / SwipeUp / LongPress / Pinch / TwoFingerTap / ThreeFingerHold（平台层）；")
    L.append("- 文本标记参数：letter_spacing / spacing / font / line_height 等内联标记（非命令）；")
    L.append("- KAG.* Lua API：jump/call/return_to_caller 等直接 API（注册键存在但非合约命令，见 EXTRA 的 api-alias）。")
    L.append("这些能力由矩阵底部「人工判级（范围外能力）」区段承载（docs/design/capability-closure-overrides.json 驱动）。")
    L.append("**EXTRA 属设计行为**：contracts 由声明式 schema registry（kag/schema.lua，经 scripts/schema_doc.lua 生成）产出；流控/API 命令按设计不在注册表——EXTRA 不是缺陷信号（A 类入册与否=产品决策待议）。")
    L.append("")
    L.append("> 状态定义：**UNWIRED**=有合约无处理器；**PARTIAL**=已注册但处理器体未以调用形触达效果面（v4 一跳穿透下仍无命中）；")
    L.append("> **CLOSED**=已注册且调用形触达效果面；**EXTRA**=已注册但无合约。")
    L.append("> **EXPERIMENTAL**=人工覆盖状态（能力存在但无消费方/无真实测试面——freeze 政策显式标注；见『EXPERIMENTAL』节与 overrides reason/note；机器判级仍为 PARTIAL/EXTRA）。")
    L.append("> ⚠ = 人工覆盖（docs/design/capability-closure-overrides.json；详见『人工覆盖』节）。")
    L.append("> \u3000* = 存在幻影绑定命中（backend.<name> 不在原生绑定面；详见『幻影绑定（v5）』节）；Consumed 列已按 v5 幻影过滤。")
    L.append("")
    L.append("> 状态定义（v7 附加）：**PAIRING/EXEMPT_PURE/EXEMPT_CONSUMED**=t192 机器判级类别——配对/别名同族效果面并接、纯状态/纯 Lua 执行豁免、非 backend 直调消费豁免；由审计批（t181/t188/t189/t190）裁决名单驱动，不依赖 overrides status 字段，机器自判 CLOSED。")
    L.append("")
    if any(r.get("v7_cat") for r in records):
        L.append("## v7 机器判级（t192）")
        L.append("")
        for cat, title in (("PAIRING", "PAIRING_GROUPS（配对/别名同族并接）"),
                           ("EXEMPT_PURE", "EXEMPT_PURE（纯 Lua 状态/专属命名空间豁免）"),
                           ("EXEMPT_CONSUMED", "EXEMPT_CONSUMED（非 backend 直调消费）")):
            rows = [r for r in records if r.get("v7_cat") == cat]
            L.append("### " + title + " — " + str(len(rows)) + " 条")
            L.append("")
            for r in sorted(rows, key=lambda x: x["name"]):
                src = {**EXEMPT_PURE, **EXEMPT_CONSUMED}.get(r["name"], "t181/t189")
                L.append("- " + r["name"] + " — " + src
                         + ("  [status: " + r["status"] + "]" if r["status"] != "CLOSED" else ""))
        L.append("")
    L.append("## Commands")
    L.append("")
    L.append("| Command | Declared | Dispatched | Consumed | Structural | Test references | Platform declaration | Package declaration | Observable declaration | 结构与声明来源 |")
    L.append("|---|---|---|---|---|---|---|---|---|---|")
    for r in sorted(records, key=lambda x: x["name"]):
        ov = r.get("override") or {}
        obs = str(ov.get("observable", "?"))
        pt = normalize_platform_tested(ov.get("platform_tested", "-")) or "-"
        pk = normalize_packaged(ov.get("packaged", "-")) or "-"
        mark = " ⚠" if ov else ""
        phmark = "*" if (r.get("phantom_hits") or []) else ""
        L.append("| " + r["name"] + phmark + " | "
                 + ("Y" if r["declared"] else "n") + " | "
                 + ("Y" if r["dispatched"] else "n") + " | "
                 + ("Y" if r["consumed_v5"] else "n") + " | "
                 + r["status"] + (mark if (r.get("override") or {}).get("status") else "") + " | "
                 + (str(r["tested_count"]) if r["tested_count"] else "-") + " | "
                 + pt + mark + " | " + pk + mark + " | "
                 + obs + mark + " | "
                 + (r["evidence"] or "-") + " |")
    L.append("")
    L.append("## 人工覆盖（⚠）")
    L.append("")
    ovs = [r for r in records if r.get("override")]
    if ovs:
        for r in sorted(ovs, key=lambda x: x["name"]):
            ov = r["override"]
            raw_note = ""
            if ov.get("status"):
                raw_note = " (raw: " + str(r.get("status_machine")) + ")"
            L.append("- " + r["name"] + " — Observable=" + str(ov.get("observable", "?"))
                     + " · PlatformTested=" + str(ov.get("platform_tested", "?"))
                     + " · Packaged=" + str(ov.get("packaged", "?"))
                     + ((" · Status=" + str(ov["status"]) + raw_note) if ov.get("status") else ""))
            L.append("  - reason：" + str(ov.get("reason", "")))
            L.append("  - evidence：" + str(ov.get("evidence", "")))
    else:
        L.append("（无）")
    L.append("")
    L.append("## 人工判级（范围外能力）")
    L.append("")
    if oos:
        L.append("| 能力 | 判级 | 证据 | 备注 |")
        L.append("|---|---|---|---|")
        for it in oos:
            L.append("| " + str(it.get("name")) + " | " + str(it.get("grade"))
                     + " | " + str(it.get("evidence")) + " | " + str(it.get("note", "")) + " |")
    else:
        L.append("（无；docs/design/capability-closure-overrides.json 的 out_of_scope 数组驱动）")
    L.append("")
    L.append("## EXPERIMENTAL")
    L.append("")
    exps = sorted(by_status.get("EXPERIMENTAL", []), key=lambda x: x["name"])
    if exps:
        for r in exps:
            ov = r.get("override") or {}
            L.append("- " + r["name"] + " - 人工覆盖状态 EXPERIMENTAL（能力存在但无消费方/无真实测试面）；机器判级：" + (("PARTIAL（合约内）") if r["declared"] else "EXTRA（合约外）"))
            L.append("  - reason：" + str(ov.get("reason", "")))
            L.append("  - evidence：" + str(ov.get("evidence", "")))
            L.append("  - note：" + str(ov.get("note", "")))
    else:
        L.append("（无）")
    L.append("")
    L.append("## 可疑翻转清单（v4 保守维持；含队长裁决）")
    L.append("")
    if suspected:
        for s in suspected:
            adj = ADJUDICATED.get(s["name"])
            if adj:
                L.append("- " + s["name"] + " — v3 " + s["v3"] + " → v4 " + s["v4"] + "；**已裁决：" + str(adj.get("verdict")) + "**（理由与 v5 候选见下）")
                L.append("  - reason：" + str(adj.get("reason")))
            else:
                L.append("- " + s["name"] + " — v3 " + s["v3"] + " → v4 " + s["v4"] + "；v4 判据翻 CLOSED 但非人工已核 VERIFIED——保守维持 v3，待裁决")
    else:
        L.append("（无）")
    L.append("")
    L.append("## 幻影绑定（v5）")
    L.append("")
    L.append("> v5 判据：backend.<name> 命中若 name 不在真实绑定面（bindings/*.cpp luaL_Reg + backend.lua shim def + backend_factory cmd 分派 + kag.lua KAG def 的并集，见上方「幻影绑定（v5）」提取模式）——该命中为**幻影**：从 Consumed 证据中剔除，并在本节列出 file:line。过时的宿主侧（web jsBackend）提供同名项不改变判定：原生绑定面以本机 src/script/bindings 与 scripts/ 为唯一口径。")
    L.append("")
    if all_phantom:
        if js_both:
            L.append("> web/jsBackend 同步存在（原生缺失，web 桥提供）：" + ", ".join(js_both))
        if js_only:
            L.append("> 原生与 web/jsBackend **均无**（两端都不存在）：" + ", ".join(js_only))
        L.append("")
        L.append("| 命令 | 幻影名 | file:line |")
        L.append("|---|---|---|")
        for r in sorted(records, key=lambda x: x["name"]):
            ph = r.get("phantom_hits") or []
            if not ph:
                continue
            for p in ph:
                L.append("| " + r["name"] + " | " + str(p.get("name"))
                         + " | " + str(p.get("file")) + ":" + str(p.get("line")) + " |")
    else:
        L.append("（无；所有 backend.* 调用名均在绑定面上）")
    L.append("")
    L.append("## 版本翻转（v4→v5 幻影过滤）")
    L.append("")
    if flips_v45:
        for f in sorted(flips_v45, key=lambda x: x["name"]):
            phnames = ", ".join(sorted({p.get("name", "?") for p in f.get("phantom", [])}))
            L.append("- " + f["name"] + " — v4 " + f["v4"] + " → v5 " + f["v5"]
                     + "；幻影名：" + (phnames or "?") + "（机器判级，非人工翻绿）")
    else:
        L.append("（无）")
    L.append("")
    L.append("## 可疑翻转清单（v5 人类级保守维持）")
    L.append("")
    if suspected_v5:
        for s in suspected_v5:
            adj = ADJUDICATED.get(s["name"])
            if adj:
                L.append("- " + s["name"] + " — v4 " + s["v4"] + " → v5 " + s["v5"]
                         + "；**已裁决：" + str(adj.get("verdict")) + "**（幻影证据见下）")
                L.append("  - reason：" + str(adj.get("reason")))
            else:
                L.append("- " + s["name"] + " — v4 " + s["v4"] + " → v5 " + s["v5"]
                         + "；人类 VERIFIED 被幻影过滤降级——保守维持人类级（v4），待队长裁决")
            for p in s.get("phantom", [])[:10]:
                L.append("  - 幻影命中：" + str(p.get("name")) + " @ "
                         + str(p.get("file")) + ":" + str(p.get("line")))
    else:
        L.append("（无）")
    L.append("")
    L.append("## UNWIRED")
    L.append("")
    unwired = sorted(by_status.get("UNWIRED", []), key=lambda x: x["name"])
    if unwired:
        for r in unwired:
            L.append("- " + r["name"] + " - 合约行 " + str(r["contract_line"]) + "；无处理器注册（flow 命令由 scheduler.lua 内联分发；详见局限）")
    else:
        L.append("（无）")
    L.append("")
    L.append("## PARTIAL")
    L.append("")
    partial = sorted(by_status.get("PARTIAL", []), key=lambda x: x["name"])
    if partial:
        for r in partial:
            L.append("- " + r["name"] + " - " + str(r["evidence"]) + "；处理器体未以调用形触达效果面（v4 一跳穿透下仍无命中；注释/字符串已剥离）")
    else:
        L.append("（无）")
    L.append("")
    L.append("## EXTRA")
    L.append("")
    extra = sorted(by_status.get("EXTRA", []), key=lambda x: x["name"])
    if extra:
        for r in extra:
            L.append("- " + r["name"] + " - " + str(r["evidence"]) + "；" + str(r.get("source_type") or "?") + "（已注册但无合约条目）")
    else:
        L.append("（无）")
    L.append("")
    L.append("## 私有辅助（_ 前缀，注册但不属命令面）")
    L.append("")
    if private:
        for name, f, ln in sorted(private):
            L.append("- " + name + " - " + f + ":" + str(ln))
    else:
        L.append("（无）")
    L.append("")
    L.append("## 数据与判级局限（v5）")
    L.append("")
    L.append("9. **v5 幻影绑定过滤**：Consumed 的 backend.* 命中按名对照真实绑定面（bindings/*.cpp luaL_Reg + backend.lua shim def + backend_factory cmd 分派 + kag.lua KAG def 的并集，动态提取）校验；不在面上的命中为幻影，从 Consumed 证据剔除并在『幻影绑定（v5）』列 file:line。幻影命中可能因注释剥离/多跳链（>1 跳穿透）漏检——只按已捕获的调用形上下文判定；web/jsBackend 同名项仅作交叉报告，不改变原生判定。")
    L.append("1. **Consumed 为调用形文本启发式**：注释与字符串字面量先被剥离（strip_lua 状态机），要求 backend./layers./kag. 的 <ident>( 调用形，或 ctx.tf. / ctx.tf[ / ctx.tf= 字段/赋值。仍可能低估（经本地别名或工具函数间接调用时本体不含直接调用；如 palette 命令经 palette 模块间接生效——如实归 PARTIAL），也可能高估（kag. 自派发计入）。脚本不执行 Lua，无法做数据流分析。")
    L.append("2. **Dispatched 为静态解析**：commands 导出表函数/赋值键 + kag.lua 显式映射与 function KAG.x 定义 + sma_commands 子表。jump/call/endmacro 等直接 API 计入 EXTRA(api-alias)；[jump]/[if] 等 token 的流控处理由 scheduler.lua 编译期内联；两条轨道并存，本扫描器按注册键计 Dispatched。")
    L.append("3. **Tested 为原始引用计数**：tests/scripts/*.lua 与 web/*.test.js 中 [<name> 或 kag.<name> 出现次数，不区分断言与非断言上下文（注释/数组/字符串也算）。")
    L.append("4. **Observable / Platform / Packaged 协议化（t197）**：Observable 可由 overrides JSON 人工覆盖（⚠ 标记）；"
             "Platform（platform_tested）/ Packaged（packaged）现为协议值（默认 '-'=无证据；platform_tested 为 "
             + ",".join(sorted(PLATFORM_ENUMS)) + " 逗号分隔去重排序子集，packaged 为 <=" + str(MAX_PACKAGED_LEN)
             + " 字符描述），非协议值由扫描器自动迁移为 '-'（诚实未验证）；平台运行矩阵/打包验证由 Phase2 分发逐项补证。")
    L.append("5. 判级只依赖命令名静态匹配；同名异构（如 vfx 的 flash 与 transition 的 flash）以注册表实际键为准。导出表引用的子表（如 TransCommands.Bezier = Bezier）经 pairs() 一并注册为调度键——EXTRA(subtable-key)，非用户命令面。")
    L.append("6. 合约计数以 command-contracts.md 的 ### 条目数为准（表头标注 134 须一致）。")
    L.append("7. overrides JSON 的 commands 键必须落在已知命令名集合内；未知键被响亮拒绝（exit 非 0），绝不静默忽略。")
    L.append("8. **v4 已修复（历史注记保留）**：v3 判据只扫 handler 直接体——同文件工具函数/委托链内的效果面调用（t110-t119 五批人工核真 18+ 例：layout/layout_slot/tween/vibrate/nameplate 的工具函数链、模块表委托 toast.show/VFX.flash/HistoryUI.show 等）不被捕获；v4 一跳穿透（同文件 local + require()d 模块函数）已覆盖该盲区。仍存在的判定噪声：跨两跳以上的链（工具函数再调工具函数）、绑定接口（binding().draw_mesh 类——sma_play 等经人工证据层覆盖）、rawset(ctx.tf, ...) 形态（判据边缘）。")
    L.append("10. **raw 口径（t185/t192 定稿）**：任何『raw/机器原判级』汇总一律以**记录级 status_machine** 为准（=overrides 人工裁决与 v7 类别应用之前的机器判级，永不丢弃）；status_counts_v4_raw/status_counts_v5_raw 为版本快照口径，仅作对账，不作最终判定依据。")
    L.append("")
    L.append("## 复现")
    L.append("")
    L.append("```")
    L.append("python scripts/capability_closure.py")
    L.append("```")
    L.append("")
    return NL.join(L) + NL


def main():
    declared_names_probe = set(parse_contracts())
    handlers_probe, _ = parse_command_modules()
    handlers_probe.update(parse_sma_commands())
    _, kag_defs_probe = parse_kag_lua_mappings()
    handlers_probe.update(kag_defs_probe)
    known = sorted(set(declared_names_probe) | set(handlers_probe))
    overrides_map, oos = load_overrides(set(known))
    records, private, declared_total, suspected, suspected_v5, flips_v45, surface_meta = build_records(overrides_map)
    guard_partial(records)   # t103 locks check the PRE-v7 machine grade
    v7_flips = apply_v7(records)  # t192 machine-grade categories (PAIRING/EXEMPT)
    guard_categories()
    guard_phantom(records)
    fp = source_fingerprint()
    generated_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(source_mtime([
        CONTRACTS, KAG_LUA, SMA_LUA, COMMANDS_DIR, TESTS_LUA, TESTS_WEB, OVERRIDES,
        BACKEND_LUA_PATH, FACTORY_PATH, BINDINGS_DIR])))
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "generated_at": generated_at,
        "evidence_scope": "STATIC_SCAN_AND_MANUAL_DECLARATIONS",
        "runtime_evidence_verification": "NOT_RUN",
        "source_fingerprint": fp,
        "scanner": "scripts/capability_closure.py",
        "sources": {
            "contracts": str(CONTRACTS.relative_to(REPO)),
            "command_modules": str(COMMANDS_DIR.relative_to(REPO)),
            "kag_lua": str(KAG_LUA.relative_to(REPO)),
            "sma_lua": str(SMA_LUA.relative_to(REPO)),
            "tests_scripts": str(TESTS_LUA.relative_to(REPO)),
            "tests_web": str(TESTS_WEB.relative_to(REPO)),
            "overrides": str(OVERRIDES.relative_to(REPO)),
        },
        "counts": {
            "declared": declared_total,
            "dispatched": sum(1 for r in records if r["dispatched"]),
            "consumed": sum(1 for r in records if r["dispatched"] and r["consumed"]),
            "tested": sum(1 for r in records if r["tested_count"] > 0),
        },
        "status_counts": {st: sum(1 for r in records if r["status"] == st)
                          for st in ("UNWIRED", "PARTIAL", "CLOSED", "EXTRA", "EXPERIMENTAL")},
        "status_counts_v4_raw": {st: sum(1 for r in records if r["status_v4"] == st)
                                 for st in ("UNWIRED", "PARTIAL", "CLOSED", "EXTRA", "EXPERIMENTAL")},
        "suspected_flips": suspected,
        "suspected_flips_v5": suspected_v5,
        "flips_v45": flips_v45,
        "binding_surface": surface_meta,
        "status_counts_v5_raw": {st: sum(1 for r in records if r["status_v5"] == st)
                                 for st in ("UNWIRED", "PARTIAL", "CLOSED", "EXTRA", "EXPERIMENTAL")},
        "status_counts_raw_machine": {st: sum(1 for r in records if (r.get("status_machine") or r["status"]) == st)
                                      for st in ("UNWIRED", "PARTIAL", "CLOSED", "EXTRA", "EXPERIMENTAL")},
        "commands": records,
        "private_helpers": [{"name": n, "file": f, "line": ln} for n, f, ln in private],
    }
    with open(OUT_JSON, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=2)
        fh.write(NL)
    md = render_markdown(records, private, declared_total, oos, generated_at, fp,
                         suspected, suspected_v5, flips_v45, surface_meta)
    OUT_MD.parent.mkdir(parents=True, exist_ok=True)
    OUT_MD.write_text(md, encoding="utf-8")
    print("[closure] declared=" + str(declared_total)
          + " dispatched=" + str(payload["counts"]["dispatched"])
          + " consumed=" + str(payload["counts"]["consumed"])
          + " tested=" + str(payload["counts"]["tested"]))
    print("[closure] statuses=" + str(payload["status_counts"]))
    print("[closure] v4_raw=" + str(payload["status_counts_v4_raw"]))
    print("[closure] v7_flips=" + str(len(v7_flips)) + " " + " ".join(sorted(v7_flips)))
    print("[closure] raw_machine(status_machine)=" + str({st: sum(1 for r in records if (r.get("status_machine") or r["status"]) == st) for st in ("UNWIRED", "PARTIAL", "CLOSED", "EXTRA", "EXPERIMENTAL")}))
    print("[closure] overrides=" + str(len(overrides_map)) + " out_of_scope=" + str(len(oos)))
    print("[closure] suspected_flips=" + str(len(suspected))
          + " v5_suspect=" + str(len(suspected_v5))
          + " flips_v45=" + str(len(flips_v45))
          + " phantom_hits=" + str(sum(1 for r in records for _ in (r.get("phantom_hits") or []))))
    print("[closure] binding_surface=" + str(surface_meta))
    print("[closure] json -> " + str(OUT_JSON.relative_to(REPO)) + "  md -> " + str(OUT_MD.relative_to(REPO)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
