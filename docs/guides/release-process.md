# Caesura (AmeKAG) — Release Process

The current CI, release-tag and Web Pages entry points share
`.github/workflows/validate-engine.yml`, then call
`.github/workflows/verify-release-inputs.yml`. Their current scope is an input
**dry-run**. They upload verification artifacts, including the exact Pages tar,
but do not create a release or deploy a site.
The older Windows manual commands below remain an operational reference;
they cannot replace this candidate verification or grant publication approval.

Before producer fanout, `scripts/prepare_release_policy.py` freezes the clean
execution source, engine version, complete validation profile bytes and the
tracked `scripts/release_input_policy.json` selection. The policy requires
Debug and Release evidence for Windows, Linux and macOS, four final package
roles, a separate Pages artifact role, the iOS configuration gate and the
Android static gate. Android compile
keeps its separately declared audit status. PR execution uses the explicit head
SHA; the caller/reusable workflow commit is recorded separately.

The aggregate uses exact job names and fixed artifact IDs from controlled
workflow outputs. It authenticates every required attempt through GitHub,
downloads the original Actions ZIP and checks its digest, applies strict U1
execution verification and U22 package receipt checks, then reauthenticates
hosted state and rechecks the exact local bytes. A failure, skip, cancellation,
missing output, changed policy/source, version mismatch or substituted byte
prevents the dry-run from passing. An old failed attempt is not erased by a
green retry. All original attempt evidence remains available.

`DRY_RUN_INPUTS_VERIFIED` does not authorize publishing. The release entry point
requires an existing numeric `v<engine-version>` tag resolving to this source;
it never creates or moves one. The Pages entry point retains the selected game
and optional externally hashed UI actions. The Web producer generates a plain
`artifact.tar` once, validates its final bytes through the same isolated Web
runtime checks, then uploads that exact tar separately from its proof bundle.
The gate binds this artifact's fixed ID and outer ZIP digest to the tar accepted
in the Web bundle, its original receipt and the same authenticated producer job.
Pages bytes are excluded from the generic Release asset list; a separate dry-run
plan records their ID and digest without deploying. Both tar copies and all proof
inputs are rechecked before that plan is returned.

`upload-pages-artifact` cannot run after this acceptance because it rebuilds the
tar. `deploy-pages@v4` publicly selects by name and has no `artifact_id` input;
a future publisher needs an explicitly authorized adapter to the Pages API's
fixed-ID deployment endpoint. Signing and any other later byte transform also
need new final-byte verification. Current hosted job names, package names and
actual Pages tar runtime evidence must be confirmed by candidate execution;
policy text or local fixture tests alone are not that evidence.

Candidate execution and remaining gaps are recorded in
[`2026-09-19-014-release-input-provenance-execution.md`](../plans/2026-09-19-014-release-input-provenance-execution.md).

## 0. Prerequisites

- **Git**: with `git` on `PATH`.
- **CMake 3.25+** and a Visual Studio 2022 toolchain.
- **Python 3.12+** for the current validation and package controllers (stdlib only).
- **gh** (GitHub CLI, `gh --version`) — authenticated (`gh auth login`).
- **SDL3**: optional on Windows — the repo ships a prebuilt package at
  `external/SDL3/SDL3-3.2.0/cmake` and `CMakeLists.txt:33-42` selects it whenever
  no `SDL3_DIR` and no `CMAKE_TOOLCHAIN_FILE` are given (this machine's
  `build/CMakeCache.txt` resolves `SDL3_DIR` to that bundled package and contains
  zero vcpkg entries). Use vcpkg (`vcpkg install sdl3 --triplet x64-windows`) only
  when you deliberately want a system SDL3 — CI's release job does.
- **Steamworks**: a `-DCAESURA_HAS_STEAM=ON` build links `steam_api64.dll`, which
  `install()` does **not** ship. Such an executable dies at startup in a clean
  extract with `error while loading shared libraries: steam_api64.dll` (reproduced
  on a `cmake --install` tree), so either package the DLL or release a
  `CAESURA_HAS_STEAM=OFF` build.

> Releases are **tagged from `master`**. All work lands on `master` first,
> gated green, and only then is tagged and published.

---

## 0.5 历史 Windows 手工步骤参考

以下图示保留构建及手工命令的操作顺序，不是当前 CI 的完整门禁清单。
执行到发布前必须另外完成上文的同一候选来源与最终字节检查，并取得该次发布授权。

```
[master 分支] 所有改动先合入 master（门禁全绿）
      │
      ▼
┌─ 1. 构建 ─────────────────────────────────────────────┐
│  cmake -B build -S . -G "Visual Studio 17 2022" -A x64 │  （vcpkg 需 -DCMAKE_TOOLCHAIN_FILE=...）
│  cmake --build build --config Release --parallel       │
└──────────────────────┬───────────────────────────────┘
                       ▼
┌─ 2. 全量门禁（跳过=禁止发版）─────────────────────────┐
│  2.1 Release 构建零错误                                 │
│  2.2 C++ 套件 0 failed / 0 skipped（build/tests/<cfg>）  │  ← CWD 必须是 build/tests/<cfg>
│  2.3 Lua 主套件 + 孤儿套件 均 0 failed（仓库根）         │
│  2.4 耦合预算 python scripts/count_coupling.py --ci     │
│  2.5 ks_check 静态契约（demo + 16 教程，0 violations）   │
│  2.6 生成物新鲜度（api_stats / gen_changelog / gen-index）│
└──────────────────────┬───────────────────────────────┘
                       ▼
┌─ 3. 生成 CHANGELOG ────────────────────────────────────┐
│  python scripts/gen_changelog.py --from-tag <prev> --tag <ver> │
│  → 手工润色（合并逐提交条目 / 删 plan: / 按影响排序）      │
└──────────────────────┬───────────────────────────────┘
                       ▼
┌─ 4. CPack 打包 ────────────────────────────────────────┐
│  cd build && cpack -C Release -G ZIP                    │
│  → build/CaesuraAmeKAG-<ver>-Windows-AMD64.zip + .sha256 │
└──────────────────────┬───────────────────────────────┘
                       ▼
┌─ 5. 验证 ZIP ──────────────────────────────────────────┐
│  unzip -l 核对内容 + 解压后从归档根 --frames 60 冒烟      │
└──────────────────────┬───────────────────────────────┘
                       ▼
┌─ 6. 发布 GitHub Release ───────────────────────────────┐
│  git tag -a v<ver>  →  git push origin v<ver>            │
│  gh release create v<ver> <zip> --notes-file CHANGELOG.md --draft │
│  （复核后去掉 --draft；或走 CI release job 拉产物）        │
└────────────────────────────────────────────────────────┘

**产物清单（一次 v1.0.x 发布）**：

| 产物 | 路径 | 说明 |
|------|------|------|
| 桌面 ZIP | `build/CaesuraAmeKAG-1.0.x-Windows-AMD64.zip` | 引擎 + demo + 资产 + SDL3/FFmpeg DLL（已发布 v1.0.1 实测 88 MB / **403 文件**） |
| 校验和 | `build/CaesuraAmeKAG-1.0.x-Windows-AMD64.zip.sha256` | CPack 自动生成 |
| CHANGELOG | `CHANGELOG.md`（仓库根） | 手工润色后的发布说明 |
| Web 站（可选） | `dist/<game>/`（package_game.sh 产物） | 静态 HTML5 播放器站（itch/GitHub Pages/Netlify） |
| GitHub Release | 网页条目（含上述 ZIP 附件） | `gh release create` 创建 |
```

---

## 1. Build

Configure once (or when options change), then build with the desired
configuration. Debug is for day-to-day work; **Release is for shipping**.

```bash
# Configure, once (Visual Studio multi-config generator):
cmake -B build -S . -G "Visual Studio 17 2022" -A x64
#  (add -DCMAKE_TOOLCHAIN_FILE=...  for vcpkg SDL3)

# Build the configuration you need:
cmake --build build --config Debug   --parallel
cmake --build build --config Release --parallel
```

The engine executable lands at `build/Release/CaesuraAmeKAG.exe`
(and `build/Debug/CaesuraAmeKAG.exe`). Live2D is off by default; add
`-DCAESURA_LIVE2D=ON -DCUBISM_SDK_ROOT="..."` if the SDK is available.

---

## 2. Run the release gates (do not skip)

A release must be fully green locally before tagging. These are the same
gates CI enforces (plus the coupling budget).

### 2.1 Build — zero errors
```bash
cmake --build build --config Release --parallel   # Release for shipping
```

### 2.2 C++ test suite (doctest / CTest)
Run from `build/tests/Debug` — **CWD matters** for resource paths.
```bash
cd build/tests/Debug && ./CaesuraTests.exe
# expect: 0 failed, 0 skipped
```
For the Release gate, the `Release` binaries are built alongside the app (`build/tests/Release/CaesuraTests.exe`);
use that instead when validating a shipping config:
```bash
cd build/tests/Release && ./CaesuraTests.exe
# expect: 0 failed, 0 skipped
```
Or via CTest:
```bash
ctest -C Debug --test-dir build --output-on-failure
```

### 2.3 Lua script suites (from repo root)
```bash
# external/lua/lua.exe is gitignored (it ships only inside the release package).
# On a fresh clone use the lua_cli build product instead -- same interpreter:
#   build/lua/Debug/lua.exe  (or Release)
build/lua/Debug/lua.exe tests/scripts/run_lua_tests.lua      # expect: 0 failed (143 measured 2026-08-27)
build/lua/Debug/lua.exe tests/scripts/run_orphan_tests.lua   # expect: 0 failed (24 measured 2026-08-27)
```

### 2.4 Coupling budget
```bash
python scripts/count_coupling.py --ci
```

### 2.5 Static contract check (.ks scenes)
Every demo scene must pass the declarative KAG command contracts before
testing (the same gate CI enforces before its test steps):
```bash
build/lua/Debug/lua.exe scripts/ks_check.lua demo/galgame_demo.ks demo/full_pipeline_demo.ks scripts/demo_story.ks
# expect: OK — all scenes pass contract checks
# also sweep the per-capability tutorial series (16 scenes, tutorial_01..16):
build/lua/Debug/lua.exe scripts/ks_check.lua demo/tutorial/*.ks
```

### 2.6 Doc / index freshness (generated artifacts)
CI regenerates generated docs and fails on drift. Refresh locally:
```bash
python scripts/api_stats.py               # regenerates docs/api/api-stats.md
lua scripts/schema_doc.lua > docs/api/command-contracts.md   # regenerates the command contracts (134)
python scripts/generate_platform_status.py --check           # platform matrix freshness guard
python scripts/gen_changelog.py          # regenerates CHANGELOG.md (see §3)
node web/gen-index.mjs                    # regenerates web/scripts-index.json
node web/gen-index.mjs --check           # freshness guard (fails red if stale/missing)
```
`--check` diffs a temp-generated index against the committed
`web/scripts-index.json` without overwriting it; CI runs this guard on all
three platforms (Windows/macOS/Linux), so any `scripts/*.lua` change must be
regenerated (`node web/gen-index.mjs`) and committed.
If `git status` shows diffs in generated files, include them in the release commit.

---

## 3. Generate the changelog

`scripts/gen_changelog.py` parses Conventional Commits (`type(scope): description`,
types `feat/fix/test/docs/...`) and emits `CHANGELOG.md` at the repo root.
It uses only the Python standard library (works in git bash and PowerShell).

```bash
# Last 100 commits (default):
python scripts/gen_changelog.py

# Last 50 commits:
python scripts/gen_changelog.py --count 50

# Only commits from a date onward:
python scripts/gen_changelog.py --since 2026-08-01

# Commits since a tag (..HEAD):
python scripts/gen_changelog.py --from-tag v1.0.0-alpha

# Title the section for a release version:
python scripts/gen_changelog.py --from-tag v1.0.0-alpha --tag v1.0.1

# Preview without writing:
python scripts/gen_changelog.py --dry-run
```

Output is grouped by semantic type (`Feat` / `Fix` / `Refactor` / `Test` /
`Docs` / `Build` / `Ci` / `Chore` / `Plan`), each entry with its short SHA.
After generation, **hand-polish** the result: merge verbose per-commit entries
into grouped, readable English bullets, drop `plan:` bookkeeping commits, and
order entries by impact. The curated `CHANGELOG.md` is what you commit.

---

## 4. Package with CPack

CPack (ZIP generator) is wired into `CMakeLists.txt` — it installs the
executable, `scripts/`, `demo/`, `assets/`, `projects/`, `shaders/`,
`web-editor/dist/` (the single-file debug panel `--editor` serves — `web-editor/dist/index.html`), `editor/dist/` (the React/Monaco IDE — OPTIONAL CMake install: `editor/dist` is gitignored so engine-only builds must not fail; the loud sentinel lives in §5.1 with two assertions: `editor/dist/index.html` + `editor/dist/assets/*.js` ≥ 1 chunk), `README.md`,
`LICENSE`, and (on Windows) the SDL3 DLL and FFmpeg DLLs.

> **The editor frontend is part of the release contract.** `getting-started.md`
> tells a stranger to unzip and run `CaesuraAmeKAG.exe --editor`; if
> `web-editor/dist/index.html` is missing from the archive the engine logs
> `web-editor/dist not found; serving API only` and every page load answers
> 404 — the API is up but there is no UI. §5.1 verifies this on the real ZIP.
> Since the Studio Phase 1 wiring (`feat(studio)`), the full React/Monaco IDE
> (`editor/dist`) also ships when a packaging job builds it first: the CMake
> install is OPTIONAL (a fresh clone has no `editor/dist`), the packaging jobs
> run a `Build editor IDE (editor/dist ships in the package)` step —
> `cd editor && npm ci --no-audit --no-fund && npm run build` — before CPack
> (release.yml `build-windows`; ci.yml `release`, `release-linux`,
> `release-macos`), and the §5.1 sentinels (`two` assertions, total 32)
> fail loudly when the build was skipped or produced a stub.
>
> A release ZIP carries **both** frontends, but a stranger's `--editor` keeps
> serving `web-editor/dist/index.html` by default — the IDE at `editor/dist/`
> is reached through the `CAESURA_EDITOR_WEBROOT` override: when the env var is
> set and `<dir>/index.html` exists the engine mounts that directory at `/`
> (log `[EDITOR] webroot override active: <dir>`, route count stays 36 in both
> modes), and an unset or invalid value falls back byte-identically to the
> probe-derived webRoot. The dev-checkout command is documented first in
> `editor/README.md`:
> `CAESURA_EDITOR_WEBROOT=editor/dist ./build/Debug/CaesuraAmeKAG.exe --editor`.

```bash
# Build Release first, then package:
cmake --build build --config Release --parallel
cd build && cpack -C Release -G ZIP
```

The ZIP is produced in `build/` — the actual name is
`CaesuraAmeKAG-1.0.0-Windows-AMD64.zip` (from `CPACK_PACKAGE_FILE_NAME` =
`CaesuraAmeKAG-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}`,
i.e. `-Windows-AMD64`, not `-win64`).
CPack also emits a companion `CaesuraAmeKAG-1.0.0-Windows-AMD64.zip.sha256` checksum.
The archive root is the versioned folder `CaesuraAmeKAG-1.0.0-Windows-AMD64/`
(not the repo root), so smoke-test paths include that top folder.

---

### 4.1 Linux AppImage（AppImage 发行适配，035 D 块）

Linux 发行除 TGZ（Sprint6-L1，cpack -G TGZ，CMAKE_SYSTEM_NAME 为
Linux-<arch> 命名）外另提供 **AppImage** 形态：单文件、免安装、任意现代
Linux 桌面可直接运行。AppImage 由 scripts/build_appimage.sh 从 CPack TGZ 产物
就地转换（**不改 CPack/install 语义**——AppImage 只重排目录结构，内容与 TGZ
同源）。

前置：Linux 主机 CPack TGZ（Linux lane 产物）：

    cmake --build build -j$(nproc) && (cd build && cpack -C Release -G TGZ)

前置：appimagetool（缺失时脚本打印安装指引并以退出码 2 结束，AppDir 仍在）。
见脚本输出指引；或 export APPIMAGETOOL=/path/to/appimagetool。

    # 缺省自动取 build/ 下最新 CaesuraAmeKAG-*-Linux-*.tar.gz
    bash scripts/build_appimage.sh
    # 显式输入/输出
    bash scripts/build_appimage.sh --tgz build/CaesuraAmeKAG-1.0.0-Linux-x86_64.tar.gz \
                                   --out dist/

产物：<TGZ 同名>.AppImage（+ .sha256 侧车）；阶段 AppDir 留在
build/appimage/AppDir（dry-run 检视用；重跑自动重建）。

AppDir 布局（AppImage 规范）：

| 路径 | 说明 |
|---|---|
| AppRun | 入口包装：cd 到 AppDir 根 -> exec usr/bin/CaesuraAmeKAG "$@"（引擎 CWD 资源解析与 TGZ lane 同构） |
| usr/bin/CaesuraAmeKAG | 引擎二进制（TGZ 根 exe 迁入；SDL_GetBasePath 上探两层的包根锚定仍成立） |
| usr/share/applications/caesura-amekag.desktop（+ 根副本） | desktop entry（tools/appimage/caesura-amekag.desktop） |
| usr/share/icons/hicolor/256x256/apps/caesura-amekag.png（+ 根副本） | 图标（tools/appimage/caesura-amekag.png，当前为占位图，正式美术接入时替换该文件即可） |
| scripts/ assets/ web-editor/ editor/ tools/ external/ README.md LICENSE | 与 TGZ 同 install 语义的 payload（lua 解释器/资源/编辑器/模板原样进 AppDir） |

运行时依赖由宿主提供（与 TGZ 同约定，不捆绑）：SDL3（系统发行包）、OpenGL、
libEGL.so.1（bgfx EGL 路径，Linux 运行时依赖见平台矩阵）；CPU 基线
x86_64 + SSE4.1。

退出码语义：0 = 打包完成（appimagetool 已就位）；2 = appimagetool 缺失（打印
安装指引，**AppDir 布局仍已生成**——dry-run 布局验证入口）；1 = TGZ 缺失/布局校验失败。

验证现状（诚实标注）：本机（WSL Ubuntu 无 FUSE/appimagetool）已验证 dry-run
布局生成（AppDir 全结构 + 退出码 2 + 安装指引）；.AppImage 实际产物的运行验证
（appimagetool 打包 + 启动 + 渲染）属 Linux 主机/CI 域——**CI 接线后置**
（.github/workflows/ci.yml 由 captain 收敛，不在本文件范围）；
verify_release_package.sh 的 30 断言以 ZIP/TGZ 为对象，AppImage 的 verify 集成
待 CI 接线时一并补充。

---

## 5. Verify the ZIP contents

Before publishing, confirm the archive is complete and runnable:

| Check | Expected |
|-------|----------|
| Executable present | `CaesuraAmeKAG.exe` at the archive root |
| Runtime DLLs present | `SDL3.dll` + `avcodec/avformat/avutil/swscale/swresample-*.dll` (FFmpeg builds) + `steam_api64.dll` when configured with `-DCAESURA_HAS_STEAM=ON` |
| Editor frontend present | `web-editor/dist/index.html` (without it `--editor` serves the API only) |
| Studio IDE present | `editor/dist/index.html` + at least one `editor/dist/assets/*.js` chunk — OPTIONAL install shipped by the packaging jobs (see §4); the two §5.1 sentinels fail loudly when a release ZIP lacks it |
| Data dirs present | `scripts/`, `demo/`, `assets/`, `projects/`, `shaders/` |
| Project templates present | `tools/project_templates/` with the 5 templates + `manifest.json` (Project Manager + `caesura.py create` depend on it) |
| Licensing | `README.md` + `LICENSE` at the archive root |
| Smoke test | Extract to a clean folder, launch `CaesuraAmeKAG.exe` from that folder, verify the demo boots without missing-resource errors |

> **Sprint 4 closed this gap.** `tools/project_templates/` (5 templates +
> `manifest.json`) now ships in the install set, and BOTH consumers resolve it from
> a package layout (verified on a simulated release tree outside this repository):
> - `scripts/caesura.py create` anchors the templates root on the CLI script's own
>   directory first (a release package puts `scripts/` at the top level next to
>   `tools/`, and a checkout does too), then falls back to a CWD walk-up —
>   `python scripts/caesura.py create my_game --template basic` works from any
>   working directory, in a checkout or inside an extracted ZIP;
> - `ProjectContext::sourceRoot()` (`src/rpc/ProjectContext.h`, the editor's
>   `/api/project/*` templates root) accepts a package layout
>   (`tools/project_templates` + `scripts` + `demo`, **no** `src/`) and
>   prefers the EXECUTABLE's own directory over the compile-time
>   `CAESURA_SOURCE_DIR` macro — so a package built on a dev machine resolves to
>   the package itself, not the developer's checkout.
>
> The stranger path is now "unzip → `python scripts/caesura.py create my_game`
> → `python scripts/caesura.py build my_game` → run the game directory"; the
> editor's Project Manager `/api/project/templates` answers 200 from the package.
> The package also bundles its own Lua interpreter (`external/lua/lua.exe`,
> installed from the `lua_cli` target), so `caesura build` inside an extracted
> ZIP needs no system Lua on PATH — verified by the 32-assertion release check
> including a PATH-stripped create→build→run probe (2026-08-28). Count per lane:
> Windows 31 strict + 1 note; Linux 30 strict + 2 notes; macOS 31 strict + 1
> note (the otool SDL3 hard gate is a real assertion there). The two extra
> strict assertions since the Studio wiring are the IDE sentinels —
> `editor/dist/index.html` + an `editor/dist/assets/*.js` chunk. The two scope
> notes are (a) on POSIX the stripped-PATH probe degrades to a note (the
> /usr/bin distro lua cannot be stripped — the property stays guarded by the
> `ran under the PACKAGED lua` assertion, 2026-08-29 Linux TGZ lane) and (b)
> the SDL3 relocatability check passes as a note where otool or a packaged
> dylib is absent (Windows/Linux, or statically linked; 2026-08-29).

```bash
# List archive contents:
unzip -l build/CaesuraAmeKAG-*.zip

# Extract + smoke test:
mkdir -p /tmp/caesura-smoke && cd /tmp/caesura-smoke
unzip "$OLDPWD/build/CaesuraAmeKAG-*.zip"
ls -la && ./CaesuraAmeKAG.exe --frames 60   # --frames = deterministic GPU smoke run
```

> The engine resolves `assets/` etc. relative to its **working directory**,
> so always launch the exe from the extracted folder, not from elsewhere.
>
> **Expected notes during the smoke run:**
> - A clean boot logs Lua/Render/DevCore/Debug/VFX/MiniGame/AI registration lines and
>   ends with `--frames N` exiting 0. Missing-resource errors would abort instead.
> - Two benign `[WARN] HotReload scan failed: ... "assets/script/"` lines appear because
>   `assets/script/` is a dev-only dir not shipped in the ZIP — not a missing-resource failure.
>
> The ZIP also bundles vendored dependency trees (`include/`, `lib/`, `cmake/` for
> freetype/soloud/zstd) and a `scripts/__pycache__/` — expected CPack artifacts, not errors.

### 5.1 Automated stranger-path check (`verify_release_package.sh`)

The table above is checked mechanically by
`scripts/verify_release_package.sh`. It extracts the ZIP into a temp dir
**outside the repository**, launches `--editor` from there, and asserts the
things a stranger actually needs — not just that files exist:

```bash
# newest build/CaesuraAmeKAG-*.zip by default:
bash scripts/verify_release_package.sh
# or an explicit archive / port / keep the temp dir for inspection:
bash scripts/verify_release_package.sh build/CaesuraAmeKAG-1.0.1-Windows-AMD64.zip --port=9876 --keep
```

32 checks in five groups (the two scope-note paths: the stripped-PATH probe degrades when /usr/bin's distro lua cannot be stripped — the `ran under the PACKAGED lua` assertion still guards the property — and the SDL3 relocatability check is a note where otool or a packaged dylib is absent. Lane composition: Windows 31 strict + 1 note; Linux 30 strict + 2 notes; macOS 31 strict + 1 note with the packaged SDL3 dylib, 30 strict + 2 notes when statically linked; the IDE sentinels are strict on every lane):

| Group | Asserts |
|-------|---------|
| contents | executable, `web-editor/dist/index.html`, `editor/dist/index.html` + at least one `editor/dist/assets/*.js` chunk (Studio IDE sentinels — the ~700B Vite index.html is no byte-floor target, the asset chunk is the stub guard), `scripts/`, `assets/`, `demo/` (non-empty, anchored on `demo/cjk_smoke.ks` — ProjectContext.looksLikeEngineRoot needs scripts+demo), `tools/project_templates/` (all 5 templates), `external/lua/lua[.exe]` (bundled interpreter), packaged SDL3 dylib relocatability (macOS hard gate: `otool -D` LC_ID_DYLIB and the engine's SDL3 load items via `otool -L` must be `@rpath/`/`@loader_path/` — an absolute brew-style install path pins the package to the build host; note-pass where otool or a packaged dylib is absent) |
| serving | the process survives in the extracted folder, `GET /` **with** the token returns **200 and the editor HTML**, no `web-editor/dist not found` in the log, `/api/ping` answers ok |
| auth | unauthenticated `GET /` is **401** — the gate must stay closed; the script never relaxes auth to turn a run green |
| token discovery | with no `CAESURA_EDITOR_TOKEN`, the engine writes `.caesura-editor-token` beside the executable, prints it on stderr, and that token really opens the editor |
| creator toolchain | out-of-repo, with lua stripped from PATH: `caesura.py create` from a packaged template, `caesura.py build --engine .` (ks_check + precompile must resolve the bundled interpreter), and the built game exits 0 on `--frames 60` |

Failure semantics are deliberate: a **missing archive** exits 2 and prints the
CPack command that produces one (`--skip-if-missing` turns that into exit 77,
the ctest SKIP convention — build products are not repository invariants); a
**present but broken** archive exits 1 and lists every failed check. An early
engine exit is reported as such (with the stderr tail) rather than blamed on
HTTP, because a missing runtime DLL kills the process before the listener binds.

> **Run it serially — the 9876 port-hygiene guard is a whole-machine check.**
> The script aborts with exit 2 while ANY `CaesuraAmeKAG` process is alive on
> the host: a leftover engine from another session can dual-bind
> `127.0.0.1:9876` (Windows SO_REUSEADDR) and answer with its own deleted
> webRoot (404 `index.html not found`) or its own token (401), faking package
> failures that are not the package's (observed in Sprint 4: 5 checks went red
> because of a stale engine, not the ZIP). Consequences for teams and
> harnesses:
> - verification must run **serially** — one editor-engine verification at a
>   time, after other engine-driving sessions (editor e2e, dev debugging)
>   have quiesced;
> - engine cleanup must always be **by PID, never by image name**:
>   `taskkill //IM CaesuraAmeKAG.exe` (or `killall CaesuraAmeKAG`) would kill a
>   parallel session's running engine mid-run — a classic cross-session
>   interference that this script's own guard exists to detect, not to cause.
	
---

## 6. Publish a GitHub release

> **What a release actually contains today (measured on the published `v1.0.1`,
> 2026-08-27).** Both release paths produce **Windows-only desktop artifacts**:
> - `.github/workflows/release.yml` (tag-triggered) has exactly two build jobs —
>   `build-windows` (CPack ZIP) and `build-web` (`caesura-web-pwa.tar.gz`). There is
>   **no Linux, macOS, Android or iOS job**, so the `find` step that globs
>   `*.apk` never matches anything.
> - The published `v1.0.1` assets are `CaesuraAmeKAG-1.0.1-Windows-AMD64.zip`
>   (403 files) and `example_game-web.zip` — verified with
>   `gh release view --json assets`.
> - Platform statuses in [platform-status.md](../status/platform-status.md) describe
>   **build/runtime verification**, not shipped artifacts: Linux/Android are
>   `verified` there yet nothing for them is attached to a GitHub release, and
>   every platform's `release` capability is `pending`.
>
> Do not promise cross-platform downloads in release notes until a job produces
> those artifacts.

Use the GitHub CLI. Two flows are supported.

### 6.1 Draft release for a version tag
```bash
# Create an annotated version tag on the release commit:
git tag -a v1.0.1 -m "Caesura (AmeKAG) v1.0.1"
git push origin v1.0.1

# Create the release and attach the ZIP:
gh release create v1.0.1 \
  build/CaesuraAmeKAG-1.0.1-Windows-AMD64.zip \
  --title "Caesura (AmeKAG) v1.0.1" \
  --notes-file CHANGELOG.md \
  --draft
```

Use the **real asset name** `build/CaesuraAmeKAG-1.0.1-Windows-AMD64.zip` (see §4).

> **`gh release create` has no `--dry-run` flag.** To test the publish flow without
> publishing, create the release with `--draft`: it uploads the asset and lets you
> review the page, but doesn't publish. Verify your exact command with
> `gh release create --help` first. (The asset-name and command above were validated
> against a real CPack run and `gh` 2.93.0; no `v1.0.1` tag existed, so nothing was
> published during verification.)

`--draft` lets you review the release page before publishing; drop it when
ready. `--notes-file CHANGELOG.md` uses your curated changelog as the body.

### 6.2 Release from CI artifacts (tag-triggered)
- Push a tag like `v1.0.1` and the CI `release` job (which runs on tags)
  builds and CPack-zips Windows automatically.
- Download the `caesura-amekag-windows-x64` artifact from the workflow run -- it
  is an **upload folder** named `caesura-amekag-windows-x64` (containing the CPack
  ZIP `CaesuraAmeKAG-<ver>-Windows-AMD64.zip`, the exe, and SDL3.dll),
  not a file with that name. Then:
```bash
gh release create v1.0.1 ./releases/CaesuraAmeKAG-1.0.1-Windows-AMD64.zip \
  --title "Caesura (AmeKAG) v1.0.1" --notes-file CHANGELOG.md
```

### 6.3 Manage existing releases
```bash
gh release list                        # list releases
gh release view v1.0.1               # view a release
gh release upload v1.0.1 extra.zip   # add an asset later
gh release delete v1.0.1 --yes       # delete a release (tag kept)
gh release delete-tag v1.0.1 --yes   # delete the tag separately
```

---

## 7. Versioning

- Project version lives in `CMakeLists.txt` (`project(CaesuraAmeKAG VERSION ...)`);
  it flows into the CPack package version and the ZIP name automatically.
- Bump the version before tagging a release (commit the bump, then tag).
- `CHANGELOG.md` sections may be titled with the version (`--tag v1.0.1`) in
  addition to the date.

See also: `docs/guides/getting-started.md` (build/runtime), `AGENTS.md` §12
(docs layout). Commit conventions: `type(scope): description` with types
`feat/fix/test/docs/review/merge/plan`.

---

## Quick reference (copy-paste)

```bash
# gates + docs
cmake --build build --config Release --parallel
cd build/tests/Debug && ./CaesuraTests.exe && cd ../../..
# (a Release-gate may also run the Release tests: cd build/tests/Release && ./CaesuraTests.exe)
build/lua/Debug/lua.exe tests/scripts/run_lua_tests.lua
python scripts/count_coupling.py --ci
build/lua/Debug/lua.exe scripts/ks_check.lua demo/galgame_demo.ks demo/full_pipeline_demo.ks scripts/demo_story.ks
node web/gen-index.mjs --check

# changelog + package
python scripts/gen_changelog.py --from-tag v1.0.0-alpha --tag v1.0.1
cmake --build build --config Release --parallel
cd build && cpack -C Release -G ZIP && cd ..
bash scripts/verify_release_package.sh          # stranger path on the real ZIP (see §5.1)

# publish
git tag -a v1.0.1 -m "Caesura (AmeKAG) v1.0.1"
git push origin v1.0.1
gh release create v1.0.1 build/CaesuraAmeKAG-1.0.1-Windows-AMD64.zip --title "Caesura (AmeKAG) v1.0.1" --notes-file CHANGELOG.md --draft
```
