# 命令合同修复与当前验证接续

本记录接续[全命令原生绑定审计](2026-10-01-005-command-native-contract-audit.md)，不替代当前唯一运行时计划，也不宣布 U1–U29 完成。当前工作树以 `6080057a4cd51f53655800dd095aa8444bef713d` 为原审计基线，最终代码提交、完整门禁与源身份尚待主代理封存。

## 命令与 API 边界

实际维护声明为 145 项 schema；公开命令名共 182 项，包含 28 个额外流程名和 9 个兼容别名。原 181 计数漏了 `sel`，旧记录保留。混入 KAG Lua 表的 18 个内部函数和 8 个 table 不再自动成为 DSL 标签：编译器、调度器与静态检查器共用公开判定，直接 Lua helper API 保留。运行时 55 项与静态检查 32 项的真实正负控分别记录。

别名在默认值前解析，显式 canonical 值优先，仍接受原类型和范围验证；动态表达式不得被提前注入的默认值遮蔽。新 quoted positional 语义由 `caesura-kag-3` 标识，缓存容器格式仍为 2；旧语义缓存拒绝或从源重编译，不能靠清空缓存改称通过。

资产目录枚举经 `IAssetReader::listDirectory` / `KAG.list_assets` 获取完整、有界的叶文件名，运行时不开放 shell/popen。维护中的独立 Lua CLI 专属预载 `caesura_cli_fs`，复用有界目录提供器；引擎 Lua 不注册该能力。已有开发环境 LuaFileSystem 兼容分支保留真实 iterator+directory userdata，strict 模式禁止走这两种宿主文件系统回退，不能把引擎程序当解释器。安全视频通过 `KAG.video_asset_*` 的 VM 所有会话和 bounded asset reader 读取内存，场景不获任意解码器 URL/原始 Render 视频入口。

## 已取得的限定证据

| 范围 | 当前结论 | 限制 |
|---|---|---|
| Camera `restore=false` | 原生产语义 8/8 控制通过；此前静态疑点被反证，未增加 camera 产品补丁 | 不因此声称所有实际 GPU 摄像机像素路径完成 |
| CLI 与目录 | `test_i18n`、`test_ks_i18n_flow` 两入口 actual0，26 项 CLI 枚举控制通过 | 与引擎 native fixture/U17 的结果分别报告 |
| 资产目录缓存 | 2 个实际 C++ 用例、64 个断言通过 | 不等于全部资源模块通过；原完整门禁中的 TEMP 查询失败保留 |
| 教程 07 | 原件曾833次读取/1次保存/WAIT12；修正后 source 定向实际1写1读、coins恢复0并自然DONE | 本次Vitest是1项通过、67项过滤未选中；不写成全Web68通过。生成bundle与完整套件另验 |
| Lua 主/隔离 | 最新 `lua-full-07` 实际153/153与58/58，两个原执行入口actual0、输入前后稳定；full05/06失败原件保留 | 绑定该次源；后续代码变化必须逐项判断适用性 |
| 原生菜单/视频 | 音乐与图库真实目录枚举、关闭/所有权条件已接受；视频75PNG两目标矩形实际蓝色解码帧与停止后清空已接受 | 不证明任意影片质量或物理音频 |
| 命令场景矩阵 | 最近封存矩阵为61份root接受场景及最早arithmetic原件，共171/182名称有选定原生场景证据 | 包含4个明确能力拒绝和离线AI回退，不得改称171个正向实现、所有参数或覆盖率 |
| U17 与 Demo/CLI 原生夹具 | SDKON下重新验证：7个用例、66个断言通过；合法Small16、实际SDL/DevCore及释放断言保留 | 1601个为过滤未选中，不是完整C++通过；旧字体fixture失败保留 |
| 官方 entry 桥接与路线目标 | SDKON 产物中无渲染器的真实 LuaManager/C 绑定回归：2用例/20断言通过；galgame、full、template各实际start/update/render一次且context退休；full新增render转发、template补三回调，route_b补独立rooftop页并汇入共同续行 | 1608项为过滤未选中；click只核回调存在，未驱动点击；双路线真实parser/compiler/button/runner目标解析通过，不是全剧情或GPU画面证明 |
| CLI CTest 注册入口 | `CaesuraCliAssetDirectory` 已由实际 SDK Debug CTest 注册运行通过 | 不等于全部 CTest/CLI 标签通过 |
| Settings 失败清理 | 真实安全枚举失败后显式 coroutine.close，11项清理/重新进入控制通过；不修改生产Settings | 两个fixture补精确安全枚举与音量宿主、明确resume错误，已由full07主套件通过；不放宽strict |
| Web presentation | 最新4个文件36项定向通过；font/有效URL回执输入遗漏已获独立增量PASS | 不是完整Web门禁；最近完整Web仍有轻量core方法前提及性能失败，最终时间拆分仍为性能FAIL，原字节已恢复，停止循环 |
| SMA 异常到 Lua 边界 | renderer draw分配异常越过C Lua ABI的真实RED已保留；局部catch在C++局部作用域退出后才调用luaL_error，原同用例10断言实际GREEN、1619过滤未选中，对应SDKON全Debug构建退出0 | 只证明异常边界，不是SMA像素/硬件或完整C++门禁；独立增量审查由另一代理进行 |

## SMA 与正式门禁仍待闭合

硬件 D3D11 compute 卡住并非仅原 SMA shader：外部常量基线也在提交后 Flush 前后边界未完成，原超时与最后阶段记录均保留。当前还不能断言唯一根因，也不能把它改记成硬件通过。WARP 真实软件计算已通过数学值比较及对象退役；其终态明确为 `WARP_SOFTWARE_COMPUTE_ONLY_PASS_DRAW_NOT_TESTED`，不等价于硬件 GPU、SMA 绘制或整套验收通过。后续独立WARP软件compute/像素对照已实际运行三组共9例：无update改变transform、同mesh多pose多输出、uint16骨骼范围（含65535索引与65536poses）全部mismatch0，均观察到真实compute并完成软件设备退役。这是新原件，不是把早期数学结果改标为像素通过；物理GPU仍未重试。

当前 SDKON 全量构建实际退出0，但随后的 Haru 场景没有通过：主代理确认 DeviceHung，截图导出在 frame 5 报 `invalid screenshot pixel buffer`，运行终态为 `STOPPED_ON_FIRST_FAILURE`。停止新的物理GPU测试，等待用户重启后再明确安排；不自动重启或自动重试。当前源Lua完整主/隔离套件已通过；完整 C++/CTest、strict及实际SDK更新/绘制/卸载仍未闭合，构建0不是命令通过。现有197个SDK文件和Haru26文件已在R逐字节核对；关闭SDK的旧构建不是永久外部限制，也不允许把旧c68 SDK二进制改标为本次产物。CPU回退/基准与本次最终CPU验证尚待完成。

Web专项诊断用于定位前提与真实失败，不能替代完整Web门禁或把诊断采集退出0写成产品通过。原完整C++1603中1591通过/12失败、Web各轮失败、SMA硬件超时、临时目录失效证据均保留。Steam实际C绑定离线记录器原RED为8用例/4通过/4失败、38断言中4失败；名称完整性、int32收窄与非有限/超范围浮点修复后，原09f93e版本14用例/73断言通过后，cloud_read返回长度guard独立增量审查及实际GREEN已闭合；最新15用例/82断言通过、1604过滤未选中；这些均不是外部账号调用。Steam账户实际成就写入没有授权，仍NOT_RUN；离线shim/拒绝合同不替代账户动作。此前SAN strictFAIL、性能INCONCLUSIVE及设备/商店条件不由本轮组件证明升级。

SMA GPU核心修复采用两行metadata前缀与实际pose行、每次draw独立output/immutable snapshot，保留CPU全部uint16可达骨骼分支及vertex尾线程guard。核心已独审；真实FXC退出0生成1772B DXBC（`906307ab13bb20c22e53db7899f3ecdfc1f9eddba2a567901dc08a26b3eabb64`），GL源码同步内嵌，两个vertex shader数组字节不变。新软件三组9例通过的源码/生成物身份已增量核对。新布局后续实际软件控制已通过：TIB请求6而仅余5时保持5→5、拒绝阶段无着色像素，恢复后1582白像素且mismatch0；独立WARP compute数学actual0。维护headless过滤组19用例/3200断言actual0、1605过滤未选中，SDKON全Debug构建日志build-sma-layout-full-01.log对应退出0。临时CMake诊断hook已由正规configure删除、actual0，当前cache已无指定key。SMA default view／opacity实际Engine像素仍待设备恢复，物理GPU未重试；完整C++/CTest/Web性能门禁仍未闭合。

最终Web计时诊断保留性能FAIL：median为5297.5ms，实际吞吐约0.06399 tokens/ms，小于原0.08门槛。预热151帧，三次测量各150个真实帧；每次约4548ms在真实rAF等待，DOM绘制约86.5–101.0ms、签名约4.90–5.15ms。各项时间包含关系不同，不能简单相加。此前142次回执复用/2次必要新帧已有独立事实；最后诊断没有证明新的冗余产品工作，也没有证明唯一OS/浏览器根因。保留原FAIL并停止相同循环，不降低阈值、不减少实际帧、不替换rAF来取得绿色。

计时结束后两个维护性能测试已精确恢复原字节，本次只读重新核对baseline SHA `0b685ce153d72a61951a41ef4a67dcfb7fc6d3ac784b9798e159be52269a5f62`、bundle SHA `03eccab781c8522f0d2ed4a12a14c5a62d19c06751a3741f461d03793e3a6ac6`；诊断helpers不在工作树。恢复工具没有独立执行receipt，`root-restore-readback.json`仅为恢复后读回，不能伪装工具执行记录。物理GPU未重试，仍待人工重启。

当前布局说明已同步至[SMA设计§9](../design/skeletal-mesh-animation.md#9-s5-gpu-蒙皮当前布局与历史证据)；round18硬件像素与round19约15.8×主机侧性能只保留为历史，不外推本补丁。182／171命令场景计数不变。

## 文档与接续

权威命令表、API统计、能力静态矩阵、计划事实与Web索引由维护生成器产生，不手写总数。生成器新增别名列及直接C函数注册计数，已实际取得原版本3 RED与应用版3 GREEN；第一份probe因aliases未包装json.array失败单独保留。命令表145、API统计、closure和计划事实的实际生成退出0；closure边界说明与plan事实生成/检查三项后续实际0。CLI专用fs与事务cache后续增量已获独立限定PASS，复用未变的DirAssetProvider核心审查。测试引用数、测试注册数和静态Consumed列不是覆盖率，也不是实际平台能力。

待主代理串行完成生成、轻量检查与最终代码/文档提交后，再记录自动解析的代码锚和文档锚；不得用 `--head` 伪装当前freshness，也不得抬升平台矩阵历史能力行。

本地原件根：`E:/CaesuraRecovery/20260924-1446`（下称 R）。下表只索引原件，运行源/参数/过滤范围以各原始report为准。

## 原件索引

| 范围 | R 下原件 | SHA-256 |
|---|---|---|
| Camera8 | `ca1/cli-camera-red-01/camera/stdout.log` | `9cf074498586db5cbf57d725803e97a50736445ff3f674afc7cbe18f91ee2397` |
| CLI2及控制 | `ca1/cli-listing-green-01/report.json` | `5be78570d643b7c56456416a9df7ddfe21f025a81e60e7344df7230201483201` |
| 缓存2 | `ca1/asset-cache-green-01/report.json` | `310af2e85dfe503f64be947f470d9288337a7ad5169bba273322d325023e1334` |
| 教程07定向 | `ca1/tutorial07-single-restore-green-01/report.json` | `73e2797ab8c544308f5730a325a7d3dea53ccbd93d7debb31f569495a2007905` |
| 生成器3RED/3GREEN | `ca1/doc-generators-02/report.json` | `eba4bad57a596ec24d3790103e66c3a590a6f383b3e8bf0daa4f6e832a5ec0ff` |
| API/closure/plan生成 | `ca1/docs-generation-01/report.json` | `ae37b19ed7eec378eedc319a7c8dbd8dedcc638c021350bdd075f8fa45671d75` |
| SDKON限定7/66 | `ca1-sdk/native-fixtures-green-02/report.json` | `6e1a0b667b32a470e5525c1195a38141ee1292e1f513219b74e9ea26f49a754c` |
| SDK场景失败 | `ca1-sdk/native-live2d-capture-01/report.json` | `43c93dccf784a4770d56bbc16f795618a8572a9f0007c3e88343a2801ba8e25a` |
| SDK截图失败 | `ca1-sdk/native-live2d-capture-01/cases/live2d-lifecycle/owned-01/engine/stderr.log` | `b9486c81a2958a5f6c82eca6ef7200302651e78252136cab822f3e131e4f19f1` |
| WARP软件数学 | `ca1/sma-warp-compute-01/native/compute-events.jsonl` | `20d44761ba2ef48ef8f7239820933e77f633c9cce1cf78a4f5adb5e2f0f14637` |
| 硬件常量未完成 | `ca1/sma-compute-isolation-02/native/compute-events.jsonl` | `9d6a9a04ba80ba61a3791b5fa861cca0f32e88272d70a80e035c615b8e15c00c` |
| 命令逐项证据 | `command-native-audit-01/command-inventory/runtime-evidence-05.json` | `815542cc958df42c15cb4723fa14dbe8a77099bfbde8549a500795830eb09a8b` |
| 官方entry/路线限定GREEN | `ca1-sdk/official-entry-native-wiring-green-03/report.json` | `3028fc84e2ede1f3f3ef15ad959f0153ce5ce9319a19b1171e8160f79e654efb` |
| Steam离线ABI实际RED | `ca1-sdk/steam-abi-red-01/report.json` | `adbb06e8ef5cbd89883bc7eec29ca7a6212a1f2ad273f9072b5f31eb1e980e72` |
| Settings显式close11 | `ca1/settings-close-contract-01/report.json` | `dac452e04ef028c450ea47b92dbb6654b0c15bd971d0d1fa0000235325586602` |
| CLI注册CTest | `ca1-sdk/cli-ctest-green-01/report.json` | `9161d7cdc1ad9f69614ca2a50a6365676b7741a509559fb995fe5f88e36e5bfa` |
| closure/plan生成检查3 | `ca1/docs-closeout-generation-01/report.json` | `94b883d491f0c56dabce5da867b992bef47d7040ac2ef3898a6fc163f56be8e4` |
| Steam限定14/73 | `ca1-sdk/steam-abi-green-01/report.json` | `4c9f7072e305e31f5ca07af47b4435b28adcde220ec75dc40c3ada3701e13ec6` |
| Webpresentation限定13 | `ca1/web-presentation-green-01/report.json` | `f6a97676da628267bfd211f800275c1ab27a9d2a286334c35b51ad95cdbcd842` |
| 最新全Lua153+58 | `ca1/lua-full-07/report.json` | `93f6ef1f78d1ce2f3a58e4da7d4efb60b55d8d77004c6c451b450745cb7f8293` |
| Steam最终15/82 | `ca1-sdk/steam-abi-green-02/report.json` | `8560ab5a5a17ba50d945e5fd10a318d6b34c4aac8a61e44a364835546acce731` |
| Web呈现4文件36项 | `ca1/web-presentation-green-02/report.json` | `b1ddc8c0eb7e7a28401af916e520d179722c59c200da1330b70bed5c31dab278` |
| CLI/cache增量独审 | `command-native-cli-cache-increment-independent-01/review.json` | `5066c40a77d6b05f5dcea99c35ffb044eddc3959f5ad8cdf69bac24edb37e30e` |
| Webfont/URL独审闭合 | `command-native-contract-web-presentation-input-independent-01/review.json` | `6cb5cf05c15f3b8a34ab52721b1ecba332eaf3771487dcbde6cba96fddce4bc9` |
| SMA异常实际RED | `ca1-sdk/sma-exception-red-01/report.json` | `2989876e32d3d96e65b5987e0be8f66ed2b0418db4ec3a90b56141bce397e7c8` |
| SMA异常10断言GREEN | `ca1-sdk/sma-exception-green-01/report.json` | `9d5e5766a71a213e342828379e0e19d4ce449b7a03dfbd7835bb2b02e82b1347` |
| SMA异常修复SDKON构建日志 | `ca1-sdk/build-sma-exception-green-01.log` | `9c38b1ad5efe06b72d697a516fe8ba4f72f25cd75e4c1ef06b3b885d5bb13e6a` |
| 最终Web计时FAIL | `ca1/web-timing-diagnostic-02/report.json` | `1f38460903d97dcf647442ebdebd3103d29f4364ec9ed50804d12196fe487f17` |
| 计时原因限定分析 | `command-native-contract-web-timing-diagnosis-01/analysis-report-01.json` | `fe77acd3558633deceebbed44a677c1189933b1f5f125d6be187ef5536bbc056` |
| 性能文件恢复后读回 | `command-native-contract-web-timing-diagnosis-01/root-restore-readback.json` | `06872e09ba6dda8a8f5779b9f0ec030bdf0666ef395d34d928c326e410cd461b` |
| SMA布局核心独审 | `command-native-sma-layout-core-independent-01/review.json` | `c26b428b73322c97755366674d5bf7fa8a1723ab84278495cd2d24f13e8f19dc` |
| SMA布局真实FXC发布 | `command-native-sma-layout-fix-01/publish-report.json` | `d826cf64046f97c736e7f69e0a9495e92311d264f83ab8499fb18287b4cd393d` |
| SMA软件三组9例 | `ca1-sdk/sma-warp-regressions-owned-green-01/report.json` | `07d2759ed99f3bbbd77797e7430a8bb61c302062c4c9927b46baad901830b6f8` |
| SMA生成与软件增量独审 | `command-native-sma-layout-generated-independent-01/review.json` | `105e55dcfe4bbdbb0d1f35fc793c3faf58bb01202871a26707626e653afad811` |
| SMA新布局SDKON全构建 | `ca1-sdk/build-sma-layout-full-01.log` | `2c757619fdc31875fbf5bd4546a54fd6d9a9371d4c9dddea9226c36286fac773` |
| SMA新布局软件控制2 | `ca1-sdk/sma-layout-software-final-01/report.json` | `526b2e38a849f7731f5aa73bc4c8c4da0877807633832d4dbc80e6064e4aa34e` |
| SMA新布局headless19/3200 | `ca1-sdk/sma-layout-headless-01/report.json` | `bb51564de8c7b910287afbfc947ea02689ae0097c7ffd180f3f71683c23bdee4` |
| 移除CMake诊断hook | `ca1-sdk/configure-remove-diagnostic-overlay-01/report.json` | `b58c8f808429a24741340b41a662c3a8743382d0653354ab737589f13e75f932` |

上述验证和当前文档更新均未提供合并、发布或商店上传授权；完整运行时计划继续。

## 重启后状态更新

用户已重启，新的硬件与独立对照仍未完成；自线程栈已定位 NVIDIA 用户态驱动调用链等待。见[重启后检查记录](2026-10-02-002-gpu-reboot-diagnostic.md)。此前的待重启状态不再作为唯一解释；软件通过和所有未闭合门禁保持各自范围。

### 普通用户目录授权后的增量验收

用户已批准本轮 GPU/SMA、Live2D、完整 Debug/C++/CTest 和必要修复回归使用普通 Windows 用户目录及正常驱动缓存；输出、TEMP 与游戏工作目录继续在恢复根 E 盘。本节更新前述待重启状态，不改变系统设置、不安装软件、不发布。

- 普通 profile 的独立 D3D11 constant/skin compute 对照通过，回切隔离 E profile 再次超时，形成 A-B-A 环境对照；保持环境组相关结论，不认定唯一驱动原因。维护 SMA 硬件七项已通过。
- 首次完整 Windows profile 原件 `ca1-sdk/normal-profile-gates-results-01` 仍为失败：C++ 1624 中 1623 通过、1 失败；CTest 74 项中 8 失败，另有预声明可选 AI 跳过。不能拼接后续定向结果改记整轮全绿。
- unavailable-scene 测试改用真实 coroutine/首帧机会后，定向 1 用例 11 断言通过。shell 路径、缺失 ccr 父目录、两个 headless 自定义命令 schema 及注册问题已分别修正并保留原失败。
- Golden 驱动原来会在命令出错后输出 DONE。新的显式模拟渲染边界执行真实 Lua render pump 并拒绝命令错误；`golden-fixture-positive-02` 的原样 Golden CTest 通过，四个 `golden-negative-*-01` 分别命中捕获/纹理命令注入失败且不再输出 DONE。这仍是模拟边界证据，不是 GPU 像素。
- SMA 实际 Lua producer 传位置数组而 C 绑定只读具名字段，已修为具名优先、仅 nil 回退位置槽并保留有限值/范围检查。最初 real-module RED 被测试 Lua 长字符串分隔符错误阻断，不能宣称其全部断言执行失败；独立 invalid-positional RED 有效。修复夹具后 `positional-sma-green-02` 为 9 用例 231 断言通过。
- `command-native-sma-cpu-pixels-normal-profile-03` 的可见正控制通过，并真实暴露默认 view 与 opacity 四项失败。默认 MAIN 与已有 modulated shader 修复后，原样 packet 04 全部像素断言通过（`PASS_CPU_PIXELS_ONLY`，严格 29 owner 帧规则）；`sma-default-opacity-contract-green-01` 为 10 用例 273 断言通过。`build-sma-opacity-green-01.log` 全 Debug 构建退出 0。新增 GPU opacity 用例尚待独立执行，不把 CPU 像素升级为 GPU 证明。
- Live2D 普通 profile 两轮 AV 观测都最终退出 `0xC0000005`，无超时/强杀。observer02 故障 RIP 的 VirtualQuery 成功且为 MEM_FREE；此前 own-module 快照中该地址位于 d3d11.dll。源码显示 bgfx DeviceLost 自动销毁会先卸载 DLL，而 Live2D 仍持共享 COM 引用；这是次生 execute AV 的具体寿命缺口。最初 GPU HUNG 和 Haru 绘制仍未闭合，不能称该命令族通过。

以上原件位于恢复根 `E:/CaesuraRecovery/20260924-1446`。当前所有命令审计、最终完整门禁和运行时计划均未完成；Web 性能原 FAIL、Steam 账户实际调用未运行等边界继续保留。

### 2026-10-03 继续：SMA 透明度与 Live2D 实际故障

`live2d-loader-sma-opacity-green-01` 的两个隔离子进程均通过：DLL loader 19 断言，真实 D3D11 SMA opacity 60,526 断言（源 alpha 255/128 × actor opacity 0/.5/1，同帧三提交；GPU 有实际 compute，分别对独立合成公式及 CPU 对照）。父进程 2 项通过不是以上子断言数量的替代。Vulkan 没有 modulated program 的审查疑点已反证为当前不可达：维护 bgfx CMake 无条件关闭 Vulkan，实际 vcxproj 各配置均为 0；不能据此宣称 Vulkan 支持，也不为该未启用后端扩建 shader 工具。

Live2D 普通计数 DLL 引用及模型/SDK/context/DLL 释放顺序修复后，`native-live2d-lifetime-green-01` 不再访问违例，实际退出 2、正常清理，但原 frame5 HUNG 保留。完整落盘日志首次显示 SDK `Failed to load effect shader` / `Fail Compile shader`。验证 runtime 漏带两份 `FrameworkShaders/*.fx`；使用同一产物、同一场景仅补入构建输出原件的 `native-live2d-shaders-present-01` 不再 HUNG，脚本和退出通过。该轮 PNG 仍与背景完全相同，不能改记像素通过。

进一步确认 Live2D 硬编码 view0 绘制被最终主场景覆盖，已改为 `VIEW_MAIN`。`native-live2d-shader-main-01` 原生正常退出/完整清理；独立 PNG 观察中，显示模型相对基线改变 23,483 个 RGB 像素，嘴型 0→0.8 改变 73 个像素且局限于嘴部区域，隐藏及卸载均逐像素恢复基线。范围仅当前 Haru、手动嘴型及选定稳定帧；不是全部模型、声音驱动嘴型或所有命令的证明。

产品还增加实际 SDK shader readiness 守卫：在模型 renderer/输出纹理创建前检查全部 VS/PS 和 input layout，保存恢复共享 IA 布局，失败缓存避免重复编译。不因文件存在便假设编译成功。missing/corrupt 两个真实拒绝控制仍待完成；第一份 missing 控制已证实两次拒绝且零保留模型，但测试入口在发布 PASS 后编码未标记为 JSON 数组的 attempts 表失败，未请求原生退出，耗尽 64 帧，实际退出 2。第二轮明确记录 capability_json 错误，纠正了此前对全局赋值的猜测；两份原 FAIL 保留。第三轮修正数组标记和终态发布顺序后，missing/corrupt 均真实退出 0、连续两次加载拒绝且零保留模型，编译失败仅一次，完整清理；不将脚本 PASS 单独作为验收。

全 Lua 第 08 轮隔离套件 60/60 通过，主套件仍因 `test_sma.lua` 三条旧源码拼写断言失败。将其替换为实际 Lua producer 输出检查后，SMA 定向 78 项通过，真实 C ABI 继续由 C++ 契约用例证明。编辑器仅受影响的纯 commandLint 用例 12/12 通过，使用已有 Web Vitest 运行时，不表示 Studio 全体验收。最终完整原生及 Web 门禁待执行；本节不授予提交、合并或发布许可。
