# Capability Closure Matrix (auto-generated)

> 由 python scripts/capability_closure.py 生成；勿手动编辑。
> 生成时间（输入源最新 mtime）：2026-09-12T19:30:34Z
> 生成命令：python scripts/capability_closure.py
> 源指纹（输入内容 sha256 前 16 hex）：448cdd5fca4cb58b
> 输出确定性：同源指纹同字节（generated_at 为输入源最新 mtime；跨机 checkout 的 mtime 差异属 by-design，确定性以指纹为准）

## 概述

- 合约总数（Declared，docs/api/command-contracts.md）：**134**
- 已注册（Dispatched）：**166**
- 触达效果面（Consumed，调用形上下文+一跳穿透 v4）：**87**
- 测试引用（Tested，启发式计数）：**143**
- UNWIRED：0 · PARTIAL：4 · CLOSED：127 · EXTRA：32 · EXPERIMENTAL(人工)：3
- **四层闭包（2026-09-04）**：Structural Closed=127 · Runtime 测试证据=143 · Platform=3 · Packaged=0
  - 列注记：Platform/Packaged 两列随 Phase2 分发逐项真实验证填充（当前无证据=诚实 0）；Runtime=语义测试证据存在（非全部效果面验证）。
- **幻影绑定（v5）**：**2** 处 backend.<name> 调用命中
  - 提取模式：union of: bindings/*.cpp luaL_Reg { name, lua_X }; backend.lua ^function Backend.X; backend_factory.lua cmd==X; kag.lua ^function KAG.X
  - 清单大小：199 个可解析名（cpp=159 · shim=69 · factory=64 · kag=20，绑定文件 15 个）
  - web/jsBackend 交叉核对（仅报告，不参与判定）：幻影名在 web/bridge.js 亦有=render_frame
    · 原生+js 均无=（无）
- **恒等式：**134 = CLOSED(127) + PARTIAL(4) + UNWIRED(0) + EXPERIMENTAL(在册 3)；166 = 134(Declared) + EXTRA(32) + EXPERIMENTAL(合约外 0)**

**范围声明（t103 MUST-FIX 3）**：本矩阵的 134 = 声明式 KAG 命令合约闭包（docs/api/command-contracts.md 全量条目）。下列能力**不在 134 内**：
- 原生手势链：SwipeDown / SwipeUp / LongPress / Pinch / TwoFingerTap / ThreeFingerHold（平台层）；
- 文本标记参数：letter_spacing / spacing / font / line_height 等内联标记（非命令）；
- KAG.* Lua API：jump/call/return_to_caller 等直接 API（注册键存在但非合约命令，见 EXTRA 的 api-alias）。
这些能力由矩阵底部「人工判级（范围外能力）」区段承载（docs/design/capability-closure-overrides.json 驱动）。
**EXTRA 属设计行为**：contracts 由声明式 schema registry（kag/schema.lua，经 scripts/schema_doc.lua 生成）产出；流控/API 命令按设计不在注册表——EXTRA 不是缺陷信号（A 类入册与否=产品决策待议）。

> 状态定义：**UNWIRED**=有合约无处理器；**PARTIAL**=已注册但处理器体未以调用形触达效果面（v4 一跳穿透下仍无命中）；
> **CLOSED**=已注册且调用形触达效果面；**EXTRA**=已注册但无合约。
> **EXPERIMENTAL**=人工覆盖状态（能力存在但无消费方/无真实测试面——freeze 政策显式标注；见『EXPERIMENTAL』节与 overrides reason/note；机器判级仍为 PARTIAL/EXTRA）。
> ⚠ = 人工覆盖（docs/design/capability-closure-overrides.json；详见『人工覆盖』节）。
> 　* = 存在幻影绑定命中（backend.<name> 不在原生绑定面；详见『幻影绑定（v5）』节）；Consumed 列已按 v5 幻影过滤。

> 状态定义（v7 附加）：**PAIRING/EXEMPT_PURE/EXEMPT_CONSUMED**=t192 机器判级类别——配对/别名同族效果面并接、纯状态/纯 Lua 执行豁免、非 backend 直调消费豁免；由审计批（t181/t188/t189/t190）裁决名单驱动，不依赖 overrides status 字段，机器自判 CLOSED。

## v7 机器判级（t192）

### PAIRING_GROUPS（配对/别名同族并接） — 5 条

- button — t181/t189
- delay — t181/t189
- endbutton — t181/t189
- endselect — t181/t189
- select — t181/t189
### EXEMPT_PURE（纯 Lua 状态/专属命名空间豁免） — 22 条

- add — t183/t185 (math.lua:83-119 binop -> ctx scope write)
- assert — t188 assert (system.lua:506-526 exprLang.evaluate + error->handle_error)
- dec — t183/t185 (math.lua:129-141)
- div — t183/t185 (math.lua:83-119)
- emb — t188 emb (system.lua:92-178 sandbox.execute/load + ctx mutation sync)
- ending — t183/t185 (system.lua:365-375 -> save.lua:176/418 + title_menu:30-53)
- eval — t188 eval (live=scheduler.lua:1099 inline)
- i18n — t183/t185 (system.lua:691-712 -> i18n.set_language + relocalize_page)
- inc — t188 inc (system.lua:471-484 nil-safe increment)
- mod — t183/t185 (math.lua:83-119)
- mul — t183/t185 (math.lua:83-119)
- random — t188 random (system.lua:528-545 integer-floor scope write)
- saveplace — t183/t185 (save.lua:550-552 -> system.lua:313-361 -> _pendingJump)
- set — t188 set (system.lua:453-465 resolve_var/infer_value scope write)
- skip — t188 skip (text.lua:1082-1094 ctx.skip_mode -> kag_runner:481-530)
- sma_anim — t190 sma_anim (sma.lua:723-731 -> sma.update re-skin @:528-533; per-frame pump caveat)
- sma_ik — t190 sma_ik (sma.lua:733-739 -> 2-bone constraint -> update_mesh)
- sma_play — t190 sma_play (sma.lua:712-721 -> binding().create_mesh @sma.lua:382-383)
- sma_stop — t190 sma_stop (sma.lua:747-749 -> binding().destroy_mesh @:452-453)
- sma_variant — t190 sma_variant (sma.lua:741-745 -> binding().destroy_mesh/create_mesh immediate)
- sub — t183/t185 (math.lua:83-119)
- unlock — t188 unlock (system.lua:399-413 -> gallery.lua:51-103 + save persistence)
### EXEMPT_CONSUMED（非 backend 直调消费） — 18 条

- auto — t189 auto (text.lua:1112 ctx.auto_mode -> kag_runner:525-536 auto-advance)
- br — t189 br (kag.lua:212 -> KAG.l real line break)
- cps — t189 cps (text.lua:1219 -> apply_text_cps:1197 ctx.text_speed -> kag_runner:482)
- nameplate — t189 nameplate (text.lua:418 -> _renderNameplate:431-454 layers+render_text)
- notify — t189 notify (system.lua:648-675 -> toast.show toast.lua:18-41 real UI)
- pt — t189 pt (text.lua:1153 ctx.text_speed -> same read point)
- quake — t189 quake (kag.lua:421 -> vfx.lua:28-70 -> node.quake.offset -> layers.lua:585-593)
- replay — t188 replay (system.lua:579-604 -> replay module + kag_runner tick :442-448/:740)
- rollback — t188 rollback (system.lua:389-397 -> kag_runner.rollback:710 snapshot chain)
- s — t189 s (kag.lua:303 -> System.wait(ms=250))
- saveload — t188 saveload (save.lua:495-523 -> saveload_menu -> SaveCommands.save/load C++)
- shake — t189 shake (kag.lua:417 -> vfx.lua:82 -> node.shake.offset -> layers.lua:585-593)
- textspeed — t189 textspeed (text.lua:1215 ctx.text_speed=floor(1000/cps) -> kag_runner:482)
- voice_off — t189 voice_off (text.lua:1126 ctx.voice_muted -> audio.lua:243 gate + save:168/375)
- voice_wait — t189 voice_wait (kag.lua:297 -> audio.lua:277-301 wait loop + click-skip)
- wait — t188 wait (system.lua:50-84 Operation/CancelToken + scheduler-dt yield loop)
- waitclick — t188 waitclick (kag.lua:312-318 waiting_input -> runner click flow)
- waitforclick — t188 waitforclick (kag.lua:388-396 waiting_input loop -> runner)

## Commands

| Command | Declared | Dispatched | Consumed | Structural | Runtime | Platform | Packaged | Observable | 证据 |
|---|---|---|---|---|---|---|---|---|---|
| Bezier | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/transition.lua:606 |
| LUTCache | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/transition.lua:605 |
| add | Y | Y | n | CLOSED ⚠ | ✓50 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/math.lua:121 |
| ai_dialog | Y | Y | Y | CLOSED | ✓7 | - | - | ? | scripts/kag/commands/system.lua:671 |
| assert | Y | Y | n | CLOSED | ✓7 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:463 |
| auto | Y | Y | n | CLOSED | ✓6 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1161 |
| bg | Y | Y | Y | CLOSED | ✓21 | - | - | ? | scripts/kag/commands/layer.lua:122 |
| bgm | Y | Y | Y | CLOSED | ✓4 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:492 |
| blur | Y | Y | n | PARTIAL | ✓4 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/transition.lua:292 |
| br | Y | Y | n | CLOSED | ✓3 | - ⚠ | - ⚠ | VERIFIED（位置级） ⚠ | scripts/kag.lua:212 |
| button | Y | Y | n | CLOSED ⚠ | ✓39 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1392 |
| call | n | Y | n | EXTRA | ✓80 | - | - | ? | scripts/kag.lua:514 |
| camera | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/transition.lua:495 |
| cancel | Y | Y | Y | CLOSED | ✓7 | - | - | ? | scripts/kag.lua:222 |
| capture_state | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/save.lua:250 |
| ch | Y | Y | Y | CLOSED | ✓686 | - | - | ? | scripts/kag/commands/text.lua:610 |
| chapter | Y | Y | n | PARTIAL | ✓3 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:323 |
| cl | Y | Y | Y | CLOSED | ✓14 | - | - | ? | scripts/kag/commands/layer.lua:185 |
| clear | n | Y | Y | EXTRA | ✓2 | - | - | ? | scripts/kag.lua:351 |
| clearscreen | n | Y | Y | EXTRA | - | - | - | ? | scripts/kag.lua:209 |
| close | Y | Y | Y | CLOSED | ✓2 | - | - | ? | scripts/kag.lua:230 |
| cps | Y | Y | n | CLOSED | ✓25 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1270 |
| csd | Y | Y | Y | CLOSED | ✓9 | - | - | ? | scripts/kag/commands/character.lua:150 |
| csl | Y | Y | Y | CLOSED | ✓14 | - | - | ? | scripts/kag/commands/character.lua:166 |
| csp | Y | Y | Y | CLOSED | ✓25 | - | - | ? | scripts/kag/commands/character.lua:120 |
| ct | n | Y | Y | EXTRA | ✓2 | - | - | ? | scripts/kag.lua:354 |
| dec | Y | Y | n | CLOSED ⚠ | ✓19 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/math.lua:129 |
| delay | Y | Y | n | CLOSED | ✓33 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:339 |
| div | Y | Y | n | CLOSED ⚠ | ✓15 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/math.lua:124 |
| edit | Y | Y | Y | CLOSED | ✓1 | - | - | ? | scripts/kag/commands/text.lua:1985 |
| emb | Y | Y | n | CLOSED | ✓16 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:103 |
| end | n | Y | n | EXTRA | ✓282 | - | - | ? | scripts/kag.lua:86 |
| endbutton | Y | Y | Y | CLOSED | ✓32 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1426 |
| endform | n | Y | n | EXTRA | ✓1 | - | - | ? | scripts/kag.lua:362 |
| ending | Y | Y | n | CLOSED ⚠ | ✓25 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:339 |
| endmacro | n | Y | n | EXTRA | ✓65 | - | - | ? | scripts/kag.lua:244 |
| endselect | Y | Y | n | CLOSED | ✓23 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1566 |
| endtag | n | Y | n | EXTRA | ✓1 | - | - | ? | scripts/kag.lua:358 |
| er | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/text.lua:972 |
| erasemacro | n | Y | n | EXTRA | ✓8 | - | - | ? | scripts/kag.lua:245 |
| eval | Y | Y | n | CLOSED | ✓98 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:198 |
| fade | Y | Y | Y | CLOSED | ✓2 | - | - | ? | scripts/kag/commands/transition.lua:568 |
| fadebgm | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/audio.lua:174 |
| fadeout | Y | Y | n | CLOSED ⚠ | ✓2 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:373 |
| fadevol | Y | Y | Y | CLOSED | - | - | - | ? | scripts/kag/commands/audio.lua:104 |
| fg | Y | Y | Y | CLOSED | ✓4 | - | - | ? | scripts/kag/commands/layer.lua:158 |
| flash | Y | Y | n | PARTIAL | ✓6 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/vfx.lua:382 |
| flush_cache | n | Y | Y | EXTRA | - | - | - | ? | scripts/kag/commands/resource.lua:254 |
| font | Y | Y | Y | CLOSED | ✓3 | - | - | ? | scripts/kag/commands/text.lua:1117 |
| g | n | Y | n | EXTRA | ✓2 | - | - | ? | scripts/kag.lua:365 |
| gallery | Y | Y | Y | CLOSED | ✓6 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:307 |
| get_texture | n | Y | Y | EXTRA | - | - | - | ? | scripts/kag/commands/resource.lua:226 |
| has_pending_transition | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/resource.lua:308 |
| history | Y | Y | n | CLOSED ⚠ | ✓9 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:205 |
| hr | Y | Y | Y | CLOSED ⚠ | ✓6 | win ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:217 |
| i18n | Y | Y | n | CLOSED ⚠ | ✓62 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:648 |
| image | Y | Y | Y | CLOSED | ✓7 | - | - | ? | scripts/kag/commands/layer.lua:219 |
| inc | Y | Y | n | CLOSED | ✓29 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:445 |
| input | Y | Y | Y | CLOSED | ✓1 | - | - | ? | scripts/kag/commands/text.lua:1706 |
| is_loaded | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/resource.lua:235 |
| is_pending | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/resource.lua:244 |
| jump | n | Y | n | EXTRA | ✓184 | - | - | ? | scripts/kag.lua:503 |
| l | Y | Y | Y | CLOSED | ✓4 | - ⚠ | - ⚠ | VERIFIED（位置级） ⚠ | scripts/kag/commands/text.lua:914 |
| layfade | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/layer.lua:339 |
| layopt | Y | Y | Y | CLOSED | ✓1 | - | - | ? | scripts/kag/commands/layer.lua:313 |
| layout | Y | Y | Y | CLOSED | ✓26 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/layout.lua:169 |
| layout_place | Y | Y | Y | CLOSED | ✓3 | - | - | ? | scripts/kag/commands/layout.lua:243 |
| layout_slot | Y | Y | Y | CLOSED | ✓25 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/layout.lua:204 |
| ld | Y | Y | Y | CLOSED | ✓3 | - | - | ? | scripts/kag.lua:399 |
| listsaves | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/save.lua:488 |
| live2d_expression | Y | Y | n | EXPERIMENTAL ⚠ | ✓1 | - ⚠ | - ⚠ | ? ⚠ | scripts/kag/commands/character.lua:189 |
| live2d_lip_sync | Y | Y | n | EXPERIMENTAL ⚠ | ✓1 | - ⚠ | - ⚠ | ? ⚠ | scripts/kag/commands/character.lua:200 |
| live2d_motion | Y | Y | n | EXPERIMENTAL ⚠ | ✓3 | - ⚠ | - ⚠ | ? ⚠ | scripts/kag/commands/character.lua:177 |
| load | Y | Y | Y | CLOSED | ✓63 | - | - | ? | scripts/kag/commands/save.lua:395 |
| loadplace | Y | Y | Y | CLOSED | ✓6 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/save.lua:519 |
| macro | n | Y | n | EXTRA | ✓67 | - | - | ? | scripts/kag.lua:243 |
| mod | Y | Y | n | CLOSED ⚠ | ✓13 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/math.lua:125 |
| move | Y | Y | Y | CLOSED | ✓3 | - | - | ? | scripts/kag/commands/transition.lua:387 |
| moveto | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/layer.lua:290 |
| mul | Y | Y | n | CLOSED ⚠ | ✓11 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/math.lua:123 |
| music | Y | Y | n | CLOSED ⚠ | ✓5 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:317 |
| nameplate | Y | Y | n | CLOSED | ✓9 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:419 |
| notify | Y | Y | n | CLOSED | ✓38 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:605 |
| nvl | Y | Y | Y | CLOSED | ✓28 | - | - | ? | scripts/kag/commands/text.lua:1025 |
| p | Y | Y | Y | CLOSED | ✓304 | - | - | ? | scripts/kag/commands/text.lua:991 |
| palette | Y | Y | n | CLOSED ⚠ | ✓28 | win ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/vfx.lua:467 |
| particle_weather | Y | Y | Y | CLOSED | ✓3 | - | - | ? | scripts/kag/commands/vfx.lua:610 |
| particles | Y | Y | Y | CLOSED | ✓2 | - | - | ? | scripts/kag/commands/vfx.lua:398 |
| play | Y | Y | Y | CLOSED | ✓6 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:469 |
| playbgm | Y | Y | Y | CLOSED | ✓17 | - | - | ? | scripts/kag/commands/audio.lua:109 |
| playbgmstop | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/audio.lua:148 |
| playse | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/audio.lua:210 |
| playstop | Y | Y | Y | CLOSED | ✓6 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:429 |
| playvoice | Y | Y | Y | CLOSED | ✓7 | - | - | ? | scripts/kag/commands/audio.lua:240 |
| position | Y | Y | Y | CLOSED | ✓4 | - | - | ? | scripts/kag/commands/layer.lua:299 |
| postprocess | Y | Y | Y | CLOSED | ✓3 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/vfx.lua:523 |
| postprocess_off | Y | Y | Y | CLOSED | ✓2 | - | - | ? | scripts/kag/commands/vfx.lua:535 |
| preload | Y | Y | Y | CLOSED | ✓11 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/resource.lua:154 |
| preload_transition | n | Y | Y | EXTRA | - | - | - | ? | scripts/kag/commands/resource.lua:280 |
| prepare_load | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/save.lua:376 |
| promote_transition_slot | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/resource.lua:294 |
| pt | Y | Y | n | CLOSED | ✓17 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1204 |
| push_backlog | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/text.lua:334 |
| quake | Y | Y | n | CLOSED | ✓2 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:423 |
| r | Y | Y | Y | CLOSED | ✓3 | - ⚠ | - ⚠ | VERIFIED（位置级） ⚠ | scripts/kag.lua:290 |
| random | Y | Y | n | CLOSED | ✓14 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:485 |
| relocalize_backlog | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/text.lua:1589 |
| relocalize_page | n | Y | Y | EXTRA | - | - | - | ? | scripts/kag/commands/text.lua:1647 |
| replay | Y | Y | n | CLOSED | ✓5 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:536 |
| reset | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/text.lua:1185 |
| return_to_caller | n | Y | n | EXTRA | - | - | - | ? | scripts/kag.lua:530 |
| rollback | Y | Y | n | CLOSED | ✓9 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:363 |
| ruby | Y | Y | Y | CLOSED | ✓8 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1084 |
| s | Y | Y | n | CLOSED | ✓7 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:305 |
| save | Y | Y | Y | CLOSED | ✓103 | - | - | ? | scripts/kag/commands/save.lua:266 |
| saveload | Y | Y | n | CLOSED | ✓12 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/save.lua:460 |
| saveplace | Y | Y | n | CLOSED ⚠ | ✓7 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/save.lua:515 |
| scroll | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/transition.lua:215 |
| se | n | Y | Y | EXTRA | ✓6 | - | - | ? | scripts/kag.lua:445 |
| sel | n | Y | n | EXTRA | ✓67 | - | - | ? | scripts/kag/commands/text.lua:1564 |
| select | Y | Y | n | CLOSED ⚠ | ✓34 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1560 |
| set | Y | Y | n | CLOSED | ✓194 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:427 |
| setbgmvolume | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/audio.lua:368 |
| setsevolume | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/audio.lua:373 |
| setvoicevolume | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/audio.lua:378 |
| shake | Y | Y | n | CLOSED | ✓2 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:419 |
| showtext | n | Y | Y | EXTRA | - | - | - | ? | scripts/kag.lua:206 |
| skip | Y | Y | n | CLOSED | ✓18 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1131 |
| sma_anim | Y | Y | n | CLOSED | - | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/sma.lua:723 |
| sma_ik | Y | Y | n | CLOSED | - | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/sma.lua:733 |
| sma_play | Y | Y | n | CLOSED | ✓6 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/sma.lua:712 |
| sma_stop | Y | Y | n | CLOSED | ✓4 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/sma.lua:747 |
| sma_variant | Y | Y | n | CLOSED | - | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/sma.lua:741 |
| sprite_fade | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/text.lua:467 |
| sprite_move | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/text.lua:507 |
| sprite_scale | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/text.lua:546 |
| sprite_swap | Y | Y | Y | CLOSED | ✓7 | - | - | ? | scripts/kag/commands/text.lua:588 |
| steam_achievement | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/system.lua:748 |
| stopbgm | Y | Y | Y | CLOSED | ✓6 | - | - | ? | scripts/kag/commands/audio.lua:131 |
| stopse | Y | Y | Y | CLOSED | ✓7 | - | - | ? | scripts/kag/commands/audio.lua:229 |
| stopvideo | Y | Y | Y | PARTIAL | ✓3 | - | - | ? | scripts/kag/commands/video.lua:123 |
| stopvoice | Y | Y | Y | CLOSED | ✓1 | - | - | ? | scripts/kag/commands/audio.lua:320 |
| sub | Y | Y | n | CLOSED ⚠ | ✓8 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/math.lua:122 |
| text | Y | Y | Y | CLOSED ⚠ | ✓62 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:845 |
| textbox | Y | Y | Y | CLOSED | ✓9 | - | - | ? | scripts/kag/commands/text.lua:382 |
| textspeed | Y | Y | n | CLOSED | ✓42 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1266 |
| trans* | Y | Y | Y | CLOSED | ✓4 | - | - | ? | scripts/kag/commands/transition.lua:299 |
| tween | Y | Y | Y | CLOSED | ✓15 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/tween.lua:201 |
| typewriter | Y | Y | n | CLOSED ⚠ | ✓7 | win ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1324 |
| typewriter_sound | Y | Y | n | CLOSED ⚠ | ✓6 | - ⚠ | - ⚠ | ? ⚠ | scripts/kag/commands/text.lua:1338 |
| unlock | Y | Y | n | CLOSED | ✓54 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:373 |
| update | n | Y | n | EXTRA | - | - | - | ? | scripts/kag/commands/tween.lua:165 |
| vfx | Y | Y | Y | CLOSED | ✓12 | - | - | ? | scripts/kag/commands/vfx.lua:292 |
| vib | Y | Y | Y | CLOSED | ✓5 | - | - | ? | scripts/kag/commands/transition.lua:455 |
| vibrate | Y | Y | Y | CLOSED | ✓13 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/vfx.lua:511 |
| video | Y | Y | Y | CLOSED | ✓4 | - | - | ? | scripts/kag/commands/video.lua:62 |
| voice | Y | Y | Y | CLOSED | ✓4 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:435 |
| voice_off | Y | Y | n | CLOSED | ✓3 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/text.lua:1175 |
| voice_wait | Y | Y | n | CLOSED | ✓4 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:299 |
| wait | Y | Y | n | CLOSED | ✓69 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag/commands/system.lua:51 |
| wait_click | n | Y | n | EXTRA | - | - | - | ? | scripts/kag.lua:325 |
| waitbgm | Y | Y | Y | CLOSED | ✓3 | - | - | ? | scripts/kag/commands/audio.lua:351 |
| waitclick | Y | Y | n | CLOSED | ✓3 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:314 |
| waitforclick | Y | Y | n | CLOSED | ✓7 | - ⚠ | - ⚠ | VERIFIED ⚠ | scripts/kag.lua:390 |
| waitsound | Y | Y | Y | CLOSED | ✓7 | - | - | ? | scripts/kag/commands/audio.lua:339 |
| xfadebgm | Y | Y | Y | CLOSED | ✓3 | - | - | ? | scripts/kag/commands/audio.lua:194 |

## 人工覆盖（⚠）

- add — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t181 核真：binop 驱动器 math.lua:83-119（resolve_var 五作用域+nil-safe 起始 0+div/mod 零除可视错误+no-op）；handler :121-141；效果面=变量状态写。
  - evidence：scripts/kag/commands/math.lua:83-119（handler :121-141）-> ctx.{f,sf,tf,mp,lf}；tests/scripts/test_math_cmds.lua:40-127
- assert — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t188 批3a §assert：exprLang.evaluate + error(scene:line) -> scheduler pcall/handle_error（开发期诊断链）；test_modern_commands.lua。
  - evidence：scripts/kag/commands/system.lua:506-526 -> scripts/kag/expr.lua -> scripts/scheduler.lua（pcall/handle_error）; 测试 test_modern_commands.lua
- auto — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t116 复核：ctx.auto_mode 状态写+明确消费点（kag_runner 自动前进）
  - evidence：scripts/kag/commands/text.lua:1111-1116（ctx.auto_mode）→ scripts/kag_runner.lua:511（auto 消费）
- bgm — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：别名链 KAG.bgm=KAG.play→play 的 backend 链
  - evidence：scripts/kag.lua:490（KAG.bgm=KAG.play）→ play（见 play 条目）→ backend.audio_*
- blur — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：模块委托链 VFX.blur→rtt.alloc GPU blur（headless 无 GPU 降级注记）
  - evidence：scripts/kag/commands/transition.lua:292 → scripts/vfx.lua:183 VFX.blur（rtt.alloc GPU blur）
- br — Observable=VERIFIED（位置级） · PlatformTested=- · Packaged=-
  - reason：t119 复核：自派发链 br→KAG.l（t117 位置级已核）——行断效果；照 l 先例用位置级口径
  - evidence：scripts/kag.lua:212-214（function KAG.br → KAG.l(ctx,params)）→ l（text.lua:906 位置级）
- button — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t110 复核：组合链（staging→render 分离系设计，架构注 :1297-1310）——button 注册本地化选项，endbutton cond 过滤+_renderChoices 绘制+blocking+命中跳转，间接真实触达 backend.render_text。
  - evidence：scripts/kag/commands/text.lua:1340-1372（注册 ctx._choiceButtons）→ :1374+ endbutton（_renderChoices 绘制+阻塞+跳转）→ TextScene draws → backend.render_text
- chapter — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：模块链+状态-流链——ChapterSelect.show（层系统+render_text）→ctx._pendingJump runner 消费
  - evidence：scripts/kag/commands/system.lua:349-361 → scripts/chapter_select.lua:51 layers.ensure/_chapter_bg + :59/:69 backend.render_text → ctx._pendingJump
- cps — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t189 批3b §cps：apply_text_cps 写 ctx.text_speed（kag_runner:482 真实读取）+ test_textspeed.lua 语义。
  - evidence：scripts/kag/commands/text.lua:1219 -> scripts/kag_runner.lua:482; 测试 test_textspeed.lua
- dec — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t181 核真：binop 驱动器 math.lua:83-119（resolve_var 五作用域+nil-safe 起始 0+div/mod 零除可视错误+no-op）；handler :121-141；效果面=变量状态写。
  - evidence：scripts/kag/commands/math.lua:83-119（handler :121-141）-> ctx.{f,sf,tf,mp,lf}；tests/scripts/test_math_cmds.lua:40-127
- delay — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t110 复核：别名链——delay=require(kag.commands.system).wait 同一实现（独立 schema 保 ms coercion），与 [wait] 帧流阻断语义完全一致。
  - evidence：scripts/kag.lua:337-347（require .wait + :343-345 裸位置防御）→ scripts/kag/commands/system.lua:50-83 wait（Operation+yield 帧流）
- div — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t181 核真：binop 驱动器 math.lua:83-119（resolve_var 五作用域+nil-safe 起始 0+div/mod 零除可视错误+no-op）；handler :121-141；效果面=变量状态写。
  - evidence：scripts/kag/commands/math.lua:83-119（handler :121-141）-> ctx.{f,sf,tf,mp,lf}；tests/scripts/test_math_cmds.lua:40-127
- emb — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t113 复核：sandbox.execute/load 执行嵌段 + rawset(ctx.tf,emb_result)——ctx.tf 触达（t110 判据边缘形态，人工判真伪）；嵌段可经 env 触达任意能力。
  - evidence：scripts/kag/commands/system.lua:92-170（sandbox 执行 + rawset(ctx.tf,...)）
- endbutton — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t189 批3b §endbutton：完整共享选择链（cond 过滤->_renderChoices->命中测试 _KAG_onClick->_selectedChoice->yield->x= 写入->_pendingJump）+ test_choice.lua 语义；配对口径见 PAIRING（t192 v7，与 button 同族）。
  - evidence：scripts/kag/commands/text.lua:1375-1479 -> src/entry/Engine.cpp:761(_KAG_onClick 派发) -> scripts/kag_runner.lua:600; 测试 test_choice.lua
- ending — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t182 核真（case d）：system.lua:365-375 状态记录（seen_endings）-> save.lua:176/418-419 持久化闭环 -> title_menu.lua:30-53 展示（render_text 原生）；测试 test_gallery_bare:25-32 真语义。
  - evidence：scripts/kag/commands/system.lua:365-375 -> scripts/kag/commands/save.lua:176/418 -> scripts/title_menu.lua:30-53
- endselect — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t113 复核：别名=endbutton（同一实现），选择块完整链（t110 已核）。
  - evidence：scripts/kag/commands/text.lua:1490-1492（return TextCommands.endbutton）→ endbutton/_renderChoices → TextScene → backend.render_text
- eval — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t110 复核：双轨——主轨 scheduler 内联（flow-inline，表达式求值入 ctx.tf.eval_result 可观察）；handler 为 strict 兜底（sandbox.execute + rawset(ctx.tf,...)——t110 判据边缘形态，人工判真伪）。
  - evidence：scripts/scheduler.lua:4/30/97-109（inline 主轨，"eval"=true）；scripts/kag/commands/system.lua:179-224（strict 兜底，rawset(ctx.tf,...)）
- fadeout — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t119 复核：模块委托 Layer.layfade（opacity 0..1→0..255 换算注记）→层透明度动画
  - evidence：scripts/kag.lua:371-385 → scripts/kag/commands/layer.lua layfade
- flash — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t116 复核：模块表委托 VFX.flash→backend.create_solid_texture+__flash 层，阻塞全屏闪白
  - evidence：scripts/kag/commands/vfx.lua:368-370 → scripts/vfx.lua:266-320 VFX.flash（backend.create_solid_texture :314 + __flash 层 :286-292, z=9998）
- gallery — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：模块委托链 gallery.lua show→backend.set_input_focus/get_resolution+layers.ensure 覆盖层，UI 全链路通过
  - evidence：scripts/kag/commands/system.lua:333 → scripts/gallery.lua:97 show → :133 backend.set_input_focus(GAME) / :135 backend.get_resolution / :143 layers.ensure(_gallery_overlay,95)
- history — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t116 复核：HistoryUI.show 模块调用→backlog 覆盖层（层系统+backend 渲染），真实可观察
  - evidence：scripts/kag/commands/system.lua:231-241 → scripts/history_ui.lua :19 backend.create_solid_texture / :27-31 layers.get / :86-87 layers.ensure(_history_bg/_history_title)+注释 :9 自证 backend.render_text；jump→ctx._pendingJump
- hr — Observable=VERIFIED · PlatformTested=win · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t213 接线（拍板 A）：kag.lua:217 空桩改委托 text_cmds.hr（保留注册键=Dispatched 依据，t195 死定义纪律同款）；TextCommands.hr（text.lua:929）绘制 _hr 层（layers.ensure+create_solid_texture+mark_dirty，viewport.logical 宽×resolve_line_height，x=32 y=cursor w=vw-64 h=2 实心条）+ 光标推进一行 + textCursorX=32；_hideHr 清理挂点 5 处（[ch]/[text]/[er]/[p]/[reset]，页清则线清）。 [035 ②+③ 2026-09-04] platform_tested=win：真实 D3D11 GPU 冒烟（build/Debug --backend dx11），palette=PostFx Lut3D create tex=5 intensity=0.80 + set 0.50 + clear(tex=65535)，脚本 token 7/7 全消费（hr/typewriter 执行）；packaged 保持 -（本批开发位，无对应发布包）。
  - evidence：scripts/kag.lua:216-218（桩改委托）→ scripts/kag/commands/text.lua:929+（TextCommands.hr/_hideHr）→ tests/scripts/test_hr.lua（27 断言：dispatch 身份 KAG.hr@kag.lua:217→TextCmds.hr@text.lua:929/裸 ctx 兼容/parse/绘制触发/光标推进/复用同节点/五路清理）
- i18n — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t110 PARTIAL 复核批人工核真：handler 经 i18n.set_language + kt.relocalize_page 全页重放，间接但真实触达渲染效果面（TextScene draws→backend.render_text），画面即时换语言可观察。
  - evidence：scripts/kag/commands/system.lua:685-712（contract+schema；i18n.set_language + kt.relocalize_page）；scripts/kag/commands/text.lua relocalize_page（全页重放）→ scripts/kag/text_scene.lua draws → 渲染循环 backend.render_text
- inc — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t188 批3a §inc：nil-safe 增量（by 参数）；test_variables.lua + test_math_cmds.lua（inc/dec 孪生）。
  - evidence：scripts/kag/commands/system.lua:471-483 -> ctx scope; 测试 test_variables.lua/test_math_cmds.lua
- l — Observable=VERIFIED（位置级） · PlatformTested=- · Packaged=-
  - reason：t117 复核：行断语义=cursor 状态（textCursorY/X + text_scene cursor + update_text_state(l)）→渲染循环消费；位置级与 letter_spacing 同类（像素级待 M4）
  - evidence：scripts/kag/commands/text.lua:906（ctx.textCursorY/X + TextScene.get_state().cursor_x/y + update_text_state(l)）→ scripts/kag/text_scene.lua:42-43/65-66（渲染循环 cursor 消费）
- layout — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t113 复核：handler 调用同文件工具函数 apply_container（:126-137 layers.move_layer 真实移动图层）——扫描器漏检形态（handler 体外本地函数触达），人工判真伪 VERIFIED。
  - evidence：scripts/kag/commands/layout.lua:169-199（handler）→ :126-137 apply_container（layers.move_layer）+ :118-123 recompute（math2.measure 槽位）
- layout_slot — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t113 复核：同链——槽位注册→recompute+apply_container→layers.move_layer，真实图层重排。
  - evidence：scripts/kag/commands/layout.lua:204-238 → :126-137 apply_container（layers.move_layer）
- live2d_expression — Observable=? · PlatformTested=- · Packaged=- · Status=EXPERIMENTAL (raw: PARTIAL)
  - reason：t119 判级：写 ctx.live2d[model].expression 状态；LIVE2D=OFF 无消费+Tested=0——feature-gated
  - evidence：scripts/kag/commands/character.lua:189-197
- live2d_lip_sync — Observable=? · PlatformTested=- · Packaged=- · Status=EXPERIMENTAL (raw: PARTIAL)
  - reason：t119 判级：写 ctx.live2d[model].lip_sync 状态；LIVE2D=OFF 无消费+Tested=0——feature-gated
  - evidence：scripts/kag/commands/character.lua:200-207
- live2d_motion — Observable=? · PlatformTested=- · Packaged=- · Status=EXPERIMENTAL (raw: PARTIAL)
  - reason：t119 判级：handler 仅写 ctx.live2d[model].current_motion 状态；本构建 CAESURA_LIVE2D=OFF（NullAnimation）无消费方+Tested=0——feature-gated
  - evidence：scripts/kag/commands/character.lua:177-186
- loadplace — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：状态-流链——ctx._pendingJump+stop_flag→runner 跳转路径（bookmark 恢复可观察；test_flow_edge_call.lua:363-381 有覆盖）
  - evidence：scripts/kag/commands/save.lua:554-556 → scripts/system.lua:334-345 loadplace（ctx._pendingJump={scene,index}+ctx.stop_flag=true）→ runner 跳转
- mod — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t181 核真：binop 驱动器 math.lua:83-119（resolve_var 五作用域+nil-safe 起始 0+div/mod 零除可视错误+no-op）；handler :121-141；效果面=变量状态写。
  - evidence：scripts/kag/commands/math.lua:83-119（handler :121-141）-> ctx.{f,sf,tf,mp,lf}；tests/scripts/test_math_cmds.lua:40-127
- mul — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t181 核真：binop 驱动器 math.lua:83-119（resolve_var 五作用域+nil-safe 起始 0+div/mod 零除可视错误+no-op）；handler :121-141；效果面=变量状态写。
  - evidence：scripts/kag/commands/math.lua:83-119（handler :121-141）-> ctx.{f,sf,tf,mp,lf}；tests/scripts/test_math_cmds.lua:40-127
- music — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t119 复核：模块委托 music_room.show（UI 全链：solid texture/render_text/get_resolution）
  - evidence：scripts/kag/commands/system.lua:343-345 → scripts/music_room.lua:21 create_solid_texture + :168 render_text + :172 get_resolution
- nameplate — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：同文件私有工具函数 _renderNameplate 内 layers./backend. 直呼——_ 前缀私有函数调用图盲区（v4 B 类）
  - evidence：scripts/kag/commands/text.lua:418（handler）→ :431 _renderNameplate（layers.ensure(_nameplate,3)+backend.create_solid_texture+backend.render_text+layers.mark_dirty）
- notify — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t110 复核：toast.show 模块表调用，toast 模块内 backend.render_text + create_solid_texture + _toast_bg layer——间接真实触达（角标 toast 可观察）。
  - evidence：scripts/kag/commands/system.lua:648-677 → toast.show → scripts/toast.lua:41 backend.render_text + :14 create_solid_texture + :36 _toast_bg layer
- palette — Observable=VERIFIED · PlatformTested=win · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t214 实现：Lut3D postfx stage（PostFxKind::Lut3D=4 + PostFxParams 纹理/lutSize 两字段，零新方法）+ BgfxRenderDevice 借纹理 uS1 采样 + 着色器×3 家族（DXBC fxc 本地编译嵌入；GL/Metal shaderc 数组生成命令已附）+ palette.lua 真名面重写（load_texture/is_valid_handle(0,h)/set_postfx('lut3d')，旧幻影 set_palette/load_image/is_valid 移除）+ web 桥统一原生面。D3D11 GPU 冒烟 300 帧：PostFxLut3D program READY + create tex=2 size=16 intensity=0.50，0 渲染错误，RC=0。 [035 ②+③ 2026-09-04] platform_tested=win：真实 D3D11 GPU 冒烟（build/Debug --backend dx11），palette=PostFx Lut3D create tex=5 intensity=0.80 + set 0.50 + clear(tex=65535)，脚本 token 7/7 全消费（hr/typewriter 执行）；packaged 保持 -（本批开发位，无对应发布包）。
  - evidence：src/render/api/IRenderDevice.h（PostFxKind::Lut3D=4/PostFxParams 两字段）+ src/render/BgfxRenderDevice.cpp(:240-246 runPostFxChain Lut3D 分支) + shaders/dx11/fs_postfx_lut3d.{hlsl,dxbc} + src/script/bindings/RenderBinding.cpp（lutId/lutSize 推导）+ scripts/palette.lua（真名面）+ scripts/kag/commands/vfx.lua:451+（handler）→ tests/scripts/test_palette.lua（26 断言）+ tests/cpp/test_render_postfx.cpp（9 用例/40 断言）
- play — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：模块委托链 kag.commands.audio playbgm/playse/playvoice→backend.audio_*
  - evidence：scripts/kag.lua:467-500 play（路由到 audio 模块）→ scripts/kag/commands/audio.lua playbgm:109/playse/playvoice → backend.audio_play
- playstop — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：模块委托链 audio.stopbgm→backend
  - evidence：scripts/kag.lua:427-432 playstop（→ audio.stopbgm）→ scripts/kag/commands/audio.lua stopbgm → backend.audio_stop
- postprocess — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：同文件工具函数 apply_postfx→backend.set_postfx/clear_postfx/is_postfx_supported——GPU postfx 可观察（同文件工具链盲区；ctx._postfx bookkeeping NOT-WIRED 注记属持久化缺口）
  - evidence：scripts/kag/commands/vfx.lua:507-515 → :230-271 apply_postfx（backend.set_postfx :258 / clear_postfx :232 / is_postfx_supported :236）
- preload — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t116 复核：同文件工具链——load_texture/load_audio 内 backend 直呼（真实资源预加载+占位符），扫描器漏检=同文件工具函数（局限8）
  - evidence：scripts/kag/commands/resource.lua:154-217（handler）→ :78-107 load_texture（backend.load_texture :97 / backend.load_texture_async :85）+ :113-131 load_audio（backend.audio_play/audio_stop :120/123）+ scene→flow.load_scene :201
- pt — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t116 复核：ctx.text_speed 状态写+注释自证消费点（kag_runner 每帧读取推进 reveal），textspeed 同款模式
  - evidence：scripts/kag/commands/text.lua:1152-1154（ctx.text_speed=params.speed）+ :1156-1170（注释自证 kag_runner.lua 消费）
- quake — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：模块委托 vfx.quake（修正绑定注记：standalone [quake] 不再跑 shake）
  - evidence：scripts/kag.lua:421-424 → scripts/kag/commands/vfx.lua:376-378 → scripts/vfx.lua:28 VFX.quake
- r — Observable=VERIFIED（位置级） · PlatformTested=- · Packaged=-
  - reason：t119 复核：别名链 KAG.r=KAG.l or KAG.br → l/br（位置级）
  - evidence：scripts/kag.lua:288（KAG.r = KAG.l or KAG.br）→ l/br 位置级链
- random — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t188 批3a §random：整数 floor 范围随机写变量（:486 死定义已随 t192 删除，:511 有效）；test_variables.lua/契约套件。
  - evidence：scripts/kag/commands/system.lua:511-528 -> ctx scope; 测试 test_variables.lua
- replay — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：模块委托链 replay.load→state.mode=playback→kag_runner replay.tick 回放推进（可观察）
  - evidence：scripts/kag/commands/system.lua:579 → scripts/replay.lua:140 load（mode=playback）→ scripts/kag_runner.lua:441/443 replay.tick(delta_ms, click_cb)
- rollback — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t113 复核：模块链——kag_runner.rollback() token 级快照弹出+重跑（下一次渲染反映，可观察倒带；blocking=true 契约）。
  - evidence：scripts/kag/commands/system.lua:389-397 → scripts/kag_runner.rollback()（快照恢复+重跑）
- ruby — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t116 复核：TextScene.add_ruby→draws→render 循环 backend.render_ruby，注音标注可观察
  - evidence：scripts/kag/commands/text.lua:1034-1059 → scripts/kag/text_scene.lua:179-214 add_ruby + :250-253 render_backend.render_ruby
- s — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t116 复核：别名链=wait 实现（KAG3 s 字符快捷等待默认 250ms），delay 同款
  - evidence：scripts/kag.lua:303-308（require kag.commands.system.wait, ms 默认 250）→ scripts/kag/commands/system.lua:50-83 wait
- saveload — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t116 复核：saveload_menu.show UI 覆盖层→结果 save/load 真实读写；headless pcall 降级注记
  - evidence：scripts/kag/commands/save.lua:495-521（:498-511 headless 降级；:512 SaveLoad.show；:515-517 SaveCommands.save/load）
- saveplace — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t182 核真（case d）：save.lua:550-552 -> System.saveplace（system.lua:313-331 内存书签 scene+token_index+tf 深拷贝）-> loadplace :334-361 经 _pendingJump 真恢复（+layers.restore_text_state :352-357）；测试 test_flow_edge_call I1 :363-385 / I2 :391-401（round-74 边界）。
  - evidence：scripts/kag/commands/save.lua:550-552 -> scripts/system.lua:313-361 -> scripts/kag_runner.lua jump 路径
- select — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t110 复核：语法糖组合——select no-op 开块（契约 blocking=false 设计如此），sel=button、endselect=endbutton 别名赋值，选择块完整语义=button/endbutton 链。
  - evidence：scripts/kag/commands/text.lua:1484-1486（开块）+ :1488 sel=button + :1491 endselect=endbutton → button/endbutton 链（见 button 条目）
- set — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t188 批3a §set：resolve_var 五作用域 + infer_value 类型推断（纯状态写）；test_variables.lua 13 checks。
  - evidence：scripts/kag/commands/system.lua:453-465 -> ctx.{f,sf,tf,mp,lf}; 测试 test_variables.lua
- shake — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：模块委托 vfx.shake→VFX.shake（层动画）
  - evidence：scripts/kag.lua:417-420 → scripts/kag/commands/vfx.lua:372-374 → scripts/vfx.lua:82 VFX.shake
- skip — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t113 复核：ctx.skip_mode 状态写 + kag_runner 明确消费点（auto-advance/seen-skip）——状态写+消费点模式（textspeed 同款）。
  - evidence：scripts/kag/commands/text.lua:1081-1097（ctx.skip_mode 切换，seen off-toggle 审计修复注记）→ scripts/kag_runner.lua:467/482-483（消费）
- sma_anim — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：sma.play_anim→actor 状态→sma.render（binding().draw_mesh）模块内状态-消费闭环；测试 0 引用但消费链真实
  - evidence：scripts/kag/sma.lua:723-731 → :601 sma.play_anim → :540-556 sma.render（binding().draw_mesh，t117 已核）
- sma_ik — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：sma.set_ik→actor IK 字段→sma.render 消费（绑定接口渲染）
  - evidence：scripts/kag/sma.lua:733-739 → :630 sma.set_ik → sma.render:540-556 消费
- sma_play — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t117 复核：绑定接口链 sma.spawn→sma.render binding().draw_mesh（GPU 网格渲染）；扫描器盲区 D 类（binding. 非 backend./layers. token）
  - evidence：scripts/kag/sma.lua:712 sma_play → :392 sma.spawn（ctx.sma_actors 状态）→ :540-556 sma.render binding().draw_mesh(handle,view,texId,...)
- sma_stop — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：sma.despawn 内 binding().destroy_mesh 绑定接口直呼——网格销毁可观察（绑定接口链盲区）
  - evidence：scripts/kag/sma.lua:747-749 → :442-452 sma.despawn（binding().destroy_mesh :448-449）
- sma_variant — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：sma.set_variant→actor variant/tex→sma.render 消费
  - evidence：scripts/kag/sma.lua:741-745 → :652 sma.set_variant → sma.render:540-556 消费
- sub — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t181 核真：binop 驱动器 math.lua:83-119（resolve_var 五作用域+nil-safe 起始 0+div/mod 零除可视错误+no-op）；handler :121-141；效果面=变量状态写。
  - evidence：scripts/kag/commands/math.lua:83-119（handler :121-141）-> ctx.{f,sf,tf,mp,lf}；tests/scripts/test_math_cmds.lua:40-127
- text — Observable=VERIFIED · PlatformTested=- · Packaged=- · Status=CLOSED (raw: CLOSED)
  - reason：t181 核真（维持 CLOSED）：效果链真实——kag_runner.lua:670 每帧 kag.text_scene.render -> text_scene.lua:267/291 backend.render_text/render_ruby（KAGBinding.cpp:77-78 真实绑定）；测试 handle 语义（test_textflow:15-66 等）。
  - evidence：scripts/kag_runner.lua:670 -> scripts/kag/text_scene.lua:267/291 -> src/script/bindings/KAGBinding.cpp:77-78
- textspeed — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t110 复核：apply_text_cps 写 ctx.text_speed（注释自证 real read point kag_runner），kag_runner.update 揭示速率消费——状态写+明确消费点，字符揭示速度变化可观察。
  - evidence：scripts/kag/commands/text.lua:1214-1216（handler）→ :1173-1198 apply_text_cps（ctx.text_speed=floor(1000/cps)）→ scripts/kag_runner.lua update()（揭示速率消费，reference :446-455）
- tween — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t113 复核：工具函数链——resolve_layer/step_tween/apply_step 内 layers.move_layer/set_layer_opacity/mark_dirty，真实图层属性动画（blocking Operation；wait=false 由 kag_runner.update 驱动）。
  - evidence：scripts/kag/commands/tween.lua:201-254（handler）→ :58-62 resolve_layer（layers.get/find）+ :73-83 step_tween/apply_step（layers.move_layer/set_layer_opacity/mark_dirty）+ :165-186 update 驱动
- typewriter — Observable=VERIFIED · PlatformTested=win · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t201 接线（B 批）：逐字揭示唯一消费点 kag_runner.lua update() 内新增 SE 触发（interval 跨边界语义：last_shown=上次触发边界，1 char/帧下 interval=N 仍每 N 字符触发一次——首版实现按帧更新 last_shown 导致 interval>=2 永不触发，t201 实测修正）；[ch]/[text] reveal 初始化含 last_shown=0（新行重置）；skip/click 即时路径在块外写 last_shown=total（瞬时揭示零爆发，防 follow-through 连响）；snapshot restore 封 last_shown=total（回滚零爆发）。v1 诚实注记：volume 无 per-SE 消费面（IAudioBackend.playSE 无 volume 参数，src/audio/api/IAudioBackend.h:38），按 plan 忽略并列入 follow-up（per-SE 音量=接口扩展，契约层）。 [035 ②+③ 2026-09-04] platform_tested=win：真实 D3D11 GPU 冒烟（build/Debug --backend dx11），palette=PostFx Lut3D create tex=5 intensity=0.80 + set 0.50 + clear(tex=65535)，脚本 token 7/7 全消费（hr/typewriter 执行）；packaged 保持 -（本批开发位，无对应发布包）。
  - evidence：scripts/kag_runner.lua:489-505（reveal 推进内间隔触发 backend.audio_play('se', sound)）+ :770-779（click 即时写 last_shown=total）+ :536-542（skip 即时同款）；scripts/kag/commands/text.lua:830/898（reveal={...,last_shown=0} 新行重置）；scripts/kag/snapshot.lua:82-88（restore 封印）；tests/scripts/test_typewriter_sound.lua（语义测试 38 断言：interval=1/3、skip、click 即时+封印、action=off、新行重置、alias 场景）
- typewriter_sound — Observable=? · PlatformTested=- · Packaged=- · Status=CLOSED (raw: PARTIAL)
  - reason：t201 接线（B 批）：与 typewriter 共享 handler（text.lua:1290 别名）与消费字段 ctx.typewriter_sound/_interval——kag_runner reveal 推进处消费（见 typewriter 条目）；t119 的 WRITE-ONLY 判级证据已被本批接线取代；v1 不含 per-SE volume（contract gap，follow-up 记录）。
  - evidence：scripts/kag/commands/text.lua:1290（TextCommands.typewriter_sound = TextCommands.typewriter）→ :1273-1289 写 ctx.typewriter_sound/_interval → scripts/kag_runner.lua:489-505 消费（backend.audio_play('se', sound)）；tests/scripts/test_typewriter_sound.lua case 8（[typewriter_sound] 场景 2 字符→2 SE+文件参数 s.ogg 正确）
- unlock — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t188 批3a §unlock：unlockedCG/unlockedMusic 写 + gallery.lua:51-103 消费 + save 持久化；test_unlock.lua 9 checks。
  - evidence：scripts/kag/commands/system.lua:399-413 -> scripts/gallery.lua:51-103 + scripts/music_room.lua（持久化经 save.lua）；测试 test_unlock.lua
- vibrate — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t113 复核：委托链——trans.vib（transition.lua:455-476 layers.get_layer(message)+mark_dirty 消息层抖动）+ blocking 300ms。
  - evidence：scripts/kag/commands/vfx.lua:495-500 → kag.commands.transition trans.vib（transition.lua:455-476）
- voice — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：模块委托 audio.playvoice（schema.coerce play 迁移条件注记）→backend.audio_play（voice 轨）
  - evidence：scripts/kag.lua:433-440 → scripts/kag/commands/audio.lua playvoice → backend.audio_play
- voice_off — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t189 批3b §voice_off：ctx.voice_muted -> audio.lua:243 播放门控 + save.lua:168/375 持久化；test_contracts_runtime.lua 语义。
  - evidence：scripts/kag/commands/text.lua:1126 -> scripts/audio.lua:243 + scripts/kag/commands/save.lua:168/375; 测试 test_contracts_runtime.lua
- voice_wait — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t119 复核：模块委托 audio.voice_wait（CLOSED 链）→等待语音完成+点击跳过
  - evidence：scripts/kag.lua:297-299 → scripts/kag/commands/audio.lua voice_wait
- wait — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t188 批3a §wait：Operation/CancelToken + scheduler-dt 逐帧 yield（stop_flag/_next_index 提前退出；ms 0/60000 守卫）；效果=暂停推进（消费=kag_runner resume）——悬浮控制型纯 Lua。
  - evidence：scripts/kag/commands/system.lua:50-84 -> scripts/kag/operation.lua + scripts/scheduler.lua（帧 dt）; 测试 tests/scripts/test_wait_delay.lua（27 checks）
- waitclick — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t188 批3a §waitclick：ctx.waiting_input -> runner 点击恢复；test_waitclick.lua 9 checks。
  - evidence：scripts/kag.lua:312-318 -> scripts/kag_runner.lua（点击流 waiting_input）; 测试 test_waitclick.lua
- waitforclick — Observable=VERIFIED · PlatformTested=- · Packaged=-
  - reason：t188 批3a §waitforclick：waiting_input while-loop 重建（与 waitclick 同机制）；test_contract_runtime_gaps.lua + test_kag3_compat.lua。
  - evidence：scripts/kag.lua:388-396 -> scripts/kag_runner.lua; 测试 test_contract_runtime_gaps.lua/test_kag3_compat.lua

## 人工判级（范围外能力）

| 能力 | 判级 | 证据 | 备注 |
|---|---|---|---|
| letter_spacing | VERIFIED（位置级） | t105 接线：scripts/kag/text_scene.lua add_wrapped_spans 逐字符 draw（commit 33fb6b19）+ tests/scripts/test_text_markup.lua 9h/9i 共 9 断言（x 序列 100/116、段边界 158、多字节 127、typewriter×spacing） | 位置级已按布局口径（measure_character：rawWidth*scale+spacing）精确复现；像素级视觉待 M4 GPU 冒烟确认。布局级（wrap/measure）t101 已核真。 |
| SwipeDown | WIRED | native 消费方已接（commit 4c074c7b：Engine 钩子 _KAG_onKeySpace + scripts/kag_demo_entry.lua 实装 + InputRouter/MobileAdapter 接线；测试 +6：test_history.lua/test_sandbox.lua） | 缺省钩子已下沉 kag 运行时（t125/t130 M-F1）：scripts/kag.lua:553-567 defaultKeySpace（handler 级 pcall require layers + type 守卫，:561-562；layers.get(message) 可见性切换 :563-566）经 KAG.gesture_defaults 导出（:586-589）+ 全局安装 _KAG_onKeySpace（:591-592，first-definition-wins），Engine 钩子路由 SDLK_SPACE；kag_runner 每帧 overlay pump（t127 M-F2，kag_runner.lua:467-481 驱动 ctx._gesture_history_co 死槽清除）。入口覆写优先（kag_demo_entry.lua 为官方覆写示例）——native 缺省行为与 web player 层全局手势（web/main.mjs:634-647）平权；真机手势 E2E 仍 hardware-gated（M4）；per-game opt-in 语义已由运行时缺省取代。 |
| SwipeUp | WIRED | native 消费方已接（commit 4c074c7b：Engine 钩子 _KAG_onKeyPageUp + scripts/kag_demo_entry.lua 实装 + InputRouter/MobileAdapter 接线；测试 +6） | 缺省钩子已下沉 kag 运行时（t125/t130 M-F1）：scripts/kag.lua:569-575 defaultKeyPageUp（input_focus 守卫 + ctx._gesture_history_co 单飞协程 :571-574 + kag.commands.system.history）+ KAG.gesture_defaults:586-589 + _KAG_onKeyPageUp 全局安装 :591-592（first-definition-wins），Engine 钩子路由 SDLK_PAGEUP；kag_runner overlay pump（kag_runner.lua:467-481）。入口覆写优先（kag_demo_entry.lua 官方示例）——与 web player 层（main.mjs:634-647）平权；真机 E2E hardware-gated（M4）。 |
| LongPress | VERIFIED | t101 复核 file:line 链（SDL_EVENT_FINGER_DOWN 长按检测 → InputRouter/驱动消费；详见 t101 task output） | t101 全链证据在案；native 手势链已接。 |
| Pinch | VERIFIED | t101 复核 file:line 链（双指缩放 → 驾驶消费；详见 t101 task output） | t101 全链证据在案。 |
| TwoFingerTap | VERIFIED | t101 复核 file:line 链（详见 t101 task output） | t101 全链证据在案。 |
| ThreeFingerHold | VERIFIED | t101 复核 file:line 链（详见 t101 task output） | t101 全链证据在案。 |

## EXPERIMENTAL

- live2d_expression - 人工覆盖状态 EXPERIMENTAL（能力存在但无消费方/无真实测试面）；机器判级：PARTIAL（合约内）
  - reason：t119 判级：写 ctx.live2d[model].expression 状态；LIVE2D=OFF 无消费+Tested=0——feature-gated
  - evidence：scripts/kag/commands/character.lua:189-197
  - note：同 live2d_motion（M4 特性矩阵 Live2D 行）。
- live2d_lip_sync - 人工覆盖状态 EXPERIMENTAL（能力存在但无消费方/无真实测试面）；机器判级：PARTIAL（合约内）
  - reason：t119 判级：写 ctx.live2d[model].lip_sync 状态；LIVE2D=OFF 无消费+Tested=0——feature-gated
  - evidence：scripts/kag/commands/character.lua:200-207
  - note：同 live2d_motion（M4 特性矩阵 Live2D 行）。
- live2d_motion - 人工覆盖状态 EXPERIMENTAL（能力存在但无消费方/无真实测试面）；机器判级：PARTIAL（合约内）
  - reason：t119 判级：handler 仅写 ctx.live2d[model].current_motion 状态；本构建 CAESURA_LIVE2D=OFF（NullAnimation）无消费方+Tested=0——feature-gated
  - evidence：scripts/kag/commands/character.lua:177-186
  - note：CAESURA_LIVE2D=ON 构建下由 Live2D 后端消费并按 M4 特性矩阵重判。

## 可疑翻转清单（v4 保守维持；含队长裁决）

- endbutton — v3 PARTIAL → v4 CLOSED；v4 判据翻 CLOSED 但非人工已核 VERIFIED——保守维持 v3，待裁决
- hr — v3 PARTIAL → v4 CLOSED；v4 判据翻 CLOSED 但非人工已核 VERIFIED——保守维持 v3，待裁决
- stopvideo — v3 PARTIAL → v4 CLOSED；v4 判据翻 CLOSED 但非人工已核 VERIFIED——保守维持 v3，待裁决

## 幻影绑定（v5）

> v5 判据：backend.<name> 命中若 name 不在真实绑定面（bindings/*.cpp luaL_Reg + backend.lua shim def + backend_factory cmd 分派 + kag.lua KAG def 的并集，见上方「幻影绑定（v5）」提取模式）——该命中为**幻影**：从 Consumed 证据中剔除，并在本节列出 file:line。过时的宿主侧（web jsBackend）提供同名项不改变判定：原生绑定面以本机 src/script/bindings 与 scripts/ 为唯一口径。

> web/jsBackend 同步存在（原生缺失，web 桥提供）：render_frame

| 命令 | 幻影名 | file:line |
|---|---|---|
| trans | render_frame | scripts/kag/commands/transition.lua:307 |
| trans | render_frame | scripts/kag/commands/transition.lua:346 |

## 版本翻转（v4→v5 幻影过滤）

（无）

## 可疑翻转清单（v5 人类级保守维持）

（无）

## UNWIRED

（无）

## PARTIAL

- blur - scripts/kag/commands/transition.lua:292；处理器体未以调用形触达效果面（v4 一跳穿透下仍无命中；注释/字符串已剥离）
- chapter - scripts/kag/commands/system.lua:323；处理器体未以调用形触达效果面（v4 一跳穿透下仍无命中；注释/字符串已剥离）
- flash - scripts/kag/commands/vfx.lua:382；处理器体未以调用形触达效果面（v4 一跳穿透下仍无命中；注释/字符串已剥离）
- stopvideo - scripts/kag/commands/video.lua:123；处理器体未以调用形触达效果面（v4 一跳穿透下仍无命中；注释/字符串已剥离）

## EXTRA

- Bezier - scripts/kag/commands/transition.lua:606；B:api-helper-export（已注册但无合约条目）
- LUTCache - scripts/kag/commands/transition.lua:605；B:api-helper-export（已注册但无合约条目）
- call - scripts/kag.lua:514；A:user-command-missing-contract（已注册但无合约条目）
- capture_state - scripts/kag/commands/save.lua:250；B:api-helper-export（已注册但无合约条目）
- clear - scripts/kag.lua:351；A:user-command-missing-contract（已注册但无合约条目）
- clearscreen - scripts/kag.lua:209；A:user-command-missing-contract（已注册但无合约条目）
- ct - scripts/kag.lua:354；A:user-command-missing-contract（已注册但无合约条目）
- end - scripts/kag.lua:86；A:user-command-missing-contract（已注册但无合约条目）
- endform - scripts/kag.lua:362；A:user-command-missing-contract（已注册但无合约条目）
- endmacro - scripts/kag.lua:244；A:user-command-missing-contract（已注册但无合约条目）
- endtag - scripts/kag.lua:358；A:user-command-missing-contract（已注册但无合约条目）
- erasemacro - scripts/kag.lua:245；A:user-command-missing-contract（已注册但无合约条目）
- flush_cache - scripts/kag/commands/resource.lua:254；B:api-helper-export（已注册但无合约条目）
- g - scripts/kag.lua:365；A:user-command-missing-contract（已注册但无合约条目）
- get_texture - scripts/kag/commands/resource.lua:226；B:api-helper-export（已注册但无合约条目）
- has_pending_transition - scripts/kag/commands/resource.lua:308；B:api-helper-export（已注册但无合约条目）
- is_loaded - scripts/kag/commands/resource.lua:235；B:api-helper-export（已注册但无合约条目）
- is_pending - scripts/kag/commands/resource.lua:244；B:api-helper-export（已注册但无合约条目）
- jump - scripts/kag.lua:503；A:user-command-missing-contract（已注册但无合约条目）
- macro - scripts/kag.lua:243；A:user-command-missing-contract（已注册但无合约条目）
- preload_transition - scripts/kag/commands/resource.lua:280；B:api-helper-export（已注册但无合约条目）
- prepare_load - scripts/kag/commands/save.lua:376；B:api-helper-export（已注册但无合约条目）
- promote_transition_slot - scripts/kag/commands/resource.lua:294；B:api-helper-export（已注册但无合约条目）
- push_backlog - scripts/kag/commands/text.lua:334；B:api-helper-export（已注册但无合约条目）
- relocalize_backlog - scripts/kag/commands/text.lua:1589；B:api-helper-export（已注册但无合约条目）
- relocalize_page - scripts/kag/commands/text.lua:1647；B:api-helper-export（已注册但无合约条目）
- return_to_caller - scripts/kag.lua:530；A:user-command-missing-contract（已注册但无合约条目）
- se - scripts/kag.lua:445；A:user-command-missing-contract（已注册但无合约条目）
- sel - scripts/kag/commands/text.lua:1564；B:api-helper-export（已注册但无合约条目）
- showtext - scripts/kag.lua:206；A:user-command-missing-contract（已注册但无合约条目）
- update - scripts/kag/commands/tween.lua:165；B:api-helper-export（已注册但无合约条目）
- wait_click - scripts/kag.lua:325；A:user-command-missing-contract（已注册但无合约条目）

## 私有辅助（_ 前缀，注册但不属命令面）

- _clear_runtime_state - vfx.lua:605
- _hideHr - text.lua:951
- _postfx - vfx.lua:287
- _relocalizeCC - text.lua:1630
- _relocalizeChoices - text.lua:1605
- _renderNameplate - text.lua:432
- _safeScenePath - save.lua:35

## 数据与判级局限（v5）

9. **v5 幻影绑定过滤**：Consumed 的 backend.* 命中按名对照真实绑定面（bindings/*.cpp luaL_Reg + backend.lua shim def + backend_factory cmd 分派 + kag.lua KAG def 的并集，动态提取）校验；不在面上的命中为幻影，从 Consumed 证据剔除并在『幻影绑定（v5）』列 file:line。幻影命中可能因注释剥离/多跳链（>1 跳穿透）漏检——只按已捕获的调用形上下文判定；web/jsBackend 同名项仅作交叉报告，不改变原生判定。
1. **Consumed 为调用形文本启发式**：注释与字符串字面量先被剥离（strip_lua 状态机），要求 backend./layers./kag. 的 <ident>( 调用形，或 ctx.tf. / ctx.tf[ / ctx.tf= 字段/赋值。仍可能低估（经本地别名或工具函数间接调用时本体不含直接调用；如 palette 命令经 palette 模块间接生效——如实归 PARTIAL），也可能高估（kag. 自派发计入）。脚本不执行 Lua，无法做数据流分析。
2. **Dispatched 为静态解析**：commands 导出表函数/赋值键 + kag.lua 显式映射与 function KAG.x 定义 + sma_commands 子表。jump/call/endmacro 等直接 API 计入 EXTRA(api-alias)；[jump]/[if] 等 token 的流控处理由 scheduler.lua 编译期内联；两条轨道并存，本扫描器按注册键计 Dispatched。
3. **Tested 为原始引用计数**：tests/scripts/*.lua 与 web/*.test.js 中 [<name> 或 kag.<name> 出现次数，不区分断言与非断言上下文（注释/数组/字符串也算）。
4. **Observable / Platform / Packaged 协议化（t197）**：Observable 可由 overrides JSON 人工覆盖（⚠ 标记）；Platform（platform_tested）/ Packaged（packaged）现为协议值（默认 '-'=无证据；platform_tested 为 android,ios,linux,macos,web,win 逗号分隔去重排序子集，packaged 为 <=120 字符描述），非协议值由扫描器自动迁移为 '-'（诚实未验证）；平台运行矩阵/打包验证由 Phase2 分发逐项补证。
5. 判级只依赖命令名静态匹配；同名异构（如 vfx 的 flash 与 transition 的 flash）以注册表实际键为准。导出表引用的子表（如 TransCommands.Bezier = Bezier）经 pairs() 一并注册为调度键——EXTRA(subtable-key)，非用户命令面。
6. 合约计数以 command-contracts.md 的 ### 条目数为准（表头标注 134 须一致）。
7. overrides JSON 的 commands 键必须落在已知命令名集合内；未知键被响亮拒绝（exit 非 0），绝不静默忽略。
8. **v4 已修复（历史注记保留）**：v3 判据只扫 handler 直接体——同文件工具函数/委托链内的效果面调用（t110-t119 五批人工核真 18+ 例：layout/layout_slot/tween/vibrate/nameplate 的工具函数链、模块表委托 toast.show/VFX.flash/HistoryUI.show 等）不被捕获；v4 一跳穿透（同文件 local + require()d 模块函数）已覆盖该盲区。仍存在的判定噪声：跨两跳以上的链（工具函数再调工具函数）、绑定接口（binding().draw_mesh 类——sma_play 等经人工证据层覆盖）、rawset(ctx.tf, ...) 形态（判据边缘）。
10. **raw 口径（t185/t192 定稿）**：任何『raw/机器原判级』汇总一律以**记录级 status_machine** 为准（=overrides 人工裁决与 v7 类别应用之前的机器判级，永不丢弃）；status_counts_v4_raw/status_counts_v5_raw 为版本快照口径，仅作对账，不作最终判定依据。

## 复现

```
python scripts/capability_closure.py
```

