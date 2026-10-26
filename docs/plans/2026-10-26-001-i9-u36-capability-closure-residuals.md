# I9 / U36 能力闭包残余裁决与 B1 扫描器修复执行记录

- 执行人：closure-engine（Lead 下引擎能力 closure teammate）
- worktree：`artifacts/validation/i9-closure-worktree`，分支 `codex/i9-u36-closure`，基线 `01c52fad`
- 计划依据：`docs/plans/2026-09-05-001-refactor-runtime-foundation-plan.md` §5.1.2/U36（2026-10-26 续排；只读自 `codex/next-iteration-plan-20261026`）
- 输入：capability-closure 矩阵（旧指纹 `eb91cd3abff8059d`）、`capability-closure-overrides.json`、2026-10-03-001 命令原生审计、`config/runtime-capabilities.json`、`scripts/capability_runtime.lua`、`tests/scripts/**`
- 本轮写入范围：`scripts/capability_closure.py`（B1 经 Lead 授权）、`tests/**`、`docs/design/capability-closure-overrides.json`、生成矩阵、本记录。未改 `src/`、未改 CI workflow、未 push/merge。

## 1. 结论摘要

| 口径 | UNWIRED | PARTIAL | CLOSED | EXTRA | EXPERIMENTAL(人工) |
|---|---:|---:|---:|---:|---:|
| 修改前（基线矩阵） | 7 | 5 | 131 | 33 | 2 |
| B1 修复后（未加人工裁决） | 7 | 2 | 134 | 33 | 2 |
| **本轮最终（含人工裁决）** | **0** | **2** | **141** | 33 | **2** |

恒等式保持 `145 = 141 + 2 + 0 + 2`；`171 = 145 + 33 + 0`。未闭合残余 **2 条 PARTIAL**：`quickmenu_log`、`quickmenu_config`（原因见 §5，属只写状态、无消费方，拒绝凑 CLOSED）。EXPERIMENTAL 2 条（`live2d_motion`、`live2d_expression`）按 C 类保留并补充拒绝证据。5 个明确拒绝命令保持拒绝，不静默成功。

历史与原始判级全部保留：`v4_raw`/`status_machine` 未改写；`GUARD_PARTIAL`（assert/endbutton）回归锁与 v7 类别未放宽；生成器证据边界声明不变（静态扫描 ≠ 运行 PASS）。

## 2. B1 扫描器修复：RED → GREEN

### 2.1 RED（真实失败先留证）

基线 `capability_closure.py` 的 `StringUtils_file_slice`（旧 :757-765）在函数体内**遇到第一个嵌套 `local function`/`function` 行即截断**，导致 v4 一跳穿透看不到其后的效果面调用：

| 命令 | handler | 一跳落点 | 截断处（旧行为） | 真实效果面（被切掉） | 旧机器判级 |
|---|---|---|---|---|---|
| `blur` | scripts/kag/commands/transition.lua:295 | scripts/vfx.lua:220 `VFX.blur` | scripts/vfx.lua:247 `local function free_a` | :256-257 `rtt.alloc`、:267 `backend.submit_vfx(...,2,...)` | PARTIAL |
| `flash` | scripts/kag/commands/vfx.lua:382 | scripts/vfx.lua:337 `VFX.flash` | scripts/vfx.lua:357 `local function cleanup` | :386 `backend.create_solid_texture`（在嵌套 pcall 内） | PARTIAL |
| `chapter` | scripts/kag/commands/system.lua:323 | scripts/chapter_select.lua:45 `ChapterSelect.show` | scripts/chapter_select.lua:58 `local function cleanup` | :78 `backend.set_input_focus`、:89/:99/:102 `backend.render_text` | PARTIAL |

RED 证据（修复前运行新增回归测试，5 failures）：`python -B tests/scripts/test_capability_closure.py` →
`test_01_slice_includes_effect_after_nested_local_function`、`test_02_...multi_level...`、`test_03_...multiline_strings...`、`test_05_...penetration...`、`test_06_real_repo_partial_commands_reach_effect_surface` 全部失败；典型输出 `'backend.create_solid_texture(255, 255, 255, 255)' not found in 'function VFX.flash(ctx, params)\n    local flashLayer = {}'`。

### 2.2 修复

- 新增 `lua_block_depths(code)`：逐行累计 Lua 块深度（`function/if/do/repeat` 开、`end/until` 合；跳过行注释、长注释、引号字符串与长字符串，跨行长括号按换行数保持行对齐）。
- `StringUtils_file_slice` 改为「匹配块闭合行」为边界（单行函数体特判），不再按下一个 `function` 关键字截断；深度按文件缓存于 `info["lua_depths"]`。
- 回归测试（`tests/scripts/test_capability_closure.py` 新增 `TestV4NestedFunctionSlices` 5 个用例）：嵌套 `local function`、多级嵌套、多行字符串/注释、单行与末函数边界、临时 REPO 的一跳穿透、真实仓库 blur/flash/chapter 链路。测试注册进真实入口：`tests/CMakeLists.txt` 新增 CTest `CaesuraCapabilityClosure`。
- GREEN：上述 16 tests OK（10 existing + 6 new，含重复断言 1 个）；生成器自动翻转 blur/flash/chapter（`Consumed n→Y`、`PARTIAL→CLOSED`），另使 `history` 的 Consumed 由 n→Y（其状态原本就是人工 CLOSED，无计数变化）。

## 3. 人工证据层裁决（A 类闭合，12 条）

每条均在 `docs/design/capability-closure-overrides.json` 记录 reason/evidence/source，并有真实注册的运行面测试：

| 条目 | 新状态 | 运行面证据 |
|---|---|---|
| `stopvideo` | CLOSED | tests/scripts/test_video.lua:104-148（factory+direct 真实路径：stop 计数/owner 清理/replacement 不误杀）、test_contracts_runtime.lua:76-77/:241-243、test_capability_closure_residuals.lua（owner handle 消费/幂等） |
| `trans` | CLOSED | tests/scripts/test_trans_scheduler_visual.lua:54-164（真 tokenizer/scheduler/layers/commands + 记录型 render 边界，断言 `submit_transition`）、test_transitions.lua、test_trans_behavior.lua |
| `quickmenu_auto` | CLOSED | tests/scripts/test_quickmenu.lua:56-72/:184-190；ctx.auto_mode 由 kag_runner.lua:960 消费；派发入口 scripts/scheduler.lua:1467 `schema.resolveHandler`（新测试断言） |
| `quickmenu_skip` | CLOSED | test_quickmenu.lua:74-87/:184-190；ctx.skip_mode 由 kag_runner.lua:888/:923 消费；resolveHandler 断言 |
| `quickmenu_qsave` | CLOSED | test_quickmenu.lua:103-149；真实 kag.save 路由 + slot clamp + save_result 契约 |
| `quickmenu_qload` | CLOSED | test_quickmenu.lua:151-170；真实 kag.load 路由 + load_result 契约 |
| `quickmenu_title` | CLOSED | test_quickmenu.lua:172-181；真实 kag.jump 路由 |
| `blur` | CLOSED（B1 自动） | test_capability_closure_residuals.lua（`rtt.alloc×2/free×2 + backend.submit_vfx effect=2/radius>0`）；运行面能力仍 C 类拒绝（见 §4） |
| `flash` | CLOSED（B1 自动） | test_effect_aliases.lua:43-74（scheduler 派发）+ test_capability_closure_residuals.lua（`backend.create_solid_texture` clamp 255/0/128/255、`__flash` 层） |
| `chapter` | CLOSED（B1 自动） | test_chapter_select.lua:27-61（真 `ChapterSelect.show` + Esc 取消 + handler→_pendingJump）+ test_capability_closure_residuals.lua（真 show 渲染帧 + Esc 完成） |
| `history` | 仍 CLOSED（人工；B1 后 Consumed 也变 Y） | 原有 test_kag_commands/history 证据，状态未变 |

新运行面测试文件：`tests/scripts/test_capability_closure_residuals.lua`（82 assertions），注册进 `tests/scripts/run_lua_tests.lua`（位于 `test_quickmenu` 之后，保持真实派发表/能力栈装载顺序）。覆盖：blur/flash/chapter 真链路、stopvideo owner 消费、7 个 quickmenu 的 `schema.resolveHandler(kag)` 派发解析、5 个拒绝命令的能力层拒绝（精确 feature/reason、0 效果调用）、SDK-off steam_achievement 的 unavailable 记录。

## 4. C 类：5 个明确拒绝命令（保持明确拒绝，不静默成功）

| 命令 | 运行面当前行为（已实测） | 状态/去向 |
|---|---|---|
| `blur` | 能力层拒绝 `render.blur` → `unsupported/command_not_wired`，handler 不执行、0 效果；headless RTT 缺失另有诊断 no-op | C（保留）+ B5 提案（真实 GPU 效果验收后才可能改能力目录） |
| `xfadebgm` | `audio.crossfade` → `unsupported/backend_route_not_wired`，0 效果；根因 backend_factory.lua:103-124 无 xfade 路由、原生音频无交叉淡化 API | C + B3 提案（音频双流） |
| `live2d_motion` | `kag.live2d_motion` → `unsupported/command_not_wired`，0 效果；Live2DBinding.cpp:119-126 无 motion API | C + B4 提案（引擎 API） |
| `live2d_expression` | `kag.live2d_expression` → `unsupported/command_not_wired`，0 效果；绑定无 expression API | C + B4 提案 |
| `steam_achievement`(SDK-off) | 无 steam 绑定时 handler 不崩：WARN + `ctx.tf.steam_achievement_result="unavailable"` 返回 true；能力层 `steam.achievements` 在 `compiled.steam=false` 时为 `sdk_disabled`；有 Steam 账号正向写入 NOT_RUN | C（条件项，外部账号/AppID） |

## 5. 未闭合 / 待裁决

1. **PARTIAL 2（quickmenu_log / quickmenu_config）**：命令可派发，但 handler 只写 `ctx.backlog_visible`/`ctx.config_open`，全仓无读取方——不是静默成功也不是真实 UI 接线。保持 PARTIAL；接线 `HistoryUI.show`/`Settings.show` 会改变阻塞语义，需 Lead 产品决策（B6 提案）。测试已如实注明"无消费方待接线"，未用断言冒充效果。
2. **EXPERIMENTAL 2（live2d_motion/expression）**：feature-gated + 绑定无 API，见 B4。
3. **生成矩阵『可疑翻转清单（v4 保守维持）』仍列出 hr/stopvideo/trans**：该区段按 hashed 机器翻转清单渲染，不读取 override 终态（hr 在本轮前已有同一显示现象）；stopvideo/trans 的裁决已写入人工覆盖区（Status=CLOSED (raw: PARTIAL)）。属生成器展示层既有口径，未手改生成区，留待扫描器展示层统一修正。
4. **B2 未授权**：quickmenu 动态注册识别不改扫描器；以人工证据层裁决并保留提案。
5. 全量 Lua 套件在 WSL lua5.4 为 **152 passed / 2 failed / 154 total**；2 个失败为既有环境项（`test_i18n`、`test_ks_i18n_flow`：standalone 缺 `lua-filesystem`，报 "Asset directory listing unsupported"），与本轮改动无关，未掩盖/未改写。

## 6. 验证命令与结果（2026-10-26）

```
python -B tests/scripts/test_capability_closure.py
  → Ran 16 tests ... OK（新增 5 类嵌套切片回归用例；RED 时为 5 failures）
python scripts/capability_closure.py
  → declared=145 dispatched=171 consumed=96 tested=156
  → statuses={'UNWIRED': 0, 'PARTIAL': 2, 'CLOSED': 141, 'EXTRA': 33, 'EXPERIMENTAL': 2}
  → v5 phantom self-check OK；suspected_flips=3（既有保守清单）
python -B tests/scripts/check_test_coverage.py
  → TEST COVERAGE OK: 216 lua + 106 cpp tests all registered.
git diff --check → exit 0
wsl -d Ubuntu -- lua5.4 tests/scripts/run_lua_tests.lua
  → Results: 152 passed, 2 failed, 154 total（2 项为上述既有 LuaFileSystem 环境项）
  → [OK] test_capability_closure_residuals（82 assertions, 0 failed）
python -B tests/scripts/test_capability_closure_residuals.lua 等价：直接 lua5.4 运行 → 82 passed, 0 failed
```

## 7. B 类提案（未执行；需 Lead/后续决策）

- **B2** 扫描器动态注册识别（quickmenu 自动判级；本次以人工证据层替代，未授权改规则）。
- **B3** 真 BGM 交叉淡化：`IAudioBackend`/`SoLoudAudioEngine` 双流 + 音量包络 + `backend_factory` `xfade` 路由 + handler 接线；完成后重评 `audio.crossfade` 目录与 corpus `audio-crossfade-refusal`（corpus external_boundary 已要求重评，禁止沿用旧拒绝策略）。
- **B4** Live2D motion/expression 原生 API：`IAnimationBackend` 扩展 + `Live2DBinding` luaL_Reg + handler 改绑定调用 + 能力目录/corpus 重评；需 Cubism SDK 实机/资产证据。
- **B5** `render.blur` 真实 GPU 效果验收（前后帧/像素判据）后，才可能把能力目录从 unsupported 改级并把 corpus blur-refusal 改正向用例。
- **B6** quickmenu_log/config 接真实 backlog/settings UI 的产品决策（涉及阻塞语义）。

## 8. 保留与边界

- 未改 `src/`、`.github/workflows`、主工作区、其它 worktree；未 push/merge/tag。
- 不把静态扫描改成运行通过：生成器仍不执行引擎；运行证据只以注册测试形式存在，override evidence 引用具体测试与运行环境（WSL lua5.4）。
- 不改写历史失败/计数；不降低断言；新增测试仅增加覆盖。
