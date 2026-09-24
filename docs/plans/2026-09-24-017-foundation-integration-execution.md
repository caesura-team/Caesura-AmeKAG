# U29 底层整合候选执行记录

本记录对应唯一[运行时底层计划](2026-09-05-001-refactor-runtime-foundation-plan.md)的整合准备；目标仍为完整U1–U29，底层优先、Studio暂停。工作树为 `artifacts/validation/u29-worktree`，分支 `codex/u29-foundation-integration`。下列本地merge提交仅保存可审查的源码组合，不表示通过合并门禁、进入master或发布。

## 2026-09-24 来源与冲突处理

从U27干净 `c25d81ebfb324b85b54252ce937039c6dc790f28` 创建隔离工作树，依次整合：

| 增量 | 输入提交 | 本地整合提交 | 实际处理 |
|---|---|---|---|
| U23（包含U22生产修复） | `ea0281bca942bcc218047fe6021517c388145697` | `8a574e454db19dadc05d9c6ff0df0e18008b3f2f` | 保留U27独有74路径及U23独有23路径原blob；加3项Mac维护CTest，下限62→65 |
| U24 | `4bb5426a75e2f9f63dcd533ad348fac3c58372a6` | `ddc9b77a36cfc4adae8f9609b0196e479404ae2b` | 保留Mac安装与cache排除，加入原Android输出复制guard和3项维护CTest，下限65→68 |
| U26 | `cbf5904bd6b10b69027dcf4f1b963a1554849eac` | `cddec53e20a0fc20542e74d090a20ef02a28f0b7` | 云恢复生产源码与incoming原blob相同；追加2项native冷进程CTest，下限68→70；6组native C++下限各+40 |

U2 Expo源码 `f646b820` 已是U27起点的祖先，不重复整合；这不补齐U2当前源码的跨平台或sanitizer执行。U26整合后的C++发现下限为Windows1517、Linux1473、macOS1360，均尚待实际发现和执行。官方API扫描得到16模块、41接口头、457纯虚方法，统计文档由 `scripts/api_stats.py` 生成；API参考总数同步为41。平台文档冲突暂保留历史执行行，后续由维护中的生成器同步，未把旧证据改为整合通过。

三次独立静态审查均未发现可行动的整合问题；报告位于本工作树 `artifacts/validation/integration-01/`。U23审查SHA256为 `a6890599f596e077f723b4e88cfa6ce2692dbac714d44834c136e08b1b29931d`；U24为 `3077f59c9c6bcac1004cd6c6ba0349b77d36bb609929b57a16bbdbcd7d12f4b6`；U26为 `12ce622ea81c596f7b9dccbbfa4ad8b15a823a01ca945f8f09964f24a45ae001`。这些报告证明来源保留、CMake注册合成、JSON政策和静态API计数，不证明运行行为。当前尚未在整合工作树执行完整配置、构建、C++/Lua/CTest、包或真实后端验证。

## 待完成的整合出口

先完成U28声明和证据边界检查，再冻结整合候选与运行前required范围。完整Debug/Release、平台配置、最终包、冷恢复、真实后端、性能/长跑及AE1–AE8/回退演练均按原计划承接。原c25分支的一小时运行及后续CPU比较保持原源码/二进制锁，不能因本地整合将它们改标为U29通过；整合候选需要自己的适用完整门禁。设备、真实SDK账号、签名与发布分别保留边界，不减少required集合取得绿色结论。


## 2026-09-24 冻结候选与首次本机执行

U28修复和生成文档同步后，验证源码冻结为干净 `a3d0267a92e3bcb7f3997276c3d824bf4b704eaf`，生产/验证实现增量提交为 `8bcd4c4d2a42518fcc00992cabf045ca21a8a7b0`。原始候选工作树保持不变，执行记录另在 `codex/u29-validation-records` 维护。运行前候选范围记录 `artifacts/validation/u29-full-gates-01/candidate-selection.json` SHA256为 `ad4cd5d46592379449be2e99dfc42da1a1ff3473b65c7bee084beb208874ad06`，保留当前release政策9个required jobs和11类artifact roles；本机Windows基础Debug/Release只是其中的执行切片。SDK/FFmpeg关闭、测试前置条件开启；设备/账号、SDK ON、包、AE1–AE8、当前源码性能/一小时长跑及回退分别继续验收。

本轮三个顺序调用均保留在冻结候选树 `artifacts/validation/u29-full-gates-01/`：

| 记录 | 实际结果 | 后续处理 |
|---|---|---|
| debug-01 / run-owned.py | 进程运行器拒绝非绝对的cmake路径，尚未启动CMake | v02解析实际可执行文件的绝对路径，保留原始失败和运行器 |
| debug-02 / run-owned-02.py | CMake实际退出1：自动发现不到Visual Studio 2022；vswhere实际返回空数组 | 原U27配置已显式选择现存VS安装目录；v03使用同一目录与新独立build目录，旧失败cache保留 |
| debug-03 / run-owned-03.py | 配置完成；构建报CaesuraDebug无法include SDL3/SDL.h；完整运行结束为FAIL，采集和严格验证均拒绝 | 只读定位为新Git工作树没有被忽略的SDL3 lib/dll；原U27显式使用主工作区现存依赖，当前继续补齐依赖前提 |

头文件实际存在，候选SDL3包的include/cmake内容与原U27依赖来源相同，不能将这次缺库引起的目标回退误写成跨模块include问题。候选缺少 `lib/x64/SDL3.dll` 与 `SDL3.lib`，配置文件因此未创建真实shared目标，随后fallback目标未携带有效头文件路径。正在保留本次完整失败并准备独立的新构建；尚未把任何构建/测试失败改称通过。

辅助运行器的首次独立审查发现声明的cache选项未实际比较；已补齐执行前逐项检查及结束后的cache稳定性。后续静态复核另外指出CMake摘要当时仅记录、未在首尾锁定，这一工具身份限制保留，不把旧selection摘要记录写成已验证的工具锁。原配置错误、缺依赖、执行失败和当前候选的最终验收状态分别记录。


### 首次完整失败的原始核验与依赖补齐

`debug-03` 已正常结束，源码及夹具首尾稳定。configure实际退出0，execute/collect/verify依次退出1/1/1；原run ID为 `f5a6f4e6-4177-4403-917d-6b17636959b0`，原run SHA256 `e39c6f4b0782fdb2c3c828330aa38c3b5864b2f2a274a01baa16df2fa9ad640c`，outer SHA256 `f397d2cc2f224700593a57a0c80aa0407e022a57cb771db733a08bdbce625b1b`。build退出1，C++因可执行文件缺失退出127；Lua主147/147、孤儿56/56及其余Python/注册/耦合检查通过。CTest的实际JUnit记录71项：46通过、17失败、8跳过/未运行；这些缺执行不算可接受绿色。采集器明确报Missing expected binary，没有产生manifest；严格验证因缺bundle拒绝。

根审计重核原始流、执行收据及实际JUnit元素，`debug-03/root-rejected-review-01.json` SHA256为 `a4399e06181617102dfb2d3f37cfb24c781457f95cbca3a8d6b1328f8bf2d2ae`，gate_result=FAIL。首次通用审计辅助脚本假设bundle存在，后续临时脚本又误用Testing/Test而非实际JUnit的testcase路径，两次辅助审计错误保留在调用记录中；最终按真实XML结构复核，没有重跑测试或改变原结果。

该失败终态后，才从原U27使用的现存SDL3包复制三件被Git忽略的x64依赖：SDL3.dll、SDL3.lib、SDL3_test.lib，共3353292字节。源/目标摘要逐项一致；候选include/cmake内容也与来源一致。仅包元数据.git-hash不同，不从来源覆盖候选的tracked元数据。91项候选SDL输入完整记录在 `sdl-dependencies-01.json`，摘要 `29501abbb97f73b1c7cba7eacd27fc51f734f2e5298adefd41a2a5e16984d5d3`。没有复用旧引擎/测试可执行文件，也没有改源码。

新的 `run-owned-06.py` 在启动前检查完整SDL输入集合、复制源与目标实际字节、CMake每阶段前后摘要、U1实际build工具路径/摘要，并在终态复查；最终静态审查无剩余可行动发现，报告摘要 `1780b30455412b19a0d4359385eed6b6f4fcfb989f9948d4c66763433ae8ec92`。中间辅助版本及其未关闭发现仍保留，最终修复不回写旧报告。`debug-04` 已在全新 `build/presets/windows-foundation-u29-04` 配置成功并开始完整Debug构建；当前尚无完整通过结论，Release及其他required范围继续未运行。

## 2026-09-24 进程身份读取回归与第二份候选

上述 `a3d` 的 `debug-04` 后续日志记录了 C++ 1517/1517、450864个断言、零失败/零跳过，Lua主147/147、孤儿56/56。CTest第40项 `CaesuraPackage_native_package_runtime` 出现实际失败：日志篡改负控还未进入后续篡改阶段，首个编辑器进程的 `process.json` 单次读取即遇到 `PermissionError`。写端已关闭完整临时JSON后原子发布；日志没有记录具体持锁者，不能将其确定归因为重命名、扫描程序或ACL。

会话中断后的OS读回确认原验证进程及已知launcher/CTest进程均已不在运行，原exec句柄也已不存在。CTest仅留下70项终态：68通过、1失败、1预先允许的可选AI跳过；第71项 `CaesuraValidationOutputPaths` 只有启动记录。完整 `raw/run.json`、JUnit、执行完成receipt及严格验证bundle均未生成，原outer保持RUNNING。原文件不补写终态；独立 `debug-04/root-interrupted-review-01.json` 记录为 `INTERRUPTED_WITH_REQUIRED_FAILURE_OBSERVED`、gate=FAIL，SHA256为 `27835676827704c86c33aaee978a8b67b867fa22eaa0de396c1eabdbfe85bded`。审计时源码/cache/91项SDL输入与开始记录相同，但该读回不能替代缺失的终态执行证据。审计辅助解析器两次因空白格式假设失败，修正后重读同一日志，没有重跑测试。

在独立 `codex/u29-process-readiness-fix` 修复读端：只对文件尚不存在或暂不可读在原就绪期限内等待，每轮检查本次owned future是否已结束；JSON解析、身份构造、monitor和清理合同不放宽。三项Windows回归通过真实 `CreateFileW` deny-share句柄和事件屏障固定读取拒绝时序，分别验证短暂占用、永久占用和子进程退出；另有非法JSON不重试的跨平台回归。未修生产代码的4项定向RED为3失败，修改后同一测试文件4/4通过；完整原生打包运行Python套件72/72、零跳过，45.618秒，实际exit0且owned清理完整。独立完整套件复核报告SHA256为 `34b20a8366c657d6a72037731051d5e0b369d184e6e9d299b35ee243ee2a0dd4`，本地修复提交 `c5600c63df088e513429cf30a9a20ac142ec1a00`。这些是该Python套件的真实回归，不是完整引擎门禁通过，也不证明原失败的具体占用来源。

Steam测试另在 `aef62e2fa725ff7a9a5e1d9435844fcd9382d29e` 保留四个SDK OFF用例的名称/init/原断言，SDK ON分支明确改为真实后端未初始化哨兵合同，不调用init且先要求不可用。每个配置的文件级用例仍为14项；实际新二进制发现尚待执行。完整ON仍未验收：Engine默认工厂及多个原生CLI/探针会初始化真实Steam，第五个回调用例还会调用真实SDK回调API。未初始化定向层不能替代完整ON或真实账号/模型/云服务验证。

第二份候选 `bbabd49d3ede5899c046fe36620f6658de84f304` 从上述修复提交整合Steam测试修正，工作树 `artifacts/validation/u29-candidate-02-worktree`，分支 `codex/u29-foundation-candidate-02`。相对原a3d仅修改3个文件：`scripts/native_package_runtime.py`、`tests/scripts/test_native_package_runtime.py`、`tests/cpp/test_steam.cpp`。原a3d与c25测量树继续冻结。新运行前selection SHA256 `0a24f4b4e1397eee82c7e21a0f3a0a26823639f715873d3e7e1bbb6700c0c554`，保留9个required作业、11类artifact、原发现门槛及所有额外验收；没有减少required集合。相同现存SDL三件库已按字节锁定，新91输入manifest SHA256 `f24eda23f92af8d970a55cd4b10b58cc0a9e328798ebb034190c532589968bc1`。新 `u29-full-gates-02/debug-01` 已配置完成并开始独立完整Debug；此时没有新完整通过结论，Release及其他原定范围继续推进。

## 2026-09-24 完整 Debug 终态与 Release 中断

`bbabd49d` 的 `u29-full-gates-02/debug-01` 随后完整结束，执行、收集、严格重建验证均通过，源码、cache及依赖检查保持稳定。C++实际1517/1517、450864断言、0失败/0跳过；Lua主147/147、隔离56/56；CTest发现71项，70通过、0失败，唯一跳过为预先允许的 `CaesuraHeadlessAiSmoke`。先前失败的原生打包运行项本次实际通过。`debug-01/root-review.json` SHA256为 `5aa005ea3b7a129adda5f5306e28a4bd7c872e4603c2920a0941996c74cce31e`，outer SHA256为 `ffc16e63e0f8d12e5f3fab0321459b3a753b521fb5e1cc7b48b3e9f7ee791a99`。本轮仍使用bbab的原53项证据采集器测试，不包含下节的新修复。

同一冻结树的 `release-01` 实际完成构建、C++1517/1517和450853断言、Lua147/56，但在CTest第35项启动后中断。续接时exec会话90104失效，OS确认目标及相关执行进程已不在；原始日志只有34项终态（33通过、1预先允许的AI跳过），不能推断其余项。外层owned执行器留下 `LAUNCH_FAILED` 失败收据，launcher退出1073807364、actual_exit_code=null、owned_tree_cleanup=COMPLETE；目标 `result.json`、完整 `raw/run.json` 与CTest XML均缺失，outer仍保持原RUNNING内容，不回写终态。独立 `release-01/root-interrupted-review-01.json` 为INCOMPLETE，SHA256 `ef6f1f4f5b0d80dbaf4600aec8db7835eb089b02e9eea0524d07fc6a7700c802`。审计辅助脚本修正了失败收据存在性与Skipped行空白格式假设，重读相同原始文件，没有重跑执行；审计时源码/cache相同不能替代缺失的运行终态。

## 2026-09-24 sanitizer 真实诊断与留存修复

Clang 21.1.8及所需运行库通过Ubuntu包元数据核对摘要后，仅解包到 `/home/ailias/.local/share/caesura-toolchains/clang21-u2-aldfnq_g`；没有apt安装、sudo或全局环境修改。实际ASan/UBSan/TSan正负控制确认该WSL运行库可执行，并保留二进制、日志和摘要。原bbab Linux干净树仅完成 `linux-sanitized` 配置，没有构建；配置成功不算引擎sanitizer通过。

真实recovering UBSan控制产生有符号溢出诊断后仍退出0；包装器捕获并丢弃stderr后，旧执行链的collector与verifier确实误PASS。源码检查还确认archive/audio CLI的预期错误用例会接受非零退出并吞掉子进程输出。新修复在每项检查建立独立诊断目录，重建四个固定sanitizer OPTIONS，跨原生清洁环境传递受控scope，在owned进程清理后记录实际PID文件。collector/严格验证器同时核对规范路径、文件完整清单、大小和摘要，拒绝漏列、交换、额外文件及复制期间新增文件；诊断优先于退出0、退出77和绿色测试计数。空运行保留空清单，不生成伪日志。

强制彩色、关闭摘要的真实UBSan控制另外证实SGR字节可造成漏判。回归先得到78项中3个失败子场景，再仅对匹配视图去除SGR，原始认证字节不变。最终父级capture环境下五套回归实际22/12/37/78/57全部通过，首尾输入相同、退出0、owned清理COMPLETE。最终独立复核重算10个日志及每套61项输入，报告 `final-increment-review-01.json` SHA256 `d11643a6eb0867777e650ecd2cb727fc1b0603796a981dd42d21ea93dab84506`。六个现有profile要求capture v1，发现下限提高到runner22、evidence78、Windows process12/POSIX process7，没有减少required集合或放宽skip。

这些证据位于 `artifacts/validation/u2-sanitizer-gate-worktree/artifacts/validation/u2-sanitizer-gate-01/`。早期real-red-01因WSL不能直接使用Windows工作树的.git路径而在bootstrap失败，未形成有效RED；real-red-02的夹具未忽略输出，实际因source_changed被拒绝，其旧status文字误标不采用；real-red-03与real-green-01虽实际复现和拒绝，但后续确认WSL `/tmp` 原始目录已消失，Windows侧review保留，不将它们作为可完整复验的原始链。未改写任何旧报告。

控制随后改用持久的 `/home/ailias/.local/share/caesura-validation/`：`persistent-red-01` 使用冻结bbab脚本，同一包装器实际退出0且旧collector/verifier误PASS；`persistent-green-01` 使用新脚本，同一包装器仍退出0，真实sidecar使collector/verifier FAIL。`real-options-02` 的6项实际运行库控制覆盖中文、空格、冒号路径及ASan/UBSan/TSan共用前缀。下一次独立WSL调用重新打开并逐项重算所有引用，又分别执行旧/新验证器，结果保持0/1；`persistent-controls-audit-01.json` SHA256为 `e691525afb279d3a1d83eebd48978e04827a476fb3dd076c04445238b6f201a6`。这些均是明确的test-fixture控制，不证明完整引擎、设备或发布验收。

验证修复已保存为干净提交 `c01b2aa940dfbfadd3b7089d83a268bcd6da41b1`，保留bbab原始冻结树。新Linux干净克隆位于 `/home/ailias/.local/share/caesura-validation/u29-c01b2aa9-a49z6ck7/source`，已于2026-09-24 09:39:57（Asia/Shanghai）启动该源码的完整现有 `linux-debug` profile与Clang address-undefined构建。隐藏Windows启动器PID18204及创建时间已回读匹配，Linux selection SHA256为 `3e438f4c60d836cf14b36dcf90aa16147cf66bcbbe78e78fb1fe77adc8e71764`，独立审查的运行器SHA256为 `06a99afd239b2eda5a1d43738320524927af97bd18521cd0e9f500c5351e84a8`。当前检查点为全量构建已结束、C++套件正在执行，尚无完整门禁结论；新证据目录为Linux源码下的 `artifacts/validation/u2-linux-full-01`，Windows指针保存在c01工作树同名目录。仍须取得c01自身适用完整Windows门禁、Linux引擎sanitizer、其余平台/SDK/账号、性能/长跑、最终包与AE1–AE8/回退证据，目标继续为全部U1–U29。

## 2026-09-24 首次完整 Linux sanitizer 终态与真实泄漏修复

上述c01 Linux运行已完整结束，outer为 `COMPLETED_REJECTED`、gate为FAIL，outer SHA256 `fee21195ce2f4e4e05c40a2e2ba0124eb05ef321fdcfd41c8687d2b1cc56e60a`，raw/run SHA256 `b34db0f50da7f52bfc8650e12cc70210cc65ee08b09fcfd7232c9bd621310df1`。configure退出0，execute/collect/verify为1/1/1。源码、已锁输入、cache及最终配置稳定；107个src编译项均含ASan/UBSan及no-recover，主程序与测试产物均实际含运行库符号。这些插桩证明没有覆盖失败结论。

C++发现1501项、450450断言，功能断言全通过、零跳过，但退出时LSan报告1884175字节/78013次分配；Lua147/56和Python22/7/78/57通过。CTest发现71项，64通过、6失败、1既定可选AI跳过，实际耗时924.66秒。失败项为Unit、Script、Audio、RpcTimeoutSmoke、ManagedRpcSmoke及Benchmark_compare_contract。前三项有实际泄漏；两个RPC的HTTP子进程功能合同通过后因LSan退出1。另有CTest判PASS的HeadlessHttpSmoke实际73/73功能检查通过，但引擎PID22637因同类诊断退出1；全profile采集器仍正确拒绝其sidecar，不能用CTest的PASS隐藏诊断。

独立修复工作树 `artifacts/validation/u2-lua-lifetime-worktree` 从c01建立。LuaManager没有析构清理导致整个VM在作用域退出/delete后泄漏；四项真实Lua userdata终结见证覆盖作用域、接口所有者、异常展开和显式关闭后重初始化，强引用保证不会提前GC。定向RED实际4/4失败并有LSan，报告SHA256 `93f81c455fcc0cd2063a39a698d63ac8a7cdd2836e0df3fcd30a7fc2b4373110`；析构复用原幂等shutdown后，同四项全部通过且诊断目录为空，GREEN报告SHA256 `8b0f79bd045f097e9fbcc3af0b9e57a844be0b2e933c1bd9d63893bc4e9c13a9`。两次更早的辅助链接失败保留，不当作测试RED。补丁经独立审查后保存为 `8b1312de202da6307f5af990b89749e1f16d48d1`；新文件进入完整和Script测试入口，注册检查204 Lua/95 C++文件通过。

SoLoud的postinit在重复初始化时直接覆盖两块重采样指针数组。既有软件音频关闭/重初始化用例功能断言1项/8断言通过，却实际泄漏384字节/2次分配、退出1，RED报告SHA256 `7ff5fa6fccb28d904fa641f97c43c7f5cbd9d1f72ca9ed2803a51e73e3a391cb`。补丁先用unique_ptr完成两块新分配，再释放旧数组并接管；同一用例GREEN退出0、诊断目录空，报告SHA256 `ddb6cb01ed7cc9fbe6a5fa9e5277169e7abc89fa30bfec5dd4825c0d9047cd7e`，其余1500项是未选中而非完整套件通过。独立审查重核原始日志，修复提交 `8eba52fc524000ea6be9a43fea349ba0f441e3d0`。这些定向运行链接冻结c01实际生产静态库并明确替换修改的生产翻译单元；不冒充新候选全量构建。

基准契约失败来自捕获环境接线：owned launcher加入受控诊断传输，但基准env_allowlist仍记录加入前环境，严格原始request对比拒绝。新真六进程回归先得到31项中1失败，报告SHA256 `5509596716321d0ce910c567ae35458034698b6b02e6b0d706389f9af745c191`；预先重建并记录相同固定传输后，普通环境31/31（`aaf1aeb37fd716a063bd98392716c3715a83278608d601ad33c741bbc818360e`）和整个suite父capture环境31/31（`96993280c03953a5c5a5b819974f521cd9a5dcd41f098ea447a1762b74c3d4eb`）通过。原采样、环境篡改拒绝、性能预算与fixture不得宣称真实性能通过的规则保留；此处不是新CPU测量。

五个HTTP子进程各240字节的图形路径诊断继续未解决。原c01两帧编辑器再次退出1，报告SHA256 `160ad350e7c937f19963e4cadcbbed9fa66b3e16e301dc524d18f2a5d7653fd0`；运行期间保存的内存映射将三处未知模块的分配地址定位到 `/usr/lib/x86_64-linux-gnu/libgallium-26.0.8-1ubuntu0.3.so`，映射报告SHA256 `7198a7f1a278045d86f076cca11df88979e8b0fb7291ece448cad646fd268329`。这只证明分配调用模块，尚不证明根因归属或独立驱动控制；没有抑制诊断、降低required范围或修改原失败结论。已准备但未启动的c01 Windows辅助运行器不会用来验证后续新源码，新候选须自身完整验证。总目标仍为全部U1–U29。

### 独立 EGL 控制与 e0 候选 Windows 验证启动

后续独立单文件EGL控制没有链接Caesura或bgfx，实际完成解除当前上下文、销毁context/surface、eglTerminate、eglReleaseThread及dlclose，逐项检查成功后输出清理标记，仍因LSan报告128字节/1次分配退出1。报告SHA256 `e95dec47687eeaa8926b6e027256411b3727e8f8010a69ea00d9704b1f96b0d5`，原始持久目录 `/home/ailias/.local/share/caesura-validation/egl-independent-z537x4kx`。返回的renderer为llvmpipe、OpenGL 4.5 Core Mesa 26.0.8；这是软件渲染证据。独立审查重核13个输入、9个原始引用及ELF直接依赖，无可行动发现。该控制缺自身maps且诊断仍有unknown module，只证明无引擎参与也会发生128字节泄漏，尚不能证明与原128字节同根因，更不能解释另外两个56字节；原完整门禁继续FAIL。

基准环境修复经独立审查保存为 `e0c019359b193d9504ca695706263bb84f36dd13`，包含上述Lua和SoLoud修复。2026-09-24 10:22:30（Asia/Shanghai）在其干净工作树启动完整Windows Debug，新build目录为 `build/presets/windows-foundation-e0`，证据为 `artifacts/validation/u29-windows-e0-01/debug-01`。隐藏启动器PID14664及创建时间已实际回读；selection SHA256 `b1dc6b5b9a775a68b5d158d792234bedcaf8530ffca201f192e27301c744e39a`，源码指纹 `b5bd19ee3d4993b270c7b9769a8f6234b3cfa38c9b9e24ceb4a8120e0adf17a3`。运行器SHA256 `fac3a9e184b3adbfb311eb99e6e72aee7e1d4b1589d46285e4ec9e84157085e5`，与此前独立审查版本仅候选SHA和build目录常量不同。91项SDL输入manifest SHA256 `525bb7df0af446303099a880d5b08bbb330df28d50bd7b1dda238212c33a8ed8`，没有复制旧引擎或测试二进制。配置实际退出0，当前检查点为完整构建进行中，未宣称e0完整通过；原Windows Debug全部required、发现下限与既定AI skip保留。

### 图形三处分配的完整独立复现

EGL控制加入真实三角形绘制、中心红色像素读回及显式GL对象删除后，完成全套EGL清理仍准确复现128+56+56字节、共240字节三处分配，实际退出1。控制报告SHA256 `06202670a9e1f6249bfabb71be7ce5a46c2abce0428e474e5a9ca9ad7879a66a`；单独补充映射报告SHA256 `2a6c06fa2d05ba8a592c87a86c7d566b84b3936585454eb9fa4944fbf556eee0`，绑定原报告、卸载前maps、sidecar、库、debug和原引擎映射，旧报告不回写。

Ubuntu符号服务对此build ID返回404，后续官方同版本dbgsym包可取得，仅私有解包。库与debug的build ID均为 `b089ec2f62ded38e9327502246aa4ccfb7929b24`。两个独立运行的三个分配PC均定位到同一库的相同偏移：`0x656842` 为 `get_cpu_topology`，`0xef1ba0` 与 `0xef1b89` 为 `u_mmInit`。独立审查重核13个输入、25个原始引用、真实ELF依赖和三个调用点，支持完整三项的无Caesura/bgfx复现；详见[可复用诊断记录](../solutions/testing/mesa-egl-lsan-independent-reproduction.md)。原sanitizer完整profile仍为FAIL，不作抑制、不减少required范围。

e0当前Windows检查点已完成全量构建、C++1521/1521与450889断言（零失败/零跳过）、Lua147/56，CTest继续执行。为后续顺序验证准备了干净Linux克隆 `/home/ailias/.local/share/caesura-validation/u29-e0c01935-7_kq9gbp/source`，源码指纹 `f5a1d64612ca501229ce2bde5058701b6bec941fb3fc42296a8d3b696d9af362`；尚未启动该树构建。Linux运行器仅替换原独立审查版本的候选SHA与build目录，SHA256 `6e8267de20c7fc3ec285e08eb71fc72e1c5c3ab3121b5462ba770bf9eee96b8b`，继续完整既有linux-debug profile，不把第三方控制结果改写为候选通过。

## 2026-09-24 e0 完整 Windows Debug 通过，Linux 接续

e0的 `u29-windows-e0-01/debug-01` 已达到 `COMPLETED_PASS`：configure、execute、collect、verify均实际退出0且owned清理完成，源码、cache、CMake、运行器、profile及91项SDL输入稳定。C++1521/1521、450889断言、0失败/0跳过；Lua主147/147、隔离56/56；Python验证套件22/12/78/57全部通过。CTest发现71项，70通过、0失败、1预先允许的 `CaesuraHeadlessAiSmoke` 跳过，耗时1405.86秒。此前的原生打包运行与基准环境契约均实际通过。

根复核重新计算原始引用、重建manifest并执行严格验证，解析JUnit元素，报告 `debug-01/root-review.json` SHA256 `c70493c15b6244a4a3d2eac4f3b769ea5f3319d211c8d5653c3bb8292b40c3cf`；outer SHA256 `950609fe87576b9603c28cca8ebeb97da384a9918663af4b036c8001919e5313`，raw/run SHA256 `e52bfe073e8f74a7f2e315e37cff079551582d92d93d05a43b7155d185b8b4d7`。这是e0、SDK/FFmpeg关闭的本机Windows Debug证据，不替代其他配置或最终交付。

独立终态审查另重算341个引用、185个不同文件，核对22份原始流、JUnit、四阶段run/result身份、91项SDL精确集合、cache实际值及manifest/raw绑定，无新发现。实际HEAD保持e0且clean；该审查没有另外重算整树指纹，也没有重跑测试。11份诊断capture完整且目录实际为空，但本Windows配置sanitizer为OFF；不能据此宣称Linux sanitizer已通过。

确认Windows启动器已退出后，于2026-09-24 11:00:25（Asia/Shanghai）启动e0干净Linux克隆的完整既有linux-debug profile，Clang address-undefined与原11项required保持不变。隐藏WSL启动器PID9176和创建时间已回读，selection SHA256 `23a9f530ee18140df024d6bf1541af179b4cac7c3641b4b3eab6417eaecb8ff3`。原始证据在新Linux源码的 `artifacts/validation/u2-linux-full-01`，Windows工作树同名目录保存指针与启动收据；此时尚无Linux通过终态。

FFmpeg-ON后续配置的独立辅助脚本审查发现测试输出DLL未绑定及最终读失败可能丢失收据两项问题，已补齐主程序/tests两处共10份DLL的最终检查，以及逐项捕获读取错误、保留原错误并保存拒绝终态。增量复核关闭两项发现，运行器SHA256 `748830aee0184feabda35be8913cfddcb9db1a5ad43d672ceb5a02626958e1fb`。仅依赖准备已实际执行：155项现存本地头文件、库、DLL与许可证复制后两端重哈希，manifest SHA256 `a938eb84c1eefb30ca857991a12dd85d02ca43ede19be02c78e3fb2c4feea9db`；未复制旧引擎/测试二进制，尚未运行FFmpeg-ON构建或测试。


## 2026-09-24 e0 Linux sanitizer 终态与 Release 恢复

e0的完整Linux sanitizer运行已正常结束为 `COMPLETED_REJECTED`，gate=FAIL。configure、两个实际二进制符号检查均退出0；execute/collect/verify均退出1，owned清理完成，无超时或强制终止。源码、166项锁定输入和cache稳定，实际107条第一方编译命令含ASan/UBSan及禁止恢复标记，主程序与测试二进制均核对实际sanitizer符号。

C++实际1505/1505、450475断言、0失败/0跳过，进程退出0且其持久诊断目录为空；Lua147/56、Python22/7/78/57全部通过。CTest71项为68通过、2失败、1预先允许的AI跳过，耗时770.16秒；失败仅为 `CaesuraRpcTimeoutSmoke` 与 `CaesuraManagedRpcSmoke`，此前失败的Unit/Script/Audio及基准环境契约已通过。五个图形子进程各留下240字节/3次分配的LSan诊断，严格profile仍拒绝，不能由功能断言通过推断sanitizer通过。

原证据位于Linux克隆 `artifacts/validation/u2-linux-full-01`；outer SHA256 `2b3675467d1f96510fa1e4c221a956fa187015a0af4aca0b855514c1d722ae74`，raw/run SHA256 `13f791d55693fdc69f89089a94787c842694ebc057d86690999f492b7f1a10b5`。根复核报告SHA256 `78f2f6c3f25fdc14beaa955a4342b236a4a1cc6724bf41a03fa3977bf30f2ef2`，重算引用、源码身份、锁定输入、实际capture集合、compile_commands及二进制、重建manifest并重跑严格只读验证，确认FAIL。

本次HTTP功能检查73/73通过，但其受控停止的owned子进程实际退出1。原合同保留受控停止的真实退出码，不能普遍将主动terminate要求为0；诊断捕获独立于功能结果，五份sidecar使完整门禁失败。补充的当次maps观察启动太晚，没有取得任何进程映射，随后只终止已核实身份的观察器，未中断验证；不把该空观察、先前c01映射或独立EGL复现改称e0当次PC归属证据。

Linux启动器退出后，Windows Release于11:23:34启动。续接时OS两次确认原PID24728及执行子进程25340均不存在，相关构建/验证进程也不在；日志停在测试翻译单元编译，原outer仍为RUNNING且缺raw/run及执行完成收据。原目录保持不变，独立 `release-01/root-interrupted-review-01.json` SHA256 `8431eac25572903ab70504b6fbac9328cd5e17ead050e9cb7d7320f2eff60bad` 记录INCOMPLETE；具体中断原因未证实。

11:35:31（Asia/Shanghai）以同一未改动的运行器、源码、cache和原完整windows-release profile启动 `release-02`，隐藏PID21852及创建时间已回读。复用同候选已有构建目录的中间编译产物，所有完整检查重新执行；没有拼接首轮部分日志取得通过。此检查点尚无Release终态。

普通GCC Linux Debug/Release为原计划及现有CI的另一组既定配置。新入口分别创建全新build与证据目录，保留完整11项required与原阈值，显式记录sanitizer为OFF；此配置不会替换上述Clang失败记录。首次静态独立审查发现finally读失败可丢失终态，已在单独v02入口加入逐项捕获、拒绝通过并保存错误的结束检查，原版保留；当前尚未执行普通Linux构建/测试。FFmpeg-ON仍仅完成此前依赖准备与入口审查，未产生完整运行证据。全部U1–U29目标继续。

独立终态复核另重哈希223个原始引用和166项输入，重算当前源码身份，重取两个实际二进制的nm输出并与保存摘要一致；重建manifest及严格验证仍为FAIL。11份capture实际库存与收据相同，仅CTest包含5份诊断，其余为空；JUnit、C++实际退出和HTTP功能/退出边界均逐项回核，没有新发现。普通Linux v02入口结束检查增量也已独立复核，原终态丢失问题关闭，尚未执行配置。


### e0 Android 独立输入准备

同一e0提交的Android独立clean工作树已完成生产源码谓词预检（6540文件、2物理链接），新请求继续使用原v2工具/依赖和临时测试签名合同，尚未执行构建。当天实际adb查询仍为空；完整输入摘要与设备边界见[U24接续记录](2026-09-20-015-android-package-validation-execution.md)。Windows Release attempt02仍顺序执行，未并行启动Android或其他重型门禁。

## 2026-09-24 e0 Windows Release 完整通过与后续配置

上述同一冻结源码的 `u29-windows-e0-01/release-02` 已于12:09结束，outer为 `COMPLETED_PASS`。完整构建、C++1521/1521及450878断言、Lua147/56、Python22/12/78/57、耦合和注册检查均通过；CTest发现71项，70通过、0失败、1预先允许的 `CaesuraHeadlessAiSmoke` 跳过，实际1348.59秒。execute/collect/verify均实际退出0，owned清理完整，无timeout或forced kill。源码、cache、运行器、实际CMake和SDL输入首尾稳定。

根复核重新读取原始日志、进程收据、JUnit、锁定输入与源码身份，重建manifest及严格验证仍PASS，未重跑测试。outer SHA256 `208666adb1bdcae3f72afcd521d3d64fb85158496190392057d1efd46a54d679`，raw/run SHA256 `ee4a1a21ddee00ee61f07227db86bfad98e44adf28618e094a7b52bcb88d2ec2`，root-review SHA256 `991b4625c293b184a170b168da00722a2adae94654d37d5ad92afd28da2108f1`。Release首轮中断记录不变；该完整通过仅属于SDK/FFmpeg OFF的Windows基础配置。

独立复核重算135项root引用、200项嵌套引用及185个独立文件，并再次核对源码、实际运行DLL、11项检查、manifest重建与严格验证，结论一致。采信 `release-02/independent-review-02.json`，SHA256 `7d08d1b7d21582674942c79e1b298ca88f0f5a6b9998f0ea3ac5847390232d7a`；其首份辅助审查误取嵌套doctest的36个断言，已另存更正为最终1521项套件的450878断言，旧辅助报告及原始测试证据均不改写。

随后针对现存e0 sanitizer二进制进行一次新的两帧编辑器诊断复现，owned进程实际退出1、清理COMPLETE，捕获一份240字节/3分配诊断及128份实时进程maps。映射与保存receipt/process.json同一进程身份绑定，三处调用在原字节不变的Mesa库内偏移分别为 `0x656842`、`0xef1ba0`、`0xef1b89`，与此前不含Caesura/bgfx的独立EGL真实绘制控制一致。新Linux证据目录为 `e0-gpu-maps-bgpjxc_a`；review SHA256 `ad7606bdf9cdceaaf4e9312ef07916827eb7c00b602e25dd6146f7d12d5b26b9`，frame-mapping SHA256 `261abe8a3c070519cb06123616a64f05fe89320ca44998c41df5f355970ac34a`。这只是本次定向重放的PC归属，不补写早前完整运行缺失的maps，也不将完整Linux sanitizer FAIL改称通过。

12:14:56由主代理独占启动既定的普通GCC Linux Debug全量配置，Windows隐藏启动器PID18536与创建时间已读回，使用单独新build与证据目录。运行器SHA256 `3fa59f3ae60b98bf6d962def53f7844b2773eb7b96178b3f8315831e67b3e546`，运行前selection SHA256 `deda4a17adc38b10b45600e96a54dfb75376d5c7f4cc89e78cc5574d3d5d7d58`，原11项required与发现门槛不变，显式sanitizer OFF。此检查点尚未形成该配置终态；FFmpeg、Android等重型门禁仍顺序执行，全部U1–U29目标未完成。

## 2026-09-24 普通 GCC Linux Debug 终态

同一e0干净Linux克隆的普通Debug完整profile已结束为COMPLETED_PASS。GCC/G++15.2.0、Debug、sanitizer/FFmpeg/SDK OFF，原11项required保持不变；configure/execute/collect/verify实际均exit0、owned cleanup COMPLETE，无超时或强杀。C++1505/1505、450475断言、0失败/0跳过，Lua147/56、Python22/7/78/57通过；CTest71项为70通过、0失败、唯一预先允许的AI跳过，耗时615.10秒。Linux启动器PID18536已确认退出。

Linux原证据目录为该e0克隆的 artifacts/validation/u2-linux-foundation-e0-01/debug-01。outer SHA256为13954318d27d629c2936aec99f0b4126cde16570f143d64eed2b1c55c082e709，raw/run为7cf9af3df335112fce6667db6eae8dffac0ad5298a22302429b612f2dbeb730b，根复核为8cf8941f9624e359c38ce2905958270fd695285a93f7c0740b49d28702ce87fc。独立复核重算197项根引用、151项锁定输入、共372个不同文件，并重新计算当前源码/夹具身份、重建manifest与严格验证。107个真实第一方编译项对应106个源码文件及107个对象，均使用g++ Debug且无sanitizer标记；实际ELF依赖也无ASan/UBSan。独立报告Windows副本debug-01-independent-review-01.json摘要bc25cd5bb271ab16bd4ea15fc5a7e2a4392f1999a1ec850d7de71b5ae2c6a2fd。

此结果只证明普通GCC Debug配置。11份诊断目录完整且为空不能升级为sanitizer证据；此前完整Clang FAIL不变。Linux Release、FFmpeg ON、SDK ON及新Android门禁仍未执行。

## 2026-09-24 Windows 最终 ZIP 的真实失败与时序诊断

e0 Release最终包使用既定CI软件音频模式和原生产ci_package_lane.py，在仓外新目录顺序执行；没有更换模式或减小必需范围。第一次D:/caesura-u29-windows-e0-package-01的CPack成功，但静态检查拒绝缺失editor/dist/index.html，运行时未执行。原ZIP摘要fa866a4159d9cbaa395ab8e410ee186a6150d24fdacf32ac6b61afd66c6961ea，lane摘要b2c33a09ac9946da0135b0dcaad5fdb153b9d593854116f13dc4d67a716a4312，失败原件保留。

缺项对应现有CI在CPack前的editor静态构建前提。按照原锁文件执行npm ci、现有38文件/636项测试以及TypeScript/Vite生产构建，全部实际退出0，生成95个逐项哈希的dist文件。Node v24.16.0/npm11.13.0；源码与锁文件首尾不变，owned清理完整。editor-build-01/outer.json摘要f3d2dfced7f745ff9729d3f4752809e58e027d73a11dec1db895a071d1179e0f。这是既有随包资源回归，不恢复Studio功能开发。

第二次新目录D:/caesura-u29-windows-e0-package-02的CPack实际退出0。新ZIP摘要20a1f7b7968f7eef25c456d64ce600e9373303256c0f4d77a957c0a57e341ab7，lane摘要51872719eaf2e1418701a674a32712c0455ba9941e61c61b9e3e3ffdd4193920，package-run摘要5b05195d252d59adc15230b937f914c459941a5c1c4c7d1dbf85bac036d18229。静态检查PASS，显式token与生成token的编辑器stage均PASS；普通Engine的60帧stage正常exit0、cleanup COMPLETE，但仅得到17615个PCM frames/35230个样本、0非零。原门禁按require_signal=True正确拒绝，runtime为FAIL、accepted=false，作者创建/构建和新作品stage未执行。源码、原ZIP、准备副本及运行副本原有文件稳定；主EXE摘要1a5454a01b2b191f725694e82d792b3e17a11caf69ffe435e68b513817305e4d。

独立只读审查定位一个P2：native_package_runtime.py用60帧要求演示产生非零音频，但Engine按实际tick差供给dt并封顶250ms，没有固定时间步长。原demo的font/pt/bg逐token yield，之后wait仅消费进入等待后的300ms，故全程PCM约0.367秒并不能证明该wait完成。原日志只出现classroom背景，无BGM启动或音频加载错误；包内demo、scheduler、wait等与e0字节一致，daily.wav为真实非静音PCM。没有逐帧记录，不能虚构当次wait剩余毫秒。

为验证继续推进后的实际行为，只执行一次预先指定的600帧诊断，使用同一ZIP payload完整新副本、同一EXE与隔离环境。D:/caesura-u29-windows-e0-audio-diagnostic-02实际正常exit0，无stop/timeout/forced kill且cleanup COMPLETE；原demo播放daily.wav，真实软件PCM为161231 frames、322462 samples、281762 nonzero、0 nonfinite，peak约0.208525。原payload及运行副本全部原有文件首尾一致，源码仍clean e0；diagnostic.json摘要a57708bc101bd684d88eed4ffa39837e4f0a0736e1dc40464f46105b981a5067。首个辅助入口因ZIP路径枚举错误在启动Engine前失败，diagnostic-01摘要c520ea6e44a4dc8c59a12f007286caf5e3c11db6c88c2b4c0721492ea191f3ac保留。

600帧仅是根因诊断，不是修改接受阈值或原包通过。现有导出回放也使用实时dt，headless或editor不能替换普通GPU入口。后续最小修复方向是在正常CLI/EngineConfig提供显式固定模拟步长，默认继续实时；同帧Lua/audio/render统一使用所选步长，验证保留60帧、原demo、真实SoLoud、require_signal=True和正常shutdown。先建立真实红绿及非法输入/不足时长/静音负控，再执行适用完整门禁和新最终包；本检查点尚未修改生产源码，e0原两份包失败不改写。

## 2026-09-24 固定模拟步长修复与真实正负控制

最小修复已经保存为干净提交 `eda98b22c1f8a88e422892311e8a17f9cb2a2144`，分支 `codex/u29-fixed-step`，工作树 `artifacts/validation/u29-fixed-step-worktree`。`--fixed-step-ms` 严格接收1–250的整数毫秒，EngineConfig内部0继续表示实时；越界配置在后端初始化前拒绝。实际Engine主循环在原实时dt计算及封顶之后统一选择模拟dt，再传给Lua、音频、GPU监测、渲染、视频、动画和小游戏。默认行为不变；固定模拟步长不保证外部I/O、输入事件、GPU驱动或实际耗时可重复。

软件音频最终包与新建作品验证继续60帧、原演示和真实SoLoud，显式传入16毫秒并要求唯一匹配的时钟标记；实际音频设备模式继续实时。真实PCM非零/有限、正常退出和输入稳定性门槛均保留。新C++主循环回归3项、CLI集成23项加入当前测试入口；六个profile的C++发现下限各增加3，CTest下限71增加到72，原11项required集合不变。

定向红绿均使用真实执行入口。旧e0 Release二进制运行新CLI套件为0/23、退出1，报告 `D:/caesura-u29-fixed-step-cli-red-01/report.json` 摘要 `ceb018cb5f5c0a39f94b184be16e5397373ad4f070a9f32ec01d55f88cc583ea`。新源码的完整诊断性Debug构建成功；新增C++回归3/3、266断言通过，1521项是本次过滤未选中。真实CLI 23/23、原生打包运行Python套件73/73全部通过。它们在提交前执行，报告诚实记录dirty e0及相同源码指纹，不能当作已冻结提交的完整门禁。C++、CLI、Python外层收据摘要依次为 `022e8f1296e13439e41e8da84946a097bc7b6e1a4737766bf94940d1ba9844ba`、`104fa68a953ed5f6efce9bd0fdfd782c816e430af37a1ebcfac076244dbf50e8`、`b6a65d7edd0602cf958431c14fdbd0736c08e9a741607618845eb932db320461`。

同一新Debug二进制、真实D3D11、原仓库演示及真实SoLoud软件输出的60帧正负控制均正常exit0、owned cleanup COMPLETE，无超时或强杀。16毫秒产生46080 PCM frames、92160样本、52514非零、0非有限，观察到daily.wav播放并被原信号门禁接受；1毫秒仅推进60毫秒模拟时间，产生2880 PCM frames、5760样本、0非零，被原门禁以 `Packaged demo produced no nonzero software PCM` 正确拒绝。报告 `D:/caesura-u29-fixed-step-gpu-01/report.json` 状态为TARGETED_GPU_CLOCK_PASS，摘要 `6c9518d77a0af002b1c005bff3be24c1501adc4b23955d030b1772bd0023231d`。源码、工具、资产与二进制的锁定输入首尾一致。这是仓库资产上的实际GPU/软件音频控制；物理音频设备未测，新最终ZIP尚未验收。

生产与测试改动已独立只读审查，无可行动发现。正式门禁辅助入口在执行前另修正同一源码/配置缓存复用检查并经增量复核，保留旧入口；只有匹配源目录和全部预期cache的构建目录可复用，所有required检查仍重新执行。

2026-09-24 13:26:01（Asia/Shanghai）启动eda自身完整Windows Debug门禁，隐藏启动器PID28172及创建时间、完整命令行已经读回。证据目录 `artifacts/validation/u29-fixed-step-gate-01/debug-01`，运行器摘要 `73b6bd3ff6fb84ac5c6c454c20902517bf40ec6e0437b0d52beb4d43bb4c47e3`；91项SDL输入manifest摘要 `19259f2d7bd008339f7194fed7aef8cd6b62e02233fdb27d971c1bb5f8ff7db4`。selection摘要 `de36fb1f96302783960af13d1079abc254d8e7a56822065ff43a55237d4533a0`，干净源码指纹 `52ea5952f0d1e4f824a8ab78f1062f05ec13683ce13a12b9afe42f6664933979`。配置已经实际退出0，完整构建后进入C++套件；当前尚无完整终态。先前e0的完整通过、失败和中断证据保留，不迁移成eda通过；新Release、平台/SDK、性能/长跑、最终包、AE1–AE8与回退仍按原范围验收，总目标仍为全部U1–U29。

U23批准操作已完成且不重复发送mutation。13:25再次只读获取GitHub master保护、rulesets与有效规则，全部与已应用快照一致：唯一context仍为 `Verify release inputs / Verify exact release inputs`、App15368、strict=false、一次PR批准、管理员例外和禁止强推/删除保持。新读回保存在记录工作树的 `artifacts/validation/u23-required-check-preparation-01/readback-20260924-132509/result.json`。未合并、发布或改动标签。

### 后续输入与托管验证准备

eda的完整Windows Debug继续由同一PID28172执行。已完成全量构建、C++1524/1524及451156断言（0失败/0跳过）、Lua147/56和Python22/12/78/57，当前CTest正在执行全部72项；尚无完整终态，不以中途结果代替严格门禁。

Linux新干净克隆位于 `/home/ailias/.local/share/caesura-validation/u29-eda98b22-euahtzbi/source`，实际HEAD为eda，指纹 `bce5cecf7d4b0dbac89600bceba92c72fb0465019bedbc8dfef14d77e3a0523c`；没有开始新Linux构建。普通GCC入口仅从已审查的e0 v02适配提交、分支及路径，保留完整检查和失败终态；入口摘要 `8098b162a4501491de638c6164ef980eb0ac8601eb25c399708911313333095f`。第一次WSL调用在默认shell解析括号路径时失败、尚未执行Python；改用 `wsl --exec` 后准备正常退出0，没有重复启动构建。

后续最终包的编辑器静态资源已单独准备。重新核对旧真实构建收据、命令日志和进程结果，确认新旧91项前端源码/配置全部同字节，将原95个dist文件复制到忽略的暂存目录并逐项重算摘要；未复制旧原生二进制，也未修改活动构建的editor/dist。`u29-fixed-step-package-inputs-01/editor-reuse.json` 摘要 `2b835331296d04659163d7f8557bdde22f8d14aa819381afddfd55f161f30efa`，状态IDENTICAL_FRONTEND_DIST_STAGED。这是明确输入相同的复用，不是重新执行636项编辑器测试或最终包通过。

托管CI预检实际发现平台文档同步锚点仍为8bcd4c4d，默认freshness检查退出1。独立文档工作树 `artifacts/validation/u29-fixed-step-docs-worktree` 从eda建立，只同步上述记录、锚点和生成文档，冻结测试树不动。矩阵逐能力status、历史commit、验证日期和证据保持原值；锚点更新不授予当前平台通过。API/命令文档生成结果无内容差异，平台及计划事实默认check已通过。该文档提交将作为另一个完整SHA进入现有只读CI工作流；本地eda证据仍归属eda，托管结果须按实际新SHA独立验收，不自动合并或发布。

文档同步已提交为 `71c9732d7efd92dd7ed43a70be6b2123765f8fdb`，相对eda仅11个docs路径改变，未修改代码、测试、workflow或required策略。提交后的默认平台freshness检查也实际通过。2026-09-24 13:40推送新独立分支 `codex/u29-fixed-step-docs` 后，GitHub读回ref与完整SHA一致；没有创建或更新PR、合并、改动tag或启动发布workflow。

13:41只调度一次现有CI入口，实际运行 [35961032218](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/35961032218)，attempt1、workflow ID291151646、path `.github/workflows/ci.yml`、event `workflow_dispatch`、head/source `71c9732d` 均由GitHub API读回匹配。源码及policy锁定job107509450875已success，Android静态job107509524280已success；三桌面Debug、iOS及Android审计作业执行中。9个required producer和11类artifact保持原选择，后续Release/最终包/聚合结论尚未取得。原始push、dispatch和读回记录位于文档候选树 `artifacts/validation/u29-hosted-71c9732d-01/`。不能将本机eda中途结果迁移为71c候选通过。

Linux及frontend准备已独立复核：脚本变化精确限定于候选、分支及路径；实际Linux HEAD/clean/fingerprint、4条准备命令的8个原始引用、旧编辑器构建的7项锁定输入和5条owned命令，以及91项双源码输入、95项源/目标产物均重新核对一致。结论READY_PREPARATION_ONLY，报告 `u29-fixed-step-linux-01/independent-preparation-review-01.json` 摘要 `0ee2b0b80566428311292be298188dfd927ba607128a54bb69235ce514ea27f1`。

另已将FFmpeg-ON的155项既有依赖复核后复制至eda树，源码首尾指纹不变；新manifest摘要 `240d2fc6bf3f715ef32456f4e3adc0baaa88d302a802d29d970c1791fef731d2`。待执行入口从已审查e0版本仅适配SHA、build目录、SDL真实来源及显式CMake路径，摘要 `125288fa2bebc157b2a0439a82961d94e18fabf64ac2599a2b9c7aaff6f4ee32`。仍未进行FFmpeg-ON配置、构建或测试，旧准备记录也未被改写成通过。


## 2026-09-24 eda Windows Debug 终态与 Release 启动

13:54已取得冻结eda98b22的完整Windows Debug终态COMPLETED_PASS。实际全量构建、C++1524/1524及451156断言、Lua147/56、Python22/12/78/57、耦合与注册全部通过；C++零失败/零跳过。CTest实际72项为71通过、0失败、1预先允许的CaesuraHeadlessAiSmoke跳过，1349.50秒，新固定步长CLI集成包含在内。configure/execute/collect/verify均exit0、owned cleanup COMPLETE，无超时/强杀。源码、91项SDL输入、cache、profile、CMake和运行器首尾稳定。

原证据目录为eda树artifacts/validation/u29-fixed-step-gate-01/debug-01。outer摘要bbad7c2c85789d294508a8001a60132604a705a42f6ee17bc4663bacbf2cef16，raw/run摘要4b30d3788c9b6b98ef33a517354ec4f4108a1e08376cbcb4ceced63d2e53b017。根重新核对原始日志、进程收据、JUnit和锁定输入，重建manifest并重新严格只读验证，11项required仍全部PASS；root-review摘要32c42584a2ab3d6b5ce6d0e528934f417467c3007e262e7adbbcd37119052ef1。没有重新运行测试；此结果属于本机eda基础配置，不替代SDK/FFmpeg ON、其他平台或71c托管候选。

OS确认原Debug启动器PID28172已经退出且没有其他本机引擎构建/测试后，于14:00:12启动同一冻结源码、同一经核对多配置cache的完整Windows Release。新目录release-01，隐藏PID14796、创建时间与完整argv已由OS读回；selection摘要aa0507242cc948bdb86cc32c5b779ee37b212ff0b8f14d48a19f6e2b9d0cc7c3。所有11项检查重新执行，不拼接Debug结果；此检查点正在实际编译，尚无Release终态。

14:00:55另对U23进行一次只读确认，没有发送mutation。master保护、rulesets和实际生效规则均与原成功应用快照一致，记录在u23-required-check-preparation-01/readback-20260924-140051/result.json；用户授权的最小变更已完成，不重复写入。

FFmpeg输入及后续入口也完成独立审查：实际155项双端依赖、91项SDL及3个库来源全部重核，入口仅有预期候选/路径/工具适配，未发现问题。u29-fixed-step-ffmpeg-01/independent-preparation-review-01.json摘要e464e30a9aaaf1841487a2a320b5dd25de27d42aa926e23873b8960972fb13d2；结论仅READY_PREPARATION_ONLY，未进行FFmpeg-ON构建。

### 同次托管 CI 的首个失败

14:02读回CI35961032218/attempt1：Linux Debug、Android静态与CMake审计、iOS CMake探测作业已报告success；macOS Debug作业107509524273为failure，其后macOS Release/最终包为skipped。原始作业日志显示严格U1拒绝cpp、validation-evidence和ctest三项；不能用job外层成功或其他平台通过抵消。原始job与日志已保存在71c树u29-hosted-71c9732d-01，实际保留的macOS诊断产物ID10792267914，GitHub声明ZIP摘要aad20c49d6c61a25126e0862b4a2a293922ee4ba9021666c4bd2637e7a783028；正在获取和分析原始产物，具体根因尚待核对。Windows Debug和Linux/Web后续包作业仍执行，整体候选未通过，不重复调度或修改required集合。

独立Debug终态复核已完成且无发现：再次核对138个根原始引用、累计196个文件引用、43项raw/bundle内容、源码/夹具身份、全部11项检查与实际过程收据；分别重建manifest并严格验证均通过。独立报告debug-01/independent-terminal-review-01.json摘要aeffefc070ef92d2cbc5664f6fae1546b7c79708b836fc13770347fe1315fdf3，状态INDEPENDENT_ORIGINAL_EXECUTION_VERIFIED。它证明复用匹配中间产物后的单次完整Debug执行，不是全新clean build，也不扩展为Release、SDK/FFmpeg、其他平台或最终包通过。

记录树的Live2D指南修正与U26静态调查记录也已独立核对当前接口、RPC解析、宏、历史范围和新增链接，零可行动发现；两份文档审查前后字节稳定。u26-docs-review-01.json摘要f4ab27da0d9e4f175fc9603c7a7840c0449e8ebe23f6ff99acb15d3ca5e367cd。指南修正仍仅在记录树，未改变活动本机及托管候选；两项SDK疑点继续等待真实复现。

macOS诊断ZIP首次只读下载因240秒超时留下未完整文件，未提取或认作有效产物；原部分字节保留。1024字节Range请求实际退出0且与原前缀一致，随后14:10以独立PID30304启动分段续取；只有最终34875802字节及上述GitHub完整SHA一致后才允许提取分析。此为取回既有失败证据，不是重新执行CI或测试。

## 2026-09-24 托管终态、七文件修复与误删后的接续

当前代码候选为 `09601b20eeeba6053baec4758ee51a1ef0b222e9`（`codex/u29-ci-portability`），基于已执行托管 CI 的 `71c9732d7efd92dd7ed43a70be6b2123765f8fdb`。七文件修复已提交；删除前的交叉代码审查无可行动发现，恢复后关键工作树的 Git 文件、索引及实际符号链接也已独立核对。当前使用 `E:/CaesuraRecovery/20260924-1446/worktrees/u29-ci-portability-worktree`。记录树 `40fa2421a56e2833824137e17bacd58e83fd0732` 的五份文档已同步至本候选；原 D 盘证据路径在上述历史段落中保留来源含义，不作为现在仍存在的运行入口。

### 原 CI 终态与已提交修复

原 run [35961032218](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/35961032218)/attempt1 的实际 source 为完整 `71c9732d` 提交，终态为 **6 success、4 failure、2 skipped**。macOS Debug、Windows Debug、Linux Release 与聚合作业失败；跳过的是macOS Release/最终包作业107513999092与Windows Release/最终包作业107516599940；Web作业成功，Linux Release/最终包作业失败。恢复时重新读取的是同一次原运行及其 artifact，没有重跑 CI、修改失败结论或缩小 required 集合。

恢复后的 macOS 完整诊断 ZIP 与小包均保留真实失败：`U26 coordinator: optional capability validates captured bytes once` 在 Cloud 侧再次执行本应只消费一次的 `afterRead`，`side == Local` 和随后 Complete 断言失败；独立 cpp 为1508项、1507通过、1失败、0跳过。Linux Release 独立 cpp 原件为1508项、1502通过、6失败、0跳过，共15个失败断言，均在音频业务合同测试中；日志实际输出模式为device。同次后续 CTest 中 C++ 全部通过是另一调用，不能覆盖独立 cpp 的失败。Linux 0.1秒静音夹具与后台设备混音时钟使句柄可能在业务断言前结束，是源码支持的时序解释；原日志没有逐次断言耗时，不能声称已经测得每次到期时刻。

`09601b20` 的改动范围如下，原业务断言、阈值和设备验证范围保留：

- `scripts/compare_benchmarks.py` 与对应 Python 测试：采集和离线复核均拒绝缺失的操作系统进程身份，不能用两个null相等认作匹配；正向工作进程等待实际 `process.json`，保留六个独立进程。
- `tests/cpp/test_cloud_save.cpp`：用 `std::exchange(afterRead, {})` 明确消费一次回调，加强恰好一次与Local侧验证。`std::function` 移后状态未指定，单纯 `std::move` 不能保证源回调已空；删除前的小型libstdc++探针只证明其实际标准库行为，不是macOS完整集成绿灯。
- `tests/cpp/test_audio.cpp`：六个业务合同测试改用既有真实SoLoud `ManualMix` 输出，以48kHz显式混音推进时间；通常每块512帧，退休控制为8块共4096帧，保留池满拒绝并增加退休后重试正控制。未延长wav、增加sleep、放宽断言或修改实际device验证；软件混音不证明物理音频设备。
- `tests/scripts/test_validation_evidence.py` 与 `test_engine_soak_driver.py`：仅规范化夹具自己创建的临时根，生产路径边界不变。
- `tests/scripts/test_release_artifact_download.py`：区分较早的socket timeout与总时限错误，明确宿主时钟边界场景；保留真实网络负控及小于0.5秒要求，生产下载实现未改，也未声称解决Windows socket shutdown的诊断现象。

删除前 Python 四套完整实际计数依次为78、18、33、15，已有相关真实红负控和交叉审查。**这些完整根报告当前缺失，计数只作为历史观察。** 两个C++文件在删除前尚未编译或运行；恢复源码不改变这一验证缺口。当前未执行 `09601b20` 的完整门禁，不能由旧71c或eda的结果推导新候选通过。

### eda 本机结果的历史观察边界

删除前 `eda98b22` 完整Windows Debug已通过根复核和独立终态复核；完整Release后来实际完成，根复核确认11项required均通过：全量构建、C++1524项零失败零跳过、Lua147/56、Python22/12/78/57，CTest实际发现72项、71通过、0失败及1项预先允许的AI跳过。Release原 `root-review` 摘要为 `cb30f7a391fa103559591b626685c7d4ae939502a2f26a883a8b469af6e1d85c`，`raw/run` 摘要为 `0793b36687a169d08ff00ce19b4eefb7797df70a1d9c3dddb67dab47748d53bd`。

误删中断了Release独立复核，**没有完成的独立复核结论**。eda Windows Debug/Release完整根证据当前未找回；上述已提交记录、对话观察和原摘要均不能替代原件，也不能把幸存的CLI或其他子用例拼成完整门禁。Debug历史独审、Release历史根复核、Release独审未完成与现在可重核的恢复文件分开记录。

### 已恢复原件与当前缺口

外部清理脚本误删构建和验证目录后，按用户要求停止开发并优先恢复。完整Git备份保留在C盘，E盘独立仓库恢复30个工作树；另恢复17,705个幸存文件。四个关键工作树的26,164条索引记录（26,156普通文件、8个符号链接）已独立核对；恢复副本的完整对象检查通过。恢复脚本外层曾返回1而其212个Git子命令和落盘报告均成功，这一差异保留，源码恢复结论来自后续独立核验，不改写外层退出码。

恢复材料位于仓库外 `E:/CaesuraRecovery/20260924-1446/`，以下路径均相对于该外部根；它们不打包进源码仓库。该根 `README.md` 与 `CONTINUATION.md` 说明恢复范围及未完成项。本文同步时重新计算 `recovery-summary.json` 的13个引用，全部大小与SHA256相同。

| 外部恢复入口 | 当前可核范围 | SHA256 |
|---|---|---|
| `recovery-summary.json` | 全部恢复类别与已知缺口索引 | `701fa9d6cab7af311bb2c2a8482c5391fc8dd567df8f9ea6248441dbf6b90caf` |
| `independent-key-worktrees-review-02.json` | 四份关键源码/索引/符号链接的独立复核 | `fc37c09754ab852de15c6edec526a33274311c8538073042246940bf9943e933` |
| `recovered-hosted-71-01/manifest-retry-02.json` | 四份小ZIP和完整macOS诊断ZIP的整包摘要、129个提取文件、原run/jobs及四份失败job文本日志 | `011e339450e03b7bdbc530498515fcc97561a18708c3b65b46f9c5e87f9a9ec5` |
| `recovered-hosted-71-01/independent-parent-review-01.json` | 五份ZIP及全部提取成员独立复核，171个文件引用重新哈希 | `1e968ac8aaa58efc9ea3c861eee9e0f91404cf8f3c7ffdc401934a3953715929` |
| `recovered-linux-selected-01/report.json` | Linux Release原artifact10792814010的三个诊断成员，仅成员CRC和删除前SHA匹配 | `69574a5e9dde115c41ad88ca998166a51ed28cc89eb197654c1cea86553ce6c1` |
| `recovered-evidence-linux-temp-01/manifest.json` | Linux/Temp原件4,071文件逐项映射和摘要；包括完整恢复的四个Linux证据目录378文件、Debug/Release CLI各98文件，其余3,497文件为限深盘点片段 | `b9a0ea740d35adbd33289af46cb5f299f658c57a1b6e3189d4aa7c47ec41fb4c` |
| `github-protection-readback-01/result.json` | 误删后的新只读服务端保护读回 | `6b41d831b7bbee457b474ff9388a854247cf7db4360f3e6fcbf786dc3c9ef244` |

五份托管完整ZIP均与删除前确认的整包摘要一致；macOS续取保留首轮超时收据和部分字节，只补缺失的272,794字节后才验证整包并提取。Linux三成员报告明确 `archive_sha256_verified=false`：未下载并验证整份31MB诊断包，成员CRC/SHA不能冒充整包digest通过。重新取得失败日志属于恢复证据，不属于重新执行或接受候选。

仍缺失的完整原件包括eda Windows Debug/Release根证据、七文件修复的完整Python根报告与准备manifest、U23原mutation/读回收据，以及多项被删依赖暂存和辅助运行器。已有子用例、Linux及Temp片段保留其来源与完整性范围，不补造缺失日志或原始receipt。U23新读回确认唯一required-check仍为 `Verify release inputs / Verify exact release inputs`、App15368、strict=false，一次PR批准及其他既有保护保持；它明确是误删后的新读回，不伪装成原操作收据，也不重复mutation。

### 后续验收范围

本次只同步文档与最新非docs提交 `09601b20` 的生成锚点，平台逐能力历史status、执行commit、日期及evidence保持原值。源码恢复、静态文档生成和通过记录不授予当前平台或发布验收。后续需重新准备并核对实际依赖输入，由根代理独占新的重型构建，对明确冻结的新候选执行原11项required完整Debug/Lua/CTest及所需Release、平台、SDK/FFmpeg、最终包、AE1–AE8和回退；发现门槛不降低。

U24设备未测、U25配额边界、U26两项SDK静态疑点待真实复现、U27三个CPU指标INCONCLUSIVE及其历史长跑范围均保留。完整目标仍为全部U1–U29，当前没有整合候选全项通过；Studio继续暂停，不自动创建PR、重跑CI、合并或发布。


### 本次文档同步检查

在恢复后的09601工作树执行现有 `generate_platform_status.py`、`generate_plan_status.py`、`api_stats.py` 和 `capability_closure.py`，全部正常返回；平台与计划的默认 `--check` 均退出0。API统计与计划事实块无内容差异；能力矩阵按现有CI规则忽略生成时间后无内容差异，其静态扫描仍明确 `runtime_evidence_verification=NOT_RUN`。为遵守本次仅docs写入边界，能力生成器只在内存中将JSON输出路径改到docs内的独占临时文件，运行原main及全部检查后删除该精确文件；未修改生成器源码或判定逻辑，未写入build目录。

`git diff --check`通过；115个本次改动文档中的内部链接与锚点全部存在。实际diff仅8个docs文件；Live2D指南与40fa记录树字节一致，U26/U29原记录作为完整前缀保留，当前todo下方历史快照不变。平台YAML除同步锚点外逐字相同，生成平台文档仅改变同步提交和生成时间。没有在本次文档任务中构建、运行引擎或测试套件、访问真实SDK/设备、调度CI、提交或推送。

### 恢复后 Linux 定向 Python 验证

15:19，独立Linux克隆在完整 `09601b20eeeba6053baec4758ee51a1ef0b222e9` 上顺序执行四个原Python入口，实际发现及通过分别为benchmark33、validation-evidence78、soak-driver18、artifact-download15，共144项、0失败、0跳过。实际解释器为Python3.14.4，各项实际退出与launcher退出均为0，owned cleanup COMPLETE；源码前后clean且指纹相同，锁定输入保持稳定。它仅证明四套Python合同，不是完整Linux/C++/Lua/CTest或真实GPU/设备验证，也不替代缺失的历史Windows日志。

原始日志和收据已逐字节复制到恢复根 `linux-portability-prep-01/directed-python-01/`，`summary.json` 摘要为 `788bd465d5fa842a2eb829b5d00cc56fc89ddf709a8acb085badcc4efd89787a`。根复核重新计算17个原始引用，核对完整unittest输出及真实进程收据，零发现；同目录 `root-review-01.json` 保留这次只读复核，不重新运行测试。

## 2026-09-24 恢复后完整 Debug 与托管失败修复续记

本次续记区分已执行终态、原件恢复、定向回归和输入准备。前一冻结候选为 `ed6e9c267d03241aa5c01b203dd0323b44c31ca8`；新的三文件源码修复已提交为 `53721aa9f7fe0a917575b41882ae9e8b2949280b`（`codex/u29-hosted-fixes`），本次核对时工作树 clean。新提交尚未完成完整门禁或托管 CI，不沿用 ed6 的通过状态。完整 U1–U29 目标、底层优先和 Studio 暂停保持不变。

以下证据路径均相对于外部恢复根 `E:/CaesuraRecovery/20260924-1446/`，不表示仓库内文件，也不将验证产物加入 Git。旧 CURRENT、历史段落和原失败收据均保留。

恢复后的 ed6 Windows 基础 Debug 已取得真实完整 11 项 PASS，并完成根复核与独立复核：C++1524 通过、0失败/0跳过，Lua 主套件147和隔离套件56通过，CTest发现72项、71通过、0失败及1项预先准许的AI服务跳过。源码/缓存/依赖、原始日志、owned收据及严格manifest已核对；此配置的Live2D、Steam、FFmpeg和sanitizer均为OFF。根复核为 `debug-terminal-root-review-02/review.json`（SHA256 `0045c3ce58b83449e1ab8c33988a918fb5a524aa3f132700fc0c630907cee7e9`）；独审为 `debug-terminal-independent-review-01/review.json`（SHA256 `48d36dbe8bc19cb6542a3627641e8c02ef90300a69f41ae85eae41907e56626e`）。这补齐的是恢复后的ed6本机基础Debug，不补造此前已删除的eda完整原件。

ed6托管run `35969560756` attempt1的12个作业终态为**9成功、3失败**。Linux Release的原独立C++入口实际退出1，1508发现/1507通过/1失败/0跳过，语音重叠用例有3条失败断言；随后另一CTest执行的通过不覆盖该原失败。macOS Release在第一条 `/usr/bin/install_name_tool -change` 因严格实际进程映像不匹配而 `LAUNCH_FAILED`，实际退出-15、launcher125、process身份为空，stdout/stderr均为空；被拒绝的实际exec路径没有保存在原件中，仍属未知。聚合gate另有 `Artifact ID must be a canonical positive decimal string` 原错误。三项失败的完整ZIP及原正文已恢复并由根重新核对；失败没有转为PASS。统一索引为 `hosted-portability-01/root-failure-evidence-review-01.json`（SHA256 `319aec42eb1bba5f01f3c0950f60a9f9e682f38bbdc5ed9f3c20a26d87e80851`）；作业快照为 `hosted-portability-01/jobs-state-10.json`（SHA256 `6394cc3d297eb6a77b97bad1c716217d7de3e81ef0d50fb9f2028abfdb5ce9ab`）。

新提交只包含两项有边界的修复。音频改动将该重叠用例切到现有真实SoLoud ManualMix时钟，先给各voice设置循环，再显式推进混音；原共存、句柄、停止和completion断言保持，生产音频代码未改。100ms夹具与设备回调时序可解释原前提失效，但原托管线程调度未被追踪，不能据此声称已复现生产音频缺陷。**新C++尚未编译或运行**，静态独审无可行动问题，见 `worktrees/u29-hosted-fixes-worktree/artifacts/validation/u29-audio-test-review-01.json`（SHA256 `3f5ec30236f83853368aa49e7e331514d2fa9662bab19191fecdda896f8b2f89`）。

Mac安装器按当前开发目录发现并锁定实际 `install_name_tool`，在请求绑定、执行前后核验选择，通过原严格owned进程身份边界执行，未放宽通用进程校验。Windows宿主替身下的完整目标Python入口已23/23通过，实际退出0；原4项POSIX专用用例未在Windows执行，真实Apple工具、签名和macOS打包仍未执行。有效RED、最终GREEN及早期夹具路径错误均保留，最终GREEN为 `worktrees/u29-hosted-fixes-worktree/artifacts/validation/u29-macos-tool-selection-01/green-02/summary.json`（SHA256 `8309bd26337f9c3c4d6f8720ef856eed2b604b567683324c8062df385a85913c`）。独审已完成、无可行动静态问题，见 `worktrees/u29-hosted-fixes-worktree/artifacts/validation/u29-macos-tool-independent-review-01.json`（SHA256 `efb4e265d141e2a4665aee7f469f98f55b069af967a4794a829d934a92e91d00`）；它不把宿主替身结果提升为真实Apple通过。

并行准备与执行各自保留边界：ed6 FFmpeg Debug02已实际结束，session90353退出0，七个阶段实际退出均为0；原始终态为 `COMPLETED_PASS`，**根终态复核与独立终态复核均通过**。独审重新核对491个引用/417个唯一文件，生产重新collect及严格验证实际退出0、owned cleanup完整，重建manifest与原件相同，0 findings；独审报告为 `ffmpeg-terminal-independent-review-01/review-02.json`（SHA256 `f398635bc56ab8eafeba4c173841236006f7f1f19a68b1e2aef889d784882143`）。根复核重算220个引用/152个唯一文件，严格验证退出0，11项PASS；C++1524零失败零跳过、Lua147/56、Python22/12/78/57、CTest发现72项/71通过/1项既定AI跳过，10个引擎及测试运行DLL和实际工具首尾稳定。配置为FFmpeg ON、其余SDK OFF；原C++日志有三次真实FFmpeg打开restore-video.mpg，覆盖隐藏D3D11中的解码、纹理、PCM提交、关闭及重开合同。该用例使用同步ImmediateVideoJobs与PCM capture，不证明并发解码工作线程或物理声卡播放。根报告为 `ffmpeg-terminal-root-review-01/review.json`（SHA256 `116af9b7750b3ad1bab4583ebd069664afcfe440ce8db1e811992c155dade2d5`）。原始终态见 `worktrees/u29-ffmpeg-validation-worktree/artifacts/validation/u29-restored-ffmpeg-01/debug-02/outer.json`（SHA256 `307d4315d666a9a64876879732174360709837a62eaec60338f65c53ce1530b7`）；固定选择见 `worktrees/u29-ffmpeg-validation-worktree/artifacts/validation/u29-restored-ffmpeg-01/debug-02/selection.json`（SHA256 `564ca5acce51c8fbd70a12d92222b10aee4d4a969416f8eede916db314b31f72`）。SDK首轮Live2D构建因staged输入遗漏 `Framework/src/Rendering/CubismClippingManager.tpp` 而REJECTED，原失败、输入01与构建目录保留。新inputs02排他补齐244项当前选择范围输入，并完成实际Windows/D3D11的57个TU闭包审计及独审，已于17:14:57启动ed6的Live2D完整Debug验证，session9214、controller PID11636及创建FILETIME134347148974595736绑定到真实Python进程，状态为RUNNING，**尚无构建或门禁终态**；244不代表全平台SDK范围。启动收据为 `worktrees/u29-sdk-validation-worktree/artifacts/validation/u29-restored-sdk-02/live2d-debug-01-launch.json`（SHA256 `4ded7c8f295010337e0efa10544680ce86514b3f3c60bd69a6d3b20d4c7290a3`）。FFmpeg及SDK本轮证据均属于ed6，不迁移为537候选通过。准备报告为 `worktrees/u29-sdk-validation-worktree/artifacts/validation/u29-restored-sdk-02/preparation-report-02.json`（SHA256 `b5bbfe5acefc7c1709106684352d6e86f298e27e5131de000a5a1eb6df530079`），独审为 `u26-restored-input-audit-02/independent-review-01.json`（SHA256 `120d21e01fcfc27135c1242b3afaa92bbb54acce4179a96b49e12b8f2c382c64`）。这次是输入准备修复，不是生产Live2D修复，也不证明模型、账号或云服务通过。

旧 `f87f7aa76969e8952be5e8cbf220bb2f2847eeb5` Windows最终包字节和来源、冷恢复输入已核验准备，旧包ZIP SHA256为 `69b4cd10de06df97ed1ccf5ae82c5ae05e8f35462ca254a66cf25f37ee552ff5`。准备终态仍是 `PREVIOUS_FINAL_PACKAGE_PREPARED_NOT_RUN`；未运行冷恢复，也尚未选择通过完整门禁的新候选替代物。旧包原件复核见 `u29-rollback-inputs-01/previous-windows-root-review-01.json`（SHA256 `bacf3ba29407abe8f2aaa73c3945a3fc8d458c8da184a73add40bf7806bdd04a`），准备见 `u29-rollback-inputs-01/previous-final-preparation-01.json`（SHA256 `98301bd269f84aacf4d7dc70eaa4d88b8bd9034267d48bb6d2a35e51361002f3`）；回退输入清单为 `u29-rollback-protocol-preparation-01/input-manifest-01.json`（SHA256 `dc295534e0d11238e9227ce4c0dc64b093e097a510500bc500d18f0c7a75cbfc`）。

另一次ed6 Windows大包恢复明确失败：artifact `10797513856` 的声明长度为40,922,792字节，本次900秒总deadline仅取得14,024,704字节，实际900.1215秒、退出1，partial SHA256为 `f1ee6b5efffca25536df5dd64bbc382276f1318d168a3098d6c003df602a540e`。完整摘要未通过，未解包、未复核内部ZIP、未运行包；本次不续取，不能写成已恢复或可用新候选。终态见 `hosted-portability-01/windows-final-recovery-01/recovery-terminal-01.json`（SHA256 `1f997ef88e6ca32402c16d552a1d3740c82cddcfda08374f1df5d088d64b843c`）。

后续仍需对新提交完成完整门禁和真实平台验证，并分别完成SDK/FFmpeg、性能与长跑、最终包及AE1–AE8/回退验收。既定required集合和门槛保持，e0默认演示60帧PCM、原sanitizer、中断和本轮托管/下载失败不因准备工作或定向通过而改写；本轮不构成整个计划完成，也不产生发布授权。

## 2026-09-24 HTTP 就绪与音频集成前提修复续记

本次当前代码提交为 `a04784ea32688ebdac69753fa2660d227b52357e`（`codex/u29-http-readiness`），包含四个HTTP实现/测试文件及一个音频integration测试文件。提交前两项补丁均已根审；HTTP有下述Windows Python真实红绿，音频新C++尚未编译/运行。当前代码尚无完整门禁或托管CI通过结论，不沿用d61或ed6的执行身份。以下外部证据路径仍相对于 `E:/CaesuraRecovery/20260924-1446/`；前文历史快照及原失败不重写。

### d61 托管终态与原失败

`d61d1939c826313f4a4eeb1db7e22d115a8f7ab9` 的run `35980952352` attempt1已完成且整体failure：12个作业中8成功、3失败、1跳过。Windows Release成功；Linux Release、macOS Debug及聚合gate失败，macOS Release因前置失败跳过。实际终态见 `hosted-fixes-ci-01/final-gate-diagnosis-01/run.stdout`（SHA256 `e472ab5ceb26b7e4efaaebedd0378f200a1f1728bdd8a59e7be5ecb7927d728f`），作业快照为 `hosted-fixes-ci-01/jobs-state-06.json`（SHA256 `6dc24923c67d2f0c7867635b6ed9fa33244faedba13517f5822058972e549a91`）。聚合最终诊断为 `hosted-fixes-ci-01/final-gate-diagnosis-01/diagnosis-agent-01.json`（SHA256 `34eac961edcbfd548df6384c0fb39e2e85cea0774dd10d6d169217b1de81349c`）：原gate在 `inputs` 阶段实际退出1，按policy顺序首先遇到空 `linux_release_artifact_id`，以 `Artifact ID must be a canonical positive decimal string` 拒绝；尚未进入产物下载及最终字节复核。实际required-check context为 `Verify release inputs / Verify exact release inputs`，App15368与d61提交匹配。聚合失败不构成11包已验，也不接受d61为合格新候选。

Linux Release首次独立C++实际退出1：1508发现、1507通过、1失败、0跳过；唯一失败为 `test_audio_integration.cpp` 的 `Audio: voice pool overlap does not crash (device)`，`isVoicePlaying() == true` 实际为false。原stdout记录两个非零句柄进入slot 0/1。该首次执行与后续单独CTest执行的通过分别保留。诊断见 `hosted-fixes-ci-01/linux-release-diagnosis-01/diagnosis-agent-provisional-01.json`（SHA256 `d675a47ff6d4c4ef1087a1afa33309a8c8ae130b55d298e2495f88a4e4cfff10`）：16份Range响应与五成员已重算大小、SHA、CRC、解压和run/lane/manifest绑定，**完整ZIP的SHA尚未核验**；原托管二进制只由receipt记录，未声称已取回其字节。最终诊断为同目录 `diagnosis-agent-01.json`（SHA256 `b7e3113420889eb23dc01f0c53c47092b7a5a487c95cfa097282b5876cd2640d`）：原完整下载已在900.0477秒触发总deadline，实际退出1，保留21,118,976/31,876,868字节partial（SHA256 `c2da470b58ccc867d426d154b372552b1d1d91d4f90ea273554ef65e6eb200d0`）。本次没有重试或续取，也未提取partial；下载已终态FAILED，Range继续保持CRC-only边界，不升级为整包摘要通过。

macOS Debug原件确认 `CaesuraHeadlessHttpSmoke` 在完成任何route检查前出现TCP所有权错误；CTest为72发现、70通过、1失败及1项既定AI跳过。原件没有保存导致失败的lsof观测字节和初次/后次监听行，不能把下述Windows竞态复现认作已证明的线上原因。诊断为 `hosted-fixes-ci-01/macos-debug-diagnosis-01/diagnosis-agent-01.json`（SHA256 `1566b4ff1f435c04312b861967fb1f1c93205f481bec9cec9934797dfdcae7c1`）。

### HTTP 真实回归与单例音频前提修正

HTTP首个RED以真实owned Python进程和套接字建立barrier：第一次真实OS枚举无listener，随后同一owned进程开始监听；旧异常处理再次枚举看见合法listener，却重抛旧错误，实际1例/1 error/退出1，HTTP请求数为0。第二个RED在实际HTTP响应后关闭listener、进程保持存活，证明旧逻辑误把运行期丢失监听当成启动重试；2例中1失败，foreign监听负控保持通过。修复只将已核验进程身份下的首请求前无监听标为typed `ListenerNotReady`；首次严格验证listener后不再进入准备态。foreign、身份变化、观测不确定或验证后丢失监听立即拒绝，HTTP前后所有权检查、重定向拒绝和token边界保持。

最终Windows相关Python完整入口为 `test_package_runtime.py` 41/41、`test_native_package_runtime.py` 76/76，共117/117、0失败/0 error/0跳过；其中HTTP定向7/7，新增7例、删除0例，实际进程退出0及owned cleanup COMPLETE。实现报告为 `worktrees/u29-http-readiness-worktree/artifacts/validation/u29-http-readiness-01/implementation-report-01.json`（SHA256 `7da04165da6ad0924bd8e499bcfc6d7ab9488429bd62e564a7bdd87934077e8e`）；根代码/原件复核为同目录 `root-review-01.json`（SHA256 `03c500d64243b90c23cb0bb13abeffc73e1dc334bafca2bbe1fac32b2f7de37e`），无可行动问题。它们是Windows真实Python进程/套接字合同验证，未运行Engine，未执行macOS/Linux观察器路径，不是跨平台或当前候选完整门禁通过。

音频只修改上述integration单例并准确更名为 `Audio: voice pool overlap survives explicit mixing (manual mix)`：要求真实ManualMix初始化成功、NULLDRIVER/48kHz正确，各voice非零句柄在设置循环后显式混音512帧；第二次mix后两个不同句柄必须同时有效，原播放状态、stopVoice、停止后状态和shutdown检查保留。100ms非循环WAV和默认Device独立推进说明原声音寿命未受测试控制；原回调时间线没有保留，不能宣称已证明生产音频缺陷。其余三个integration用例字节不变，4例仍为4例，生产音频未改。实现报告为 `worktrees/u29-http-readiness-worktree/artifacts/validation/u29-audio-integration-clock-01/implementation-report-01.json`（SHA256 `ac188a1171e502379b452ab469ce206325bc34e67fcc2656288b5c871f912853`）；根审同目录 `root-review-01.json`（SHA256 `5362e69a6cd1ec28612ec24dcb22266796c2be028db81a71b016230000cb7ddb`）重核42引用，无可行动静态问题。**新C++尚未编译或执行，GREEN仍待新候选实际验证**；旧二进制和已有ManualMix测试通过不代替本改动。

### ed6 SDK 已封存终态与首次 Haru 基线失败

前文运行中的ed6 Live2D SDK Debug现已取得完整11项PASS及根/独审终态：C++1525通过、0失败/0跳过，Lua147/56通过，CTest72发现、71通过及1项预先允许的AI服务跳过。实际配置为Live2D ON，Steam/FFmpeg/sanitizer OFF；244项SDK输入、91项SDL、链接与运行能力查询分别绑定原执行。根报告 `sdk-terminal-root-review-01/review.json`（SHA256 `3bf552d4f3dc75072d1670cac5670491a53cfb26ac977280931ce43233dbe8d8`）与独审 `sdk-terminal-independent-review-01/review-01.json`（SHA256 `4258b0e9393b84320ae6f0ee79f24105fb6deacc196b6378b276cb3bb0656d10`）均无可行动问题；两者绑定原outer `7e3ccd018006d720d8ce1b2fa195b6af2c06e278d0b0ed27a19da1bd7de6dc16`、raw/run `06d049b05eb7c8397022a762ac210521db7f84579542032126be7c0159ea5a0e` 和严格manifest `59a73f223aea76091a8d143c146277c79243d2c6a355dcd7e83787d0444e5fb2`。这是 `ed6e9c267d03241aa5c01b203dd0323b44c31ca8` 的该配置通过，不迁移a047，也不证明真实Haru模型、动作、口型、Steam账号或云服务。

首次独立真实Haru `load-render` 基线（allocator off）已执行，实际子进程退出1、未超时、owned cleanup COMPLETE，终态为 `DIAGNOSTIC_FAILED`，错误为 `No-motion model not stable`。原summary为 `u26-real-probe-runtime-preparation-01/attempt-load-render-off-01/summary.json`（SHA256 `9dddc4c6d980da3460d204da3f78b2c3f07fc25e881763678f28638166a8cbb5`）。对原PNG的只读复核确认两次hidden相差0个RGB像素、hidden/shown相差23921个、两次shown相差308个；报告为 `u26-real-probe-baseline-diagnosis-01/root-pixel-analysis-01.json`（SHA256 `999c5fe42270c3f80f5b43635f0837f0612a9fa5d77dcd65a364b8a9a00d8820`）。像素差本身不证明原因，原失败与阈值保留，仍在诊断，不升级U26或平台能力。

本次文档仅把当前同步锚指向a047代码提交，并追加上述独立证据范围；平台逐能力的历史status、执行commit、日期及evidence保持，生成声明仍为 `NOT_REVERIFIED`。后续须对冻结新候选完成原完整门禁、实际平台和最终包验证，再推进性能/长跑、AE1–AE8及旧包冷恢复；不降低发现门槛，不将准备、单项通过或原件恢复拼成整合候选通过，不产生发布授权。


## 2026-09-24 Pose动作与音频时钟前提修复续记

当前代码为 `5cbf2e8e93e748b3d59667ee5ebf83554a1c5bd8`（`codex/u29-pose-audio-integration`），包含模型目标逐帧透明清理、CubismPose接线、动作缓存group/index与LoadMotion参数修正，以及12个音频业务合同用例的真实ManualMix时钟。新候选完整SDK/C++/Lua/CTest尚未运行。 五个代码文件已独立审查，但代码审查不替代新C++执行或实际模型验收。以下路径相对于 `E:/CaesuraRecovery/20260924-1446/`；所有旧失败与缺失原件状态保留。

d531025d4b2c7ab2ebaedb43b14508ad79c67b29 的 run35988975245/attempt1 已最终失败：12项检查中10成功、2失败。Windows/macOS Debug及Release打包、Linux Debug、Web最终包和移动端静态/探测作业报告成功；Linux Release和最终required gate失败，不是完整候选通过。Linux首required C++实际exit1，1508发现/1507通过/1失败/0跳过，451107断言中2失败，唯一失败为旧 Device 短语音手动stop用例；后续独立CTest通过不覆盖它。大诊断包仍仅5个成员的范围/CRC/本地SHA验证，小包完整SHA通过。最终gate在inputs阶段拒绝空linux-release-execution ID，linux-package输出同样缺失，downloads为空，没有开始完整验包。见 `http-audio-ci-01/status-05/snapshot.json`、`linux-release-diagnosis-01/diagnosis-agent-01.json` 和 `final-gate-diagnosis-01/diagnosis-agent-01.json`（后两者均在同一http-audio-ci-01目录）。

音频实际RED来自上述单例。改动保持生产音频代码与fixture不变，手动stop例改用真实ManualMix/NULLDRIVER/48kHz，显式512帧推进并要求stop与shutdown后零自然完成通知。另对同文件11个已确认依赖短音源未结束的业务合同前提进行修正；这些是静态风险，未伪称11个新运行失败。11例原82条直接断言完整保留，新增11个初始化REQUIRE与17次既有混音helper调用；76个源码TEST_CASE注册名称及顺序不变、integration4例全文不变。每例显式总输出最多32ms，低于100ms/250ms素材长度；已审退休淡出与暂停语义。报告及独审分别为 `worktrees/u29-pose-audio-integration-worktree/artifacts/validation/u29-audio-clock-completion-01/implementation-report-01.json`（SHA256 `36bab575c654bf807479a2f1bb453635006bd944dcc8d0239b811dd15dab4b59`）与 `independent-review-01.json`（SHA256 `5f2f9fa8d17a51243151e12fe266aef3092a292b5d7c0322b04b07d448473588`）。静态统计不是新运行发现数或断言数。

原Haru基线shown两帧308 RGB差及exit1保留。单独Clear变体消除静态像素差，却仍显示两组手臂；其构建外层因旧map解析假设错误exit1也保留，编译/链接实际0另经只读核实。再接入模型声明的CubismPose，加载失败不发布模型，motion/expression后、模型Update前更新pose；CubismUserModel持有所有权，不增加公共接口或额外动画效果。初始load-render/off混合产物只执行一次，child21420实际0、cleanup COMPLETE，9图/33记录/201 owner frames，静止与隐藏RGBA差0，23465可见RGB像素；根与独审实际查看确认仅两臂。独审 `u26-live2d-pose-runtime-01/independent-terminal-review-01.json` SHA256 `04d29697892feb5d6c7af2616598e7b4f94e68bc52fd48b6f066e338488848d6`；这不是完整新候选、动作或无泄漏通过。

该混合EXE首次motion/off在同步调用后发生访问冲突：child17648/creation134347247264376580实际3221225477（0xC0000005），launcher0、owned cleanup COMPLETE，无超时或强杀；宿主session24220外层实际-1保留。六图与初始对应RGBA一致，journal19最后为before_synchronous_call，尚未写motion_returned/child result，也未达到模型卸载或Engine shutdown。wrapper报告missing result是崩溃后的证据缺件。见 `u26-live2d-pose-matrix-runtime-01/root-motion-failure-review-01.json` SHA256 `1627d5413077dfebb7e47ce36ec15e6d8af9ee4f1274a237c7389f2f1557d94d`。

只读诊断确认旧调用给LoadMotion非空modelSetting，却遗漏group/index；实际SDK默认nullptr/-1，经GetMotionFadeInTimeValue、IsExistMotionFadeIn进入JsonMap字符指针查找，并对空group进行strcmp。实际map和PDB身份已核对，调用合同缺陷与本次阶段吻合；未取得故障栈/故障IP，不能声称已确认具体崩溃指令。诊断 `u26-live2d-motion-crash-diagnosis-01/diagnosis-report-01.json` SHA256 `bfc91e857c93fb3c0f0fc951020d25704418b76b8ae154d3eced9f4af26001b1`。本改动让动作缓存保留真实group/index并随LoadMotion传入，保留model3淡入淡出覆盖、stem与group/index别名、substring选择及unknown返回false。私有map value类型已变，必须完整重编译相关翻译单元，不能继续用旧单TU混合产物代表新代码。StartMotion(false)的所有权风险本次未改，仍需独立相位分配证据。实现报告为 `worktrees/u29-pose-audio-integration-worktree/artifacts/validation/u26-motion-setting-fix-01/fix-report-01.json`，GREEN待完整新构建后的原等价探针。

新SDK完整Debug入口已准备，复用逐项核对的244项SDK与91项SDL，11个required check、发现下限和唯一预准AI跳过保持。入口准备与静态独审不算配置/构建/执行通过。源码、文档与生成锚冻结后由root单独运行；U26动作/表情/释放/无Pose与异常输入、口型、Steam账号，U24设备、U25配额、U27正式性能与长跑、U28/U29完整平台/包/AE1–AE8/旧档回退均保持未完成范围。

旧f87 plain basic producer-A实际子进程0、四marker各1、存档字段正确、149静态文件不变；外层仍REJECTED/exit1，因为原协议要求的frame-limit和初始化INFO字符串在该Release EXE中不存在。实际帧数和退出原因未知，不改写原协议通过。仅原样保留6820字节save11（SHA256 `f5dc1a42c6dfecf42edc4150f08b435ba81432d944a7493aae682fc78f8e1469`）作为未来冷读输入，见 `u29-old-plain-basic-a-observation-01/observation-and-retention-01.json`；未运行consumer或整个回退矩阵。

U23保护fresh04与fresh03逐字节一致：唯一context `Verify release inputs / Verify exact release inputs`、GitHub Actions App15368、strict=false、一次批准、管理员例外及禁止强推/删除保持，未重复mutation。误删恢复缺失的原本机根日志继续列为缺失；以上新证据不冒充恢复的旧日志。平台逐能力历史status/执行commit/日期/evidence均保持，生成声明仍为NOT_REVERIFIED，不产生合并、标签、发布或商店授权。
