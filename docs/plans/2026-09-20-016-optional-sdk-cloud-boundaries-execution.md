# U26 可选 SDK 与云存档边界执行记录

本记录对应当前唯一计划U26。当前已完成不可用Steam后端保留本地provider、分块发布/短读防护、typed snapshot transport，以及云分叉双方保全与跨进程恢复的本地协调器合同；最新SDK OFF完整Debug门禁绑定08bfc276，C++1455/1455、Lua147+56通过，CTest58通过及1项预声明可选服务跳过。两个独立SDK ON配置在106160f3已实际编译链接并通过未初始化查询，早期Live2D动作组编译错误已修复。完整SDK ON门禁、真实模型动作/lip-sync、Steam账号功能和真实云服务协作仍未验收，U26未完成。下文按时间保留原失败、阶段边界及当时的待办，不能把旧阶段状态当成当前未实现清单。

## 证据起点

只读盘点的MAIN源码为65e5b425，U23/U27中24个相关模块、测试和能力入口文件与其Git和原始字节一致。指定Steam1.65/Cubism Native-5-r.5的12个SDK/header/lib/model文件及Haru声明21个资源存在，只证明文件前提，不证明SDK ON编译、真实客户端、模型动作或账号能力。原审查与旧U4/U19日志身份在主工作区artifacts/validation/u26-readiness-01/readiness-01.md与inventory-01.json；旧配置不是本轮验收。

## 不可用 Steam 后端保留本地存档

在独立u26-worktree、3da09e36基线上新增一个真实C++回归，使用实际NullSteamBackend，覆盖steam、steam://、steamcloud三个别名。首先证明本地save/load成功，再要求configure返回false、provider同一指针、原slot仍可发现，JSON与scene/token元数据可读且原文件字节不变。RAII按正确生命周期恢复原注册指针；没有替换被测SaveManager逻辑或真实Null后端。

未修生产源码首次实际Debug目标构建成功，回归1方法失败、42断言中18失败：三个别名均返回配置成功并替换provider，原slot因此不可见/读回null，原物理字节仍在。1415其他用例是定向过滤未选中。原red-01.log摘要527593b830a3571837c3c2af324cd9bc5fd4a92ae1df58a93ca036ce3b0c5de8，原二进制a138cce300e1aa29834d22cd2b3a165d895e839758cc2b0e028d9252cb0f3db8及源码/test快照由red-receipt-01.json锁定。

修复在provider替换前检查后端指针及isAvailable()，使用公共ISteamBackend接口。第一次增量构建因缺少完整接口include报C2027；原build-green-01.log保留，补入../steam/api/ISteamBackend.h后build-green-02成功。相同冻结测试实际1/1、42断言全通过；cloud_save/storage/steam/runtime_backend_availability四个源码过滤得到85/85、1249断言全通过，1331为未选中，不能冒充完整套件。已注册available mock的云存读往返、未注册拒绝和加密写入相邻控制仍通过。

最终SaveManager.cpp摘要2a7ecd87f0dd1311e6706ae8e5deeac6a2aaabd18540b22346cf65b73b207583，测试摘要de01cbcd1d9fb3a89357dc3d4c68ecf4646c2335bf9f1db557720c81205aef2c与RED完全一致。storage耦合仍4/4（archive/debug/di/steam），count_coupling --ci通过，没有具体实现头依赖。独审无可行动发现，u26-null-steam/independent-review-01.md摘要a9dec6450d74e1d0854589a2e3ca02e8d1004089daf5c4be3b6af57388fd8cd7，JSON摘要395d5671472949517243968eb7eed444d3df14433142a5bd6ae2c896939ff8ba。六native profile的C++最低发现数按实际新增1方法各加一，CTest门槛不变。

## 初始阶段的下一步（历史记录）

建立共同祖先后本地/云端分叉保留双方的合同。HTTP真实loopback超时/重试与Steam失败/重复/迟到回调、Cubism加载失败及motion释放按实际边界分别回归。只有经过真实复现的疑点才修复；尚无本轮账号、设备、商店发布或真实SDK功能验收，后续完整Debug/C++/Lua/CTest与选定SDK ON产物继续独立验证。

## 分块覆盖与短读的真实回归

在b470dece基线上先新增三个公开provider行为测试，生产代码不动。实际目标构建退出0，定向3方法全部失败，49断言中9失败：覆盖中途拒绝后旧600000字节读回空、大档换为9字节后仍读旧600000字节，单文件/首块/末块短读被接受。仅metadata短读已有正确拒绝。原red-01.log SHA6cb146e998f497e4de4bd87212a06eebfdc3244dea95917c1d623154b4bfd2de及原测试快照保留；其1416其他用例为过滤未选中。

再新增六项发布合同，第二轮实际RED为6方法1通过5失败、58断言中28失败。唯一通过的是最终head拒绝控制；legacy迁移、staged读回、cleanup故障计数、恶意metadata与删除故障仍有失败。新增恶意metadata删除期望随后根据已有生产恢复注释澄清：允许只删除调用者精确slot与其.meta，不能跟随非法引用；原RED和此前测试快照保留，此前提修正不冒称生产修复。最初三个测试正文始终未改。

最终实现读取原直接文件及legacy size,count布局，严格核对每次实际读取长度、metadata边界和精确分块长度。新分块使用32位小写十六进制随机generation，检查整个合法块命名空间避免覆盖已有对象；全部新块写入并逐字节读回后，只在最后一次head写入发布。大转小也使用该发布布局。发布前失败只清理本次暂存对象，发布后旧对象清理失败保留新版可读并记录诊断；删除传播实际SDK失败，不宣称事务删除。非法head恢复只删精确slot/head，不猜测引用，不删除其他slot。

实际GREEN目标构建退出0；九项定向9/9、147断言全部通过，cloud_save/storage邻近74/74、1313断言全部通过；1416和1351均为未选中。原cloud16方法和storage49方法全部保留，新增9方法，另两条storage断言强化清理检查。六native profile的C++最低发现数再加9（Windows1425、Linux1381、macOS1268），CTest门槛不变。storage耦合检查仍通过。

最终四文件身份由u26-cloud-chunks/freeze-03.json锁定：CloudSaveProvider.cpp SHA269972cdd3e366f741c16c6fe83b17c34c512dec09f28babeceeb25c2f355c6a，header d192f06be2564d0d3d83273cf195ad9608560f3829e312546bc3084c3eb4a0e6，cloud测试84ace4eebfb34e15a1864d94a62651d92c65019c3f8b2987edeab84032e2e3c3，storage测试b5219b1570ca898f14954e8edd262f9d6051e0cbb934f4d54422a05034c789f2。独审无可行动发现，报告SHA49574d853ace8321d6d1c0466e473eaf3cbbeaebd33cb90f03cd5e3d45d09855，独立12项原件核对JSON d3edc428d962af974273be7262856edccb7daaea6a5b4e236fd6cec05cff8ec2。

证据仅覆盖同步SDK拒绝不改变目标对象的合同；现有bool接口没有rename/CAS，官方FileWrite说明未明确保证失败覆盖在断电/撕裂写下保持旧head。因此不宣称真实Steam崩溃原子性、并发写入或跨设备冲突解决。generation熵异常/碰撞耗尽只做静态审查，失败清理可能留下不可读孤立对象；后续SDK与冲突验收仍保留。


## SDK OFF 完整门禁与 SDK ON 首次编译（2026-09-20）

clean `969a31c94fe200e8374066bd9f0f5a130ea90260` 的 Windows Debug 完整执行 `4d95de30-e585-473e-8097-8a3cf7d0dcca` 已完成：全量构建退出0；C++ **1425/1425、427279断言，0失败/跳过**；Lua主套件147/147、隔离套件56/56；CTest57项为56通过及预声明可选AI一项跳过，0失败，669.62秒。runner、collector、strict verifier各退出0，11检查全部成功，源码与夹具前后稳定。run.json SHA `e3a230bff337135a6beed58d17e727a7ec086067ed838803cdbea91a7bfeab32`；独立复核67份原日志/收据引用，review SHA `4c46fe9f606459bee6fa28a052474dd49247c5428191a6044e5f997d6289106b`。HTTP实际73/73，PID3768、创建身份134343276464807771、端口5405，经请求停止后实际退出1，cleanup COMPLETE，无超时或强杀；HTTP独审 SHA `1f633cba75e5cb9c225d9d4513c2fcd41db3b0ac1971808e0c30a13ae461435a`。本段仅对应SDK关闭的该源码，不证明后续补丁或SDK运行。

SDK ON 输入锁固定本机已有 Steam SDK、Cubism Native-5-r.5 的真实头文件/源码/库及工具。第一次隔离 configure 尚未进入SDK源码：MSBuild FileTracker在初始化CommonApplicationData时抛出路径异常，最终CMake报找不到C/C++编译器。四个只读.NET环境对照表明直接缺失字段是子进程allowlist遗漏的SystemDrive：原环境及仅补ProgramData失败，仅补SystemDrive和两者都补时均能解析C:\ProgramData。原尝试及CMakeConfigureLog完整保留，没有修改系统环境或SDK。

新尝试stage02在新的两个build目录补入本机SystemDrive/ProgramData，保留相同VS、14.44.35207工具集和10.0.26100 SDK，并补锁实际amd64 MSBuild等工具。两个configure均退出0；SteamBackend.cpp实际编译并产生caesura_steam.lib，CubismFramework也编译成功。CaesuraLive2D实际编译在Live2DBackend.cpp:397报C2660：零参数GetMotionCount不存在；SDK区分GetMotionGroupCount()和GetMotionCount(groupName)。本补丁只将外层组循环改为GetMotionGroupCount，内层逐组动作计数/缓存合同不变。原失败不是被关闭SDK隐藏；新编译及可执行链接仍待随后执行，不能把静态库成功称为真实客户端/模型通过。

原件均在本工作树artifacts/validation/u26-sdk-on-01：compile-stage-01/02.json、分命令stdout/stderr/owned receipts、compiler-diagnosis-01.md/json、inputs-lock-01/02.json。两轮源码与锁定输入均稳定，首轮未进入后续命令；第二轮最后一条退出1。测试模型、账户、成就/统计、云同步服务和motion/lip-sync实际运行均NOT_RUN。云冲突的typed读取与持久保全合同另有只读设计，仍未实施，不将旧显式push/pull的成功升级为冲突安全同步。


## SDK ON 编译链接与未初始化查询完成（2026-09-20，stage03）

上述编译修复提交后的 clean `106160f380d9080610db4c480e7ee4faf8d1b21c` 已完成独立 stage03：Live2DBackend.cpp 重新编译成功，CubismFramework/CaesuraLive2D 目标成功；分别启用 Steam 与 Cubism 的两个 Windows x64 Debug CaesuraTests 均实际链接成功。实际编译宏、link.command/link.read核对到steam_api64.lib和Cubism Framework/选定Debug Core库；Steam EXE导入steam_api64.dll，复制DLL与锁定SDK相同。build目录沿用-02名称，源码身份以上述提交和收据前后fingerprint为准，不能从旧目录名推断版本。

随后只执行两个精确未初始化查询：Steam **1/1、2断言**；Cubism **1/1、3断言**，均通过。1424与1425其余用例是过滤未选中，未运行完整SDK ON套件。两份EXE摘要分别为`f3657d95ffaec9132e6bec458971b7e20a3060798fa6f86623d5bff8c8070137`与`e7d9d7913964c7058bea97b481da7ca443fa1a801ef27d12cfd68da6e3bbaff3`。五条owned命令全部exit0、cleanup COMPLETE，无超时/强杀/停止请求；独立复查时五个精确PID均不存在。

源码与输入前后稳定；独审另重算242份选定SDK/源码/配置文件及10份工具，252/252大小与SHA匹配，并核对41份原件及实际链接产物。原`link-stage-03.json` SHA `93bf0268196f17e1438fd73370606da33c9203dca18bdb5bf299b302ac65559b`，`inputs-lock-03.json` SHA `1b99aebb54c8ed920b0f7b782501c2ad82a5919d718c71d140743836224aec31`；独审`sdk-stage03-independent-review-01.md` SHA `e4256235c27159893abf1f6ec30fe90bc8736e1e322f9e6119652b45f008b692`，JSON SHA `869f5a204b70b8204d943d337e737defb296122b553ee0e00bcde589b8a1be8d`，均位于本工作树`artifacts/validation/u26-sdk-on-01/`。

该结果仅证明两个独立SDK ON配置的Debug编译链接和真实后端未初始化时正确报告不可用；不证明完整SDK ON门禁、Engine运行、Steam客户端/账户/成就/统计/云同步、模型加载/动作/lip-sync、GPU、许可证或分发权限。stage01/02原失败继续保留；SDK OFF 969a31c9完整门禁不改标为新源码或SDK ON结果，U26仍未完成。


## cfa46876 的 SDK OFF 完整 Debug 门禁

干净cfa4687606a62842479a545294bddf461ea66b47的run d35e0064-d76d-42f3-85f3-7a24d0a2b0a8已完成全量Debug构建、C++1425/1425及427279断言、Lua147/147与56/56、CTest57发现56通过及仅1个预声明AI/Ollama不可达跳过；runner/collector/strict全部0。独立只读审计189个证据引用全部匹配，6486源码条目fingerprint 2b61f51a857336198b75f7ca628abd3021cd5caab5ab259bcff040b9e5d7d950、470夹具条目e74b366cdabae7bc68fda7c61cc6c003d1c0d2090b454e3aed5a360b4413b53a与原首末及当前clean值相同，strict错误为空。

HTTP实际73/73，package-web-ok/artifacts两项仍NOT_RUN，未把它计成75；本轮SDK OFF验证不新增Web发布证明。HTTP PID17572、creation134343322564789093，受控STOPPED actualexit1、无超时强杀且cleanup COMPLETE，审计时精确PID不存在。顶层11项runner记录仅提供exitcode，不补造每个子进程PID或原件未表达的清理声明。审计位于u26-cloud-chunks/full-debug-cfa46876-01，MD摘要ac1d64d5251879c4a29fd1f33dc50bac7bc214ac32a7289d0d350a477f9ca393、JSON aa34223d1815de08b2da63745a26f82767b1d6714b63be8a2c7342b3abd6d7c4、freeze10d758cde175d9e2dea9c37a85943f5f795f7695208ca37d895fb456ef72a534。

SDK ON的两个已链接测试仍仅证明未初始化适配器合同；本次OFF全量通过不提升SDK账号、模型、动作、GPU或真实服务状态。云分叉保全与typed snapshot transport继续按既有设计推进，普通HTTP PUT/Steam FileWrite没有已证明CAS，不作为安全条件发布能力。


## 六态只读快照及 Windows/Linux 回归（2026-09-20）

新增可选 ICloudSaveSnapshotTransport 接口，明确 Present、Missing、Unavailable、Failed、Invalid、Unsupported；只有 Present 暴露完整字节，空文件与不存在分开。Local 以实际句柄/描述符核对路径、普通文件、限额和前后身份；HTTP 使用真实流接收限制及完整响应状态分类；Steam 对无法证明的缺失和零长度保持不确定，短读或 head 变化不返回部分载荷。三个旧定向复制入口不变，当前 conditional-write 能力均为 Unsupported，没有把 ETag、时间戳或 generation 当成 CAS。

首轮 Windows 真实 RED 为 9 方法 1 通过、8 失败，448 断言中 122 失败；源码实现后同九方法 9/9、448/448，七文件邻近 146/146、3862/3862。Windows GREEN 原报告 phase-a-run-green-01.json 摘要 3a4d8c97406e615b7ad21b717571dc16d1c24410db2afe59056840d3f1bedfca。随后本机 WSL 实际配置并构建 CaesuraTests，九方法 9/9、439/439，邻近 144/144、3840/3840；OS条件分支导致计数差异，过滤未选中不作为完整用例跳过。Linux 原报告 linux-green-02.json 摘要 65ea8990c63c6f4788cecdb7b2aac1eb19b24d9b3faf681599cf41fa4b1a462e，四条命令均 exit0、无超时/强杀、cleanup COMPLETE。

两平台执行锁定相同八文件，HEAD60dbbca9 上的 dirty fingerprint b441a8faec35143c3953710b048993af3109e991a8aa22ad50c105f61be05d4a 前后一致。Linux 使用 CMake3.28.3、GCC15.2、SDL3.4.2、OpenSSL3.5.5，属于本机 SDK OFF 定向证据；原 ELF 摘要 a2035962e4a4e81c30e344a207d3bebe54532ea0078c915a55898330ca7cc77b 来自执行时记录。后续只读复核发现 WSL boot_id 已变化，原 /tmp 构建目录不再存在，不能再次重哈希该 ELF 或内层控制文件；D盘原始日志、外层 owned 收据及八源文件已重核。没有因此重跑定向测试或改写原成功记录，下一次 Linux 构建需保存在持久目录。

独立审查未发现可行动缺陷，independent-review-01.md 摘要 5ca229c081922541a73b621758783703e5ec55018ac210615ed08179d3a6d086；JSON cae6a514b431f93b8204efbe36b397419122b60cc006d88aeb9fc8ab8857fdc0。原25方法完整保留，只新增9方法，六native profile C++最低发现数随之增加9；完整 Debug/C++/Lua/CTest仍需在本阶段干净源码上重新执行。路径替换的全部交错、真实HTTPS/压缩响应、Steam账号和macOS均未由此验证；共同祖先、双方持久保全、冲突日志及恢复继续推进，U26未完成。


## Typed snapshot 干净完整 Debug 门禁

干净提交 a1a516cb37da0e792bc67242886d6a18d4362174 的 windows-debug run 36276871-eed6-4b67-96dd-99affa4d53b8 于 2026-09-20 实际完成：全量 Debug build exit0，C++ 1434/1434、427727断言、0failed/0skipped，Lua 147/147 + 56/56；CTest发现57项，56通过、0失败、1项CaesuraHeadlessAiSmoke按既定可选服务规则跳过。其余验证器、耦合与测试注册检查通过，执行器、collector、strict verifier均exit0且owned cleanup COMPLETE，无超时强杀。

源码fingerprint 5d45569116b871012bc55f90b050039f5ba9a204dba73973153ebda332bb3223及fixture SHA256首尾不变。根代理重新读取49份bundle/outer原始引用共82980124字节，全部hash一致；原run SHA256 e2a050f43400dee2f9cb395bf92e761eaf594722a8c38ee0ea30c4bd9699b89c，manifest 5691a0155d2eb30ef34b436c494b22c76846c56e9687019693eb67cc509c3afe，root-review-01.json ead1f7294ec0b5ce55b1640a06655369f137c59156d5c48aefcae357017114cc。此profile不包含独立HTTP transport smoke；不把可选服务跳过称作执行通过。

此结果验证本次只读typed snapshot增量，未关闭冲突记录保存、恢复协调器、真实SDK账号/模型生命周期等U26剩余工作；下一切片以不可变本地记录及实际写入故障回归继续。


## 冲突保全 B1 首次真实 RED

内部 CloudConflictStore 三方法暂为空返回，原34个cloud测试方法原字节保持并追加6方法。主代理对冻结四文件实际编译成功，6方法全部失败：307断言中273通过34失败，1434个原方法过滤未选中。全部失败只在预期状态与record缺失的三个目标断言位置；真实Crypto/SaveManager生成并load的CAES A/B/C、文件及HTTP/Steam snapshot前提通过。源码fingerprint0a061d082aa26a8d3feaa92f9e4ff746045f710aea3087ad24007b9ea0e495c0首尾一致，命令无超时强杀且cleanup COMPLETE。原run SHA256 127d0316b0f2ccf71a09c013719a3011df18a139f6e5cceb6288806b112840be，root-red-review-01.json 89a6e1f2958f5f47aeaecdc2be67ab79ce906006d6f00be4a02a40b1b139f83d；binary03835c44fb8ede2c673861e15adfcce56d5f85246d0d7982114449a9c4a5a5e9。

预提交24格失败矩阵在本次RED因无法建立基准而尚未运行，不能将其注册数当覆盖。实现后需要实际逐格hook命中及不改旧记录的GREEN。新对象重开磁盘与真实child重启、提交后Indeterminate及远端CAS仍分别记录；B1只保存不透明原始字节，不凭本地记录宣称CAES语义校验或同步提交成功。


## 冲突保全 B1 Windows/Linux GREEN 与独立审查（2026-09-22）

CloudConflictStore 现已实现 owner 限定的不可变本地观察记录：在新目录保留 base/local/cloud 原始字节，以最后原子发布的 manifest 封闭记录；使用显式外部 manifest SHA 和同 context 基准，不自动选择最新记录、不调用远端写入。输入和目录扫描受配额限制，失败保留已有记录及未完成目录，不自动删除。manifest 发布后的复验失败返回 Indeterminate 并保留 candidateRef；此分支尚无实际提交后故障证据。

在 a1a516cb 上的冻结 dirty fingerprint e727f5e4de00bff5f81e54892f664934820e830b24d2a2a4e2917cf4f19e291c，Windows 实际 Debug 定向六方法 6/6、1673/1673 断言，相邻 101/101、3612/3612；Linux 实际重新配置、构建后，同六方法 6/6、1673/1673，相邻 150/150、5513/5513。四角色乘六个预提交 writer checkpoint 共24格均真实执行并核对命中及旧记录不变。相邻套件包含新六方法，不能将两行相加作为独立发现数；过滤未选中不等于完整套件跳过。所有命令正常退出0、owned cleanup COMPLETE、无超时强杀。

主代理原始复核 phase-b-root-green-review-01.json SHA256 bc11c8ce5166f98bd5eb28001aeaedd69c9962fd2651d882ea29fd8d87a76f58。独立只读审查完整读取实现、六组测试、typed reader 和原子 writer，重核14项源码锁及39项源码/证据引用，在 B1 合同内未发现可行动缺陷；review-01.md SHA256 8ecd2c2370175ebdc5ad62ed2a5dcf157ae6cf69a2cb9345777a21c5f36bb24e，JSON 4248af1e98ff6159ed48a4403ca980395120deda8c2075facf0cfd95c88c7470。Windows 二进制 efb39890609338af12696d83a0810b65358fe6d93246ccac0122bf6ca66399a3；Linux /var/tmp 持久构建二进制 faf072f18b6d295f69aa3e7b23db12252a0b4583c83fec95492f1fd93576a94b，9月22日只读重哈希均匹配。

原34个cloud测试正文保留并新增六方法；六 native profile 的 cpp 最低发现数按 id 各增加6，其他阈值不变。当前源码仍须完整 Debug/C++/Lua/CTest 门禁。此批证明不透明字节保全与预提交失败，不证明 CAES 当前 key/policy/envelope 有效性、账户身份、远端 CAS、SaveManager 恢复协调、真实独立进程重启、提交后故障或掉电持久性。路径攻击、非 ASCII root、碰撞耗尽、线程/重入和部分扫描边界也未由本批实际运行，U26继续推进。


## B1 不可变记录的干净完整门禁（2026-09-22）

干净 b36aa8db18cca81efbc03cbf981ac7cf86bdec0a 的 Windows Debug run87ea101e-599e-4e16-8554-3181f43329fd 已完成全量构建0、C++1440/1440与429400断言、0failed/0skipped，Lua147/147+56/56；CTest发现57项、56通过、0失败、1项CaesuraHeadlessAiSmoke按既定可选服务规则跳过。全部11检查及执行器、collector、strict verifier均0，源码fingerprint3706cbff2bd371e12aa9680c841bb0534a3427b08e7c6dce60a91a9f7dd4b06b及fixture首尾稳定，owned进程正常完成、无超时强杀。

根代理重读49份bundle/outer原件共83957443字节，全部摘要匹配。原run SHA256 a838a8492adbbda44fcb045cc8ae30e71dab9f86aa9791b443d726c39196b773，manifest7437df9a35e20eb76be8bac68c528d8bc86605af955ee1980b9a57b73c805098，root-review-01.json d54d4dbe71148f958192599c4339aac55508c3286c3c789ff110e682e85a2147。此profile没有独立HTTP smoke，不证明真实SDK账号或独立进程恢复。B1b随后新增probe与冷进程测试，必须使用新执行结果；本完整门禁不转写为新测试已通过。


### 2026-09-23 — B1b/B1c 冷进程提交边界

在 b36aa8db18cca81efbc03cbf981ac7cf86bdec0a 的生产实现上，只新增独立 host probe、Python 驱动和 CTest 注册，未改变 store/provider/crypto 生产行为。

- B1b 首次 Windows/Linux configure、target build、驱动均退出0。每端四场景、18个新进程：manifest 发布前受控 `_Exit`、发布前进程自身异常终止、发布后正常返回、发布后异常终止。前置终止保留三份 opaque 原件但记录 Incomplete；发布后 fresh reader 按原引用 Complete；错误外部 SHA 的单独读者得到 InvalidRecord。旧 seed、A/B/C 和读前读后整个记录树逐字节保持。根各重算66个输入/原流摘要，所有 owned cleanup COMPLETE，无超时或 runner 强杀。
- B1c 追加第五场景，总计每端23个新进程，Windows/Linux configure/build/run全部0。原 Replace hook 仅在真实发布前捕获 manifest 和预先计算引用；正式 manifest 已落盘且 SHA 输入与原文完全相等后，测试 Crypto 明确调用真实 SHA，确认与预备摘要相同，然后仅抛一次异常。原生产 preserve 返回 Indeterminate、无可用 record，但 candidateRef 和 operationId 完整保留。先验证同一对象异常展开后可读，再由普通 Crypto 的全新进程按原返回 candidateRef 冷读 Complete。没有再 preserve 或扫描最新记录来代替恢复。
- 原四场景不变，两个平台第五场景的实际注入次数、已发布文件、输入匹配、真实摘要、一次异常、同对象及冷进程恢复全部核对。根各重新核对81个输入/原始流引用，reader 目录零写。Windows报告 SHA256 `9ea3b9780c387ae07dc002be29c6e981c846e00d3545616d2e6cae14b5d18508`，Linux报告 `30aa08631ff473179ae38f4e06ea08fd2945a7d8225cf63d0512f8b441c52a4f`；根复核在 `artifacts/validation/u26-conflict-design-01/phase-b1c-{windows,linux}-01/root-review-01.json`。原B1/B1b二进制和首次记录分别保留。
- CTest 实际 discovery Windows/Linux 各58项，新 `CaesuraCloudConflictRestart` 各恰好一次。纯host构建注册，跨编译/Emscripten不注册；桌面profile的CTest最低发现数57→58，C++最低数不变。尚未以新增后的源码重跑全量门禁，b36的完整门禁继续只对应b36。

这些证明进程终止/受控后端异常下的本地记录边界，不证明断电持久性、真实磁盘故障、OpenSSL/BCrypt实际故障、目录fsync或远端CAS。原件是opaque测试数据，B1 EqualObserved仍不是已验证CAES祖先。B2/B3的账户/key/policy/context、持久祖先游标、选择后复核和显式导出继续实施；SDK真实账号与网络侧条件未由本结果完成。


### 冷进程门禁集成与计时诊断（2026-09-23）

干净35dc5c82的完整Windows Debug run5096f19d-d24b-4178-b642-97096406c4c2实际FAIL。build、C++1440/1440（429400断言）、Lua147/56和其余独立检查通过；CTest58项中55通过、2失败、1项预声明AI服务skip，strict verifier拒绝。失败为新CloudConflictRestart与release_artifact_download。原始run/collector/verifier未改写；根重核28原始流，失败复核SHA256 d989547343b8f74e72eff670a6dec59456e67b0d12758261d3f7f25a12017cd9。

重启probe增加仅失败时输出的真实writer checkpoint/code/Win32错误。原CTest深路径下seed在CreateTemporary阶段得到PublicationFailed/ERROR_PATH_NOT_FOUND(3)，未进入write。相同probe/production与原五场景使用较短的build/ccr目录后23个真实子进程全部通过，中文checkout组件仍保留；正式CTest只缩短该证据根，未改变场景、断言或480秒预算。新注册入口实际通过（含fixture为2项），证明当前路径前提下的冷进程合同，不宣称Windows任意长路径支持。Python进程的长路径CreateFile控制不等价于该C++映像，不用于补充此能力声明。

原下载器超时用例再次实际复现：blob headers阻塞返回TimeoutError，但报告seconds=0.093、deadline_exceeded=false。本机Python3.12.9的monotonic使用GetTickCount64、分辨率15.625ms；perf_counter使用单调且不可调的QueryPerformanceCounter、分辨率100ns。尝试移除最后预算内的socket timeout后，Windows阻塞读取约0.8秒才返回，原四个时间断言失败；此尝试保留并撤回，未改预算或断言。最终只将七处预算/elapsed观察统一为perf_counter，保留原socket超时和owned watchdog。

最终Windows原14方法通过（CTest4.14秒），Linux原14方法通过（3.247秒），另保留原header方法三个真实传输失败报告，实际0.105497/0.102813/0.105371秒均deadline_exceeded=true。合法完整传输和凭据隔离、残留字节/摘要负控仍由同一未修改suite执行。根复核两份完整五场景/23child报告共162引用及原命令流，目标修复收据01799a69e9fd05b1f25e8d564bd08a6a32b197d97bb9e3134cfbd0a4f5d29192。新修复后的完整门禁尚待执行，不能把这些定向通过拼接成35dc完整PASS。B2/B3协调器、历史导出、真实SDK/账号与条件云写入边界继续按计划未完成。


### B2/B3 当前策略协调器与历史导出（2026-09-24）

新增纯虚可选ICloudSaveCoordinator，由Registry已有ISaveManager查询；无新后台线程/服务槽位。SaveManager保留旧push/pull行为，增加配置撤销和重入保护。内部CloudCoordinatorState使用普通目录/OS身份校验、固定严格JSON、排他token、现有真实原子writer与B1 store，保存原始观察和外部SHA，receipt先于祖先游标发布。历史导出只产生受控新副本；checkCloudPublication在选择或观察过期时拒绝，未过期也只返回UnsupportedConditionalWrite，不调用旧writer。账户scope/验证epoch由宿主明确提供，不推测账号。

初始六方法Unsupported骨架构建成功；冻结12个新回归实际12失败（296断言/13失败），原40云存档方法2594断言通过。根保留原二进制、源锁和流；失败主要在绑定前置，未把更深断言写成已执行。第一实现对同一12方法实际通过3621断言，原40方法也通过。随后新增3个审查用例：严格元数据与未知内容/未完成导出容量均通过；恶意provider在真实磁盘/HTTP读取后篡改typed Missing元数据，实际复现1方法9断言失败，错误地当作OneSideMissing并写observed.bin。生产修正将矛盾状态、错误/字节计数和未知枚举归为Invalid/MalformedMetadata，禁止缺失语义与保全晋升。

修正后Windows原12方法3621断言及新3方法490断言全通过；WSL Linux15/15、4111断言通过，存储/云/原子写/迁移/绑定/黄金存档邻近165/165、9624断言通过。根重核源码、命令、原始流，owned退出均0、无超时强杀，Linuxrun SHA256 ee9e3d1479a29a712fbea342167869670bf5661a080e7271644b9abe3838cdb9，Windows审查GREEN run SHA256 676d37840af05aba62bacb5887edec7e3ce9167ba08e78e00f4b17c3fbf64ef4。Windows发现1455、Linux发现1439；筛选未选中的方法不算执行skip或完整通过。

桌面C++门槛按新增15个无平台条件方法提升15（Win1455/Linux1411/macOS1298），保留原平台保守下界；实际Linux发现1439另外记录，不能将下界冒充发现数。CTest仍58。生成api-stats与手册同步两个可选接口，纯虚方法452、接口头40来自源码扫描，不是覆盖率。

本批尚缺协调器receipt/祖先游标的独立child终止/重开五场景、最终完整Debug门禁及后续审查。现有B1五场景23child只证明底层opaque记录，不代替B2/B3的CAES/选择/receipt恢复。真实Steam账号、Live2D运行与SDK ON、远端CAS、断电持久性及整个U26继续未完成。

### B2/B3 完整 Windows 门禁与双平台冷进程恢复（2026-09-24）

干净提交 `7f09c75aab8af82422a4cf3d34914f1166cff443` 完整Windows Debug验证通过：全量build0、C++1455/1455与433511断言（0失败、0跳过）、Lua147/147＋56/56；CTest58项中57通过、0失败、1项预声明CaesuraHeadlessAiSmoke可选服务跳过，657.90秒。执行器、collector、strict verifier均0，源码与fixture首尾稳定，owned清理完成且无超时强杀。run为 `684f120f-98f8-40e5-810a-3e3b16ed6658`，原run SHA256 `c8e739fc6ef4037b7238a077d27205da56c2352a979d9bbf95ce4605179d4f1a`；根重新核对29份流/报告与CTest XML，复核文件SHA256 `564c77d6f088457ce726f8e0614be2340e35d19574aef3c9de0d8e9677204581`。此完整结果只覆盖该提交，随后新增探针另行验证。

新 `CaesuraCloudCoordinatorRestartProbe` 使用真实SaveManager/Crypto生成并load两份大于70KB的AES-256-GCM CAES存档，通过Registry取得可选协调器。云端是明确标注的文件测试适配器，两端均调用既有typed磁盘reader；旧式读取和所有端点写入均拒绝并计数，不将它称作HTTP或Steam网络验收。五场景为游标Replace前受控退出、Replace前进程自身异常终止、发布后正常返回、发布后异常终止、实际已发布游标参与真实SHA计算后抛一次异常。每个场景由不同原生进程建立基准、操作、按原receipt引用重开、重复token、错误SHA/错误钥匙负控、原始CAES历史导出与重复导出；加上真实夹具生成共51个独立进程。

首次Windows构建失败于新测试目标漏连Debug日志库（LNK2019），原始 `u26-coordinator-cold-win-01` 保留；仅补齐测试target依赖后，Windows与Linux首轮分别51/51通过。原B1五场景/23进程驱动及其正文保持不变，两端重新执行均通过。进一步审查发现内容哈希不能排除相同字节重写，因此只加强driver的dev/inode、大小、mtime/ctime身份断言（不比较读取可改变的atime）；产品源码未改动。加强后Windows `u26-coordinator-cold-win-03` 与Linux `u26-coordinator-cold-linux-02` 再次分别51协调器＋23 B1进程全部通过。游标发布前未晋升新祖先；发布后Indeterminate由新进程得到RecoveredSelected；旧祖先重复token只返回Superseded，不倒退游标。错误外部SHA与新进程错误密钥均被拒绝。读取/重复操作不访问实时端点，历史树内容和文件身份不变，导出只增加独占副本且原CAES字节完全相同。

加强版冻结源码fingerprint为 `811799667d5ff791796454c1769b58c4634e95c37a335752a49194547915c2e2`，各命令首末稳定，全部owned清理完成、无runner超时强杀；场景自身异常退出Windows为74、Linux为SIGKILL(-9)，受控前置退出为73，均由父进程核对真实OS身份。Windows协调器报告SHA256 `a663a64d4f361ed16aec217bb255700bed267f603ecd8a6ff46e492adcaf2908`，Linux报告 `f2c0f23461a851a4c045f82c94a2d76c845dc7b25112a1f0bd2f88b42a5d2d85`。根各复核230份原始引用、74个独立进程、全部源锁和最终真实文件树；复核文件分别为 `c76811fa9d3b99cfe3d9d1d4e99752a37935b6f9cd0032e0ca665b2ec91992d5` 与 `4992acad6c23fb577da75a751af5ccff7c0f9f11bcfe478fefe22cbc17160cd2`。Windows短临时fixture及Linux `/var/tmp` 目录全部留存，报告绑定绝对路径与物理身份；没有承诺任意Windows长路径。

新增CTest只注册host构建，C++方法数不变。Windows Debug/Release实际各发现59项；发现Release原门槛漏计旧B1探针，因此六桌面profile统一最低59（Debug加1，Release补齐旧探针后加1）。该数是注册门槛，不声称其他平台完整执行。新增探针后的干净完整门禁继续进行；真实Steam账号/客户端、合法Live2D模型及SDK ON、远端CAS和断电持久性仍分别未验，整个U26尚未关闭。

### 协调器冷恢复的干净集成门禁（2026-09-24）

新增探针后的干净提交 `08bfc27645a335b91f860144c49d99cc2142d8a4` 已完成单次 Windows Debug 门禁，run `3b720275-8b5e-49aa-8ce9-ac09b494ce06`：全量构建0，C++1455/1455、433511断言、0失败/0跳过，Lua147/147＋56/56；CTest实际59项，58通过、0失败、1项预声明CaesuraHeadlessAiSmoke可选服务跳过，691.30秒。新协调器和原B1冷进程入口均在本次CTest内执行通过。

执行、收集、strict verifier全部退出0；源码fingerprint `60b874c1d13708c9b2e656eab8af104de3d84f92d6ffb61092b4959a7de370e8` 与夹具首尾稳定，owned清理完成，无超时强杀。根重新核对29份原流/报告摘要及CTest XML，原run SHA256 `e307643aac530ae0fba54eca374584bac5460accf715bee56100674aeccc3764`，`u26-coordinator-cold-full-01/root-green-review-01.json` SHA256 `5b50e6c326f4e9fddfe2ca3e83a64072e5ea885da19c80127b4cd096327cae37`。前述失败和双平台定向证据继续保留；这次完整通过不扩展到Linux完整门禁、真实账号/SDK、远端写入或掉电验证，U26其他条件验收仍未完成。

## 2026-09-24 e0 SDK ON 前提复核

在e0桌面工作树完成一次有界只读检查，231项选定SDK/平台/工具输入全部与旧锁一致；Steam仍为1.65，Cubism为Native-5-r.5。Haru.model3.json及其25个唯一引用资源（包含4个Sound wav）全部存在并重哈希。artifacts/validation/u26-e0-preflight-01/preflight-01.json摘要99001d0f39a692fc96b5e8a01392cfbdc982432b605f9648e0159afafd8dedb9，review-01.md摘要44b1ba8597aa0c5ba4bf8295cb63e883a72b50900411bcdf63d9cd021fb6ad6b。旧SDK测试仍仅属于106160f3的两个构建与未初始化查询，不升级为e0完整ON。

已核对实际进入边界：Steam ON的Engine init会调用SteamAPI_Init，失败为非致命，因此完整套件通过也不能代表账号功能。Windows Live2D成功路径需要真实bgfx D3D11 device/context；headless失败后Null回退不算模型验证。外部主仓Haru路径不在e0资源根，必须把锁定样本复制到真实资源根内，不能用外部路径/junction绕过PathConfinement；主目标POST_BUILD复制shader，单独tests构建不足以证明shader就位。

当前KAG motion/expression/lip-sync仅维护ctx状态，能力目录明确command_not_wired；现有IAnimationBackend动作/表达式/参数入口可作SDK真实路径验证，但不能宣称现有KAG自动口型已闭合。新SDK ON runner仍需锁定完整91项SDL输入、实际链接库/运行DLL/shader，分别保留Steam、Live2D与双ON配置。此检查点没有configure、构建、引擎/模型运行、Steam账号或云服务操作。

## 2026-09-24 当前真实 SDK 路径的只读调查

eda98b22代码与71c9732d文档候选上的有界调查已完成，165项选定Cubism输入、Haru配置及25项引用均重新核对。原生模型加载/显示可通过已有HTTP加载入口与真实D3D11观察，动作、表情和参数控制仅有C++接口；当前Lua/RPC不存在相应动态绑定，三个KAG命令仍为command_not_wired。Haru自身动作包含嘴部曲线，不能据嘴动推断音频同步。手动ParamMouthOpenY参数控制与自动语音包络接线分别记录，后者当前未实现，本次未扩大为自动音频算法开发。

静态调查记录两项待真实SDK复现的问题，均为STATIC_FINDING_PENDING_REAL_REPRODUCTION，runtime_reproduction=NOT_RUN。其一，Live2DBackend::playMotion将model.setting传入LoadMotion，却省略group/index；所选Native-5-r.5默认NULL/-1，后续fade查询将组名送入无空指针保护的字符串比较。其二，LoadMotion/LoadExpression创建的每次调用实例以StartMotion(...,false)交给队列，后端只保留原始字节，SDK队列也不承担这些实例的释放。尚未记录实际崩溃、SDK对象释放计数或测得泄漏；先做真实控制，不以静态疑点替代回归。

旧stage03的caesura_live2d.lib、CubismFramework.lib和CaesuraTests.exe当前字节仍与原收据一致；构建目录 `D:/caesura-u26-live2d-on-969a31c9-02`。旧实际编译输入中的Live2DBackend.cpp、.h、Live2DUserModel.h与当前eda逐字节相同，可用于明确限定的诊断复现；旧Engine库不可与当前EngineConfig/Engine头混用，整个旧产物不重新标为当前候选。后续应先做真实D3D11/Haru加载与图像控制，再分别以独立子进程观察Idle/0动作和F01表情的行为、异常及生命周期，避免一个异常遮蔽另一个问题。

只读报告为eda树 `artifacts/validation/u26-fixed-step-readiness-01/readonly-review.json`，最终摘要 `2756b4c47c48879dfe201da2b48d3606da049cbd7c523095fbe59cb5ead9afcd`。没有配置、构建、SDK/原生进程、测试、账户或凭据操作，也未读取活动Debug构建目录。记录工作树中的Live2D指南已即时移除不可执行的fg/model3、motion/expression用法，改为真实接口范围，修正实际编译宏并将旧运行结论标为历史；这些文档修正未进入正在运行的71c CI候选。

### 2026-09-24 误删后的证据可用性

上述SDK输入、Haru引用、旧stage03构建字节与只读报告摘要均是误删前调查记录；其中旧D盘构建路径不能再作为当前可执行入口。源码已恢复至E盘，原构建和依赖输入须在后续真实验证前重新定位、逐项核对；当前没有新SDK ON构建或模型运行。两项静态疑点继续为待真实复现，不升级为已观察崩溃或泄漏。恢复范围和现存清单见[U29恢复后记录](2026-09-24-017-foundation-integration-execution.md#已恢复原件与当前缺口)，其中零散文件不补成完整SDK验收。本文与Live2D指南修正已进入09601基础上的文档整合，未改变原71c托管执行的源码。
