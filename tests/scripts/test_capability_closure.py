#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_capability_closure.py — t197 (032 §5) four-layer closure productization suite.

Covers the four contracts:
  A  matrix column evolution: Command | Declared | Dispatched | Consumed |
     Structural | Runtime | Platform | Packaged | Observable | 证据
     (Status -> Structural rename; Tested -> Runtime with checkmark+N;
      Platform Tested -> Platform; both Platform/Packaged protocolized below)
  B  overrides value-domain protocol: platform_tested (missing / '-' /
     comma subset of win,linux,macos,web,android,ios, deduped+sorted) and
     packaged (missing / '-' / non-empty string <= 120 chars); non-protocol
     values migrated to '-' (no evidence = unverified); survivors exit 2.
  C  overview stats: legacy line kept byte-stable (generate_plan_status.py
     regex dependency) + new four-layer line + column notes.
  D  cases 01-10 below (header / structural sample / runtime pos-neg /
     platform-packaged protocol fixture / illegal platform exit 2 /
     four-layer counts + matrix reconciliation / legacy line +
     plan_status-compatible regexes / idempotency / migration counts /
     status validation unchanged).

Run directly:  python tests/scripts/test_capability_closure.py

No pytest registration and no ctest wiring needed -- this repo never
auto-discovers tests/*.py (the Captain wires CI). The suite touches only
temp dirs; docs/design/ and scripts/ are only read, never written.
"""

import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))

import capability_closure as cc  # noqa: E402

SURFACE_META = {"pattern": "test", "union": 0, "cpp": 0, "shim": 0,
                "factory": 0, "kag": 0, "files": 0}


def rec(name, status="CLOSED", tested=0, override=None, declared=True,
        dispatched=True, consumed=True, contract_line=1,
        evidence="scripts/kag/commands/x.lua:1"):
    """Minimal record shaped like build_records() output (render-surface keys)."""
    return {
        "name": name, "declared": declared, "dispatched": dispatched,
        "consumed": consumed, "consumed_v3": consumed, "consumed_v5": consumed,
        "consumed_hits": [], "hits_v5": [],
        "tested_count": tested, "tested_files": [],
        "status": status, "status_machine": "CLOSED",
        "status_v3": status, "status_v4": status, "status_v5": status,
        "phantom_hits": [], "phantom_real": [],
        "evidence": evidence, "alias": None,
        "contract_line": contract_line, "source_type": "def",
        "override": override, "flip": None, "flip_v5": None, "v7_cat": None,
    }


def render(records, declared_total=None, private=None, oos=None):
    return cc.render_markdown(
        records, private or [],
        declared_total if declared_total is not None else len(records),
        oos or [], "2026-09-04T00:00:00Z", "deadbeef", [], [], [], SURFACE_META)


def cells(line):
    if not (line.startswith("| ") and line.endswith(" |")):
        raise AssertionError("not a table row: " + line[:60])
    return [c.strip() for c in line.strip().strip("|").split("|")]


class TestFourLayerColumns(unittest.TestCase):
    """Contract A: column evolution."""

    def setUp(self):
        self.records = [rec("alpha"), rec("beta", status="PARTIAL", tested=3),
                        rec("gamma", status="UNWIRED")]
        self.md = render(self.records)

    def _header(self):
        return [l for l in self.md.splitlines() if l.startswith("| Command |")][0]

    def test_01_header_has_four_layer_columns(self):
        h = self._header()
        self.assertEqual(
            h,
            "| Command | Declared | Dispatched | Consumed | Structural | Test references "
            "| Platform declaration | Package declaration | Observable declaration | 结构与声明来源 |")
        self.assertNotIn("| Tested |", h)
        self.assertNotIn("| Status |", h)
        self.assertNotIn("Platform Tested", h)

    def test_02_structural_column_equals_status(self):
        rows = {c[0]: c for l in self.md.splitlines()
                if l.startswith("| ") and (c := cells(l))[0] in ("alpha", "beta", "gamma")}
        self.assertEqual(rows["alpha"][4], "CLOSED")
        self.assertEqual(rows["beta"][4], "PARTIAL")
        self.assertEqual(rows["gamma"][4], "UNWIRED")
        # overrides status adjudication stays final + ⚠ (t185/t192 discipline)
        md2 = render(self.records + [rec("delta", override={"status": "CLOSED",
                                                           "observable": "VERIFIED"})])
        for l in md2.splitlines():
            if l.startswith("| ") and cells(l)[0] == "delta":
                self.assertEqual(cells(l)[4], "CLOSED ⚠")

    def test_03_runtime_checkmark_and_dash(self):
        rows = {c[0]: c for l in self.md.splitlines()
                if l.startswith("| ") and (c := cells(l))[0] in ("alpha", "beta", "gamma")}
        self.assertEqual(rows["alpha"][5], "-")   # tested_count == 0
        self.assertEqual(rows["beta"][5], "3")   # references, not passing executions
        self.assertEqual(rows["gamma"][5], "-")

    def test_04_platform_packaged_protocol_output(self):
        recs = self.records + [
            rec("delta", override={"platform_tested": "win,linux", "packaged": "v1.2.3"}),
            rec("epsilon", override={"observable": "VERIFIED"}),
        ]
        md = render(recs)
        rows = {c[0]: c for l in md.splitlines()
                if l.startswith("| ") and (c := cells(l))[0] in ("delta", "epsilon", "alpha")}
        # protocolized output: deduped + sorted subset; default '-'
        self.assertEqual(rows["delta"][6], "linux,win ⚠")
        self.assertEqual(rows["delta"][7], "v1.2.3 ⚠")
        # override presence still annotates the cell with ⚠ (mark semantics
        # unchanged); the protocol value itself defaults to '-'
        self.assertEqual(rows["epsilon"][6], "- ⚠")
        self.assertEqual(rows["epsilon"][7], "- ⚠")
        self.assertEqual(rows["alpha"][6], "-")
        self.assertEqual(rows["alpha"][7], "-")


class TestProtocolDomain(unittest.TestCase):
    """Contract B: value-domain protocol + migration + exit-2 guard."""

    def test_05_illegal_platform_value_exit_2(self):
        self.assertIsNone(cc.normalize_platform_tested("?"))
        self.assertIsNone(cc.normalize_platform_tested("win,foo"))
        self.assertIsNone(cc.normalize_platform_tested(["win"]))
        self.assertIsNone(cc.normalize_packaged(123))
        self.assertIsNone(cc.normalize_packaged("x" * 121))
        self.assertEqual(cc.normalize_platform_tested("win,linux,win"), "linux,win")
        bad = cc.validate_protocol_fields({"a": {"platform_tested": "win,foo"}})
        self.assertEqual(len(bad), 1)
        self.assertEqual(bad[0], ("a", "platform_tested", "win,foo"))
        # exit-2 wiring: load_overrides rejects illegal values loudly (migration
        # stubbed out to expose the guard; in the normal flow migration fixes
        # legacy values first, the guard catches anything that survives).
        with tempfile.TemporaryDirectory() as td:
            f = Path(td) / "overrides.json"
            f.write_text(json.dumps({"commands": {"a": {"platform_tested": "win,foo"}}}),
                         encoding="utf-8")
            with mock.patch.object(cc, "OVERRIDES", f), \
                    mock.patch.object(cc, "migrate_protocol_fields",
                                      lambda data, path: (0, 0, 0, 0)):
                with self.assertRaises(SystemExit) as cm:
                    cc.load_overrides({"a"})
                self.assertEqual(cm.exception.code, 2)

    def test_09_migration_counts_and_rewrite(self):
        data = {"commands": {
            "a": {"platform_tested": "?", "packaged": "?"},
            "b": {"platform_tested": "win,linux,win", "packaged": "tool x"},
            "c": {"platform_tested": "-", "packaged": "-"},
        }, "out_of_scope": []}
        with tempfile.TemporaryDirectory() as td:
            f = Path(td) / "overrides.json"
            import json as _json
            _json.dump(data, io.open(f, "w", encoding="utf-8"), ensure_ascii=False)
            # first migrate: '?' -> '-', canonicalize b, c untouched
            got = cc.migrate_protocol_fields(data, f)
            self.assertEqual(got, (1, 1, 2, 1))  # bad_pt=1, bad_pk=1, chg_pt=2, chg_pk=1
            saved = _json.load(io.open(f, encoding="utf-8"))
            self.assertEqual(saved["commands"]["a"],
                             {"platform_tested": "-", "packaged": "-"})
            self.assertEqual(saved["commands"]["b"],
                             {"platform_tested": "linux,win", "packaged": "tool x"})
            self.assertEqual(saved["commands"]["c"],
                             {"platform_tested": "-", "packaged": "-"})
            # second migrate: no changes (idempotent)
            got2 = cc.migrate_protocol_fields(data, f)
            self.assertEqual(got2, (0, 0, 0, 0))
            saved2 = _json.load(io.open(f, encoding="utf-8"))
            self.assertEqual(saved2, saved)

    def test_10_status_validation_unchanged(self):
        self.assertEqual(cc.VALID_OVERRIDE_STATUS,
                         {"CLOSED", "PARTIAL", "EXTRA", "UNWIRED", "EXPERIMENTAL"})
        with tempfile.TemporaryDirectory() as td:
            f = Path(td) / "overrides.json"
            f.write_text(json.dumps({"commands": {"a": {"status": "BOGUS"}}}),
                         encoding="utf-8")
            with mock.patch.object(cc, "OVERRIDES", f):
                with self.assertRaises(SystemExit) as cm:
                    cc.load_overrides({"a"})
                self.assertEqual(cm.exception.code, 2)


class TestStatsLine(unittest.TestCase):
    """Contract C: legacy line byte-stable + four-layer line + reconciliation."""

    def setUp(self):
        self.records = [
            rec("a", tested=3),
            rec("b", override={"platform_tested": "win"}),
            rec("c", tested=2),
            rec("d", status="PARTIAL", override={"packaged": "v1"}),
            rec("e", status="UNWIRED"),
            rec("f", status="EXPERIMENTAL"),
            rec("g", status="EXTRA", declared=False),
        ]
        # declared_total = contracts in fixture (a..f: 6); g = EXTRA (contract-out)
        self.md = render(self.records, declared_total=6)

    def _legacy_line(self):
        return [l for l in self.md.splitlines() if l.startswith("- UNWIRED：")][0]

    def test_06_four_layer_line_and_reconciliation(self):
        self.assertIn(
            "- **结构扫描与人工声明**：Structural Closed=3 · Test references=2"
            " · Platform declarations=1 · Package declarations=1",
            self.md)
        # reconciliation: the line counts must equal the table column counts
        # (⚠ is a manual-override annotation; strip it before value compare)
        rows = [cells(l) for l in self.md.splitlines()
                if l.startswith("| ") and len((c := cells(l))) == 10 and c[0] in
                ("a", "b", "c", "d", "e", "f", "g")]
        for c in rows:
            for i in (5, 6, 7):
                c[i] = c[i].replace(" ⚠", "")
        self.assertEqual(sum(1 for c in rows if c[5] != "-"), 2)   # Runtime ✓
        self.assertEqual(sum(1 for c in rows if c[6] != "-"), 1)   # Platform
        self.assertEqual(sum(1 for c in rows if c[7] != "-"), 1)   # Packaged
        self.assertIn("测试引用只表示源码中发现引用；未读取运行日志，不代表执行、通过或覆盖率",
                      self.md)
        self.assertNotIn("Runtime 测试证据", self.md)

    def test_07_legacy_stats_line_preserved_for_plan_regex(self):
        line = self._legacy_line()
        self.assertEqual(
            "- UNWIRED：1 · PARTIAL：1 · CLOSED：3 · EXTRA：1 · EXPERIMENTAL(人工)：1",
            line)
        # generate_plan_status.py fact_closure regex shape (t196):
        #   re.escape(key) + r"[^：]*：\s*(\d+)"
        for key, expect in (("UNWIRED", "1"), ("PARTIAL", "1"), ("CLOSED", "3"),
                            ("EXTRA", "1"), ("EXPERIMENTAL", "1")):
            m = re.search(re.escape(key) + r"[^：]*：\s*(\d+)", line)
            self.assertIsNotNone(m, "regex miss for " + key)
            self.assertEqual(m.group(1), expect, key)

    def test_08_render_idempotent(self):
        md2 = render(self.records, declared_total=6)
        self.assertEqual(md2, self.md)
        # double render of the migration fixture is byte-stable too
        d1 = render([rec("a", tested=3), rec("b", override={"platform_tested": "win,linux"})])
        d2 = render([rec("a", tested=3), rec("b", override={"platform_tested": "win,linux"})])
        self.assertEqual(d1, d2)


class TestV4NestedFunctionSlices(unittest.TestCase):
    """U36/B1 regression: v4 one-hop penetration must include effect-surface
    calls that appear AFTER a nested function inside a penetrated body.

    Before the fix StringUtils_file_slice() stopped at the first nested
    `local function`/`function` line, so real calls such as
    `backend.submit_vfx` (VFX.blur), `backend.create_solid_texture`
    (VFX.flash) and `backend.render_text` (ChapterSelect.show) were cut out of
    the slice -- blur/flash/chapter were misgraded PARTIAL. These tests are the
    RED/GREEN lock for that defect (plus the real repository handler chain).
    """

    def _info(self, lines):
        return {"lines": list(lines)}

    def test_01_slice_includes_effect_after_nested_local_function(self):
        info = self._info([
            "function VFX.flash(ctx, params)",
            "    local flashLayer = {}",
            "    local function cleanup()",
            "        flashLayer.alpha = 0",
            "        flashLayer.dirty = true",
            "    end",
            "    backend.create_solid_texture(255, 255, 255, 255)",
            "end",
            "",
            "function VFX.next(ctx)",
            "    return true",
            "end",
        ])
        body = cc.StringUtils_file_slice(info, 0)
        self.assertIn("backend.create_solid_texture(255, 255, 255, 255)", body)
        self.assertNotIn("function VFX.next", body)

    def test_02_slice_includes_effect_after_multi_level_nested_functions(self):
        info = self._info([
            "function ChapterSelect.show(ctx)",
            "    local bg = {}",
            "    local function cleanup()",
            "        local function inner()",
            "            return true",
            "        end",
            "        return inner()",
            "    end",
            "    backend.render_text('Chapter Select', 20, 8, 255, 200, 60, 255)",
            "    layers.ensure(ctx, '_chapter_bg', 190)",
            "end",
            "",
            "function ChapterSelect.next(ctx)",
            "    return nil",
            "end",
        ])
        body = cc.StringUtils_file_slice(info, 0)
        self.assertIn("backend.render_text('Chapter Select'", body)
        self.assertIn("layers.ensure(ctx, '_chapter_bg', 190)", body)
        self.assertNotIn("function ChapterSelect.next", body)

    def test_03_slice_handles_multiline_strings_and_comments(self):
        info = self._info([
            "function X.blur(ctx)",
            "    local msg = [[",
            "        function not_code() end",
            "    ]]",
            "    -- function commented_out() end",
            "    backend.submit_vfx(1, 2, 0, 0, 0, 0, 4, 0, 0)",
            "end",
            "",
            "function X.after(ctx)",
            "    return true",
            "end",
        ])
        body = cc.StringUtils_file_slice(info, 0)
        self.assertIn("backend.submit_vfx(1, 2, 0, 0, 0, 0, 4, 0, 0)", body)
        self.assertNotIn("function X.after", body)

    def test_04_one_line_and_last_function_slices_are_bounded(self):
        info = self._info([
            "function X.one_liner(ctx) return true end",
            "function X.last(ctx)",
            "    return true",
            "end",
        ])
        first = cc.StringUtils_file_slice(info, 0)
        self.assertIn("function X.one_liner", first)
        self.assertNotIn("function X.last", first)
        last = cc.StringUtils_file_slice(info, 1)
        self.assertIn("function X.last", last)
        self.assertIn("return true", last)

    def test_05_penetration_reaches_effect_behind_nested_function(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "scripts" / "kag" / "commands").mkdir(parents=True)
            handler_path = "scripts/kag/commands/synthetic.lua"
            handler = (
                "function SyntheticCommands.flash(ctx, params)\n"
                "    local VFX = require(\"vfx\")\n"
                "    VFX.flash(ctx, params)\n"
                "end\n"
            )
            module = (
                "local VFX = {}\n"
                "function VFX.flash(ctx, params)\n"
                "    local layer = {}\n"
                "    local function cleanup()\n"
                "        layer.alpha = 0\n"
                "    end\n"
                "    backend.create_solid_texture(1, 2, 3, 255)\n"
                "end\n"
                "return VFX\n"
            )
            (root / handler_path).write_text(handler, encoding="utf-8")
            (root / "scripts" / "vfx.lua").write_text(module, encoding="utf-8")
            saved_repo = cc.REPO
            try:
                with mock.patch.object(cc, "REPO", root):
                    cc.V4_FILES_CACHE.clear()
                    hits = cc.v4_penetrated_hits(handler, handler_path)
            finally:
                cc.V4_FILES_CACHE.clear()
                cc.REPO = saved_repo
        self.assertIn("backend.", hits)

    def test_06_real_repo_partial_commands_reach_effect_surface(self):
        """The three U36 false-PARTIAL commands must penetrate to backend.*."""
        handlers, _private = cc.parse_command_modules()
        for name in ("blur", "flash", "chapter"):
            self.assertIn(name, handlers, name + " handler is registered")
            handler = handlers[name]
            hits = cc.consumed_for({"file": handler["file"]}, handler["body"])
            self.assertIn("backend.", hits,
                          name + " handler one-hop must reach a backend.* call")


if __name__ == "__main__":
    unittest.main()