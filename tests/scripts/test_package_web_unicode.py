#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_package_web_unicode.py — Track A e2e: /api/package/web with true Unicode
story paths and output names (M1-A widenUtf8 + Unicode-safe storyPath/outName).

Launches the real engine in --editor mode (same pattern as
tests/headless_http_smoke.py), POSTs /api/package/web with a Chinese project
dir (tests/projects/<GAME>) and a Chinese out name, then asserts:
  * 200 + outputDir == dist/<outName>
  * dist/<outName>/index.html, assets/, cache/story/story.lua and
    demo/<game>/story.ks exist on disk (real Unicode paths)
  * no mojibake: no U+FFFD and no Latin-1 expansion of the UTF-8 bytes in
    index.html / MANIFEST.txt; MANIFEST round-trips the Chinese game name
  * negatives: ".." -> 400, ":" -> 400, control char -> 400

Standalone (not registered): python tests/scripts/test_package_web_unicode.py
Exit: 0 = all passed, 1 = any failed.
"""

import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

# GitHub Windows runners default stdout to the legacy cp1252 code page; this
# test prints Chinese response details, so force UTF-8 output (round 32 CI).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENGINE = os.path.join(REPO, "build", "Debug", "CaesuraAmeKAG.exe")
PORT = 9876
BASE = "http://127.0.0.1:%d" % PORT
TOKEN = "unicode-pkg-smoke-token"
GAME = "unicode_故事(50%)"
GAME_KS = "tests/projects/%s/story.ks" % GAME
OUT = "dist/故事输出(50%)"


def kill_residual_engines():
    """Port hygiene: stop any engine already holding 9876 by process NAME
    match (CaesuraAmeKAG only — never touch node.exe)."""
    try:
        subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process -Filter \"Name like '%CaesuraAmeKAG%'\" | "
             "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }"],
            capture_output=True, timeout=30)
    except Exception:
        pass  # non-Windows or no powershell: rely on the poll below


def request(path, data=None, timeout=30):
    hdrs = {"Content-Type": "application/json",
            "Authorization": "Bearer " + TOKEN}
    req = urllib.request.Request(BASE + path, data=data, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        return e.code, body


def main():
    failures = []
    def check(name, ok, detail=""):
        print(("PASS " if ok else "FAIL ") + name + (" " + detail if detail else ""))
        if not ok:
            failures.append(name)

    if not os.path.isfile(ENGINE):
        print("FATAL: engine binary not found at", ENGINE)
        sys.exit(1)
    web_ready = (os.path.isdir(os.path.join(REPO, "web", "node_modules", "vite"))
                 or os.path.isfile(os.path.join(REPO, "web", "dist", "index.html")))
    check("web-ready", web_ready, "(web/node_modules/vite or web/dist/index.html)")
    if not web_ready:
        print("SKIP: web packaging toolchain unavailable")
        sys.exit(0 if not failures else 1)

    kill_residual_engines()
    time.sleep(0.5)

    project = os.path.join(REPO, "tests", "projects", GAME)
    out_dir = os.path.join(REPO, OUT)
    proc = None
    try:
        # ---- fixture project (story + assets/) ----
        os.makedirs(os.path.join(project, "assets"), exist_ok=True)
        with open(os.path.join(project, "story.ks"), "w", encoding="utf-8") as f:
            f.write("[ch text=\"Unicode probe\"]\n[p]\n[end]\n")
        with open(os.path.join(project, "assets", "x.png"), "wb") as f:
            f.write(b"\x89PNG")  # 4-byte placeholder

        # ---- launch engine (--editor, cwd=repo root) ----
        env = dict(os.environ)
        env["CAESURA_EDITOR_TOKEN"] = TOKEN
        proc = subprocess.Popen([ENGINE, "--editor"],
                                cwd=REPO, env=env,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        ready = False
        for _ in range(80):
            if proc.poll() is not None:
                break
            try:
                st, _ = request("/api/ping", timeout=1)
                if st == 200:
                    ready = True
                    break
            except Exception:
                time.sleep(0.5)
        check("server-ready", ready)
        if not ready:
            print("engine exited rc=", proc.poll())
            return 1 if failures else 0

        # ---- positive: Unicode storyPath + Unicode outName ----
        st, body = request("/api/package/web",
                           data=json.dumps({"storyPath": GAME_KS,
                                            "outName": "故事输出(50%)"}).encode("utf-8"),
                           timeout=300)
        resp = json.loads(body) if body else {}
        if st != 200 or resp.get("ok") is not True:
            print("PACKAGE WEB FAILURE LOG:\n" + resp.get("logTail", "<missing logTail>"))
        check("package-web-unicode-200", st == 200 and resp.get("ok") is True,
              "%s %s" % (st, body[:300]))
        check("package-web-outputDir", resp.get("outputDir") == OUT,
              str(resp.get("outputDir")))
        check("package-web-logTail-complete",
              "PACKAGE COMPLETE" in resp.get("logTail", ""))

        # ---- real on-disk artifacts with Unicode paths ----
        check("out-index.html",
              os.path.isfile(os.path.join(out_dir, "index.html")))
        check("out-assets-dir",
              os.path.isdir(os.path.join(out_dir, "assets")))
        check("out-story-lua",
              os.path.isfile(os.path.join(out_dir, "cache", "story", "story.lua")))
        check("out-demo-game-ks",
              os.path.isfile(os.path.join(out_dir, "demo", GAME, "story.ks")))

        # ---- mojibake check on the packaged text artifacts ----
        def read_utf8(path):
            with open(path, "rb") as f:
                return f.read().decode("utf-8", errors="replace")
        index_html = read_utf8(os.path.join(out_dir, "index.html"))
        manifest = read_utf8(os.path.join(out_dir, "MANIFEST.txt"))
        check("no-fffd-in-index", "\\ufffd" not in index_html)
        check("no-fffd-in-manifest", "\\ufffd" not in manifest)
        # Latin-1 expansion of CHINESE UTF-8 bytes would surface as the
        # "Ã¤/Â" style sequence (widenAscii's U+00E4 signature) — reject it,
        # and require the Chinese game name to round-trip as proper UTF-8.
        check("no-latin1-in-manifest", "\u00e4" not in manifest.lower())
        check("manifest-utf8-cjk-roundtrip",
              GAME in manifest, "(game name in MANIFEST.txt)")

        # ---- negatives ----
        st, body = request("/api/package/web",
                           data=json.dumps({"storyPath": "tests/projects/%s/../x.ks" % GAME,
                                            "outName": "x"}).encode("utf-8"),
                           timeout=30)
        neg_err = ""
        try:
            neg_err = json.loads(body).get("error", "") if body else ""
        except Exception:
            pass
        check("negative-dotdot-400", st == 400 and ".." in neg_err,
              "%s %s" % (st, body[:200]))
        st, body = request("/api/package/web",
                           data=json.dumps({"storyPath": "C:/demo/x.ks",
                                            "outName": "x"}).encode("utf-8"),
                           timeout=30)
        check("negative-colon-400", st == 400, "%s %s" % (st, body[:200]))
        st, body = request("/api/package/web",
                           data=json.dumps({"storyPath": "demo/example_game/story.ks\u0001",
                                            "outName": "x"}).encode("utf-8"),
                           timeout=30)
        try:
            neg_err = json.loads(body).get("error", "") if body else ""
        except Exception:
            neg_err = ""
        check("negative-control-400",
              st == 400 and "control characters" in neg_err,
              "%s %s" % (st, body[:200]))
        # ---- outName sanitizer locks (review NIT-b) ----
        st, body = request("/api/package/web",
                           data=json.dumps({"storyPath": GAME_KS,
                                            "outName": ".."}).encode("utf-8"),
                           timeout=30)
        check("negative-outname-dotdot-400", st == 400, "%s %s" % (st, body[:200]))
        st, body = request("/api/package/web",
                           data=json.dumps({"storyPath": GAME_KS,
                                            "outName": "<>:\"|*?"}).encode("utf-8"),
                           timeout=30)
        check("negative-outname-all-stripped-400", st == 400, "%s %s" % (st, body[:200]))
        st, body = request("/api/package/web",
                           data=json.dumps({"storyPath": GAME_KS,
                                            "outName": "a<b"}).encode("utf-8"),
                           timeout=30)
        resp2 = json.loads(body) if body else {}
        check("positive-outname-illegal-stripped",
              st == 200 and resp2.get("outputDir") == "dist/ab",
              "%s %s" % (st, body[:200]))
        # cleanup the stripped-name output produced above
        shutil.rmtree(os.path.join(REPO, "dist", "ab"), ignore_errors=True)
    except Exception as exc:
        check("no-exception", False, repr(exc))
    finally:
        if proc is not None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
        shutil.rmtree(project, ignore_errors=True)
        shutil.rmtree(out_dir, ignore_errors=True)

    if failures:
        print("UNICODE PACKAGE E2E: %d FAILED" % len(failures))
        sys.exit(1)
    print("UNICODE PACKAGE E2E: ALL PASSED")
    sys.exit(0)


if __name__ == "__main__":
    main()
