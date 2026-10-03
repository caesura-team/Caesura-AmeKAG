# 032 高级阶段方向（2026-09-03 用户反馈）

> 状态：方向记录 + 进度跟踪，非执行授权。任何执行批次按分步放行惯例等用户『执行』口令，或并入既有阶段计划。

<!-- plan-status:generated -->

## 事实状态（自动生成 — 勿手改）

- **closure: PARTIAL=5 / CLOSED=131 / UNWIRED=7 / EXTRA=33 / EXPERIMENTAL=2**
  - 证据：docs/design/capability-closure-matrix.md:16 (stats line)
- **Node migration: COMPLETE**
  - 证据：scripts/package_game.mjs; src/rpc/services/PackagingService.cpp findNode & no findGitBash; scripts/caesura_build.py find_node; scripts/package_distribution.py _find_node -- all satisfied
- **Unicode UTF-8 widening: COMPLETE**
  - 证据：external/lua/liolib.c contains lua_fopen_utf8; external/lua/lua.c contains wmain; external/lua/lauxlib.c contains lauxlib_fopen_utf8; external/lua/loadlib.c contains loadlib_fopen_utf8 -- all satisfied

<!-- /plan-status:generated -->



## 1. platform-status 语义命名修正 ✅ 已完成

- **反馈**：`platform-status.md` 的 `Repository HEAD Commit` 保存的是 evidence HEAD（最后一个非 docs 提交），而非真实仓库 HEAD——多数时候二者一致，但矩阵同步 commit 后必然漂移。第三方读者易误解，影响自动化审计口径。
- **处置（t178 已落地）**：标题改 `Evidence HEAD Commit`；JSON 键 `evidence_head_commit`；yaml 双键兼容（新键优先）；证据头解析 git→yaml 回退 + is-shallow 守卫。
- **遗留**：yaml 键名迁移（`head_commit` → `evidence_head_commit`）+ md 重生成（docs sync 批）。

## 2. PackagingService 的 Git Bash 依赖（产品级技术债）

- **现状（原始）**：Windows Web packaging = C++ → CreateProcessW(Git Bash) → package_game.sh → Lua/Node/Vite。CreateProcessW/显式解析/Unicode/handle list 已安全（round 32），但系统侧存在 Bash runtime 依赖。
- **风险**：`Caesura Studio → 一键 Web 发布` 要成为『普通用户不用管环境』的体验，必须先消除（开发环境无碍，产品分发是硬伤）。
- **目标轨迹（选定 ①，②为后续可选）**：
  1. ✅ `package_game.sh` 收敛为平台无关 CLI（t179：`scripts/package_game.mjs` Node ESM 唯一实现 + sh 7 行薄包装 + CLI 契约测试）；t180 进行中：PackagingService/caesura build CLI/package_distribution 全部切 Node（findGitBash→findNode），消除产品路径的 Bash 依赖。
  2. C++/Rust helper → deterministic packaging API（引擎内直接驱动，最终可二进制化，无 shell 中介）— 后续可选演进。
- **判定触发**：Phase2 分发接线（release 包 + Studio Build Manager 一键 Web）排期时，本项应已消除（t179/t180 完成即达）。

## 3. Capability Closure：PARTIAL 作战地图（阶段主线）

- **现状（t185 后）**：134 contracts / 93 CLOSED / **37 PARTIAL** / 0 UNWIRED / 31 EXTRA / 4 EXPERIMENTAL。UNWIRED=0 是阶段里程碑——最危险缺口已清零。
- **阶段策略**：停止扩展命令（029 feature 冻结不变），逐命令效果面闭环：
  1. 人工核真每个 PARTIAL 的当前真实效果面（file:line 证据 + 运行面对照，杜绝 token 启发式误判）；
  2. 输出缺口清单（处理器有/效果面断在哪一站——幻影绑定/守卫降级/半程链路分别留档）；
  3. 接线（P0-P2 既有能力接线不受 029 冻结限制）+ 每条带语义测试锁；
  4. 每批经对抗复核后更新 overrides 与矩阵（计数随证据走）。
- **进度**：
  - 批1+批2（首批十命令 text/button/select/i18n/math/history/ending/saveplace/fadeout/music）：t181/t182 核真 → t183 overrides 证据层 → t184 music 语义测试锁 + text.lua 注释修正 → t185 扫描器 v6（status 裁决层，PARTIAL 51→37）→ t187 对抗复核在飞。
  - 批3（其余 36 个 PARTIAL 只读核真）：t188 批3a（15 命令，15/15 建议 WIRED ✅）→ t189 批3b（文本展示型 16）/ t190 批3c（SMA 5）在飞。
  - 下一迭代：PAIRING_GROUPS/EXEMPT_PURE 机器判级改进（守护：白名单+幻影哨兵 exit 5；顺序=t187+批3 终态后）。
- **度量目标**：PARTIAL 逐批下降；每批落地以『真实效果面闭环 + 测试锁 + 矩阵计数同步』三合一验收。

## 4. 文档事实化（2026-09-03 用户反馈，已立项）

- **教训**：代码验证体系曾快于计划文档（032 的『37 PARTIAL / t180 进行中』在收敛后过时）——计划文档不再人工维护事实进度，由生成器从仓库事实生成。
- **落地**：`scripts/generate_plan_status.py` 生成 032 的『事实状态』段（marker 包围，静态事实=closure 计数/Node 迁移判据/Unicode 四站点判据）；`--check` 守卫（ci.yml 已接线）；platform-status 的 `last_updated`（yaml 人工旧值）→ `Generated At`（generator execution time），--check 对时间戳行归一化，不再作为验证依据。

## 5. 四层闭包产品化（方向——CLOSED 不再当完成度百分比）

- **认知**（2026-09-03 用户工程评价）：127 CLOSED ≠ 127 个命令全部真实运行时验证过——CLOSED 有一部分来自机器判级（v7 机制/人工 observable 证据），因此 Closure 必须分层：
  1. **Structural Closed**（结构闭包）=『代码架构证明能到效果面』——即现状 Status 机判（v7 三类豁免+机器 Consumed）；
  2. **Runtime Verified**（运行闭包）=『真实跑起来证明效果正确』——语义测试实测（现有 tests/ 证据，需协议化判据）；
  3. **Platform Verified**（平台闭包）=『Windows/Linux/Web/Android/... 都跑过』——overrides 的 platform_tested 字段（现多 ?，需按平台补充）；
  4. **Packaged**（发布闭包）=『最终 package 中也存在』——overrides 的 packaged 字段（现多 ?）。
- **目标形态**：矩阵每命令四层独立列/标记（每层有明确判据协议），计数表展示四层计数（诚实呈现，不求百分百），Status 列措辞明示=Structural Closed。
- **推进方式**：划入下一阶段主线（029 冻结期可做判据协议与逐列填充；Platform/Packaged 列随 Phase2 分发推进逐项真实验证），落地为独立派单批（先协议后填列）。
- **✅ 已产品化（2026-09-04 t197）**：矩阵列演进完成——`| Command | Declared | Dispatched | Consumed | Structural | Runtime | Platform | Packaged | Observable | 证据 |`；Status 并入 Structural、Tested 并入 Runtime（✔N=测试证据）；overrides 值域协议化（platform_tested=平台枚举或 `-`、packaged=产物标识或 `-`，非法 exit 2；现状 `?` 全部迁移为 `-`=无证据诚实）；四层计数行=Structural Closed 127 / Runtime 测试证据 137 / Platform 0 / Packaged 0（实测对账一致）；旧统计行保留（plan-status 生成器兼容）；新测试 test_capability_closure.py 10 用例。**Platform/Packaged 保持 0 直至 Phase2 真实验证（by design）**。

---
（方向记录；执行批次按分步放行惯例派单。）
