# Engine Capability Matrix (Mermaid)

> 下表保留历史代码能力快照。当前目标差异、有效构建条件与运行时可用性由 [runtime-capabilities.json](../../config/runtime-capabilities.json) 和实际宿主查询决定，参见[目标能力指南](../guides/target-capabilities.md)；本表不能覆盖其 unsupported/approximate 结果。

> 2026-08-16 readiness audit (阶段 G 终态 / refreshed to round 113): this matrix tracks **82 code-level capability surfaces** (existing rows refreshed through round 98; round 97-98 script robustness: expr nesting budget + schema coerce semantics corrections; **round 102-113 stage G: post-processing stack [R11], declarative tween [S13], declarative layout [S14], Scene Builder, i18n/迁移工具链以说明方式纳入（见文末「工具链边界」）**).
> A present interface or conditional implementation is not counted as release validation.

## Readiness Snapshot

| Layer | Completion | Evidence boundary |
|---|---:|---|
| Modular architecture migration | 98% | Static/API targets, composition ownership, Engine-owned DebugProtocol, owner-thread RPC DTO dispatch, Lua-hook coexistence, shared production linkage and source-level gates are in place |
| Core visual-novel usability | 74% | Headless/KAG/save/audio unit paths strong; R7/R8/C2 verified on real GPU (D3D11 + OpenGL 4.3); mini-game Lua binding + demo end-to-end; remaining gap: Metal backend and macOS/Linux hardware validation |
| Cross-platform product release | 34% | Multi-platform CI builds exist; Windows D3D11+GL real-GPU pixels verified; optional SDKs (Steam/Live2D) and non-Windows packages not release-verified |

The percentages are deliberately conservative. Unit-test success demonstrates regression coverage,
not that optional SDKs, assets, drivers or editor transport have been exercised in production conditions.

```mermaid
graph LR
    subgraph "Rendering"
        r1["Multi-backend GPU<br/>D3D11 · OpenGL · Metal"]
        r2["Layer Compositing<br/>BG/FG/MSG · dirty rect"]
        r3["Texture Pipeline<br/>async load · budget · LRU"]
        r4["2D Particles<br/>emitter · physics · GPU"]
        r5["Video Playback<br/>MPEG-1 · FFmpeg"]
        r6["GPU Monitor<br/>adaptive quality · degrade"]
        r7["Text Rendering<br/>FreeType · CJK · Ruby"]
        r8["Transition Effects<br/>blend · wipe · custom"]
        r9["Render-to-Texture<br/>offscreen · viewport blit"]
        r10["Batch Protocol<br/>deferred GPU submit"]
        r11["Post-Processing Chain<br/>bloom · vignette · LUT · softblur"]
    end

    subgraph "Scripting"
        s1["Lua 5.4 VM<br/>coroutine · sandbox"]
        s2["KAG Neo-Genesis Parser<br/>tokenizer · 123 commands"]
        s3["Flow Control<br/>if/jump/call/switch/macro"]
        s4["Instruction Budget<br/>anti-infinite-loop"]
        s5["Hot Reload<br/>script watch · live edit"]
        s6["Error Recovery<br/>pcall guard · ErrorUI"]
        s7["Conditional Wait<br/>[until exp timeout]"]
        s8["Conditional Choices<br/>[button cond]"]
        s13["Declarative Tween<br/>[tween] · 5 easings"]
        s14["Declarative Layout<br/>[layout] hbox/vbox/grid"]
    end

    subgraph "Audio"
        a1["3-Bus Audio<br/>BGM · Voice · SE"]
        a2["Cross-fade<br/>volume · position query"]
        a3["3D Spatial<br/>listener · position"]
        a4["Per-handle Volume<br/>SE control"]
    end

    subgraph "Content"
        c1["Live2D Animation<br/>Cubism 5 / PNG fallback"]
        c2["3D Mini-Games<br/>enter→render→leave"]
        c3["Save System<br/>JSON · AES-256-GCM"]
        c4["Schema Migration<br/>v1→v5 auto-upgrade"]
        c5["CARC Archive<br/>pack · encrypt · sign"]
        c6["Ed25519 Signing<br/>tamper detection"]
        c7["Cloud Saves<br/>provider abstraction"]
        c8["Steamworks<br/>achievements · stats"]
        c9["Asset Pipeline<br/>provider chain · priority"]
    end

    subgraph "Development"
        d1["Editor RPC<br/>HTTP + stdio DTO dispatch"]
        d2["Structured Logging<br/>ring buffer · subsystem stats"]
        d3["Frame Profiling<br/>GPU submits · Lua GC"]
        d4["NullJobSystem Mock<br/>synchronous testing"]
        d5["Headless Mode<br/>no-GPU test env"]
        d6["Dev Mode<br/>checkerboard placeholder"]
        d7["Lua Debugger<br/>yield pause · owner bridge"]
        d8["AI Dev Assistant<br/>explain · fix · review"]
    end

    subgraph "Platform"
        p1["Cross-Platform<br/>Win · Mac · Linux"]
        p2["CI Pipeline<br/>3-platform build+test"]
        p3["Thread Pool<br/>priority · onComplete"]
        p4["Input Routing<br/>KAG↔Game focus switch"]
        p5["Texture Budget<br/>auto-detect 6 tiers"]
        p6["Sandbox Quota<br/>resource limiting"]
    end
```

## Capability Breakdown

### Rendering (11 capabilities)

| # | Capability | Interface | Status |
|---|-----------|-----------|--------|
| R1 | Multi-backend GPU (D3D11/OpenGL/Metal) | `IRenderDevice` | ✓ D3D11 + OpenGL 4.3 real-GPU verified; Metal engine-side complete (embedded shaders, backend selection, Live2D Metal render path implemented) -- runtime validation requires macOS hardware |
| R2 | 3-layer compositing (BG/FG/MSG) with dirty-rect optimisation | `ILayerManager` | ✓ |
| R3 | Async texture loading with budget enforcement + LRU eviction | `ITextureManager` | ✓ |
| R4 | 2D GPU particle system (emitters, physics, colour) | `IParticleSystem` | ✓ |
| R5 | Video playback (MPEG-1 via pl_mpeg, FFmpeg optional) | `IVideoPlayer` | ✓ |
| R6 | Adaptive GPU quality monitor with automatic degradation | `IGpuMonitor` | ✓ |
| R7 | Text rendering (FreeType atlas, CJK support, ruby/furigana) | `IRenderDevice` | ✓ CJK font (NotoSansCJKsc, OFL) + `text_set_font` face switching; D3D11 GPU smoke test loads real OTF (786 glyphs), renders CJK + ruby; `loadTTF` guarded against uninitialized GPU + glyph-failure diagnostics |
| R8 | Transition effects (blend, wipe, custom shader) | `IRenderDevice` | ✓ blend/wipe/custom-shader paths; Transition program compiled on D3D11 + OpenGL 4.3, demo flash+crossfade rendered on both backends (2026-08-06) |
| R9 | Render-to-texture with viewport blit | `IRenderDevice` | ✓ |
| R10 | Batch draw-call protocol for multi-layer scenes | `IRenderDevice` | ✓ |
| R11 | Post-processing chain (bloom / vignette / LUT color grade / soft blur; redirection of VIEW_MAIN to a scene RTT + per-stage full-screen passes over a ping-pong scratch-RTT pool; bloom multi-pass bright/downsample/blur/additive; fxc-compiled DXBC embedded, GL/Metal/Vulkan constant-copy degrade; command surface `[vfx ... postfx=bloom|vignette|lut|softblur|none]` + `Render.set_postfx` family) | `IRenderDevice` (PostFxKind/PostFxParams + createPostFx/setPostFxParams/destroyPostFx/clearPostFx/isPostFxActive/isPostFxSupported) | ✓ (round 102; Null/headless full degrade; D3D11 四段 program READY 冒烟验证于 round 114 发布终验; GL/Metal/Vulkan 恒等降级不空程序) |

### Scripting (40 capabilities)

| # | Capability | Interface | Status |
|---|-----------|-----------|--------|
| S1 | Lua 5.4 VM with coroutine-based scheduler | `ILuaManager` | ✓ |
| S2 | KAG Neo-Genesis parser (123 contract commands, 9 categories) | Lua tokenizer | ✓ |
| S2a | KAG3 bare positional args (13 families, 15 commands: delay/wait/se/voice/play/jump/call/link/unlock/macro/erasemacro/save/load/gallery/ending) | tokenizer + scheduler | ✓ |
| S2d | KAG3 expression compatibility (TJS `&& \|\| ! != ?:` translated string-aware; visible scene:line errors instead of silent else) | `kag/expr.lua` | ✓ |
| S2e | KAG3 variable system (`%f.x%` interpolation, `lf` call-frame stack, `mp` message params, dual-style expression env) | schema + scheduler | ✓ |
| S2f | KAG3 control-flow completeness (`[elsif]` alias, `[call *label]` intra-scene, `[end]`→ending, unknown-tag warnings) | tokenizer + scheduler | ✓ |
| S2g | Modern utility commands (`[set]` typed, `[inc]`, `[random]`, `[assert]`) | system commands | ✓ |
| S2h | Command metadata (category/blocking/desc on all 123 contracts; emitted by schema_doc + dumpContracts) | schema | ✓ |
| S2b | Exact token offsets (byte-accurate '[' position + end_offset via dual Cp) | `parse_with_offsets` | ✓ |
| S2c | Parser performance (4000 tokens 362ms -> 259ms, 64.75ms/1000tok after dropping 9 redundant prefix patterns) | lpeg.lua | ✓ |
| S2i | KAG scene-level debugger (breakpoints on scene+cmd/line, single-step, scope inspection; RPC kagSetBreakpoint/kagDebugContinue/kagDebugStep/kagInspectScopes) | `kag_debug.lua` + scheduler + RPC | ✓ |
| S2j | Mod loader (scene/resource override by priority; mods/<name>/<path> wins over base; path-traversal guarded) | `mods.lua` + flow + resolve_file | ✓ |
| S2k | Input recording/playback (auto-demo + regression; [replay] command, JSON persistence, choice coordinates) | `replay.lua` + kag_runner | ✓ |
| S2l | Accessibility (closed captions for voiced lines, settings toggle, TTS interface probe) | text commands + settings | ✓ |
| S2m | Scene hot reload (edit a running .ks -> re-parse + position remap + state preserved; HotReload watches assets/script + mods; RPC kagReloadScene) | `kag_runner.reload_scene` + HotReload + flow | ✓ |
| S2n | LLM-driven dialogue ([ai_dialog] async query; OpenAI-compatible / Ollama endpoints; graceful fallback text; AI binding + sandbox whitelist) | `AIBinding` + system commands + backend | ✓ (mock + **real Ollama e2e verified** on 2026-08-13: engine --headless RPC eval against live Ollama gemma3:4b — sync AI.query + async AI.query_async both return real replies; **model auto-discovery**: empty config model on Ollama endpoints asks the server (GET /api/tags, cached) instead of 404-ing on a hardcoded default; read timeout 15s→60s for cold loads; conditional C++ test (skips when Ollama unreachable) + ctest CaesuraHeadlessAiSmoke (exit 77 skip on CI)) |
| S2o | Demo/video export (recorded replay drives the game while each frame is captured to PNG; --export-replay/--export-dir; screenshot readback fixed) | `Engine::run` + `BgfxDebugCallback::screenShot` | ✓ |
| S2p | Colorblind/high-contrast filter (config.accessibility.color_filter presets; VFX effect 4 matrix pass over RTT scene layers; D3D11 + GL shaders) | `IRenderDevice::setColorFilter` + fs_vfx effect 4 | ✓ (RTT scene layers; direct-drawn UI text unaffected) |
| S2q | Declarative conditional wait (`[until exp="..." timeout=ms]` — wait until a TJS expression is truthy; per-frame yield + cancellation; Ren'Py needs python loops for this) | scheduler + compiler (AOT exp/dump) | ✓ |
| S2r | Conditional choices (`[button cond="f.x > 1"]` — false choices hidden at [endbutton]; Ren'Py menu `if` parity; all-hidden blocks dissolve) | `kag/commands/text.lua` + `kag/expr.lua` | ✓ |
| S2s | Inline text markup (`{color=#rrggbb}` per-span colors, `{size=N}` glyph scaling affecting wrap, `{b}` synthetic bold, `{i}` italic top-edge shear, `{s}` strikethrough middle bar — all stackable; unknown `{tags}` literal) | `kag/text_layout.lua` (parse_markup/wrap_spans) + `kag/text_scene.lua` (add_wrapped_spans) + `IRenderDevice::renderText(scale, bold, italic, strike)` | ✓ (34 Lua assertions; typewriter reveal + backlog use stripped plain text; strike bar geometry headless-tested) |
| S2t | NVL mode (`[nvl]` full-screen accumulated text block, Ren'Py parity; `[nvl clear]`/`[p]` page break, `[nvl off]` exit; speaker as 「Name」： inline prefix styled by `nameplate_style.text_color` — format customizable via `[nvl prefix="%s："]` schema param (persists per session, `%`-safe); wraps with the line, instant draw — zero new state fields; cursor reused from text_state so save/rollback persists the page) | `kag/commands/text.lua` (`nvl` handler + ch/text/p/er accumulation) + `kag/text_scene.lua` (`commit` seals prior reveal draws, `instant` draws skip typewriter) + save/snapshot `nvl_mode` | ✓ (29 Lua assertions; typewriter only animates the appended line) |
| S2u | Localization pipeline (`{key}` token expansion with markup-name whitelist + per-line translation `lines["<scene>:<fnv1a(msg)>"]`, content-addressed keys; applied by [ch]/[text] before markup parsing **and by [button]/[sel] choice labels at registration**; empty placeholder falls back to original; settings Language hot-switch **+ full-page redraw** (already-displayed page / NVL page / backlog / active choice labels / closed captions re-localize with the new language via `page_src` replay — exceeds Ren'Py, which keeps displayed lines in the original language; layout y-cascade for re-wrapped translations; typewriter sealed) + save persistence (`state.language` + backlog `src`); `scripts/ks_i18n.lua` template generator with --update merge and --missing CI gate, extracts dialogue + choice labels) | `scripts/i18n.lua` (fnv1a/localize/expand/load) + `kag/commands/text.lua` (ch/text/button localize + relocalize_page/relocalize_backlog) + `kag/text_scene.lua` (page_src lifecycle) + `assets/lang/{zh,en,ja}.lua` | ✓ (67 Lua assertions; generated ja.lua template with 25 demo keys incl. choice labels) |
| S2v | Contract runtime coverage (**123/123 contracts execute at runtime**; round-90 grep audit saturated 118; stage-G commands 由专项套件覆盖 — test_tween.lua / test_layout_cmds.lua / test_vfx_postfx.lua) | scheduler + `test_contract_runtime_gaps.lua` + tween/layout/vfx-postfx suites | ✓ (25 + 28 + 54 + 18 Lua assertions) |
| S3 | Flow control (if/else, jump/call/return, switch/case, macros) | Lua scheduler | ✓ |
| S3a | Save/load loop continuity (for/while/if/switch stacks lifted into ctx and serialized via `loop_stacks` in capture_state; [load] resumes an in-progress loop to completion) | Lua scheduler + save snapshot | ✓ |
| S7 | Declarative command contracts (typed params, clamping, $var/${expr} interpolation, required/choices) | `kag/schema.lua` | ✓ + **round 97/98 schema coerce semantics corrected** (positional args typed coercion, `choices` array normalization, default-value normalization, empty-file defect) — `test_schema_coerce` depth suite (+85 round-97 assertions: type/default/enum/positional/idempotence) |
| S8 | Static .ks validator + contract audit gate (ks_check --audit-defaults, CI) | `scripts/ks_check.lua` | ✓ |
| S8a | Truncation detection (offset stream stops before end-of-input + trailing comment handling) | ks_check | ✓ |
| S9 | Parameterized macros (args + %arg% substitution, nested expansion, deep-copied splice) | Lua scheduler | ✓ / nested macro **DEFINITIONS** ([macro outer][macro inner]...[endmacro][endmacro]) + depth-based recursion guard (>100 splice depth errors; 1000+ sequential calls & 2000-iteration loops pass) |
| S10 | Label index (O(1) jump, scene-scoped, restored/invalidated on swap) | Lua scheduler | ✓ |
| S11 | [if] expr cache keyed by env identity (no stale variables across scenes) | Lua scheduler | ✓ |
| S12 | i18n runtime API (set_language/current_language/translate/reload/default_language; mid-scene language switch, fallback chain, hot-reload) | `scripts/i18n.lua` + text pipeline relocalize_page | ✓ |
| S13 | Declarative tween (`[tween target= attr=x|y|alpha|scale from= to= dur=100-30000 delay=0-30000 ease=linear|ease_in|ease_out|ease_in_out|back_out wait=true|false]`; from 缺省=当前值; from/to 支持 `${expr}` 插值; ctx.tweens 管理器单时间线 delay 相/t 累计/终点精确落 to; 阻塞 wait=true 与 [wait] 同构, 非阻塞 fire-and-forget 经 kag_runner.update 每帧钩子; 与 [position]/[layout] 组合) | `kag/commands/tween.lua` + `kag_runner.update` (pcall+空表短路) + Web 双帧循环防御性驱动 | ✓ (round 106; test_tween 28 断言; tutorial_16_tween.ks; tween.parity 3 用例; 5 缓动端点矩阵) |
| S14 | Declarative layout (`[layout name= kind=hbox|vbox|grid gap= padding= align= cols= w/h/x/y=]` + `[layout_slot parent= layer= index= size="WxH"]` + `[layout_place parent= layer= x= y=]`; 容器是"计算器"非渲染层 — recompute 后 layers.move_layer 写现有层坐标, 渲染管线零改动, 与 [position]/[tween] 组合; settings 迁移试点坐标等价证明逐像素一致) | `kag/layout_math.lua` (纯函数) + `kag/commands/layout.lua` + `test_settings_layout_pilot.lua` | ✓ (round 107; test_layout_cmds 54 断言; layout.parity 4 用例; hbox/vbox/grid/gap/padding/align/越界/空容器) |
| S4 | Instruction budget sandbox (anti-infinite-loop, per-frame cap) | `ILuaManager` | ✓ (preserved through DebugProtocol attach/detach, breakpoint yield/resume and inherited coroutine hooks) + **round 97 expr nesting budget**: deeply-nested expression evaluation cut from O(n³) to bounded <=0.001s (`fix(script): expr nesting budget (O(n3) deep-nest cut)`) |
| S5 | Hot reload (watch scripts/, live-reload without restart) | Engine-owned `HotReload` instance | ✓ |
| S6 | Error recovery (pcall guards, ErrorUI, graceful degradation) | scheduler + bindings | ✓ |
| S6a | Modal UI input loops (settings/music_room/gallery/chapter select -- per-frame yield + GAME key routing: UP/DOWN/LEFT/RIGHT/ENTER/ESC/F/mouse) | ui modules + Engine input | ✓ |
| S6b | UI state persistence (settings values, favorites, unlock sets, visual text state) | settings/music_room/save | ✓ |

### Audio (4 capabilities)

| # | Capability | Interface | Status |
|---|-----------|-----------|--------|
| A1 | 3-bus audio (BGM with cross-fade, Voice with interrupt, SE) | `IAudioBackend` | ✓ |
| A2 | Fade, position query, per-bus volume control | `IAudioBackend` | ✓ |
| A3 | 3D spatial audio (listener position, 3D SE placement) | `IAudioBackend` | ✓ |
| A4 | Per-SE-handle volume and stop control | `IAudioBackend` | ✓ |

### Content Systems (10 capabilities)

| # | Capability | Interface | Status |
|---|-----------|-----------|--------|
| C1 | Live2D animation (Cubism 5 SDK / PNG static fallback) | `IAnimationBackend` | Partial: PNG fallback + D3D11 (Windows) verified; **Metal render path fully implemented (was stub); GL shader deployment fixed (active-renderer FrameworkShaders copy)** -- GL/Metal runtime validation needs Linux/macOS hardware. SDK is bundled in thirdparty/. See `docs/guides/live2d-setup.md` |
| C2 | 3D mini-game framework (enter→update→render→leave loop) | `IMiniGameBackend` | ✓ lifecycle + JSON scenes + 20-API Lua binding (`mini_game` global, sandbox-whitelisted); real-GPU D3D11 child-process test (enter→update→render→leave) + programmatic `enter(0)` mode; demo_minigame.lua runs end-to-end on D3D11 and OpenGL 4.3 |
| C3 | Encrypted save/load (JSON, AES-256-GCM) | `ISaveManager` | ✓ round-trip suite (round 79): ciphertext unreadable without key, graceful no-key failure, wrong-key GCM rejection, magic-gated plaintext pass-through, forged-magic rejection, tampered ciphertext+nonce rejection, slot-boundary mirroring |
| C4 | Schema migration (v1→v5 auto-upgrade, pluggable migrations) | `ISaveManager` | ✓ |
| C5 | CARC archive packaging (compress, encrypt, sign) | `IArchiveWriter` | ✓ |
| C6 | Ed25519 digital signature (tamper detection for .carc files) | `ICryptoEngine` | ✓ |
| C7 | Cloud save provider abstraction (local / remote pluggable) | `ISaveProvider` | ✓ abstraction + local path + **HTTP cloud provider (push/pull/delete against a REST endpoint, offline degrade) + Lua save.configure_cloud/cloud_push/cloud_pull; mock-server round trip tested** |
| C8 | Steamworks integration (achievements, stats, cloud saves) | `ISteamBackend` | Conditional (needs Steam SDK/account): full Lua surface (19 APIs incl. cloud list/write/read/delete/quota), overlay/stats/store fixes, Null-backend tested; real SDK round trip needs a Steam dev account |
| C9 | Asset provider chain (Dir → CARC, priority-ordered, integrity check) | `IAssetProvider` | ✓ |
| C10 | Tutorial sample library (**16 递进式教程** tutorial_01–16, each runnable to [end] with line-by-line commentary; covers hello/text/layers/audio/branching/effects/saveload/system-ui/interpolation/loops/switch/expr-combo/commands/flow-timing/expr-deep/**tween(16)**; engine compile + Web-player double verification) | `demo/tutorial/*.ks` + `assets/lang/{zh,en,ja}.lua` | ✓ (round 90 added 14/15; round 106 added tutorial_16_tween; ks_check zero WARN; sample-library path table 16 rows) |

### Development Tools (10 capabilities)

| # | Capability | Interface | Status |
|---|-----------|-----------|--------|
| D1 | Editor RPC (HTTP plus stdio JSON-RPC) | `IEditorServer`, `IRpcServer`, `IRpcDispatcher` | Full: both transports use owner-thread DTO dispatch and are CLI-wired; managed-coroutine `run/eval` + breakpoint lifecycle (set/remove/clear/continue) + inspect + frame capture implemented on both transports; stdio smoke (`headless_rpc_smoke.py` 45/45) and HTTP smoke (`headless_http_smoke.py`, 75 assertions — t15 实测 75/75, 2026-08-28) end-to-end tested via ctest; **web full-tutorial regression sweep** (15 parametrized scenarios: tutorial 01–13 + showcase + example_game, all DONE; web gaps 62→79) + **G4-2 SceneOutline panel** (active-document outline rendering, label-click reveal navigation; editor 205→210) + **G4-3 outline-driven live jump** (label-click drives the running scene to a `*label` via `/api/eval` + `flow.find_label` + `kag.jump` `_next_index` — zero engine change; editor 210→234) + **G4-4 live engine position cross-reference** (`useEnginePosition` hook polls `/api/eval` for `_CAESURA_CTX`; outline row highlight + panel ▶ scene name; editor 234→245) + **InspectorView depth** (live engine status bar + `commandLint.ts` param lint with `KNOWN_COMMANDS` single source; label bidirectional jump + follow engine position; editor 245→270) + **editor store depth** (round 88: store actions + reveal-queue consumption, editor 333→368) + **rpc client depth** (round 89: evalRaw POST / response parsing / error propagation / URL encoding, editor 368→402) + **panel integration E2E** (round 90: `panels.integration.test.tsx` mounts real App+store — doc→outline→reveal→Inspector chain, setEngine broadcasts across StatusBar/Outline/Timeline/VisualView, ActivityBar mount/unmount; editor 402→408) + **round 91-98 editor depth** (round 95 SceneOutline virtualization [windowed rows, reveal-into-view]; round 96 inspector lint/disconnect/jump-status/dual-link + jump idempotency; round 97 activitybar/statusbar a11y + scene truncation + state depth; editor 408→506→**530**（round113 基线；现 35 个测试文件，未重测）; round 108 Scene Builder 面板 +32 (SceneBuilder.tsx + lib/sceneBuilder.ts + store 扩展 + resolveInsertLine; 三类操作 [bg]/[csp]/[ch]+[p] 光标行插入 + Inspector 联动; /api/assets 扩展 fg 槽位 + kind 字段, 生成行校验复用 LSP; verify 全链实测) |
| D2 | Structured logging (ring buffer, subsystem error counts, per-subsystem stats) | `IDebugManager` | ✓ |
| D3 | Frame profiling (GPU submit count, transient allocs, Lua GC timing) | `IDebugManager` | ✓ |
| D4 | NullJobSystem mock (synchronous task execution for deterministic testing) | `IJobSystem` | ✓ |
| D5 | Headless mode (no-GPU Engine init for CI/test environments) | `EngineConfig` | ✓ |
| D6 | Dev mode (checkerboard placeholder textures, verbose logging) | `ITextureManager` | ✓ |
| D7 | Lua debugger (breakpoints, step control, inspection) | `DebugProtocol` | ✓ Engine lifecycle, KAG resume arbitration, stdio commands, stale pause rejection and managed result/error cleanup are tested |
| D8 | AI dev assistant (`kag/aidev.lua`: local rule-based diagnostic explainer + structural scene review [flow balance / missing [end]]; LLM-enriched explanations, fix suggestions, full scene generation with self-review; exposed to the IDE via /api/eval) | `kag/aidev.lua` + `backend.ai_query` + AiPanel Dev Assist section | ✓ (26 Lua assertions; local paths work offline, LLM degrades gracefully) |
| D9 | LSP navigation (goto-definition / find-all-references for `*label` ↔ `[jump]`/`[call]`/`[link]`; cross-scene targets return name-only) | `kag/lsp.lua` (definition/references) + Monaco providers | ✓ (12 Lua assertions; Ctrl+Click + context menu in IDE) |
| D10 | Skeletal mesh animation (SMA S1–S5: CPU soft-skinning math + **GPU compute skinning** (bgfx compute; D3D11 DXBC + GL 430 GLSL, draw transform baked into bone-buffer slots, final NDC output — verified pixel-identical to the CPU path on a real D3D11 device) + bgfx renderer + Lua driver [JSON/hierarchy/LERP] + **round-18 playback controls** (loop/duration/rate/pause/resume/seek/play_anim crossfade/on_done_anim) + **2-bone IK** + **E-mote parts/variant switching** + `[sma_play]`/`[sma_anim]`/`[sma_ik]`/`[sma_variant]`/`[sma_stop]` contracts + `sma.*` binding incl. `set_skin_mode`) | `IMeshRenderer` + `SmaMeshRenderer` + `SmaSkinner` + `kag/sma.lua` + `SmaBinding` | ✓ (12 C++ + 70 Lua assertions; **D3D11 GPU child test: GPU skin == CPU skin pixel-for-pixel (14 assertions)**; headless uses Null backend; Metal/SPIR-V fall back to the CPU skinner) |

### Platform Infrastructure (10 capabilities)

| # | Capability | Interface | Status |
|---|-----------|-----------|--------|
| P1 | Cross-platform (Windows MSVC, Linux GCC, macOS Clang) | `IPlatformBackend` | Partial: CI build coverage; real GPU behavior is not verified on all platforms |
| P2 | CI pipeline (3-platform build + doctest suite, GitHub Actions) | `.github/workflows/ci.yml` + `.github/workflows/deploy-web.yml` | ✓ (阶段 G 终态 / round 113 baseline: C++ 976/976, Lua 132/132 + 24 orphan, web 297/297, editor 530/530, ctest 10 + AI-skip, coupling/coverage PASS; Release build + CPack ZIP end-to-end measured; web GitHub Pages 手动部署链路 round 109 就绪) |
| P3 | Multi-threaded task system (priority queues, main-thread callbacks) | `IJobSystem` | ✓ |
| P4 | Input routing (KAG ↔ Game focus switch, resize callbacks, unified pointer path) | `IInputRouter` | ✓ (STEP12: `submitPointer(PointerEvent)` feeds the same dispatch as `processEvent` — KAG/GAME mutual exclusion + phantom-click guarantees shared by construction; Down/Move/Up→left-click semantics, LongPress→right-button press/release pair, Pinch→wheel delta from cumulative scale with baseline reset on focus switch; legacy SDL touch→mouse injection kept as compatibility path; native touch sources pending Track M/I) |
| P5 | Texture budget auto-detection (6 tiers, 128MB–4GB) | `ITextureBudget` | ✓ enforcement tests (round 79): tier-boundary exact mapping, tier5 override-only, quota-full reject + release recovery, quota-0 all-reject |
| P6 | Lua sandbox resource quotas (textures, emitters, handles) | `ISandboxQuota` | ✓ |
| P7 | MobileAdapter (lifecycle callbacks, touch → mouse/wheel event mapping, DPI scaling) | `IMobileAdapter` (platform) | ✓ core mapping + lifecycle + **SDL finger events bridged (normalized -> pixel) + orientation change events (Lua _G.onOrientationChanged); 87 unit tests**; native mobile SDK integration still needs a device |
| P8 | Display metrics service (unified pixel/logical size, scale factor, DPI, orientation, safe-area insets via one snapshot query) | `IDisplayService` (platform) | ✓ desktop (SDL3 window/display query, dpi = 96×contentScale mapping + Null headless fallback; safeArea always zero on desktop/headless); real safe-area insets / mobile implementation NOT done (Track M/I) |
| P9 | Unified app-lifecycle events (single feed across desktop SDL3 / Android JNI / iOS notifications; consumers implement `ILifecycleListener` once, no platform ifdefs; 6-event enum: Pause/Resume/Background/Foreground/LowMemory/Terminate) | `ILifecycleService` (platform) | ✓ desktop (SDL3 event watch posts Background/Foreground/LowMemory/Terminate through the LifecycleService hub — registration-order dispatch, dedupe, self-removal-safe snapshot; Engine listener maps to onPause/onResume audio suspend/resume + `_G.onLowMemory`/`_G.onTerminate` via IMobileAdapter; +7 unit tests); mobile native event sources NOT implemented (Track M/I) |
| P10 | OS audio-focus / interruption arbitration (platform-agnostic event feed; consumers implement `IAudioFocusListener` once, no platform ifdefs; 4-event enum FocusGained/FocusLost/InterruptionBegin/InterruptionEnd driving a Normal/Lost/Interrupted state machine) | `IAudioFocusService` (audio) | ✓ desktop hub (`AudioFocusService` header-only: ordered listener dispatch on the posting thread, duplicate listeners ignored, spurious InterruptionEnd is a no-op; Engine listener drives `IAudioBackend` suspend/resume, idempotent with the lifecycle background suspend); desktop/Web have no OS arbitration — the hub is the documented entry point for Android JNI / iOS audio-session interruption callbacks, native wiring NOT implemented (Track M/I) |

---

> 📷 `engine-capacity-matrix.png` 是对应 mermaid 的静态渲染快照（round-98 世代）；
> 若重新在支持 mermaid-cli 的环境执行 `mmdc` 可再生成，矩阵以本文表列为准。

**Total: 85 tracked capabilities across 6 domains (2026-08-23 STEP10–STEP14: +P8 display metrics, +P9 unified lifecycle events, STEP12 unified pointer path folded into P4, +P10 audio-focus arbitration; 阶段 G 终态 / round-113 refresh: +R11 post-processing chain, +S13 declarative tween, +S14 declarative layout; P2 baseline refreshed to round-113 numbers 976/132+24/297/530; S2 contract census 118→123; C10 tutorials 15→16; D1 editor depth extended 408→506→530).** 工具链（ks_i18n / xp3_tool / tlg2png / package_game / template）不单列矩阵行——见文末「工具链边界」说明。 See the readiness snapshot above for
the distinction between architecture completion, core usability and release readiness.


### 2026-08-16 additions (阶段 G / round 101-113 final)

- R11 — 后处理链（round 102）：`[vfx ... postfx=` + `Render.set_postfx` 家族；
  VIEW_MAIN→m_sceneRtt 重定向 + 逐 stage 全屏 pass（scratch RTT 乒乓）+ VIEW_POSTFX=40；
  bloom 多 pass（bright/½ 降采样/¼ blur×2/additive）；四个 fxc 真编译 DXBC 内嵌，
  GL/Metal/Vulkan 恒等拷贝降级；schema 契约 118→119（vfx postfx 枚举）；api-stats 38 API
  自动计入；test_render_postfx 9 用例 + test_vfx_postfx 18 断言；round 114 发布终验
  D3D11 四段 shader 冒烟 READY。
- S13 — 声明式补间 `[tween]`（round 106）：5 缓动（linear/ease_in/ease_out/ease_in_out/back_out）
  端点精确、from 缺省=当前值、${expr} 插值、delay 分相、阻塞/非阻塞双模式；ctx.tweens
  管理器 + kag_runner 每帧钩子；雪杀手 Web parity + tutorial_16；孤儿套件 21→22。
- S14 — 声明式布局 `[layout]`（round 107）：hbox/vbox/grid 容器=计算器（纯函数
  layout_math），recompute 写现有层坐标、渲染零改动；[layout]/[layout_slot]/[layout_place]
  三独立契约；settings 迁移试点坐标等价证明；孤儿套件 22→24；契约 119→123。
- S2 — 契约普查 118→123（tween + layout 家族）。
- C10 — 教程库 15→16（tutorial_16_tween）。
- D1 — editor 506→530：round 108 Scene Builder 面板（[bg]/[csp]/[ch]+[p] 三类操作、
  光标行插入、Inspector 联动、/api/assets fg+kind 扩展，R108-C 契约违例修复后
  [csp] 必填 name 生成正确）；editor-api-reference 与 scene-builder-rpc-bridge 同步。
- P2 — 基线终态：C++ 976/976（8858 断言）、Lua 132/132 + 24 orphan、web 297/297
  （20 文件）、editor 530/530、契约 123；round 113 template 套件 4/4；round 114 发布
  终验 Release 构建零错误 + ZIP 87.97MB/403 文件 + 解压冒烟 D3D11 干净启动/退出。

### 2026-08-23 additions (Track P STEP10–STEP14)

- P8 — Display metrics 服务（commit f50c6af8）：`IDisplayService.currentMetrics()` 单次快照
  返回 pixel/logical 尺寸、scaleFactor、DPI（=96×contentScale 文档化映射）、orientation、
  safeArea（Orientation/Insets/DisplayMetrics 按值传递，类型定义在接口头文件，AGENTS.md §2）。
  实现 `SDL3DisplayService`（桌面 SDL3 实时查询，窗口经 `IPlatformBackend` 懒取）+
  `NullDisplayService`（无头/测试固定零值）；构造点组合根 `entry/createDisplayService()`
  工厂，`BackendRegistry` 新增 set/getDisplayService 槽位（#23）；Lua 绑定
  `DevCore.get_display_metrics()`。安全区/DPI 细节：桌面零 inset，移动端待 Track M/I。
- P9 — 统一生命周期事件服务（commit 6f8414e1）：`LifecycleEvent` 六事件（Pause/Resume/
  Background/Foreground/LowMemory/Terminate），消费方实现 `ILifecycleListener` 一次接入
  全平台，无平台 ifdef。桌面源 `Engine::appLifecycleWatch`（SDL3 watch，主线程投递）post
  Background/Foreground/LowMemory/Terminate，Pause/Resume 预留移动端 JNI 源；
  `LifecycleService` 中枢按注册顺序派发、去重、快照派发（自移除安全）。Engine 自身作为
  首监听器映射 onPause/onResume 音频挂起恢复 + `IMobileAdapter.onLowMemory/onTerminate`
  （`_G.onLowMemory`/`_G.onTerminate`，无 Lua 安全）；`BackendRegistry` 新增
  set/getLifecycleService 槽位（#24）。移动端原生事件源待 Track M/I。
- STEP12 — 统一指针输入路径（commit e1a5eceb）：`IInputRouter` 新增纯虚
  `submitPointer(const PointerEvent&)` 与类型
  `PointerAction{Down,Move,Up,LongPress,Pinch}` /
  `PointerEvent{x,y,scale,pointerId,activePointers}`（类型定义在接口头文件，
  AGENTS.md §2）。InputRouter 抽出共享分发函数——submitPointer 与 processEvent
  共享 KAG/GAME 互斥与防幻点击保证；LongPress→右键按下+抬起对，Pinch→累积 scale
  换算滚轮增量（焦点切换基线复位）；旧 SDL touch→mouse 注入保留为兼容路径；
  +5 单测。
- STEP13 — 组合根默认存档 provider（commit 78f5d1a5）：`SaveManager` 在
  `m_saveProvider == null` 时 save/load/list/delete 全部守卫短路（运行时静默无存档）；
  `Engine::init` 现缺省安装 `LocalFileSaveProvider`，宿主仍可经 EngineConfig 注入
  自定义 provider 覆盖；+单测锁定。
- P10 — OS 音频焦点仲裁（commit 84179efa）：见 Platform Infrastructure 表 P10 行；
  `IAudioFocusService` 四方法（addListener/removeListener/post/currentState）+
  `IAudioFocusListener` 监听器接口；`BackendRegistry` 新增 set/getAudioFocusService
  槽位（#25）。移动端原生中断回调接线待 Track M/I。

### 工具链边界说明（Matrix 不单列工具类能力的原因）

能力矩阵只跟踪**引擎代码级能力面**（runtime 表面/接口/命令/绑定/编辑器内核）。
阶段 G 新增的**内容工具链**属于仓库工具层（非引擎运行能力），不在矩阵单列，
统一在 guides/ 记录：

| 工具 | 位置 | 记录文档 |
|---|---|---|
| i18n 提取/回填（ks_i18n extract/collect/backfill + --keys/--missing 门禁） | `scripts/ks_i18n.lua` | `docs/guides/i18n.md`（工作流节） |
| XP3 归档解析器 | `tools/xp3_tool.py`（27 unittest） | `docs/guides/xp3-compat.md` + `docs/guides/kag3-migration.md` |
| TLG5/6 图片解码器 | `tools/tlg2png.py`（38 断言自洽测试） | `docs/guides/tlg-compat.md` |
| 一键打包/分发 | `scripts/package_game.sh` + `deploy-web.yml` | `docs/guides/packaging-ux.md` |
| 项目模板 | `demo/template/` + `scripts/verify_template.sh` | `docs/guides/template-quickstart.md` |
| 示例游戏验证 | `scripts/verify_sample_game.sh` | `docs/guides/sample-game-verification.md` |

这些工具的可执行性由对应测试/脚本断言守护（tools 65 断言、i18n 39 断言、template 4/4、
verify 5/5），不进入 runtime 能力计数；如未来希望将某工具提升为引擎能力（如 xp3 作为
资源提供者链的正式一环），再以独立 R/S/C 行纳入本矩阵。

### 2026-08-16 additions (round 91-98)

- S4 — round 97 expr nesting budget: deeply-nested expression evaluation
  hardened from worst-case O(n³) to a bounded cut (<=0.001s), preventing
  pathological deep-nest expression stalls; part of the round 92-96 zero
  regression perf pass (round 97).
- S7 — round 97/98 schema `coerce` semantics corrected and deepened:
  positional-argument typed coercion, `choices` array normalization,
  default-value normalization, and an empty-file defect fixed; round-97
  `test_schema_coerce` depth suite (+85 assertions covering
  type/default/enum/positional/idempotence).
- P2 — round 98 test baseline: C++ 963/963, Lua 128/128 + 20 orphan,
  web 282/282, editor 506/506, ctest 10 + AI-skip.
- D1 — editor depth continues 408→506 (round 95 SceneOutline
  virtualization, round 96 inspector lint/disconnect/jump-status/dual-link,
  round 97 activitybar/statusbar a11y + scene truncation + state depth).

### 2026-08-12 additions (generation-gap round 9)

- S2t — NVL mode `[nvl]`/`[nvl clear]`/`[nvl off]`: full-screen accumulated
  text (Ren'Py NVL parity) with page-break cursor reuse and save/rollback
  persistence of the page.
- S2 — command set census refreshed to 81 contracts (nvl + sma_play/sma_stop).

### 2026-08-15 additions (round 71-75)

- S3a — save/load loop continuity: for/while/if/switch stacks
  lifted into the engine ctx and serialized via `loop_stacks` in
  capture_state; [load] resumes an in-progress loop to completion.
- S9 — nested macro definitions ([macro outer][macro inner]...[endmacro][endmacro])
  plus a depth-based recursion guard (>100 splice depth errors;
  1000+ sequential calls and 2000-iteration loops pass).
- S12 — i18n runtime API (set_language/current_language/translate/reload/default_language; fallback chain; hot-reload).
- [sel]/[button] `x=` result-capture.
- kag3_import macro-arg (`&N`/`%N%` conversion) + `goto`→`jump` alias.
- S2 — command set census refreshed to 117 contracts (add/sub/mul/div/mod/dec,
  csp/csd/csl, textspeed/cps, palette/vibrate, notify, preload, goto alias).
  (118 after round-86 `_meta` completion: 11 KAG3-compat commands gained
  {category,blocking,desc} metadata.)

### 2026-08-15 additions (round 88-90 / 90% milestone)

- S2v — contract runtime coverage 100%: 118/118 commands execute at runtime;
  12 previously dead handlers exercised by `test_contract_runtime_gaps.lua`.
- C10 — tutorial sample library grown to 15 tutorials (round 90 added
  tutorial_14_flow_timing + tutorial_15_expr_deep), path table +2 rows.
- P2 — round 90 test baseline (C++ 849/849, Lua 126+19, web 183, editor 408)
  plus Release build + CPack ZIP verified end-to-end.
- D1 — editor depth: store actions (round 88) + rpc client (round 89) +
  full-App panel-integration E2E (round 90), editor 270→408.

### 2026-08-12 additions (generation-gap round 6)

- S2s — inline text markup `{color=#rrggbb}`…`{/color}` (Ren'Py `{...}` parity;
  b/i/size consumed for source compat; unknown tags literal).
- D9 — LSP navigation: goto-definition / find-all-references for labels.
- D10 — Skeletal mesh animation S2/S3 (CPU soft-skinning + bgfx renderer + Lua
  driver + commands + binding).

### 2026-08-12 additions (generation-gap round 5)

- S2q `[until exp=... timeout=...]` — declarative conditional wait (AOT-compiled
  expression, per-frame yield, Operation cancellation, timeout guard).
- S2r `[button cond=...]` — conditional choices at [endbutton] (Ren'Py menu `if`
  parity; TJS conditions evaluated per block; all-hidden blocks dissolve).
- D8 `kag/aidev.lua` — AI-assisted development: local rule-based diagnostic
  explainer, structural scene review, LLM fix suggestions + scene generation
  with self-review; IDE surface via /api/eval (AiPanel Dev Assist section).

### Performance notes (2026-08-07 pass)

- Tokenizer: 4000-token scene parses in ~205 ms (52 ms/1000tok) after the
  -28.5% prefix-pattern pass; scene cache avoids re-parsing.
- Scheduler: 4001 coroutine resumes in ~16 ms (-36%): schema require
  hoisted out of the per-token loop; schema.coerce builds its location
  string lazily (error paths only).
- ks_check.lineOf: line-start index + binary search replaces per-token
  O(n) text scanning (O(n²) -> O(n log n)) — editor keystroke latency.
- expr.translate: bounded compiled-chunk cache (per-env identity).

### Module hardening pass (2026-08-07, per-module sweep)

- audio: 4-slot voice pool (overlapping lines fade, not cut) + BGM
  ducking (35% while a voice plays, exact restore); wave-cache LRU
  UB fixed (flushWaveCache rebuilt the LRU index; eviction guards)
- steam: overlay state via GameOverlayActivated_t (was IsOverlayEnabled
  -> permanent input pause); stats loaded via RequestCurrentStats +
  UserStatsReceived_t; StoreStats batched (1s throttle)
- input/entry: SDL_EVENT_WINDOW_RESIZED routed through
  InputRouter::notifyResize (was documented but never wired); per-frame
  GPU-time globals only written on change; single instruction-budget
  reset per frame
- archive: thread-local BCrypt key-handle cache (algorithm + key object
  reused across blocks; move-semantic RAII); save JSON compact (-40%)
- debug/job: LogEntry fixed char[256] (zero alloc per log call);
  isWorkerThread O(1) thread_local (was O(n) scan); dead member removed
- script/debug: RenderBinding getters cache backend pointers (registry
  lookup per binding call removed); DebugProtocol line hook fast path
  when no breakpoints/step
- render: fillViewport solid-pixel texture cached per color (was a GPU
  texture create per call); dead SimBatch worker + shouldUsePlmpeg
  removed
- minigame: sweep-and-prune collision detection (O(n^2) -> O(n log n))
- resource: decoded-asset cache (instant scene re-entry, cancelAll
  clears); ImageDecoder stb-first order fixed a 1x1-PNG crash in
  bimg::imageParse; dead cancel-generation field removed
