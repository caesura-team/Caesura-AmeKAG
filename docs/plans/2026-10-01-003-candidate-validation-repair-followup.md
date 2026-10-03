# 候选失败原件、UI 读档与验证入口修复接续

日期：2026-10-01。本记录接续[c41 分项验收与验证入口修复](2026-10-01-002-c41-candidate-and-validation-repairs.md)，对应[当前唯一计划](2026-09-05-001-refactor-runtime-foundation-plan.md)。完整目标仍为 U1–U29，底层优先、Studio 暂停。既有失败、分项通过及条件未测范围分别保留，不将多个旧版本的结果相加为整合候选通过。

## 源码身份与记录边界

本轮已完成失败取证的候选为 `f2cfba32e407b32c8ee253d0461719793405c18f`。后续 UI 修复已本地提交为 `f199950cde0f7542a7093f6aa1971ae4ccf9998e`；uuid 修复整合提交为 `7951fe5edb42239c5e93e1f243912d773c85c179`，collector 修复整合提交为 `dd1da69986ab45cbe4245d4fe64af536db2ed8f8`。这些是实际修复历史，不是最终候选通过证明。

最终整合代码锚：`0c473bf19220aa820391672431d450f78a560f2e`。该锚绑定已整合的代码；文档提交自身不作为此前产品二进制的源码身份。完整新候选验证仍按后续实际原件分别接受。

## SMA：保存字节修复通过，sanitizer 失败保留

此前真实 RED 中，HTTP 返回200/`ok:true`，但正确目标文件未更新，新内容落入含反斜杠的错误文件名。修复直接使用已经验证的相对路径。当前真实 GREEN 已核对正确目标的实际新字节、请求前后来源以及原资产不变，限定接受为 `PASS_SMA_SAVE_BYTES_FIX_SCOPE_ONLY`。

这次诊断沿用 c41 库并编译链接已修复 main；它不是完整新候选构建。RED、GREEN 两次原生退出均为1，LSan 诊断与整体 FAIL 保留。保存字节修复通过不能抵消 sanitizer 失败，也不推出 Windows/Apple 完整 HTTP 合同通过。维护中的完整 HTTP 回归在后续 f2cf CI 另被 uuid 前提错误阻断。

## f2cf 托管 CI 真实终态

GitHub run `36814066483`、attempt1 已终态 **FAIL**：12个作业中4成功、4失败、4跳过。Windows、Linux、macOS Debug 均失败，后续四个 Release/Web Package 作业跳过，必需聚合 Verify 失败。Android 与 iOS 分项 job 成功不能代替这个候选的完整门禁或最终包验收。

三种桌面系统的原始回执分别确认 HTTP smoke 中的 `UnboundLocalError: uuid`。函数后部的局部 `import uuid` 使更早使用的 `uuid` 被判为未赋值局部变量。Windows 原 CTest72项中70通过、HTTP smoke失败、1项原有可选 AI 跳过；owner 正负控和18项 driver 测试实际通过。不能用该 Windows owner 通过解释或抹去 EAS macOS 的另一份 owner 失败。

uuid 最小修复删除重复的局部导入，保留原全局导入、请求、断言和清理。实际编译器符号表及一行逆变换已审查，限定结论为补丁审查通过；不将静态审查写成实际 HTTP GREEN。整合后的必要托管验证仍待新候选执行。

## EAS：完整运行失败与可接受的分项

用户指定的 EAS 三 lane run `01a0f5b5-cbf8-748f-a799-a23a2eb45296` 绑定 engine f2cf、bootstrap `b799f235f26f163560b9a521a7ef4112743870f4`，整体 **FAILURE**，两 lane失败、一 lane成功。原失败不改写。

| Lane | 实际原件 | 接受边界 |
|---|---|---|
| macOS Debug | 构建退出0；C++1536、Lua147+57通过；CTest72项为69通过、2失败、1既定可选跳过，实际退出8；collector与strict退出1 | HTTP uuid错误及 owner正控的进度截止失败分别保留；不是 macOS完整通过 |
| iOS Simulator C++ | 编译和实际C++均退出0，1536用例、453525断言全部通过，0失败/跳过 | 后续第二次动态 collector导入找不到 `validation_sanitizer`；缺少运行后binary摘要，原lane仍FAIL，不能仅用计数恢复PASS |
| iOS device compile | 当前源码、42条命令及84份命令日志、源码前后、arm64 Mach-O身份与iPhoneOS14.0目标已核验 | 仅 `PASS_IOS_DEVICE_UNSIGNED_COMPILE_SCOPE_ONLY`；无签名、真机运行、UIKit生命周期、Metal渲染、物理声音或商店结论 |

collector 修复把真实后续 collector 导入与解析放回受控的 fetched-source 上下文，同时保留原路径、模块恢复和负控清理事务。真实导入/解析红绿及本地完整20项工具测试通过；Apple命令在该工具回归中使用夹具，因此它不代表新的 Apple lane 通过。后续 EAS bootstrap 必须实际包含修复后的 driver，不能将旧 b799 源码包改标为已修复。

## Web UI 读档：真实空白正文 RED 到范围 GREEN

探针语法问题修正后，实际 Chrome 证明 Load 点击命中正确按钮且在视口内，日志收到加载完成；但加载前保存的结尾文本变为空，页面没有异常。新观察没有调整点击坐标、等待条件或存档游标。

根因是 UI Load 未显式设置推进方式，使用桥接接口的 `autoClick=true` 默认值，消费了恢复后的等待点。生产修复仅让该 UI 调用显式传入 `autoClick:false`；公开桥接默认值、Lua恢复器和原冷恢复探针合同保持不变。

真实 main/Wasmoon/Lua 回归先取得冷、热加载2项失败，再取得2项通过。后续正控明确区分 `[ch]` 与紧随的 `[p]` 两个输入等待：第一次实际 Advance 为 `WAIT:1`且保存页保留，第二次为 `WAIT:3`并显示下一页；没有增加循环次数绕过断言。完整 Web53文件、644测试通过，0失败/跳过，当前 bake与Vite build亦实际退出0。

真实 Chrome02 使用实际新诊断包和不变的 basic 作者文件，完成 root路径的选择A、UI保存/读取与原槽字节保持，限定接受为 `PASS_WEB_UI_LOAD_DISPLAY_FIX_SCOPE_ONLY`。该包来自 f2cf 加已审 UI补丁的实际构建，不是原 c41 最终artifact；也不代表双模板、双origin、三进程冷恢复全部通过。早期 CDP失败、探针语法失败、hot-load正文丢失及诊断入口的路径失败原件均保留。

## owner 修复与后续整合

owner 可信通知修复已取得 `PASS_OWNER_NOTIFICATION_FIX_SCOPE_ONLY`。原确定性 RED 使用真实 owned 子进程：子进程实际退出0，清理完整，但受控延迟 helper返回/future最终发布时，旧观察器仍触发进度截止失败。修复使用父级保留等待句柄提供的可信退出通知，区分被测工作已完成与后续清理/回执发布；不以未验证的结果文件代替所有者身份或实际退出。

原定向执行17项通过、1项新增 forged-result 测试前提失败保留。该测试错误地要求被故意伪造为 `EXITED` 的文件呈现 `STOPPED`；生产拒绝是正确的。仅该用例前提修正后的单项 GREEN 通过，生产代码未因此改变。随后完整 process15、runtime48、owner24共87项全部通过，实际 owned 退出与源码稳定已核验，增量独审03通过。原0.4/4/8秒测试时限、停滞/身份/伪造结果负控和清理合同不变。

owner 修复来源提交为 `20485ab19df0f6c92de8f07b841b9a909e3f1864`，已整合至本记录代码锚。上述是 Windows 真实Python owned子进程的限定证据，未还原原 macOS EAS 的瞬时调度顺序，也不是原生引擎长跑、完整新候选门禁或最终包验收。

原 EAS owner失败与本轮实际子进程回归分别保留，不推定旧失败的瞬时因果。当前代码锚已绑定本轮已接受的修复范围；整合后的平台和候选门禁仍须实际验证。

下一最终包的 cold 准备沿用两个模板、两个origin、每origin三个连续独立Chrome进程与原语义/文本/slot/owned合同。原作者12步保留历史身份；匹配的create/derive/check可以复用，UI和候选字节改变后的包必须基于最终精确artifact重新生成、展开与静态验证。原 c41包或一次hot-only诊断不能改标为新最终包已通过。

## 原件索引

以下为外部验证档案内的相对标识与实际SHA256。原始日志、二进制、个人工作目录及凭据不入库。

| 原件 | 外部档案相对标识 | SHA256 |
|---|---|---|
| SMA字节修复限定接受 | `u2-sma-posix-save-red-green-root-01/review.json` | `db1abe347183a83c2d56a50602f58ba0946554135fc793b374f76edd4aeafaf2` |
| f2cf GitHub终态 | `u29-f2cf-hosted-ci-preparation-01/observation-05/report.json` | `7e66dd296e51d64c5bb4f2b442c0c19eadc1fa16af8d0c96ad0f643a384522c9` |
| Linux实际uuid失败 | `u29-f2cf-ci-linux-failure-01/diagnosis.json` | `294fbd373c5000e5ed6e8ea59519cdfe9e74d31f3a9139ed55e1d8c6c36b2da5` |
| macOS实际uuid失败 | `u29-f2cf-ci-macos-failure-01/diagnosis.json` | `f35c14e4e552937dedb3208d627010525232c651c8efc34427ff54ffb934c3ec` |
| Windows实际uuid失败 | `u29-f2cf-ci-windows-failure-01/diagnosis.json` | `80e187ee82f7f7bb5d7bf7106ee075729540d7139ac726dad89cbe216479d306` |
| f2cf EAS终态 | `u2-f2cf-eas-preparation-01/operation-observe-abd4df22dfcd4c4a94495fb5f77b860d/result.json` | `5fcc5c80faff24184873b9299e1861c5f8df77b9dcd932560f83a8ed0ee5678e` |
| EAS失败原件归纳 | `u2-f2cf-eas-failure-readback-preparation-01/terminal-findings-01.json` | `38b7f44565e8e932bf7e8a4568603dd4b597c89e507fa44de74c13c23826dd62` |
| iOS device未签名编译接受 | `u2-f2cf-ios-device-compile-acceptance-01/review.json` | `72215300e24e876cdb28ce0aa4d40fb899f2b4e822dc90a26c846e0f0542f919` |
| UI真实Chrome修复范围 | `u29-web-ui-load-display-root-acceptance-01/review.json` | `b044ef9cda10d404168856e8395fcbe8ce4a330f4dc68bb8c00e372101abf7cd` |
| UI完整Web与TDD | `u29-web-ui-load-display-tdd-01/report.json` | `77e2784aaa32eb5be71b433b27a34177a4643323e1374697a74a8885c0d33d55` |
| uuid补丁审查 | `u29-http-smoke-uuid-root-review-01/review.json` | `925305999230590da9e278250da1144e9c35dae492d774603681359d05cef306` |
| collector修复范围 | `u2-eas-collector-import-root-review-01/review.json` | `3f8e2690e25b11d7e9ec17c45d0f11b00a08d66a2ad6b6dfc9ac5984fa4a07fe` |
| 原Web作者12与cold失败 | `u29-c41-web-continuation-01/terminal-summary.json` | `5eac5a3b680b9848b819ddcd187235371773f9979c0736e7c0b4da2187638488` |
| 下一最终包cold草案 | `u29-post-owner-web-cold-preparation-01/handoff.json` | `a6e2edd302312bdc3249fc6c39abc2edb8a558d819a1fb2be242ef18ccaf4ea1` |

| owner修复限定接受 | `u2-owner-progress-terminal-root-acceptance-01/review.json` | `dcc0504f0ff7230498d2c41c48f193b793a638b039f2fb6308bd34c60dc4718b` |
| owner完整87项 | `u2-owner-progress-forged-result-fixture-fix-01/full-01/report.json` | `5964afe615696705a506ac0d37101b0b88ad49cbaf72374dbbafb1d66a6a1fe1` |
| owner原真实RED | `u2-owner-progress-terminal-tdd-01/red-negative-summary.json` | `be840bc512ae6b60ae75579218db91024b57a33e48fe41a03084808b41c1e658` |
| owner原定向前提失败 | `u2-owner-progress-terminal-type-fix-01/targeted-01/report.json` | `10bef54e3bf9636a46d01b01c3bd0377a73dfe6aa15e8913ae8271356623c93c` |
| owner单项前提修正GREEN | `u2-owner-progress-forged-result-fixture-fix-01/single-fix-01/report.json` | `5204869704f8a88108aa32f358470249800c58834d6f81a6236abd1a259b512f` |
| owner增量独审03 | `u2-owner-progress-terminal-independent-03/review.json` | `d8c11ce71a3ca2afac3208d4c4bb161130a21e277dbd55324195aec2f0b53f8c` |

## 完整目标与未闭合项

原计划 U1–U29 的29个单元及其验收要求全部保留；R1–R14、AE1–AE8没有因本轮分项通过而删除。owner限定修复及本地87项验证已完成；仍需完成匹配新代码和工具的必要CI/EAS门禁、最终包的双模板冷恢复、U28声明同步和U29逐项核对。历史CPU INCONCLUSIVE、sanitizer FAIL和设备/账号/物理输出条件保持各自范围，不追加“反复运行直到绿色”的门槛。平台能力行和生成日期不在本记录中手工修改。
