# U12 接续开发与交接核对 — 2026-09-08

本文是接续执行记录。唯一迭代计划仍为 [运行时可靠性与交付闭环计划](2026-09-05-001-refactor-runtime-foundation-plan.md)，方向保持 **底层优先、Studio 暂停**。

## 13:51 整合进展

下文“接手”和“尚未完成”章节保留首次接续快照；本节更新随后实际完成的工作，旧章节不再表示当前缺口。

- 宏定义/重定义/擦除/动态展开建立历史屏障；选择打开和活动宏栈拒绝回滚，空或全部隐藏选项保留合法历史。真实编译器/调度器回归已通过。
- 回滚现在复用 U11 图层和字体声明准备候选，先完成克隆和资源准备再替换会话。准备失败保留旧会话；提交后严重失败安全停止并允许重新启动。旧协程、reload/jump/load/restore 标记、voice及暂态效果均有回归。
- 快照保留独立文本绘制值、局部/控制状态、角色位置、textbox/nameplate样式、当前说话人、NVL前缀及进入前可见性。活动tween跳过当前历史点；不可重建图层的捕获异常不会阻断点击，且保留原因及更早的有效点。
- `stablepoints-continuation-red-02` 的54通过/7失败在修复后为 `stablepoints-continuation-green-01` 的61通过/0失败。事务回归 `transaction-green-10` 为140/140；原生真实资源/HTTP AI迟到完成 `native-debug-rollback-01` 为2用例/128断言通过，过滤未选中项不算完整套件跳过。
- `lua-main-u12-final-01/02` 保留真实失败。NVL夹具先前返回布尔渲染句柄并继承其他测试的合成图层，异常退出还泄漏后端替身；修正为无GPU句柄、独立场景和作用域清理后，`lua-main-u12-final-03` 为147/147，`lua-orphan-u12-final-01` 为43/43，均源码稳定。
- 完整原生profile `raw/windows-debug-u12-01` 已完成Debug构建，并在独立C++阶段记录1292/1292用例、399001断言、0 failed/0 skipped；但任务中断时CTest仅执行到第2项，原进程已不存在且没有最终run.json。该目录保留为部分证据，不算完整profile通过。最终完整profile与增量后的完整Web正在补齐。
- README内联微信/支付宝原始二维码、赞助入口和Android/iOS标识已在master合入。Codex规范迁移也已交付；用户授权的后续合并不重复请求，运行时代码仍遵守完整门禁。

U12仍待最终完整验收与代码交付；U2剩余内容和U13–U29继续保持完整目标。

## 接手范围与保留状态

- 工作目录：`D:/文件存放处/code/Caesura(AmeKAG)`。
- 旧任务：[Caesura 项目开发](codex://threads/01a06eee-0a0e-7ec2-a322-b5c015917c12)。已读取最近两轮记录，并以源码、日志和 GitHub 实际结果逐项核对。
- 分支：`codex/u12-rollback-safety`；HEAD：`84e766eda8c1a61bc23cb457a70da0c0fa8210fe`。该提交将 `origin/master` 合入 U12 分支，包含 U11 的 `35245667` 及 U12 首批稳定点修复 `a5ba8c49`。
- 接手时暂存区为空，有 12 个已修改文件：执行记录、计划入口、`cancel_token.lua`、`expr.lua`、`snapshot.lua`、`validation_profiles.json`、表达式/operation/rollback session 三个 Lua 测试，以及 Web backlog/flow/runner bridge 三个文件。全部保留，没有 reset、checkout 覆盖、stash、提交或推送。
- 未跟踪的 `%SystemDrive%/` 保留且未读取其内部。未跟踪的 `2026-09-05-003-runtime-foundation-todo.md` 原内容保留为历史快照，顶部补入当前进度；它原本停留在 U5/U7，不能作为 U12 当前状态。
- AGENTS.md 的模块 API 边界、BackendRegistry 唯一访问点、组合根所有权、合并前完整构建/测试要求继续有效。旧任务的历史排期与暂停/执行指令不覆盖当前唯一计划和本次用户要求。

## 已完成与证据适用范围

| 范围 | 核对结果 | 依据与限制 |
|---|---|---|
| U11 交付 | 已确认 | 本轮通过 `gh run view` 在线复核 run `34180028627`，SHA `35245667143c3247555efac62f8344e635cf70e7`，10 个 job 均 completed/success。对应 [GitHub CI](https://github.com/ailiasdesu/Caesura-AmeKAG/actions/runs/34180028627)。仅证明该提交和 workflow 配置，不代表当前 dirty U12 已通过原生验证。 |
| U12 稳定点/控制帧 | 已有实现与历史测试证据 | `a5ba8c49` 及当前未提交 `snapshot.lua` 修复；局部变量、六类控制字段、解锁集合、inline 回滚、取消顺序、连续回退已有定向日志。未重复实现。 |
| 表达式命名空间与取消重入 | 已有实现与历史测试证据 | 缓存匹配 f/sf/tf/mp/lf 身份；取消前分离回调队列。旧“第二次调用奖励 8”问题已由后续 10 奖励回归关闭。 |
| Lua 全套 | 复用旧执行记录，未宣称本轮重跑 | `u11-restore/u12-main-integration-01.json` 与 `u12-orphan-integration-01.json` 均退出 0，日志分别为 147/147、38/38。本轮没有修改 Lua 实现或测试；这些旧 receipt 未记录完整源码摘要，适用范围不能升级为当前全量原生验收。 |
| Web 回滚与 skip 手动继续 | 本轮完整集成通过 | 旧 `web-rollback-alignment.md` 已有 source/bundle 两文件 79/79。本轮完整 Web 再验证所有 494 项，包含该修复；没有重复改写 bridge。 |
| Web 配对性能测量 | 本轮完成 | 统计回归先 RED，再 GREEN；完整实际测量通过，详情如下。 |

指定的两个计划文件与 `artifacts/validation/u12/`、`artifacts/validation/u11-restore/` 均存在。旧完整 Web 485/488、后续 487/488 的失败日志继续保留，未改写为通过。

## 本轮完成的性能统计修复

仅修改生产仓库中的 `web/perf-baseline.test.js`，没有修改引擎实现。原缩放检查分别选择大小场景的中位耗时，可能将不同相邻测量组的时间相除；现在保持三组配对及交替执行顺序，计算每组 `wall2000 / wall1000`，再取三项比率的中位数。

- 严格 `< 2.5` 门槛、吞吐量和内存预算保持不变；每次实际运行仍检查 DONE、token/tick 数、64 个历史点、完整已读标记和错误事件。
- 确定性测试覆盖配对身份、原始样本不变、持续退化、单个快样本不能掩盖退化、恰好 2.5 不通过，以及非有限/非正计时拒绝。
- `artifacts/validation/u12/perf-paired-statistics-red.{json,log}`：3 failed / 3 passed；6 个实际 benchmark 未选中。
- `perf-paired-statistics-green-01.{json,log}`：6 个统计测试通过；仍不是实际性能结论。
- `web-resume-full-01.{json,log}`：**35 个测试文件、494 passed、0 failed、0 skipped，退出 0**。源文件运行前后 SHA-256 映射一致；accepted receipt 验证通过。耗时 73.89 秒为 Vitest 报告的本次执行时间。
- 本次实际三组配对：1138.3574/2728.5670 ms → 2.396934；1194.5670/2756.2115 ms → 2.307289；1224.7219/2718.2087 ms → 2.219450。中位比率 **2.307289**，满足原门槛。

以上是本机 jsdom + Wasmoon + 真实共享 Lua runner 的集成与测量证据。统计方法修复不等于引擎性能优化；没有据此宣称真实浏览器、GPU、设备音频或其他机器性能通过。

独立代码审查未发现 perf 补丁阻断项。本地 artifact 验证包装器曾在 `source_stable=false` 时仍返回 shell 0，已修正；故障注入确认子命令 0 且源码变化时包装器返回 1、不生成 accepted receipt，稳定时才返回 0。结果见 `wrapper-source-probe-result.json`，审查结论见 `resume-review.md`。本轮未重新运行 C++/CTest 或完整 Debug 构建；合并前仍须执行当前最终代码对应的必需门禁。

## U12 尚未完成的内容与下一切片

下列是当前源码确认的风险或证据缺口，尚无本轮修复，不能标记为完成。中途执行被打断后已重新核对：`scheduler.lua`、`kag/commands/text.lua`、`kag_runner.lua` 没有本轮增量，计划中的 `test_rollback_boundaries.lua` 尚未创建。

1. **宏/选择历史屏障。** snapshot 共享 tokens/macros；scheduler 动态宏展开原地改写 tokens、erasemacro 修改 macros，而现有注释声称的历史清理未落实。endbutton/选择结算边界也需真实场景回归。先测试动态/嵌套宏、擦除宏、打开选择和选中后 deferred jump；屏障应保留无选项场景的合法历史，不将静态展开一概禁用。
2. **回退后的旧继续信号。** `commit_rollback` 未像 `finish_session` 那样清除 pending reload/jump/load/restore。先以公开入口产生实际待处理信号，验证下一帧与手动继续仍沿历史位置执行，再修被证明必要的清理。
3. **图层资源与准备失败边界。** 当前回滚只使用旧 `Layers.capture_snapshot/restore_snapshot` 的属性快照，无法重建被删节点、旧纹理和父子结构，或移除未来节点。复用 U11 `kag.layer_state` 的 prepare/apply/discard 接口；先测准备失败保留旧 owner/co/画面/变量/历史，提交后严重失败安全停止与可重新 start。不得直接套整个 SaveState 或 Presentation.apply，避免改变 tf、历史及“BGM 不回卷、voice 停止”的现有回滚合同。
4. **真实迟到 completion/AI 与扩展历史内存。** 复用真实 Engine/worker/配额回归，验证 rollback 接线实际失效旧回调、新请求仍可完成；现有 Lua 请求数组替身不能替代该证据。资源字段稳定后，再测带控制帧和真实资源声明的 64 项历史内存。

U12 保持进行中，U13–U29 保持后续计划状态。交接完成不表示 U12 或整个路线图完成，也没有发布、合并或恢复 Studio。

## 旧任务处置

用户要求接手完成后归档旧任务。关键进度、原始证据位置、保留改动及下一切片落入本记录后，已调用应用归档操作，返回旧任务 `01a06eee-0a0e-7ec2-a322-b5c015917c12` 的 `archived: true`。旧任务从活动列表移除，历史记录仍可从归档找回；项目文件和旧验证产物保留，没有永久删除。
