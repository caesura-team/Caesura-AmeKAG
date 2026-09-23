# U27：Web 成本、正式 CPU 比较与长跑执行记录

本记录承接[唯一 U1–U29 计划](2026-09-05-001-refactor-runtime-foundation-plan.md)和[U15 截图生命周期](2026-09-08-005-u15-screenshot-lifecycle-execution.md)。2026-09-20 当前状态：冻结源码的完整 Web 维护套件已有 **638/638、0 失败/0 跳过**；首次正式 Release CPU 比较已保留六个独立进程的全部 180 个正式样本及 36 个预热样本，三个工作负载均通过事先锁定的噪声与 10% 回归规则，原始数据独立复算一致。真实后端至少一小时长跑仍未执行，整个 U27 未完成。下文保留各次历史失败与当时的未测状态。

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


## 真实 CPU 采样协议与比较器实现

三个原CPU doctest用例保持普通运行行为与原上限；仅在六个显式协议环境变量全部有效时启用原始JSONL输出。每个进程固定三个指标、两次warmup与十次measurement，共38条header/sample/footer记录；测量使用steady_clock纳秒。Lua格式化10000次必须返回10000，table读取10000次必须返回1515000；SMA固定8192顶点、每block十轮变形，所有结果有限且以显式little-endian float32分量序列求SHA256。正确性检查和JSON输出在计时区间外，不人为填时长或截掉慢样本。缺变量、非法值、执行错误或结果不正确均失败。Lua状态由RAII回收，同一指标的十二轮保留同一VM及默认GC策略。

原实际Debug二进制执行0但没有协议记录，原始RED保留；新增维护测试在旧二进制得到4方法中的14个失败子例。新Debug目标构建成功后，维护4/4通过（4.862秒），独立实际owned正控获得38条记录、exit0、cleanup COMPLETE。最初配置自动VS实例失败及缺SDL库导致的头文件失败原样保留；明确选定已安装VS实例和原选定SDL3 CMake包后才完成构建。该结果只证明Debug协议，不是完整原生门禁或Release性能。采样器冻结SHA256为19a429266651c694e6c7af06dbe0707c0f143351b023e31559a555f056a86362。独审确认工作量、输出与原断言；仅将完成标记移到十二个成功样本之后，没有已证实的假通过反例。独审u27-cpu-sampler/independent-review-01.md摘要3cc9be06ca458ba49182871a1cbdc047c2c6db4dd5c7c8674444cdba2ab68a26。

比较器要求外部锁定的干净production/measurement SHA、相同sampler、完整Release U1原回执与收集件、真实二进制与runtime输入、机器观察、工作负载和事先确定的数值policy。采样固定AB/BA/AB次序，base/candidate各三独立owned进程、每指标2+10轮。原始stdout逐条重读，检查连续编号、结果与SMA摘要，不用汇总值代替原样本。十样本取上中位数；三个配对进程比率结合预声明的组内RMAD、跨进程离散和顺序漂移判定PASS/REGRESSION/INCONCLUSIVE。缺失或不一致为FAIL，baseline-only不评价候选，fixture始终FIXTURE_ONLY/77、gate_pass=false。

独审真实文件反例确认两P2：比较中晚改源码、构建绑定或stdout仍能返回fixture比较PASS；拒绝源码内output后异常路径仍写FAIL文件。两个新增方法在旧实现得到7 FAIL；修复后完整Windows30/30、29.536秒，WSL30/30、7.750秒，零跳过。比较结束重新校验_source/_build及全部stdout/stderr/runtime_request/run_receipt；输出保护在异常写入路径外先执行。独审八个控制确认正常fixture仍PASS但gate=false，源码输出完全不写且Git保持干净，六类晚改都FAIL/INVALID。原RED、fixture和原作者记录未变。最终比较器SHA256为8906b1b9a0f46d26769c4eb5dbed150cab3e97e837a5e629d8b986951708bf22，测试为460e643a8da9aa73c58635d9f4177baec881d6e70469542ecb0e3b2882fa83d7；独审u27-formal-design/independent-review-02.md摘要b304723425ed5d3a8e9fd6be1faad0fe7d2f828c35b83456050337b78d821922，无剩余可行动发现。

两个新CTest入口已注册，实际发现59唯一项；六native profile的CTest最低发现数57升为59，adversarial的57保持。首次两个入口实际2/2通过（24.63秒），其中比较器当时为独审修正前28方法版本；不把该CTest结果改标为后续30方法版本。最终新候选仍需完整Debug/Release门禁、正式三进程十样本比较及至少一小时真实后端长跑。当前无正式跨版本性能结论，覆盖率未测量。


## 首轮完整门禁与事先冻结的测量输入

干净cce3a7e794dd6f5b21f49ee57ee3cf83b0b9c03f的首轮完整Debug运行b418c1c1-8aa4-4e85-bc64-5c6a094e9880整体FAIL，run.json摘要fb19080c7de76fbf015be0e231bc35f3fd9c6b29c9045bff9ca2e830b06f6f47。全量构建、C++1415、Lua147/56及其余profile检查通过；CTest59项中两项Bash入口失败，一个预声明AI跳过，其余通过，767.44秒。真实CMake cache选中了C:/Windows/System32/bash.exe，两个入口均因WSL调用环境退出127。原runner、collector及strict verifier保持FAIL，不能拼接定向结果改绿。后续明确配置Git Bash并在新的完整run复验。merge c13451c461b4d352e00adbfbac8d0df0f02b95c2整合U23 8950b4f0的原API响应保存与U22兼容性增量，未改变sampler或既有Web成本实现。

正式Release测量前已锁定D:/caesura-u27-inputs-01/policy-01.json，SHA256为5d58a21bb0f1e84d35ac230f897725d671fb2da445fbe8891f25636598ca033a；workload-01.json摘要95da28f83cb0f13b7f84695088c9e53d5f15cc1a4d55bcc5b343fd01a685ab31。三个CPU指标统一允许10%可重复退化预算；组内RMAD不得超过10%，三个进程中位数spread不得超过20%，首末顺序drift不得超过15%，任一measurement固定block少于50微秒则INCONCLUSIVE。数值在任何正式Release候选样本采集之前确定，不根据结果调整。源码/算法/工作量与policy独审无不一致，报告u27-formal-design/premeasurement-review-01.md摘要4afd8c23693f001d33adf0566d557b8e0b58f6fffc4f081ba8a05dda04742b09。50微秒是样本时长下限，不是时钟分辨率。

机器原始观察锁定Windows11 10.0.26200、AMD64/16逻辑CPU、原进程0至15亲和性及现有High Performance电源计划GUID；未修改电源或亲和性。machine-01.json摘要f18d52b48231c7b836caea3501f97ea9c4d4c001ba37eb705db404f696ea37f8。正式测量要求先结束本轮所有自有构建/测试/浏览器/Android流水线，再顺序运行六个sampler；不声称用户/OS后台活动全部消失，超限噪声按冻结规则拒绝。

首选production基线65e5b425的sampler-only overlay d48aaf90ec79723223a26eaa0fc925e1a8bc35f8确实只改一个测试文件，sampler与candidate相同且src/scripts零差异。其首次Release全量构建及C++1404/1404、402726断言通过；但完整U1运行5468be18-bb33-4e6a-97c9-aa900863933e被严格门禁拒绝：CTest33项中原HTTP smoke意外SKIP（原脚本丢弃Engine输出、rc1后标NO GPU），AI为预声明可选跳过。原301.89秒CTest和整体FAIL保留。没有原Engine stdout可定位rc1，不能将其认定为无GPU或把该baseline投入正式接受比较；不降低HTTP required检查或改阈值。


## 2026-09-20 首次正式 Release CPU 比较

首选 U21 baseline 的完整门禁失败后，在正式采样之前的 2026-09-19 20:48:01 UTC 明确改用 production `d418845dc62dfb9d24a04927d0fd030bedfa8ec8`。其测量 checkout `b008bc387739f50319f8095504f65fa189655b1c` 仅覆盖同一 sampler 测试文件，生产 src/scripts 没有变化。候选 production/measurement 均为 `93b8ddc2f76db3c2a851367d816daed013311926`。原 policy、workload、machine 不变；baseline-selection-02.json 摘要 `c7a29f61086b93fe4bd46a33aa025f2f6bf057dcc81f5fdd164c0eaa2f015806` 保存先后关系。此次比较的基线是较后的 U22 提交，不能称为对原 U21 baseline 的通过结论。

候选干净 93b8ddc2 的完整 Debug run `4e2368d2-cc2a-40dd-8ef9-4dce25b454ab` 与 Release run `02c45af0-9031-4ccd-a7c2-862a76b1a7ab` 均完成全量构建、C++、Lua、CTest 及其余 profile 检查，runner/collector/strict verifier 均 exit0。Release C++ 为 1415/1415、427077 个断言、0 failed/0 skipped，Lua 为 147/147 与 56/56，CTest 为 59 个发现、58 通过及 1 个事先声明的可选 AI 跳过，632.22 秒；实际 HTTP 检查通过。新 base Release run `065a4a1e-a0f5-4656-a66a-6e5d8ae3a90c` 同样通过严格 U1，C++1415/1415、Lua147/56，CTest48项为47通过及1个预声明AI跳过，582.36秒。未用定向结果拼接完整候选通过。

正式请求 `D:/caesura-u27-inputs-01/request-01.json` SHA256 `a31701e1a7d698bea148da17051f59fabf6738783c7427458a203847e9f7236d` 绑定两侧干净源码、完整 Release U1、实际二进制和相邻 SDL3 DLL、机器及工作负载。base/candidate 二进制摘要分别为 `1bdd5a2673417eb01082fe0f33de1abb8abcd1ea50a10c76354cd492532e1b24` 和 `2f4d31db938ed9a634129c6140d818ab6442a0156f40bebf994374e51bbda1a4`。采样前本轮自有构建、测试、浏览器及 Android 执行全部结束；机器和输入在执行后再次核验一致，不主张所有用户/OS后台活动均不存在。

唯一一次正式 collection 在 2026-09-19 21:20:20.305240 至 21:20:24.636790 UTC 按 AB/BA/AB 执行。六个 UUID 和 PID/creation 身份不同、时间区间没有重叠，均 actual exit0、cleanup COMPLETE，无超时、强杀或 stop 请求。每个进程三个指标，各保留 2 次预热和 10 次正式测量；180 个正式样本与 36 个预热样本全部进入原始记录，没有剔除慢值、追加测量或替换结果。每个采样进程实际只选择三个 Perf 用例、1412 filtered，不能改称其执行完整 C++ 套件。72 次 SMA 结果摘要全部为 `4fdacb2d04bfab2fa46c980232d7c15827dffd151238b5bd4144c24e02fd52ef`。

| CPU 工作负载 | base 三进程上中位数（ms） | candidate 三进程上中位数（ms） | candidate/base 配对比率 |
| --- | --- | --- | --- |
| Lua format + append，10000 | 4.5450 / 4.5666 / 4.5091 | 4.4098 / 4.5212 / 4.6799 | 0.970253 / 0.990058 / 1.037879 |
| Lua table reads，10000 | 0.2546 / 0.2436 / 0.2623 | 0.2542 / 0.2600 / 0.2490 | 0.998429 / 1.067323 / 0.949295 |
| SMA，8192顶点×10 | 1.5556 / 1.5827 / 1.5687 | 1.6004 / 1.6150 / 1.5929 | 1.028799 / 1.020408 / 1.015427 |

实际最大组内 rMAD 为 6.7382%，最大进程间 spread 为 7.3449%，最大顺序 drift 为 6.1250%；所有样本均满足固定 block 时长下限，三个指标的全部配对比率均不超过1.10。结果为 `MEASURED / PASS / fixture=false / gate_pass=true / release_ready=false`。这只说明这三个 CPU 工作负载没有超过事先锁定的回归规则，不证明引擎整体提速，也不覆盖 GPU、Web 或音频性能。

原 collection `D:/caesura-u27-formal-01/collection.json` 摘要 `f9d877d08aeb1bb1948744e998cf5dd8ff54a90a88ffc99c0d71728ba7c10c87`；原 comparison `D:/caesura-u27-comparison-01.json` 摘要 `c050a5d7c3776ed4b284162900a30d8be234069654cb38acde708779f5744272`。u27-worktree/artifacts/validation/u27-cpu-sampler/formal-independent-review-01.md 摘要 `7fbbca54fcc3b82969dc6c2a244b4184c74eb2dfbe1a6dae00a17ed2117dae46`，JSON摘要 `80c5e7689b6e21b875ca0e3c3cc112367fd6f868594bc30df1367b80c8efedf8`。独审重新验证两个完整 Release U1，重读24个子进程原始引用并独立计算所有中位数、rMAD、spread、drift和配对比率，逐值一致，无可行动发现；它是保存数据的复算，不是新的独立性能测量。首个审阅脚本将 Lua table 结果误设为100000，按原源码及原样本更正为1515000后才完成复算；未改产品数据。

为保持原始 comparison 可重放，两侧测量工作树继续冻结在93b8ddc2和b008bc38。后续长跑实现转入独立 u27-soak-worktree。JobSystem 回调队列与当前执行 batch、异步资源、真实渲染/音频资源、Lua heap及RSS的静止点观测仍须落实；目前没有真实后端一小时结果，U27 与整体 U1–U29 仍未完成。


## Job 与 Async 所有权快照回归

为真实长跑的静止判定新增 owner-thread Job/Async 只读快照，Null 与直接替身明确 unsupported。Job 分别记录 worker pending、排队 completion 与当前派发批次；Async 同时锁定容器读取 waiter、inflight、buffered 与 cache 实际数量，cacheBytes只计载荷数据，不代表GPU或分配器总量。取消后的worker及宿主转移载荷须由其他所有者继续计数，不能据loader容器为零宣称全局空闲。

最初空实现实际10方法1通过9失败（553断言、152失败），实现后10/10及相邻84/84通过。独审随后发现shutdown在main mutex内析构排队回调捕获对象，析构回入snapshot会异常终止。新增真实捕获析构回归在旧实现编译成功后实际exit1，doctest报Terminate handler called，18条之前的断言通过；25秒超时没有触发，未将其误记为超时死锁或猜测异常类型。

修复将待取消deque在锁内转移，锁外销毁，所有权计数在整个销毁过程保留，最后还原外层派发数量；不执行取消回调，不清零重入栈上的既有债务。修复后的新增快照11/11、599断言；六文件相邻85/85、1244断言，原1415/1341项分别为过滤未选中。实际进程退出0、无超时强杀、cleanup COMPLETE、源和binary稳定；独审P2关闭、无其他发现，报告u27-job-snapshot/independent-review-02.md SHA256 `9984bb7bf57dff6dcea2f5ec1ca554fa0a701c5b299bba1ff27f8f654ace2d2d`，JSON `e6234270fd83e14173a7d0a47000c0dbdf3392a5cc8143400e7db77e627d719f`。原RED、首次GREEN与修复后GREEN分开保留。

## 宿主快照首次真实 RED

新的IEngineHostSnapshot纯虚接口区分direct drain与普通SDL事件交付，普通SDL队列所有权暂为incomplete；计数设计包含deferred、当前draining与dispatching载荷及实际完成的owner-loop帧，重叠阶段不相加成资源总量。空getter冻结后主代理实际完整编译CaesuraTests成功，八个新增方法全部失败，530断言中371通过159失败，1426个既有方法过滤未选中，退出1且清理完成。原三测试文件既有3+5+26方法主体保留，首次RED记录u27-host-snapshot/build-and-red-01.json及原stdout/stderr。生产计数实现仍在推进；这些Null/确定性后端回归不证明真实GPU、音频或一小时长跑，RPC接线及其余资源快照仍未完成。


## 宿主与真实 ManualMix 音频快照联合 GREEN

宿主getter补齐后八个既有RED方法全部通过，530断言；普通SDL事件路径仍明确async ownership incomplete，successful owner-loop帧与renderOneFrame/export计数分开。作用域计数覆盖实际载荷销毁，取消/重入不清零外层所有权。音频API随后以真实SoLoud ManualMix建立5方法RED：585断言中157失败，全部是新增snapshot字段，真实mix/完成/配额等先决断言通过。实现只读getter后统一重建CaesuraTests；Audio/Host/Job/Async合计24/24、1714断言，12个相关文件270/270、37879断言全部通过。分别1415/1169方法为过滤未选中，不能当完整套件。

联合执行源码在93b8ddc2的未提交快照，fingerprint a35dc5fbb95d7b78d8c33123cba4ef74d479a2b75fd2fee69c0bda6e494eed3d首末相同，锁定文件未变。构建与两测试进程均exit0、无超时强杀、cleanup COMPLETE；binary SHA256 38b5c53ef1b8175e250dbccb034fb18774c3aaffa6c53081432ae98f5ed43bde。原联合收据u27-audio-snapshot/integrated-green-01.json SHA256 215ab31462201fcac5e811ad564046965d6a871123bc223c28d311b5afb6bb8e，根代理只读审查root-review-green-01.md摘要9a9374af0d2eac4ea10b9798a2a4c528a086ebc9316e179a39f7a35c89a636b9，无可行动发现。

音频getter保留尚未cull的owner记录、退役句柄、缓存、待消费通知与恢复source，Null明确unsupported；SoLoud各读操作内部加锁，不声称Device跨字段原子。静止判定还有Engine宿主已接收通知、当前Lua回调和registry owner引用这一具体缺口，已形成audio-host-debt-plan-01.md并继续真实RED/GREEN。物理音频、GPU释放、RPC完整导出、干净完整候选及一小时真实后端长跑仍未完成。


## 宿主音频通知所有权的真实回归

新增六方法在冻结空字段实现上实际 RED：1152 断言中 181 失败，全部属于新增支持位、pending、active 与 Lua owner 引用观测；真实 ManualMix、暂停、runner 替换、回调异常及安全 shutdown 的先决断言通过。生产修复仅在后端通知转交与 Lua 回调派发周围保留作用域计数，并由 getter 读取；取消/清理不清零仍在栈上的 active 债务，原容量上限未变。接口与六方法在 RED/GREEN 之间逐字节不变。

修复后统一重建并执行 30/30 新增快照方法（2866 断言）和 12 文件 276/276 相关方法（39031 断言），均 0 failed；1415/1169 项分别为过滤未选中，不代表完整套件。构建与两个测试进程分别 PID3184/29140/23584，均实际 exit0、无超时强杀、owned cleanup COMPLETE。源码 fingerprint a0f96c20b75e17c8070aad69f8a6d616d797f6fb888c874d52451208e75c9955 首末相同，26 个源码锁及原日志回读一致；binary SHA256 18f5f0864b4f8a6ea2dc6d6344317d94234f394ef02669cc36179b5f8beb7363。

原结果 u27-host-snapshot/audio-host-debt-green-01/integrated-green-01.json 摘要 df11bd3aa4c4d353074975f8edffdb110dea0099b28509b0267d6ad46736f88d；root-review-01.json 摘要 7baef6572f5d858f25d7cbcba9e6c956426beabcd4b5a80752c0478d6cb9a790。根代理审阅无可行动发现。此结果关闭本轮宿主音频通知观测缺口；物理音频、完整渲染资源与在途截图回调、RPC 导出、干净候选完整门禁和一小时真实后端长跑仍未完成。

## RPC 快照导出与真实 main 验证

RPC DTO新增jobs、async_loader、host、audio四组38字段，两个transport共用只接收值对象的序列化helper；原7字段和stdio嵌套/HTTP平铺结构保持。main在原OwnerRpcQueue executor中通过BackendRegistry访问三个后端并引用Engine接口，四个快照各读一次，完整复制Host四个音频通知字段。整数保留uint64，不经过浮点或有符号中间转换；未知枚举保持Unknown，SDL/Unknown不能被提升为完整所有权。读取不推进帧、不混音、不排空或等待；四个独立观察不是全局原子空闲状态。

9个新增方法在空字段实现上取得有效RED：4通过5失败，629断言中60失败，精确为12个对象大小差异及48个缺失组；所有前提通过。实现后实际同时构建CaesuraTests和CaesuraAmeKAG成功，9/9、2381断言及三文件邻接86/86、3184断言全通过，1445/1368方法分别过滤未选中。源及17项锁首尾相符，全部命令exit0、无超时强杀、owned cleanup COMPLETE；`u27-rpc-snapshot/run-green-01.json`摘要c24f1d26587e7ce0d668cde6378b2b9f72f3eec88f786e2e9a4435f50c0e96f5，main二进制2e2ce899319d36d995a78e5024ab86cf2baef3046d281c6335b7048ed513cbb9。

随后对同源同binary执行一次真实HTTP和一次stdio main smoke，分别独占进程和目录。HTTP `--editor`实际返回Device音频supported/running，全部字段及类型满足合同，owner帧从0增至3；PID6476、creation134343374525878081、受控STOPPED实际exit1、无超时强杀、cleanup COMPLETE且监听消失。stdio `--headless`实际返回Null音频unsupported/unknown，帧从1增至5；Engine PID11184、creation134343375107010412收到stop后退出0，stdout reader完成；其独占Python控制器PID3052也exit0且整个owned树清理完成。没有用HTTP受控退出1冒充正常退出0，也没有把Null计数当真实设备负载。

两路径锁定21份源码/helper、368份运行输入56727456字节、请求和原构建收据，前后全部匹配。根代理复算各自14/18份原始证据并解析真实请求/响应，`main-smoke-01/root-review-01.json`摘要6278ab2b6dd6634796ae819bb9a44908e5a56e6785cec2fa25c8ec705072138a；没有剩余可行动发现。实际main未注入uint64极值，极值精确序列化证据来自独立transport回归。以上不证明物理可听输出、GPU fence、普通SDL事件所有权完整、一小时真实后端长跑或完整合并门禁。Renderer资源与独立在途截图callback债务仍继续按既定计划实现。


## 入口边界回归修复与新 main 复验

干净 77f7c0d3 完整 Windows Debug 运行 75b3db72-de10-436f-8696-981d6b8c3ef2 未通过：1454 个 C++ 用例中 1453 通过、1 失败、0 skipped，432335 断言中 1 失败。唯一失败为原有 Main entry point uses texture manager interface，禁止 main 直接 include di/BackendRegistry.h；CTest 同一用例对应失败，57 pass、1 fail、1 可选 AI 服务跳过。Lua 147+56 全部通过。原失败收据、collector/strict 非零及原测试正文保留，root-review SHA256 9803308be6222d854c9f558d85f89cc8091fd98e103f645d7f190447f625a2a0。

修复将后端快照读取移入 entry/RuntimeStats.cpp，main 在原 owner executor 中接收值聚合并执行原序列化映射。entry 仍只依赖既有 API，未新增 RPC 依赖；四快照各读取一次，无帧推进/混音/排空。原 source encoding 测试逐字节不变。新增四方法覆盖全部字段值拷贝、缺后端、实际 owner pump 及真实 Job 回调重入完成债务。

实际重建 CaesuraTests 和 CaesuraAmeKAG 后，新增四方法及原入口约束共 5/5、109 断言通过，三文件 RPC 相邻 90/90、3286 断言通过；1453/1368 分别是过滤未选中。源码 fingerprint a89b34e38277137892ac0eb4a828725f175d2435ccb95ff0b8aa674626629a35 首尾相同，17 个源码锁、六份原始流重读相符。main SHA256 225004739490bd6d4a377af4d97fffdd05f8336bb2de8bbe07e37344c23025b4，tests SHA256 bab7bd5a2ea55e1a354787abfe1deb283b95be3f37cc13293e4235bea6b0ac43；所有命令 exit0、无超时强杀、cleanup COMPLETE。

新二进制执行 main-smoke-02：HTTP PID23028/creation134343408124868258，Device 模式、owner 帧0→3，受控STOPPED实际exit1且监听已释放；stdio Engine PID18652/creation134343408710021116，Null模式、帧1→5，stop响应后exit0，reader已join，控制器PID12284也exit0。30个源码/helper锁和390份运行输入首尾相符。根代理重读两条路径14/18份原始文件及响应，root-review-01.json SHA256 0efb5eb8a24ed6c3e48f1175971cc437d61f588de4c80501b2c96c0ca9d0e463；新请求SHA256 7e7221bfd3c6554681954749e790f15b690136eae9c0fc377d37ec93cecab151。没有把旧 main smoke 当作入口修复后的证据。

六个 C++ profile 的发现底线同步增加本批43方法（39快照/RPC+4入口回归）；这是最低发现合同，不是声称已在所有平台执行。后续干净完整门禁继续运行；Renderer资源、独立截图回调债务和一小时真实后端长跑仍未完成。


## 入口修复后的完整门禁

干净提交661da729ee72ae2f35f4d324f5fc012e045a8b3e的Windows Debug run65ec8807-b306-4ead-83eb-746a9ef94f20实际完成：全量build0，C++1458/1458、432437断言、0failed/0skipped，Lua147/147+56/56，CTest59项中58通过、0失败、1项预声明可选AI服务跳过。所有11检查及collector/strict verifier通过，源码与fixture首尾稳定，owned进程均正常完成无超时强杀。

根代理复核49份原始引用84907218字节，manifest SHA256 8e0850cb3f5c87ce6730559438b2eebb394761201869dd8d7f8da7bace40496e，原run 188b0678bd8f371b9bc15dfea8553109960a2ed62ec2541974b83092c33f3559，root-review-01.json bd1489e72885b0c73bd370d060a5e60a914bf71787f49b7fa1607298be9e8bf3。此完整结果关闭前次Main入口约束失败；旧失败原件保留。Renderer队列/资源观察的12文件8方法候选随后应用，开始独立空getter RED，不能借用本完整门禁宣称新Renderer实现通过。


### 2026-09-23 — renderer snapshot / readback ownership / real D3D11 observation

基于 661da729 的未提交后续实现已完成以下定向验证，尚不沿用前一干净提交的全量门禁作为新实现通过证据：

- Phase A 为公共 renderer snapshot 增加 11 项 bgfx allocator 计数、进程唯一 context generation 和截图队列 waiting/submitted/terminal、预留及 PNG 逻辑字节。Null 后端明确 unsupported；getter 不推进帧。8 个新回归先失败，GREEN 为 8/919，邻接 181/5910。
- Phase B 增加实际 callback publication/execution ledger，按 context/request/name 匹配，RAII claim 横跨真实 PNG 完成路径；取消、take、reset、超时不自行清偿 native obligation，实际 bgfx shutdown 返回后才退休未执行项。6 个新回归 RED 为 6 失败，GREEN 为 6/939；Phase A 8/919 和邻接 187/6849 同时通过。原始记录见 `artifacts/validation/u27-render-snapshot/phase-b-green-01/root-green-review-01.json`（SHA256 ede74c789bbd4dd90742d6665a9aaa85a317dd8d06d5fa61f84614805e311324）。
- Phase C 保留原四场景，新增 opt-in `ownership` 真实 hidden SDL HWND / Direct3D11 / Engine / TTF / texture / RTT / PNG 场景。第一次构建 C2668 发生在新 probe 的 `requestScreenshot({})`；显式写成 `ScreenshotOptions{}` 后编译通过，原失败和唯一 harness 修正保留在 phase-c-actual-01/02，没有修改 production、断言或预算。
- 实际 Phase C 68 个精确检查全部通过；8 帧预热基线中 texture=5、framebuffer=2，保留额外 texture+RTT 后为 7/3，同一谓词拒绝；销毁后第 1 帧所有 11 项计数精确恢复（预先上限 16 帧）。context 从 1 到 2；旧完成截图跨恢复保留，take 后队列与 readback debt 归零；新上下文真实 PNG 像素、TTF 与恢复前一致。warm/held/fresh 均为 640×360、11540 字节，提交帧为 1/10/11。
- 正式 driver 独立复核原始向量、帧序列、PNG 文件、loaded executable/SDL/D3D11、构建 receipt 和源码身份，原生 PID23632 正常退出0且 owned cleanup COMPLETE。根重新核对 22 个产物引用及源码锁。原报告 SHA256 11483cca8d1931dbceb440fd0dec410dc64a690b1e3357dd9ba275246509888f，原 probe result SHA256 7a8753800590c76ec3aa19148c60005d4195c43eb5152fbfbb2e3bda26abbd71。

这是 allocator handle 计数与逻辑截图所有权证据，不是显存字节或 GPU fence。native 未回复请求的 shutdown 退休、post-Core 字体恢复失败、真实 OS device removal、其他后端/平台和一小时 workload 仍为 NOT_MEASURED。RPC render 分组、实际主程序接线、全量门禁及一小时 soak 继续执行；U27 整体未完成。C++ 最低发现门槛按实测新增 14 个方法调整为 Windows1472、Linux1428、macOS1315；CTest 门槛未改。


### Renderer RPC 与实际主程序接线（2026-09-23）

Entry通过BackendRegistry取得renderer并恰好读取一次owned snapshot；main逐项复制到不依赖render实现的RPC DTO，共用serializer发布render的13字段、resources的11字段和screenshots的6字段。保持19个uint64精度及原始能力标志，Unknown/未来枚举不推导readiness或全局idle。

五个新方法在旧实现先得到1通过/4失败（5686断言、170失败），六个原stats方法为1通过/5失败（2335断言、24失败）。实现后五方法8794断言、原六方法3667断言、相邻167方法17044断言全部通过；筛选未执行部分不计为完整套件通过。测试binary SHA256 7ee6434302617318ce2a4cd49cb35f6437ff0b569e6403ee5d978c74a4c1a40d，源码首末稳定，根收据SHA256 ee46e702867ac1e48fda26fc1c0ee23e8aacbeb3b90588a0d74157c2d3a3ce58。原renderer独审只发现过时Phase A注释，已修正文案，未改变行为。

新构建实际main SHA256 a96b092515b405e5c5ab7a0bb22b2ff372da5cb5875ec0d488a505a7e427786e，锁定46个源码/helper与409份运行输入。HTTP默认Editor实际Direct3D11支持和资源计数通过，stdio默认Null明确unsupported且计数为0；owner帧均0→3。原始响应完整、没有注入极值，受控退出与进程身份被核对。HTTP报告8bce741e404c2a6ea9e1884b0985ee8134807676a70a9ef384b7af40b135e3dc，stdio报告13cdc2aedea1795b9587d5cd8e25b3225fe5ea8225a49b9abb178ac48043c375；根回核32原始文件，收据430590bbe65a31660d0a75e21cb70d2590e6ef87c876ab38b0961ecf3110d353。

最低C++发现数现按新增五方法调整至Windows1477、Linux1433、macOS1320。检查发现前次14个renderer方法仅写入Debug门槛，Release仍为原计数；这些方法无Debug限定，因此本次同时补齐Release的19方法增量。计数为跨平台注册合同，不宣称其他平台已执行。最终完整门禁、一小时真实后端长跑、native unanswered readback与post-Core失败仍未完成。

### Renderer/RPC 集成后的完整 Windows 门禁（2026-09-24）

干净提交 `3c5b8c0a76fbde101554550e6a2c2a06db9ec937` 的完整 Windows Debug 验证已通过：全量构建退出0，C++1477/1477、444441断言、0失败/0跳过，Lua147/147及56/56通过，CTest59项中58通过、0失败、1项预声明可选AI服务跳过。execute、collector和strict verifier均退出0，进程所有权清理完成，无超时或强杀；源码与运行输入首尾稳定。CTest用时657.33秒。

运行标识为 `8f1c685b-4a76-4a36-8527-d8ccb14f49da`，源码fingerprint为 `799fe91583e786b7fc97943210d676109bb9ea10fceba3614f8ef84f9f02ac8f`。原始 `artifacts/validation/u27-render-full-01/raw/run.json` SHA256为 `90871d5ad7a6c818f9fea8ab99900c17264db52e93b343bd1c96d370ea32b481`；根复核 `root-green-review-01.json` 重新校验29份原始流/报告及CTest XML。此结果覆盖该提交的renderer/RPC集成和计时修复，不替代尚未执行的一小时Release真实后端工作负载，也不提升native unanswered readback、post-Core失败及其他平台的证据状态。U27整体继续保持未完成。
### 2026-09-24 — 原生长跑静止点检查器

新增纯观测检查器 `engine_soak_contract.py`，先以空实现运行14个冻结回归，真实得到94个失败子断言；实现后同一测试正文14/14通过。检查支持/完整性标志、实际Device/Direct3D11/DirectDrain模式、异步与音频逐阶段债务、截图队列与原生readback、11项渲染计数及固定缓存/内存预算；缺失、未知、负数、浮点和布尔冒充计数均不默认成零。上下文代次变化要求显式新基准。RSS与OS句柄只作为诊断计数，不替代private bytes。

root重算红绿原流、源锁及保存的源码，确认测试正文未变；`quiet-root-review-01.json` SHA256 `99ab642916094a6562788e1490c72dab2b2e7fad9fa49be4b3cc79e2de16d978`。新增CTest入口实际执行通过，Debug实际发现60项；六桌面profile按ctest id将最低发现数59提升至60。注册脚本第一次调用路径错误，原exit2保留；改用现有 `tests/scripts/check_test_coverage.py` 后204 Lua/94 C++文件全部注册，属于注册完整性而非覆盖率。`quiet-registration-02/run.json` SHA256 `7ed16fdd7e5428dcd46a48f6ddb2dd76bc4a86220b4ef16ae8dba8241221869f`，configure/discovery/CTest/注册检查退出0，源码首尾稳定。

本批输入是手工构造的观测fixture，不是原生后端或长跑证明。实际workload探针、逐周期进度/身份/时长验证、真实资源与阻塞任务负控、短跑和连续一小时运行仍待完成，不能用静止点检查器通过替代U27验收。

### 2026-09-24 — 真实循环诊断与两处生产缺陷修复

新增 opt-in `CaesuraEngineSoakProbe`，在隐藏的真实 SDL HWND、Direct3D 11、静音 Device 音频和正常 Engine owner loop / DirectDrain 下运行固定有限资源。每轮实际渲染图片与中英文文字、加密保存、改变页面、加载并恢复整帧、异步完成与八项取消、显式 voice 中断及自然完成、两次 runner 回滚，再销毁瞬态纹理/RTT并观察静止点。输入复制到新的仓外目录，原日志及失败不覆盖。探针完成只代表循环执行结束，验收仍由外部检查器判定；没有把这些 opt-in GPU/Device 探针注册为无设备 CTest 的自动通过项。

初次运行暴露 checker fixture 对 bgfx 名称的错误假设：真实名称是 `Direct3D 11`，而非 `Direct3D11`。先改 fixture 在旧 checker 取得4项失败，再同步名称后14项通过；测试方法正文和数字预算不变。

真实 Device voice 随后触发十秒 watchdog。定位到 `SoLoud::init` 第四参数原来传入2，实际是设备缓冲大小，第五参数才是双声道。交错2/AUTO/2/AUTO四个独立实际 WinMM 进程中，小缓冲均在两秒后仍未自然结束、source clock仅约0.00744秒；AUTO均自然结束。生产 backend 的同一冻结80 ms WAV及同一 probe 在修复前退出1、自然完成0、live/session均1；仅将 Device 缓冲参数改为库的 AUTO 默认后退出0，0.12613秒观察到一次自然完成、live/session均0且重复consume为0。ManualMix/Software的2048参数保持。WinMM的 `reported_buffer_size` 从4变为8192，是后端报告值，不能通称每声道帧数；这些观察不证明物理可听输出。根复核 `device-clock-root-review-01.json` SHA256 `4c9d7f5d0a2c63924bfdfae19fb455674c47dee027308b6ad71840cbd93c6518`。

音频修复后第一个完整循环通过，第二轮命中已释放的纯色纹理ID：`createSolidTexture` 从 `m_solidCache` 返回旧ID2，`isValid`为false。`destroyTexture`已清理普通路径映射但未清理颜色映射；最小修复只删除指向被销毁ID的颜色条目，保留其他live颜色的去重。保存的原探针和工作负载字节相同，修复后的 `native-diagnostic-06` 完成22轮，证实该失效ID问题已消失。

根审计没有把06的进程退出0升级为验收成功：原静止点检查器拒绝第21/22轮，纹理分别27/28，对比预热26；逻辑纹理字节每轮增长9216。源码追踪确认是新夹具丢弃异步回调交付的纹理ID，未执行显式释放；同步页面缓存与异步上传是独立资源。此项修正仅在夹具保存/释放回调ID，不扩写生产代码，也不放宽预算。06原始run保留，`root-review-01.json`明确为QUIET_REJECTED，摘要 `7530b2c220162614daa99e3ef15e824ed99ca395bd4564a27ea202ad55affb58`。

修正后的 `native-diagnostic-07` 源码/输入/实际binary首尾稳定，22轮全部执行，20轮预热后两轮正式测量仅1.30744秒，静止点检查无错误；GPU纹理基线6、逻辑纹理18432字节。66张640×360 PNG逐一解码、摘要及背景像素复核，各轮A与恢复后的整帧RGBA完全相同且与B不同；实际截图可见中英文文字。root审计摘要 `aae2c48b0b21f03e28934d07e096ceed67b6823d27d4c4a38ffdad4582c8be5e`，原run摘要 `b505ad456ae2d9910f94159f3613034afda6c23f11609b228d8b9d8de1df9e22`。所有本轮正常结束命令均保留PID/creation、原始流摘要及owned cleanup COMPLETE，无超时强杀；早期watchdog与构造失败保持FAIL。

本次主代理按caesura-review核对修复的缓存所有权、SoLoud参数位置、注册表访问和夹具释放；子代理额度不可用，未伪称独立子代理审查。没有新增公共接口签名、后端单例入口或跨模块依赖，耦合检查通过。行/分支覆盖率未测量，真实D3D11/WinMM之外的后端未测量。后续冻结提交的完整Debug门禁单独记录；正式维护的时长/事件验收入口、上下文重建、跨进程冷恢复、主动资源/阻塞/坏档/时限负控、>=120秒短跑和>=3600秒长跑继续未完成，U27整体保持未完成。

### 两处生产修复后的完整 Windows 门禁

干净提交`10089617fd6168c8dbb87083da9257485fbc5625`的run`aad62a4b-b44e-408c-81c3-d5e0f333e736`完成全量Debug构建，C++1477/1477、444441断言、0失败/0跳过，Lua147/147与56/56全通过。CTest60项中59通过、0失败、仅预声明可选AI服务跳过，用时636.15秒。execute、collector、strict verifier均退出0，源码/fixture首尾稳定，owned进程清理完整，无超时强杀。

原始`artifacts/validation/u27-device-texture-full-01/raw/run.json` SHA256为`3252c0a365052541c1e734557514fa48dfd630a96bd11a4fe98e01c313e4ff54`；根重新校验29份流/报告并解析CTest XML，`root-green-review-01.json` SHA256为`1d9ba8c7a7620f094507a142fc4c46f452d8968ac35bad07148c464cbed6d050`。子进程精确筛选的doctest日志另有1476未选中，完整末尾总结仍是1477全部发现/通过，未混淆二者。此门禁覆盖该冻结提交及新探针的实际Debug编译，不把未执行的GPU/Device opt-in探针、短跑、长跑或恢复负控写成CTest已验证。U27余项继续执行。
## 2026-09-24 单上下文事件验收器与独立计时诊断

新增维护入口 `engine_soak_trace.check_trace`，按实际原生事件顺序核对纹理/RTT、截图请求与消费、加密保存、读取、异步完成、语音自然结束、取消批次和回滚。它要求同一受控 PID/creation、有限单调时钟、连续三个合格静止帧、固定的二十周期预热基线和原资源/内存预算；进程自报时长不得超过外部控制器实测区间，循环间停滞也不能用来补足测量时长。截图票据和图形上下文的代次是不同分配器，只分别要求稳定，不能错误地要求数值相等。

首次12项测试在空验收实现上实际RED；补充循环间长空档、实际测量起点时钟差异后保留15项RED/GREEN。根审查又发现Python字典比较会接受同值浮点资源ID，新增回归实际两项子断言RED，随后严格检查整数类型。最终18/18通过，新增一致的静止点资源增长拒绝和完整长模式正控，均明确为合成事件测试，不是原生长跑。初始12项、15项和最终18项测试方法AST全部保留，未削弱旧断言。证据位于 `artifacts/validation/u27-soak-workload/trace-*`。

真实诊断08重新配置后实际发现61项CTest，新旧两个Soak合同入口均通过；六桌面profile最低发现数据此从60提高到61。Release探针完成22周期，测量段2周期/1.2821825秒，外部控制器观察整个受控命令15.157秒，首尾源码和输入/二进制/DLL摘要稳定。维护trace验收器接受这一诊断，66张实际PNG用已有有界RGBA8解码器重新解码，每组完整像素A等于restored且不同于B。run.json摘要 `dfd5ea7c3dcae790a378f4db275496c8fd27426cbba5303cdae7fc210db305f4`，根审查摘要 `a0dea76306f275193259fa9d22b91729d0003182f16601217cb063d2e5ff4690`。

本增量没有修改引擎、探针或Lua负载。之前10089617的完整Debug证据仍只绑定该源码；此新增Python合同只取得定向验证。诊断08没有完成120秒短版、3600秒长版、上下文重建、冷恢复或实际故障负控，校验器返回空错误不授予整个Soak或U27通过。受控维护运行器和这些剩余验收继续推进。
## 2026-09-24 维护运行器与实际进程观察

新增 `scripts/run_engine_soak.py`，从已配置的Windows foundation构建目录明确构建Release探针，使用全新仓外runtime及固定BMP/WAV/字体输入，记录源码、配置、二进制和DLL摘要。当前只开放diagnostic模式且始终 `accepted_soak=false`；没有把暂时未实现的短版、长版、上下文重建、冷恢复或故障负控暴露为可通过的选项。

进程监督复用既有owned runner，按其原子发布的PID/creation进行OS身份观察；就绪后独立调用Windows模块枚举，要求实际探针、邻接SDL3与系统D3D11映像匹配，并保存模块路径/摘要。启动期限、进度期限、总期限、非零退出和观察异常都会失败并保留实际清理收据，不能以退出0代替工作负载验收。PNG验证复用现有有界RGBA8解码器，逐组检查整张恢复图像相同、改变后的图像不同。

11项维护回归先在未实现入口上实际RED，随后11/11通过且测试方法AST完全保留。进程部分使用真实受控Python子进程；图像部分是合成PNG，均不冒充原生后端证据。新入口加入实际CTest后发现62项，三个Soak合同入口3/3通过；六桌面profile最低发现数由61更新到62。证据在 `artifacts/validation/u27-soak-workload/driver-{red,green,discovery}-01`。

第一次维护诊断在MSBuild FileTracker的CommonApplicationData路径解析处失败，未运行探针。补回用户/公共目录变量后的第二次仍失败。根查阅[MSBuild FileTracker源码](https://github.com/dotnet/msbuild/blob/main/src/Utilities/TrackedDependencies/FileTracker.cs)，并使用真实.NET子进程对照：原失败环境的GetFolderPath(CommonApplicationData)为空，ExpandEnvironmentVariables保留未展开的SystemDrive；只补回SystemDrive后分别恢复为C:\ProgramData。最小修正保留该变量，原两次失败和对照进程均留档；没有修改系统环境配置、源码或验收阈值。

维护诊断03在源码fingerprint `1d9c7ea9e155649be49eaf2cd87d3bde63735380f33e16ece7d614aae61b7391` 首尾稳定时通过。实际PID25788/creation134346634650684644完成22周期，测量段2周期/1.3084895秒，外部观察15.047秒。OS模块观察、全部输入/二进制/结果/事件和66张PNG摘要经根回读；原始run SHA256 `76ad0dd6fab99ddc55ea42dad4b68374e91eac6ce3128de67516ec5bd4fda763`，根审查SHA256 `58edbece71202dab448332e6a9d21199989ba144da3692a9349d56fe0ad02e7b`。全部owned收据实际exit0/cleanup COMPLETE，无超时/强杀；实际探针PID后续已不存在。新完整Debug门禁仍待冻结候选后执行，这项诊断不完成U27或整个计划。
