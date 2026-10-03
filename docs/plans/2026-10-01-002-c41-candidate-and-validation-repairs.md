# c41 候选分项验收与验证入口修复

日期：2026-10-01。本记录接续[运行时与夹具修复](2026-10-01-001-candidate-runtime-fixture-validation.md)，对应[当前唯一计划](2026-09-05-001-refactor-runtime-foundation-plan.md)。完整 U1–U29 仍未完成，底层优先、Studio 暂停。

## 来源与范围

冻结产品候选为 `c41efa4655e78370352f106bda16c337842b0d8a`，工作树指纹 `cface998da6449a47bed9d33bd89a4c5b795d632db5fd7f57b60761da0588d7e`。它与 c68 仅有六份文档差异；6338 个非文档条目及13项代码原件锁已逐项核对。c68 的 SDK/P6 原件继续标记 c68，不改贴 c41 身份。

后续修复在独立工作树整合，当前代码提交为 `7ae0ace5bc14ece2156f29827cc118e533822bf3`。该版本包含 EAS 源码导入修复、iOS 测试表达式兼容修复、CDP 传输修复和 macOS 断言失败诊断信息。另包含经真实 RED 与独审支持的 SMA 最小路径修复，其 GREEN 待验。它尚未取得完整候选验证；下面 c41 的产品结果不能直接改名为该版本通过。

## c41 已接受的分项

| 范围 | 实际结果 | 适用边界 |
|---|---|---|
| 托管 CI | run `36774316104`、attempt1，12 个 job 成功；10 个必需 producer、11 类 artifact 完成身份核验 | `DRY_RUN_INPUTS_VERIFIED`，`release_ready=false`；不是发布许可 |
| Foundation Windows Release | ordered11 项实际退出0，collector/strict 通过；C++1553、453641断言、0失败/跳过；Lua147+57；CTest72发现、71通过及原有唯一可选 AI 跳过 | 原失败保留，实际构建及原始日志绑定 c41 |
| 一小时真实后端长跑 | 3670.1214042秒，5100个测量样本、6120总样本，51上下文、50次重启、18360张 PNG；独审通过 | `PASS_LONG_SOAK_SCOPE_ONLY`；不推导性能改善 |
| CPU 配对样本 | 180测量样本、36预热样本、3对；比较器实际退出2 | `INCONCLUSIVE`：基线 `lua_table_reads` RMAD 0.1568335 超过原0.1阈值；没有重复采样或放宽阈值 |
| 原生效果矩阵 | 18场景、82图通过，独审接受 | 使用实际 `FAULTS=ON` 变体；与 Release 的 OFF 变体分开，原 OFF 失败不改记 |
| 最终 Windows 包作者/恢复 | 全部35个选定步骤接受，含两个模板、加密正负控、坏候选拒绝、旧包回退及冷热恢复 | 原第28步引擎实际成功但 Pillow 观测失败；原日志经离线观测恢复，未重跑引擎；其余步骤分别有实际 owned 回执 |
| SDK/P6 | c68 完整 SDK Debug11项及 strict 通过，选定四项 P6 owner 范围独审接受 | 原三项失败保留；不等于物理声音、Steam账号或全部 SDK 平台通过 |
| Android | c41 自动签名构建证明已回读并接受 | 不包含真机、商店或物理输入输出验收 |

Windows 最终 ZIP 的内层原件为41150903字节，SHA256 `737254e5e83650d8c07b3693b7be05bbbe59b4e282d7b0cc5688c785137b1298`。Web 最终 ZIP 的内层原件为20323184字节，SHA256 `72c11ead2fd2a4f22c5a060ccbe31580b41f4e98fc057946927126f8a9d4b47f`。验收引用这些原件，没有验收后重新打包。

## Web 传输故障与续接

原 basic 作者链前五步通过，第六步在 CDP 读取完整字体响应时以1006断开。安装的 Node24.16 / Undici7.25 对压缩响应单次解压有4 MiB限制；相同字体的未压缩及64 KiB分片压缩对照通过，而单块压缩对照失败。实际 Chrome 中亦观测到断开后浏览器仍存活，排除了把该症状直接记为浏览器退出。

`31bd3c31fed7c5d42b7c08e34b0f9d9de4062a48` 使用公开、连接级的 Dispatcher 接口，为已限定的本机 CDP 连接取消压缩协商；没有全局修改 Node，也没有放宽原40 MiB响应限制。37项 Node 协议回归、27项 Python runtime 回归、三个字体传输对照和实际 Chrome 完整字体读取通过；错误接受值、扩展、重定向和端点负控保留。

续接绑定 **产品 c41 / 验证控制31bd**，复用原 basic 前五步。新增 basic browser 和 kag3 六步共七步已实际接受，因此两个模板的12步作者链通过；两者均含真实浏览器根路径、子路径及离线重载。原失败、旧 active lock 和旧 receipt 历史均保留。

第八步 basic cold 在首个 `produce-a` 阶段失败，尚未启动第二个浏览器进程；kag3 cold 未运行。实际 CDP `Runtime.evaluate` 返回 `SyntaxError: Unexpected token ']'`，对应冷恢复探针的 choice 表达式多出的闭括号；静态检查同时定位到 load 表达式的同类错误。全部 owned 进程已退场、端口关闭，未发生超时或强杀。该失败不能作为冷恢复产品行为结论，修正探针后仍须完成原两模板、两来源、三个新进程的完整冷恢复合同。

## U2 当前失败与限定结论

Linux Clang sanitizer 当前11项命令均退出0，C++1536/453376断言、Lua147+57、CTest69发现/68通过及原可选 AI 跳过。但严格 collector/verifier 退出1：HTTP smoke 的受控引擎退出时产生240 B LSan报告。因此该 profile 仍为 **FAIL**，不能用外层执行收集成功抵消。

单次补充诊断中 HTTP73项通过，原生进程 PID512 仍退出1。两份 maps 的差异均为匿名、不可执行映射，所有文件映射一致；60个 unknown-frame 出现位置可归属到已捕获的 Mesa ELF。三个分配调用点在当前文件 SHA、BuildID及精确 ELF 地址匹配后，复用既有符号结果定位到 `get_cpu_topology` 和两处 `u_mmInit`。整体采集因原 maps 全字节相等条件不满足，继续保留 `INCOMPLETE`；限定模块归属不证明引擎生命周期无责或泄漏无害。没有增加排除项、关闭泄漏检测或重写原 FAIL。

用户仅批准既定 Linux 验证根总上限由5 GiB提高到8 GiB，目录范围、C盘启动16 GiB/停止10 GiB/保留8 GiB约束及旧证据均保持。完整运行439次空间采样的峰值为6630195200字节，未超过8 GiB。

经独立审查，本轮模块归属诊断到此收束，按计划 U2 及风险表的第三方兼容性条款保留明确限制：109条第一方编译记录包含 ASan/UBSan 及不恢复 UBSan 标志，未过滤 C++、Lua 等10项检查通过且对应捕获清单没有诊断；这不等于全代码覆盖或完整 profile 通过。同一精确 Mesa 库在无 Caesura/bgfx 的独立 EGL 绘制及完整 teardown 对照中仍产生相同240 B/3分配，支持限定第三方兼容性记录，不能证明所有引擎生命周期正确。既定未选的两个 RPC 集成项有当前 c41 非 ASan 的实际 CTest 通过，Web 固定 `/tmp` 路径项有 Linux CrossFS30 与同 blob 复用桥；这些是替代功能证据，不是对应路径的 sanitizer 通过。本轮没有新增排除项。

TSan 基本可行性由明确竞态源码、对应 ELF/BuildID 与真实 race 诊断支持。原编译/运行退出回执缺失继续保留，空正控日志不证明退出0；不声明完整正负控门禁或引擎 TSan 通过。该限定结论已回答计划的可行性检查，无需为此重复控制实验。

EAS 完整三 lane run `01a0f4f9-e4e3-74ab-8d73-9e513af89bd2` 实际失败。两个 iOS lane 被 doctest 格式化文件时间的旧部署目标兼容问题阻断；`83416b6a08de806524d9218d4e089c486fce1724` 保持原比较语义，只阻止该表达式进入时间格式化路径，尚待实际 Apple 复验。macOS 完整运行中 C++1536、Lua147+57通过，但 CTest 有一个 owner 观测测试失败；独立一次诊断实际通过，未解释或覆盖原失败。新增断言 `msg=result` 仅补充今后必要运行的失败信息，没有调整原8/4/0.4秒合同。

另发现 SMA 保存分支把 `/` 强制替换为反斜杠，Linux 实际产生了错误文件名。原 HTTP 检查提交相同内容且只检查响应，不能证明目标文件已更新。现已用旧引擎完成真实 RED：在新隔离副本上只追加4字节合法尾空白，HTTP返回200/`ok:true`，正常目标仍为原 SHA256 `164be3831b611c98c794fda60afcbc3b5e6efd85c50a1d716db2d92638f2d934`，错误反斜杠文件则为请求的新 SHA256 `39fc61f405a7b367cfd7d7502fb055a0bc135a263b7f594c3e5cbe8cf00e2656`。原生 PID497受控退出1，保存字节断言与 LSan 诊断分别记录，所有进程清理完成、原输入未变。

整合树的最小修复移除该分隔符替换，直接使用已验证的相对路径。维护中的 HTTP 回归改用本轮独占文件名，提交实际不同内容并核对落盘字节、原资产不变和 POSIX 不出现反斜杠文件；只清理本轮拥有的两个名称。当前仅完成语法与差异检查，GREEN、完整构建及平台验证仍待实际执行。

## 原件索引与剩余工作

下列路径相对于本机恢复根 `E:/CaesuraRecovery/20260924-1446`，记录可复核原件；构建和验证产物不入库。

| 原件 | 路径 | SHA256 |
|---|---|---|
| 当前 CI 终态 | `u29-c41efa46-hosted-ci-01/terminal-readback-01/terminal-report.json` | `4d88f3813b37d603a0f21749431d9c67e2ade6b17fb4ca5c16df6ad5bcd5751e` |
| 当前 Release | `fr3-recovery-preparation-01/release-verification-02/report.json` | `9bdbb241f34077ba195143c5c0c8e5c402fbddcd8fa7268ba7c2b8383f2cd212` |
| 当前长跑 | `u27-c41-soak-root-01/scope-acceptance.json` | `52153ec3adef05ba95668d45468349a711ef06dfa86d4a465820c06f2e600330` |
| CPU限定结论 | `u27-c41-cpu-independent-01/review.json` | `915ab7f68abb9bacdc3da47b772767a01e0808373e087110ac1940df7f369286` |
| 原生35步接受 | `u29-c41-native35-root-acceptance-01/review.json` | `9dc2cf80835f8764885c3cb93ddc05d1ec8f51e7204c78784019d8f873262dda` |
| CDP修复范围 | `u29-cdp-transport-scope-acceptance-01/review.json` | `24dbee1097550f0307f576fec7d8776a005c61ac3b44bf8e15be36a6274adddd` |
| Web作者12步与冷恢复原失败 | `u29-c41-web-continuation-01/terminal-summary.json` | `5eac5a3b680b9848b819ddcd187235371773f9979c0736e7c0b4da2187638488` |
| Linux限定模块归属 | `u2-c41-http-module-diagnostic-readback-01/report.json` | `af5088a552595a6ccbd3ae01a4b71995e4bbae6e9f7fdc943bef21b2035cf7f8` |
| U2限制与替代验证处置 | `u2-c41-sanitizer-disposition-review-01/review.json` | `5eae209965f4171a31bc68059b7d499a4f489c7b1357fe5bfb795247b8d8c837` |
| SMA实际改内容 RED | `u2-sma-posix-save-regression-preparation-02/terminal-readback-01/diagnostic-report.json` | `9b682f001800de4a0c3e0942534bb45a41d4635d1735d8bf8766315107b442d5` |
| EAS三 lane失败 | `u2-c41-eas-detail-02/terminal-summary02.json` | `d41af721dc74786679733bde4625f8ca3fb4205334a42bd19044d5ac63f65a91` |
| Mac一次诊断 | `u2-macos-owner-diagnostic-readback-01/review.json` | `73410aeed92a7820a121d0abe4c4ca607787bddfe9be12c73b6d75a7b27504d3` |

剩余工作包括完整 Web 续接、SMA 字节回归、整合修复后的必要 Apple/候选验证，以及 U28 声明与逐项 U1–U29 最终核对。sanitizer 的原严格 FAIL 与本节限定处置均保留。设备、账号和物理输出仍按各自条件单列。当前没有合并、发布、修改标签或恢复 Studio。
