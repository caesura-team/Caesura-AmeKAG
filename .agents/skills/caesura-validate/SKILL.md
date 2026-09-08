---
name: caesura-validate
description: 核对 Caesura 当前源码、实际构建产物和测试证据，运行适用合并门禁，区分历史、定向、真实后端与发布证明。
---

# Caesura 验证
以 AGENTS.md、当前计划和 scripts/validation_profiles.json 确定必需范围，不在失败后缩小范围。
- 开始前记录分支/完整SHA/dirty状态、配置、实际binary与夹具；先确认无同目录构建仍运行。
- 复用 scripts/run_validation.py、collect_validation_evidence.py、verify_release_candidate.py。先查看当前 --help，再给明确profile和新的run目录。
- Windows C++完整运行从 build/tests/Debug；Lua使用 build/lua/Debug/lua.exe，主套件与隔离套件分别执行。其他preset按实际产物路径，不能捡旧二进制。
- CTest、Web与真实后端检查按风险/计划触发；编译成功、Null后端、jsdom与真实设备效果分别记录。
- 完整通过要求实际发现的全部必需用例通过；allowed optional skip必须事先写在profile。过滤未选中不冒充完整套件。
- 同一源码/配置/依赖的已接受证据可复用；变化后验证受影响范围。完整候选门禁有一次对应当前快照的执行，不能拼接旧结果冒充单次全绿。
- collector只解析已有日志；verifier不能伪造PASS或授予发布授权。运行期间源码变化、缺日志、缺required检查均不能通过。
报告实际计数、失败/跳过、源码稳定性、receipt及清楚的未测边界。覆盖率和测试注册完整性分开。

详细执行约定见 [Codex 工作流](../../../docs/team/codex-workflow.md)。
