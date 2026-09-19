# U26 可选 SDK 与云存档边界执行记录

本记录对应当前唯一计划U26。已实证修复不可用Steam后端误替换本地provider，以及分块覆盖失败、大转小和SDK短读；969a31c9 的 SDK OFF 完整 Debug 门禁已通过；SDK ON 首轮已编译 Steam 模块并在 Live2D 动作组 API 处暴露真实编译错误。后续修复验证、双方冲突保全及真实 SDK 功能仍待执行，U26未完成。

## 证据起点

只读盘点的MAIN源码为65e5b425，U23/U27中24个相关模块、测试和能力入口文件与其Git和原始字节一致。指定Steam1.65/Cubism Native-5-r.5的12个SDK/header/lib/model文件及Haru声明21个资源存在，只证明文件前提，不证明SDK ON编译、真实客户端、模型动作或账号能力。原审查与旧U4/U19日志身份在主工作区artifacts/validation/u26-readiness-01/readiness-01.md与inventory-01.json；旧配置不是本轮验收。

## 不可用 Steam 后端保留本地存档

在独立u26-worktree、3da09e36基线上新增一个真实C++回归，使用实际NullSteamBackend，覆盖steam、steam://、steamcloud三个别名。首先证明本地save/load成功，再要求configure返回false、provider同一指针、原slot仍可发现，JSON与scene/token元数据可读且原文件字节不变。RAII按正确生命周期恢复原注册指针；没有替换被测SaveManager逻辑或真实Null后端。

未修生产源码首次实际Debug目标构建成功，回归1方法失败、42断言中18失败：三个别名均返回配置成功并替换provider，原slot因此不可见/读回null，原物理字节仍在。1415其他用例是定向过滤未选中。原red-01.log摘要527593b830a3571837c3c2af324cd9bc5fd4a92ae1df58a93ca036ce3b0c5de8，原二进制a138cce300e1aa29834d22cd2b3a165d895e839758cc2b0e028d9252cb0f3db8及源码/test快照由red-receipt-01.json锁定。

修复在provider替换前检查后端指针及isAvailable()，使用公共ISteamBackend接口。第一次增量构建因缺少完整接口include报C2027；原build-green-01.log保留，补入../steam/api/ISteamBackend.h后build-green-02成功。相同冻结测试实际1/1、42断言全通过；cloud_save/storage/steam/runtime_backend_availability四个源码过滤得到85/85、1249断言全通过，1331为未选中，不能冒充完整套件。已注册available mock的云存读往返、未注册拒绝和加密写入相邻控制仍通过。

最终SaveManager.cpp摘要2a7ecd87f0dd1311e6706ae8e5deeac6a2aaabd18540b22346cf65b73b207583，测试摘要de01cbcd1d9fb3a89357dc3d4c68ecf4646c2335bf9f1db557720c81205aef2c与RED完全一致。storage耦合仍4/4（archive/debug/di/steam），count_coupling --ci通过，没有具体实现头依赖。独审无可行动发现，u26-null-steam/independent-review-01.md摘要a9dec6450d74e1d0854589a2e3ca02e8d1004089daf5c4be3b6af57388fd8cd7，JSON摘要395d5671472949517243968eb7eed444d3df14433142a5bd6ae2c896939ff8ba。六native profile的C++最低发现数按实际新增1方法各加一，CTest门槛不变。

## 下一步

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
