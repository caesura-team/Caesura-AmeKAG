# U27：Web 快照与恢复路径成本执行记录

本记录承接[唯一 U1–U29 计划](2026-09-05-001-refactor-runtime-foundation-plan.md)和[U15 截图生命周期](2026-09-08-005-u15-screenshot-lifecycle-execution.md)。本机共同性能问题已取得一次完整 Web **511/511、0 失败/0 跳过**的冻结执行证据；完整 U27 的 Release 多进程基线及一小时长跑尚未执行，不能据此标记整个 U27 完成。

## 固定诊断与原始失败

此前 U13/U14 的固定配对未建立版本回退关联，因此没有再次进行该版本配对，也没有先删除兼容性检查。以 `68a6e74db3317ea9610fa1767626cdeaceadfb35` 为冻结起点，固定 Node 22.23.2、原 `perf-baseline.test.js` 十二项、预热/三样本、配对顺序及全部严格预算，串行执行一次 C→P。C 仅记录 worker 元数据；P 在真实 worker 22516 内记录 V8 CPU 与 Lua 指令位置，未更改电源、优先级或亲和性。前后源码/依赖/工具身份一致，没有重试。

C/P 均为 11 通过、1 合成吞吐失败。C 的三样本为 2417.2/2224.3/2346.6 ms，吞吐 1.70458 帧/ms；P 为 2172.2/2190.5/2190.2 ms，吞吐 1.82628 帧/ms。帧是实际 scheduler tick，不是渲染 FPS。P 比 C 快不能作为优化证明；没有扣除或估算插桩开销。

诊断发现快照模块惰性加载，初始安装时捕获计数器未接上。原始 `snapshotCaptureCount=0` 与全零分桶均为 **NOT_MEASURED**，不能解释为零次捕获或每 250 次捕获的阶段分布；原始报告未被改写，未补跑 P 替换它。其他实际观察仍独立保留：合成 1000 行为 3000 tokens、4000 VM ticks、64 个历史点、2000 个已读标记和 2000 条 backlog；统计夹具中的 4001 不冒充 VM 计数。

三个测量样本的 Lua drive 约 1939/1942/1963 ms，最终上下文转换约 147/166/146 ms。14,262 个 Lua 指令位置中，已读全表复查 4563 个、深复制 702 个；指令占比不是耗时占比。V8 的 Wasm/桥接区间与这些边界共同支持以下有限修改，不支持省略完整已读扫描。原始 C/P、时钟对齐区间、未测修正及独立分析位于主目录 `artifacts/validation/u27/68a6e74db3317ea9610fa1767626cdeaceadfb35/source-profile-01/`。

## 快照分配回归

真实 `snapshot.capture` 先锁定 128 次捕获、每次 8 个变化的文字绘制值、相同缓存预热及 `<1024 KiB` 预算。仅在这个有界区间停止 Lua GC；度量是包含输出和尚未回收临时表的 Lua 管理堆增量，不是 GC 后保留堆、累计 malloc 流量或吞吐。

原实现为 2267.2 KiB；类型早退前移后 1196.2 KiB，仍失败；减少默认表及字段名数组后 1038.2 KiB，仍失败；最终为 1015.2 KiB，通过未修改的预算。名为 `green-01`、`green-02` 的目录实际退出 1，始终保留为失败。

修改限定为：

- 标量先返回，顶层空表不创建无用的复制映射；递归空表仍进入原映射，保留图中别名与环。
- 六个控制字段均为 falsy 时直接创建六份独立空表；任意 truthy 输入继续接受一次完整 `M.copy`，统一的 100000 节点预算、深度及循环校验不变。
- 固定图层字段名数组改为私有常量，实际字段和验证未删减。

增加默认输出隔离、递归空表别名、跨字段节点预算和深度正负边界检查。最终 47 项测试在归档基线模块上为 46 通过、仅分配预算失败，在修改后为 47 通过；五个相邻实际 Lua 套件为 23/82/316/36/42 项通过。独立审查确认没有调用者数据绕过验证。

分配切片冻结 `094909fc8a7936d37ff879a03b35ecdcaaeb64da` 后，原十二项单文件检查通过，合成中位数 1651.2 ms。但随后完整 Web 为 **504/506**：吞吐 1.75543 帧/ms、配对规模比 2.59137，均未达原门槛。该完整运行源码/夹具稳定，receipt `7c9fab89-867a-4451-9a9f-9e1bb420c047` 及 collector 仍为 FAIL，没有用单文件通过覆盖失败。

## 保持完整工作量的后续修改

已读扫描原本逐项分别判断值为 true 和缓存命中。私有缓存只含 true/nil，`next` 不枚举 nil，因此一次 `cached_flags[index] == value` 已同时证明“已缓存且仍为 true”。不等时仍检查布尔值、键范围和新增项，仍遍历所有条目并检查删除/同数量替换；缓存先失效再更新、失败后重建的顺序不变。52 项 Lua 特征检查在改写前后均通过，覆盖非布尔值、非整数键、元表钩子及原有错误恢复，属于等价回归，不伪造行为 RED。

Web 协程恢复仅对第一 yield 值为 nil 增加本地短路；所有非 nil 值仍经过原 JavaScript Promise 判定，`coroutine.status` 顺序及所有返回值不变。真实 Wasmoon 回归在旧实现观察到两次无意义的 nil 判定，其他值与 Promise 行为通过；增加短路后全部五项通过，加上原恢复/音频用例共 20 项通过。兑现、拒绝、owner 显式关闭以及迟到完成均经过真实 Promise 和 Lua 协程；没有模拟 resume 的返回结果。首轮夹具误以为 errored coroutine 会自行关闭，已纠正为由 owner 显式关闭，原日志保留，不声称修复了不存在于该 helper 职责中的自动关闭问题。

## 冻结的完整 Web 结果

源码 `a699bc5cceb6db4393cc3a24024d6dd592392122`，受控 profile `windows-web-u15`，run ID `ad5180f9-f78b-4e2e-897b-371bd93c38f8`：

- Vite 实际构建退出 0；完整 Web 38 文件、511/511，0 失败/0 跳过。
- 合成三样本 1584.7/1630.2/2171.6 ms，按既定中位数 1630.2 ms 通过原吞吐预算；未删除最慢样本。
- 三个配对规模比为 2.27179/2.34507/1.86988，中位数 2.27179 通过原 `<2.5` 规则。
- 原内存、工作量、统计负控制全部通过，源码与夹具前后稳定、dirty=false；collector 与严格 verifier 均 PASS。

原始记录在隔离工作树 `artifacts/validation/u15-web-final-02/`；证据 manifest 位于 `artifacts/validation/u15-web-final-evidence/a699bc5cceb6db4393cc3a24024d6dd592392122/ad5180f9-f78b-4e2e-897b-371bd93c38f8/windows-web-u15/`。此前 `u15-web-final-01` 的完整失败继续保留。此次 PASS 不证明固定改善百分比、所有主机性能或 Release 三进程结果；CI 和最新原生完整候选继续独立验证。U27 原计划的至少 10 样本、3 个独立进程及至少一小时长跑仍是明确未完成项。

## 2026-09-20 测量入口修复与真实短测

主工作区u27-current-performance-audit/formal-readiness-01.md/json重新核对后确认：上述a699收据的configuration实际为Debug，历史Web完整套件不构成正式Release多进程基线。d447的生产Web ZIP/TAR原件及摘要仍可核对，但没有正式性能样本；当前compare_benchmarks.py、run_engine_soak.py与长跑fixture尚未实现。三进程/每进程十样本的版本交错比较及一小时真实后端长跑继续未完成。

既有perf-bundle计时器同步调用async播放函数，回调丢弃Promise，因而没有测到场景完成。以原函数机械抽取后先做确定性回归，10项中9失败：首个barrier释放前已启动8项，最大并发8，完成时钟应为5/6ms却得到0/0ms；错误未传播且资源未dispose。修复将warmup与各样本串行await、所有调用者返回Promise、三个入口等待计时器，并注册await dispose。回归10/10通过；原次数10/6/6、各路径一次warmup、偶数样本上中位数、数值阈值和超时保持。源码/原RED/GREEN/独审保存在u27-worktree/artifacts/validation/u27-bundle-timing/，独审无可行动发现。

在独占本地资源窗口对修复后的真实Wasmoon入口执行一次，3/3通过、0失败0跳过，test29.93秒、Vitest30.39秒，owned Node正常exit0、无timeout/force、cleanup COMPLETE。tiny源/compiled-token bundle中位数6.4/7.0ms；story为965.8/908.5ms、bundle/source吞吐比1.063；synthetic为1288.5/963.7ms、比例1.337，两个比例均满足原>=0.8。原JSON摘要a6b4de6cea7ba0db2bfb80aca3bb7cb610328e688d3b49afc9f894c7fdd33d87，real-perf-01-outcome.json绑定ce5基线上的三文件dirty补丁，运行前后fingerprint一致；不能改称干净候选或正式3×10验收。

独审指出原日志将tokens/ms误标为tok/s；之后仅修正两个输出标签及显示小数，计时和断言未变，代码提交da9d833aad97b9440b7d5b3121439f18973b7439。原短测文件SHA和旧日志保留，完整新Web套件尚待执行。这些是Lua compiled-token bundle对source的短测，不是Vite生产包、浏览器、真实GPU/音频或soak结果，也不建立跨版本改善结论。

## 2026-09-20 完整 Web 维护验证与后续整合

在干净源码a1575ec760a8d13729c70d830dd9bcdb1acd8827上执行完整Web套件，首次attempt01为605通过、34跳过、无测试失败，验证驱动明确返回FAIL。新工作树缺少cache/story与web/dist，导致相关用例未执行；该次outcome摘要23ab4cbeb260c8a4647e15f8706a896d11c0fd8d222ca4964e987b2c6f6e4ccb保留，没有以Vitest自身exit0覆盖维护门禁失败。

之后用已锁定的实际Lua工具执行ks_bake（24场景、6资产），用该工作树Vite构建Web分发资源；两个受控命令均exit0、cleanup COMPLETE。工具只承担生成夹具职责，不将U23的Lua工具二进制标为U27新构建。生成目录和工具身份在第二次运行前后核验。独占资源窗口内attempt02完整结果为638/638、0失败、0跳过，实际Node exit0，无timeout或forced kill，owned tree cleanup COMPLETE；起止1789846077.9407818至1789846250.6753986，源码dirty=false，前后fingerprint均为b5d432d4d2f9c7ff724fb2f5354b5ce731a9f0f7015b048e0bb1363f18f9e007。原Vitest JSON摘要c80211b2415af92042b439c8cd47a4de2d01a24b2edbb8dd350e8803a9155925，outcome摘要1788aeb24cca603259b1000a696d971285fa6c66bdd87b4e71c93df1da05f636；原件位于u27-worktree/artifacts/validation/u27-bundle-timing/full-web-a1575ec-02.*。资源存在后原缺资源提示占位用例不再注册，因此639变为638；未降低发现门槛或修改跳过规则、性能阈值。

该结果只验收冻结源码的完整Web维护套件。随后merge e5dd83a19ceadf8c0f34e668c85c65e904fc6050整合U23 c0045049及U22 d418845d的进程与临时目录验证修正，冲突仅为文档同步锚点，Web三文件保持；不将a157运行改标成新merge的完整验证。正式Release多进程采样、配对版本比较和至少一小时真实后端长跑仍未完成。
