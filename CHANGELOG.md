# Changelog

All notable changes to **Caesura (AmeKAG)** visual novel engine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

> Draft pending **G1** version decision: proposed **1.1.0** (new capabilities, behaviour changes and fixes since `v1.0.1`); fallback **1.0.2** only if the release is judged fix-only. Curated from the preview of `python scripts/gen_changelog.py --from-tag v1.0.1 --tag 1.1.0 --dry-run` (v1.0.1..HEAD, 797 commits); every entry is traceable to that preview and to the dated records under `docs/plans/`. The commit-level list is not copied verbatim here.
>
> Note: `v1.0.1` never received its own changelog section (release notes live in GitHub Releases); this draft is based on the `v1.0.1` tag (2026-08-21). Nothing below is tagged, published or release-approved; `release_ready=false` until the G1/G2/G3 gates are decided.

### Added

- **Runnable validation evidence**: real-execution collectors, strict receipts/seed/source binding, and rejection of failed or missing runs (`54c537a6`), later extended into run/attempt-bound release dry-run input gates (`c9ab5858`).
- **Save, restore and rollback**: explicit save-state contracts, real cold-start restore, rollback safety points and side-effect cancellation (`b176a741`, `a5ba8c49`, `cd730f32`); rejected packages roll back to the previous known-good package.
- **Runtime lifecycle**: cancellable loads, atomic save replacement, job-completion barriers and safe engine teardown (`f9907fb5`, `d6692b4e`, `3f7e335b`).
- **Archive and distribution security**: host-selected CARC publisher trust plus allocation, bounds and concurrency hardening (`6efe18bc`, `1d9a71f1`); priority-ordered multi-CARC mounting and differential CARC patches.
- **Rendering evidence and effects**: D3D11/OpenGL 18-scene/82-image regression matrix, GPU-recovery and screenshot ownership, GL/Metal post-processing shader arrays (`c82a031d`, `6fee4957`, `7f02e25a`).
- **Input, audio and Web**: multi-pointer/gesture and IME/audio lifecycle ownership (`66bd645c`); Web audio envelopes and audio-clock waits (`c7471141`).
- **RPC and error outcomes**: runtime error presentation plus explicit RPC exit/timeout outcomes and owner-queue bounds (`c906ed1f`).
- **Target capabilities and controlled packages**: enforced target capability contracts (`9243e20d`); Windows ZIP, Linux TGZ/AppImage, macOS TGZ/DMG, Web ZIP and Pages tar built and validated as final packages with runtime receipts (`00944056`, `f779cc5d`, `59718416`).
- **Conditional platform lanes**: controlled Android compile and TEST-signed APK/AAB packages (`bbf78ddc`); the iOS compile probe was promoted to a hard gate (`0d6f4284`).
- **Cloud saves and optional SDKs**: explicit cloud prepare/read/history snapshots, immutable conflict observations, and local-save preservation when a registered Steam backend is unavailable (`7f09c75a`, `a1a516cb`, `b36aa8db`, `b470dece`).
- **Command contract audit**: per-name native evidence for 182 public/control/compatibility names, with 4 explicit refusals and 1 offline fallback recorded (see [2026-10-03-001](docs/plans/2026-10-03-001-command-native-audit-results.md)).
- **Platform status matrix**: evidence-HEAD drift gate and generated matrix refresh (`2be0c0e6`), keeping a single seven-tier platform-wording source of truth.

### Changed

- **Unified candidate CI**: 12-job run across three desktops (Debug and Release&Package), Web Final Package, iOS and Android probes, plus a release-input aggregate; 10 required producer jobs are bound to fixed artifact roles.
- **Release entry point**: `.github/workflows/release.yml` now performs a **Release input dry-run** only (`DRY_RUN_INPUTS_VERIFIED`, `release_ready=false`); it creates no GitHub release, uploads no release assets and deploys no Pages site.
- **Android/iOS CI status**: Android compile/package and the iOS compile probe are required gates instead of `continue-on-error` audits.
- **Web benchmarking**: story-throughput measurement moved to real Chromium, with validated font-resource reuse and startup-readiness retries (`b1a5b296`, `3aec41d4`).
- **Builds and packages**: source-built SDL3 in CI, explicit Linux EGL runtime dependency, macOS runtime-dependency relocation with TGZ/DMG lanes, and Unicode-path fixes for packaged runtime copies (`e5399223`, `a0d38919`).
- **Stricter failure semantics**: failed/skipped/cancelled/missing jobs, changed policy or source, version mismatch and substituted bytes now block the release dry-run; golden drivers no longer report `DONE` for command errors.
- **Bounded performance work**: validated-layer and backlog transfer, seen-scan, snapshot and rollback-capture reductions (`0ae83992`, `a699bc5c`, `094909fc`, `da2dae15`).
- **Docs and planning**: dated execution records retained per candidate; the platform matrix remains the single platform-wording source (AGENTS §12); this draft restarts versioned changelog maintenance after `v1.0.1`.

### Fixed

- **KAG command contracts**: transition interruption, `[load]` restore lost after replay clicks, called-scene choices, `blt` fields/alias precedence, playback fade lifecycle, SE saturation, video entry, cache transactions and story recovery (`4e095830`, `5b9cc419`, `a4fdaab9`, `7ae0ace5`).
- **Live2D and SMA**: DLL unload access violation after device loss, explicit rejection of missing/corrupt SDK shaders, MAIN-view presentation, Cubism motion ownership transfer, voice-PCM mouth drive, and SMA default-view/opacity/skeleton-domain/tail-thread fixes (`5cbf2e8e`, `cce16914`, `91fbb0ac`, `7c23b22a`).
- **Runtime and saves**: coherent cold-start sessions, rollback presentation cleanup, cloud-chunk staging, correct-key encrypted load, and rollback to the previous package after a rejected package (`b176a741`, `cd730f32`, `766407a5`, `bef1a30b`).
- **Resource and audio leaks**: SoLoud resampler release on mixer re-initialization, owned Lua state destruction, and device voice-progress/solid-texture recovery (`8eba52fc`, `8b1312de`, `10089617`).
- **CI and validation stability**: macOS cleanup counter-proof timing, Linux Vite `/@fs` decoding, owned-browser startup readiness, platform-matrix evidence drift, Android slices upload paths and JDK input isolation (`3aec41d4`, `4a8b9284`, `2be0c0e6`, `c6cf2e10`, `877972de`).
- **Web runtime**: Service Worker takeover and offline package, root/subpath packaged smoke, loaded-save page waiting for input, repeated font decoding and the throughput measurement host (`f199950c`, `b1a5b296`).
- **Packaging**: macOS runtime-dependency relocation, Linux AppImage/TGZ lanes, Windows package static/runtime validation and Unicode paths (`e5399223`, `00944056`).
- **Release input gate**: exact-artifact authentication, CDP/offline controls, download-deadline accounting and Pages tar binding (`c9ab5858`, `59718416`, `0ce00398`, `63d5b58c`).
- **Tooling and docs**: clean generated markdown headers, ast-grep skill metadata without a BOM, and Unicode subprocess handling (`6f969dda`, `a2c585df`).

---

## [1.0.0] - 2026-08-25 (GA Release)

### Added
- **KAG Unified Semantic Layer (`scripts/kag/semantic.lua`)**: Single source of truth for `.ks` AST, control flow graph, translatable text, and diagnostics.
- **Direct AST Compilation (`scripts/kag/compiler.lua`)**: Compiler directly consumes AST models without redundant re-parsing.
- **Story Flow Graph Generator (`scripts/generate_story_flow.py`)**: Generates Mermaid diagrams, JSON topologies, and `--lint` diagnostics for broken jumps and orphan labels.
- **i18n Localization Pipeline (`scripts/extract_i18n.py`, `scripts/import_i18n.py`, `scripts/lint_i18n.py`)**: AST-driven extraction with deterministic hash keys, CSV/PO exports, and runtime dictionary compilation.
- **Unified Creator CLI (`scripts/caesura.py`)**: `create`, `doctor`, `flow`, `i18n`, `check`, and `build` commands.
- **Web Editor Story Map (`web/story-map.mjs`)**: Visual story branching graph in Web player/editor.
- **Live2D Extended Bindings**: `[live2d_motion]`, `[live2d_expression]`, and `[live2d_lip_sync]` command contracts.
- **CI/CD GA Pipeline (`.github/workflows/release.yml`)**: Multi-platform automated builds with SHA-256 integrity verification.

### Changed
- Refactored `scripts/generate_story_flow.py` and `scripts/extract_i18n.py` to eliminate regex-based semantic drift.
- Preloaded `kag.semantic` into engine sandbox rules (`scripts/kag/init.lua`).
- Updated `demo/example_game/` showcase with full 6-scene multi-branch storyline and 100% zh/en/ja translation coverage.

### Fixed
- Fixed UTF-8 string decoding issues in Windows subprocess calls across creator scripts.
- Clamped Live2D motion/expression/lip-sync parameters to valid ranges with graceful null-backend fallbacks.

---

## [1.0.0-rc.1] - 2026-08-24

### Added
- Multi-platform First-VN parity verification across Windows, Linux, Web, and Android.
- Android JNI IME input bridge with Japanese Romaji/Kana/Kanji conversion.
- Web PWA offline caching with Service Worker (`web/sw.js`).
- 16-module architectural coupling enforcement with CI gating.
- Full 1,052 C++ doctest suite with 385,299 assertions.
- 158 Lua test suites and 319 Web Vitest unit tests.

---

## Historical Releases & Evolution Rounds

### Round 101–127 — Productization & Steam SDK Integration
- **Web Editor expansion**: live syntax highlighting, scene outline, LSP diagnostics panel.
- **KAG3 ecosystem tooling**: `.xp3` archive parser and TLG5/6 image decoders.
- **Packaging & distribution**: `package_game.sh` one-click builds, Web baselines, CPack Release ZIP.
- **Cross-platform hardening**: Metal readiness, Android (NDK/arm64) build chain, CARC nonce-reuse detection.

### Round 81 — Editor live cross-referencing & Job exception isolation
- **Editor**: live engine position cross-reference in the scene outline.
- **Editor**: drive the live scene to a label via an eval jump.
- **Script**: LSP label rename across definition and navigation sites.
- **Script**: i18n plural tables, numeric formatting, and CLI round-trip.
- **Tests (C++)**: job exception isolation + engine lifecycle round-trip.

### Round 80 — Scene outline panel & LSP label rename
- **Editor**: scene outline parser and read-only panel wiring.
- **Script**: LSP label rename across definition + navigation sites.
- **Script**: i18n plural tables + numeric format + CLI round-trip.
- **Tests**: HTTP smoke over the round-71-80 command surface.

### Round 71–79 — KAG3 Compat & Advanced Control Flow
- **Script**: `ks_check` structural warnings, missing expression, duplicate params.
- **Script**: KAG3-compat command batch (arithmetic, character, text speed, effects, notify).
- **Web**: palette LUT binding, real i18n module, goto/i18n parity.

