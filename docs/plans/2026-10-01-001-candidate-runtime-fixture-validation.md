# c68 运行时、验证夹具修复与当前证据接续

日期：2026-10-01。本记录是[当前唯一计划](2026-09-05-001-refactor-runtime-foundation-plan.md)的执行续记；完整目标仍为 U1–U29，底层优先、Studio 暂停。记录中的 PASS 只适用于相应来源和执行范围，不表示全目标、设备或发布验收完成。

## 源码与修复

运行时和验证入口修复先归入 `bef1a30bd2d5674a6da786bae7356e04983e48c1`；后续三个验证夹具／目录修正提交为 `c68ffbc02cccda227a85b104013124b55d415326`，源码指纹 `383a2bc28fb3ed148322e01cc9362532625d29418cbdd0f7a7362039f139c40a`。

- **语言恢复错误分类**：精简 AE4 mirror 漏掉 `assets/lang/{en,zh,ja}.lua`，暴露了真实 Web 缺文件恢复缺陷。启动允许内置字典，但恢复把 native `ENOENT=2` 当作通用错误码；实际 Wasmoon 为 `ENOENT=44`、`EACCES=2`。现由既有 host 适配表分类，native 只接受其缺文件码，Web 只接受其缺文件码；权限、未知及损坏字典继续拒绝，准备阶段不提交部分 locale。实际 Wasmoon 缺文件与 MEMFS 权限正负控均保留。补齐镜像语言资源不代替此产品修复。
- **首条 callee save 的显示位置**：真实 slot 为 `token_index=2 / display_token_index=0 / caller.index=3`，读取器正确拒绝位置 0。producer 只把“显示位置 0、执行／恢复位置 1”的新场景边界保存为显示位置 1：首条 inline save 恢复到 2，挂起边界的外部 save 保持恢复到 1。普通文本／page wait 和读取器拒绝非法位置的合同不变，没有在故事前插入空操作避开问题。
- **Cubism CRT**：显式及分配置 CRT 选择、实际 vendor-link 矩阵已有独立证据。首次完整 SDK gate 中新增 CTest 在长路径下报 FTK1011，目标 tlog 路径实际为 261 字符；c68 将工作根缩短为 `${CMAKE_BINARY_DIR}/crt`，七个 case 使用 `0..6` 子目录。原描述名、14 配置、真实编译／链接检查及预算保留。该定向长前缀复验已通过，不把旧 SDK 整体失败改记为通过。
- **Web runtime 验证夹具**：跨文件系统目录替换仍使用真实租约与原保留断言，replacement 在租约文件系统内创建。随后 clean checkout 的完整 Linux 套件暴露另一个前提错误：仓库内拒绝目标的父 `artifacts` 不存在。c68 将目标改为 `ROOT` 的未创建直接子路径，保留 `RuntimeContractError`、原文件不删除和目标不创建断言；生产拒绝逻辑不改。

## 已实际接受的范围

| 范围 | 来源与结果 | 边界 |
|---|---|---|
| 完整 Lua / Web | bef1 对应实际输入：Lua main147、orphan57；Web52文件/642tests，全通过无 skip；通过独立 Git/blob 与字节映射桥按未触及范围引用到 c68 | 不是新 SHA 重新执行；原报告 SHA、运行标识和 raw 字节不改。纯 EOL 差异分别记录当前摘要 |
| AE4 | 同组实际原件含 native Lua CLI36、Wasmoon/jsdom12；六例各八条事件／回放一致，六个负控实际拒绝 | 不是 C++／GPU／真实浏览器或物理声音证明。旧 mirror 失败、部分 Web10条原件保留 |
| Windows Web runtime fixture | 当前修正文件绑定 c68，完整适用27项 PASS；原 checkout 的 `artifacts` 仍不存在 | 不替代 Linux CrossFS |
| Linux CrossFS | CI `36767544505/attempt1`、job `110065512404`，artifact `11122740792`；完整30项 PASS、0 failed/skip。实际子进程 PID2505，`/dev/shm` st_dev26 与 `/tmp` st_dev2049 不同；owned及私有目录清理 COMPLETE | 仓库内路径拒绝与真实目录 replacement 两例均通过；不是整个 CI 通过或浏览器 UI 验收 |
| 当前 SDK-ON Debug | c68，raw run `ec592c4d-5ad2-49c8-ac4d-dc328d104724`；ordered11项实际退出0，C++1554/453655 assertions、0 fail/skip，Lua147/57，CTest73发现、72 PASS及唯一既定 `CaesuraHeadlessAiSmoke` skip；collector/strict实际0且独审 PASS | 95项生成输入已实际核对，17657项输入前后相同。主构建实际链接，无 LNK4098/NODEFAULTLIB；不代表 P6 场景运行 |

当前 SDK CTest 内的 14 项 CRT 矩阵是实际生成／配置检查，记录 `actual_link_checked=false`；此前单独执行的 14 项 vendor-link 回归为 `true`。两份证据分开引用，不相互改写。

## CI 文档失败与本次同步

c68 CI 的 Linux `Platform Matrix Freshness & Validation` 已失败。原日志 `u29-c68-platform-freshness-failure-01/job.log:5788` 明确记录：YAML 锚点为 `877972de0cded46af8c21dedb1edb17cd0397e25`，自动计算的有效代码 HEAD 为 c68。该运行不能作为整体通过的 CI／发布输入；其他 job 的局部成功不抵消这项 FAIL。

本次仅把平台矩阵的**文档同步锚**改为 c68，并通过标准生成器重建[平台状态文档](../status/platform-status.md)。生成器的自动有效 HEAD 排除 `docs/`，因此后续正常 docs-only 提交仍锚定这次代码提交。没有使用 `--head` 覆盖，也没有修改能力行的原 commit、日期、状态或范围，更没有提升设备／发布等级。原 CI 失败日志、先前 SDK FTK1011 和测试前提失败均保留。

新 CI 前的后续标准检查还发现闭环矩阵的源指纹、保存模块行号和新增回归引用数量过期。已使用原 `capability_closure.py` 正规生成并逐行核对；能力状态与覆写未提升，引用数量属于结构扫描，不是覆盖率或运行通过。Android 静态回归检查为 88/88，仅证明源码合同，不是设备、渲染或签名执行。

## 尚待完成

- 新的正常候选 CI、必需 Verify / aggregate 及最终包须绑定其实际 source/run/attempt/artifact/digest。不能把旧包或旧运行改名为当前候选，也不能以文档同步代替新 CI。
- P6 的当前 SDK selection／导出准备只解决输入前提；当前模型、observer、场景和真实执行仍需独立验收。SDK strict PASS 本身不是 P6 PASS。
- 当前 Foundation Release 完整 strict、本地 `CaesuraEngineSoakProbe` 与原固定预算的一小时 long 尚待各自终态接受；最终 ZIP 不含该本地探针，不能作为替代。历史 long PASS、原 FAIL 与正式 CPU INCONCLUSIVE 保持原范围。
- 当前最终包的完整 native author／恢复、Web author12＋cold2以及其他 U29 项继续。Web 准备中的运行、artifact、最终包和 minified 资源在实际产出／取回前保持 PENDING，不借用 49ae 包。
- 用户指定的 EAS Apple 三 lane、设备、账号与物理输出仍按各自实际条件推进。此前 49ae EAS 启动被免费 CI/CD 额度拒绝，没有 worker job-run；不把 GitHub Apple 或旧日期的额度记录当作当前 EAS 通过。

## 原件索引

本机恢复根为 `E:/CaesuraRecovery/20260924-1446`；以下路径相对于该根。它们是实际证据索引，不是仓库内可再执行的提示词。

| 证据 | 路径／关键摘要 |
|---|---|
| locale 因果与原 RED | `u29-49ae-ae4-failure-diagnosis-01/locale-cause-01/`、`u29-wasm-locale-error-tdd-01/red-01/`；真实 Wasmoon44/2及旧失败保留 |
| 首条 save slot 观察与回归 | `u29-wasm-locale-caller-observation-03/`、`u29-inline-save-cursor-tdd-01/`；原不可恢复 slot 和后续回归分开保存 |
| 组合源码及回归桥 | `u29-c68ffbc0-regression-bridge-01/report.json`，SHA256 `8ce6882b27fc0b3f0a904b8aef0aaddf339906904a83c6b3d1d757db5d23dc39` |
| 三处 fixture/layout 修正独审 | `u29-three-fixture-delta-independent-01/review.json`，SHA256 `e3f47835ea992d047eb1d23424653a072aa39c4aecccb330ca53f4a37e7f7ba1` |
| CrossFS 当前原件与接受 | `u29-c68-crossfs-acceptance-01/report.json`，SHA256 `958ad0aa0c2c4a7e309c440b7cc41dd3e4b7ec5c790b413cdcf6ec16513de988`；容器 SHA256 `2f9c358e63e6afbf2bedb003708cc313da7f4ef32e0c8c7b8830cbb475cc1e3d` |
| 当前 SDK 完整终态独审 | `u26-c68-sdk-terminal-independent-01/review.json`，SHA256 `5534392ed5f00d009de3c1c3af3ce60d2dc6488966812f42c4320d55ddd38dfa` |
| SDK raw / strict manifest | `sd2/sdk-debug-raw-01/run.json`，SHA256 `2259fc26ca8e885ba67df9ed7e6be6e464ee869e0b9e0739e5cd50ab2d66cc61`；对应 c68/run UUID/windows-debug manifest SHA256 `fc505070ea68d7aebf70241bd1d226463d47c9ba8d38d12d452add4ee915b9a1` |
| CI 首失败 | `u29-c68-platform-freshness-failure-01/job.log`，保留原 freshness 错误，不用 override 获取绿色 |
| 后续准备 | `fr2/`、`u29-c68-web-author-preparation-01/`；均不自动构成运行通过或发布授权 |

下一步仍由[当前待办](2026-09-05-003-runtime-foundation-todo.md)和各项实际准入承接；本记录不宣布 U1–U29 完成。
