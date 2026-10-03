# 被调用场景选择跳转修复与 3a3 验收记录

状态：**3a3 不是完整候选通过。** 最终包 AE3 热恢复流程在执行 `[load]` 之前暴露了生产控制流缺陷。本提交修复该缺陷；新候选的完整门禁、最终包与 AE3 矩阵仍待验证。完整目标保持 U1–U29，底层优先、Studio 暂停。

## 3a3 已取得的实际证据

- Windows Release 一小时长跑 `long-02` 实际测量 3663.0840784 秒、5000 个测量循环、50 个上下文和 49 次重建。原生、控制器及 launcher 均正常退出 0，清理完整。固定 64 MiB 预算未变：private bytes 基线 379846656、峰值 419090432、阈值 446955520。18000 张 PNG 的原字节与解码指标独立核对，6000 组 A/B/restored 满足恢复图像合同。这是固定 Windows 负载证据，不证明普遍无泄漏、CPU 性能或物理可听性。
- 原 `long-01` 在约 53 分钟中断，缺少历史退出和清理收据，保持未通过；7c23 内存失败、8d3 语音接纳失败均保留。
- AE5 真实 Chrome 原生 Promise 交付边界复验通过 99 项检查。6 份 decoded PCM 与 2 份 rendered PCM 已独立复算：停止窗口为静音，恢复 A 与后续新请求有预期输出。原 PCM16 期望值错误及首次失败保留；修正从冻结整数推导 Chromium 正样本归一化，没有放宽容差。这是 OfflineAudioContext 和 JavaScript 可观察边界证据，不是内部解码线程、物理输出、完整 KAG load 或最终 Web 包验收。
- 最终 Windows ZIP 的两次真实 create、四条作者 check/build/package 链和四组错误／正确密钥对照，共 22 步及其原始 owned 收据已独审通过。原 f87 密文不变；错误密钥拒绝且会话状态不变，新的正确密钥进程继续执行匹配原保存位置的恢复断言。
- 坏当前 ZIP 被实际 preparer 以 `Archive identity mismatch` 拒绝，实际退出 1、launcher 0、完整清理，未启动坏包引擎。此后四个新的 e51 回退进程恢复原用户档并完成实际断言。该结果是显式旧包回退流程，不是自动更新回滚器。
- AE7 当前 CI 快照的一项正控和摘要替换、必需作业取消、必需作业缺失三项负控通过生产验证器。传输明确是离线快照回放，`release_ready=false`；不替代联网身份或标签入口验证。

## 真实缺陷与最小修复

AE3 basic/hot 的原生进程正常退出，但没有产生完成记录。运行时记录已显示 A 保存、B 状态和 `AE3_LOAD_A` 真实选择输入；runner 随后打印丢弃 `*load_a` 的跨场景跳转诊断。因此验收器正确拒绝，该运行不能计为恢复通过。

成功跨场景 `[call]` 的 `load_tokens` 设置 `_scene_changed`。scheduler 已经内联切换到 callee 的令牌和局部帧，却保留此标志。callee 内 `[select]` 后的 `[end]` 使协程结束；runner 把旧标志当成尚未处理的场景替换，丢弃了 callee 本地有效选择。故障发生在 `[load]` 执行之前，增加等待或帧数无法恢复被删除的跳转。

修复只在成功内联进入 callee、完成令牌与编译状态接管后清除已消费的 `_scene_changed`。真正的跨场景 jump/link 优先级和 return 时的悬空 callee 选择剪枝均保留；失败加载和预算拒绝分支未修改。

## 回归与诊断范围

扩展既有 `test_select_crossscene_flow.lua`，通过真实 production runner、call 和选择点击覆盖直接／嵌套调用及左右两条分支，没有直接注入 pending jump 或 selectedChoice。

| 验证 | 实际结果 |
|---|---|
| 未修复真实 Lua RED | 9 passed / 14 failed，退出 1 |
| 有效 GREEN（green-02） | 23 passed / 0 failed，退出 0 |
| 完整 Lua 主套件 | 147 passed / 0 failed |
| 完整 Lua 孤立套件 | 57 passed / 0 failed |

`green-01` 因控制脚本编辑前置失败仍执行了未修代码，其实际退出 1 原件保留，不算 GREEN。既有场景替换丢弃、caller 选择经 call/return 保留、callee 悬空选择在 return 剪枝的对照仍通过。实现独审没有剩余可行动问题。

原生诊断从原 CLI 产物建立新副本，仅替换 `scripts/scheduler.lua`，其余输入、1500 帧、16 毫秒固定步长及 120 秒预算不变。实际退出 0、完整清理，出现 restored-a、next-choice 和 `A:left`／reward=1 完成记录。它证明修复可改变原失败路径，**不是未修改最终包或新候选的完整 AE3 验收**。原 3a3 包与失败锁保持不变。

## 原件入口

恢复证据根为 `E:/CaesuraRecovery/20260924-1446`，本轮 Windows 写入均在该根，D 盘原项目未修改。

- 长跑：`u27-3a3-soak-01/long-root-review-02/report.json`；独审 `u27-3a3-long02-independent-01/review-01.json`。
- AE5：`u29-3a3-ae5-execution-01/run-02/report.json`；独审 `u29-3a3-ae5-run02-independent-01/review-01.json`。
- 密钥对照与回退：`u29-3a3-native-pairs-independent-01/review-02.json`、`u29-3a3-fallback-independent-01/review-01.json`。
- 原 AE3 失败：`u29-3a3-native-final-preparation-01/v02/runs/ae3-basic-hot-01/report.json`。
- 诊断：`u29-3a3-ae3-hot-diagnosis-01/diagnosis-01.json`。
- Lua TDD：`u29-called-scene-choice-tdd-01/summary-01.json`。
- 实现独审：`u29-called-scene-choice-review-01/review-01.json`。
- 单文件替换诊断：`u29-called-scene-choice-native-diagnostic-01/run-01/report.json`。

## 后续验收

冻结修复后的新候选，执行对应完整 Debug／Release、C++、Lua、CTest、托管门禁和最终包验收；不能把 3a3 的通过项迁移为新 SHA 的通过。继续完整 AE3 热／冷／保留 B 对照、Web 作者新进程 UI 恢复、当前候选长跑和固定性能比较。基线 Release 已完成新配置，完整基线执行仍待进行。Linux full04 和 Web 准备已有静态审查，但绑定 3a3 的选择必须在新候选下重新明确身份与输入。原 Linux 泄漏与性能 INCONCLUSIVE、设备／账号条件、U28 声明和 U29 整体验收继续保留。没有合并、发布或标签修改。

## 5b9 托管门禁的平台文档锚失败

代码提交 `5b9cc41991751c787c2655dec58b71872b252f53` 的 CI `36682513882/attempt1` 中，Linux Debug 已完成严格原生执行及后续 Web 检查，但平台矩阵新鲜度步骤失败。提交后本地不带覆盖参数的 `generate_platform_status.py --check` 也实际退出 1：YAML 同步锚仍为 c6cf2e10，有效代码 HEAD 已为 5b9cc419。提交前检查不能证明提交后锚仍有效。

文档同步提交只把 `evidence_head_commit` 更新为 5b9cc419 并重生成平台状态页，不修改各能力的原执行提交、日期、状态或未测边界，也不将同步锚升级为新平台验证。同步后须在文档提交完成的 HEAD 上再次运行标准 `--check`；不得用 `--head` 覆盖绕过正常新鲜度检查。原 CI 的失败、skip 和已完成步骤分别保留，新候选仍要获得自身完整结果。
