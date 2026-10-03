# U28 平台与能力声明的证据边界

对应[当前计划U28](2026-09-05-001-refactor-runtime-foundation-plan.md)，在U29隔离整合工作树、基线 `cddec53e20a0fc20542e74d090a20ef02a28f0b7` 上实现。此记录不关闭U29整合门禁，也不将父分支的历史通过改写成整合候选通过。

## 已复现的问题与修复

未修改生成器时，3项文档负例出现4个断言失败：仅凭文档引用输出“100% Evidence-Backed”；页尾固定输出Linux11/11、Web368项；全部平台能力置pending并去掉证据后仍输出四个平台已通过。注册进既有测试后，4项新增回归保留4失败/1错误的原始RED。修复后，普通平台生成只说明格式、引用和同步，历史证据为 `NOT_REVERIFIED`，当前证据默认 `NOT_RUN`；页尾完全由实际矩阵状态生成。同步commit不再表述为运行源码身份。

能力扫描仍使用原扫描及计数逻辑，但将“Runtime测试证据”和通过勾号改为测试源码引用；Platform/Package/Observable明确是人工声明，JSON标记runtime evidence verification=NOT_RUN。原10项测试中3项新标签断言先失败，修复后全部通过。README、兼容性速查与发布指南同步移除已过期的固定数量和Steam DLL安装描述；没有改变兼容政策或SDK功能支持范围。

新增 `platform_evidence.py` 仅作为既有U1/U22验证器的只读适配器。调用方独立锁定选择文件摘要、完整候选SHA、可信profile及证据根；所有选定声明均必需。`execution`重读原始报告并核对run/context/profile/cache；`package_receipt`重算最终包和原收据，保留 `RAW_STAGE_LOGS_NOT_INCLUDED_NOT_REPLAYED`。未知 `package_runtime`/设备类型拒绝，诊断fixture不能升级为current verified。没有新增执行器、网络认证或发布批准。

独立审查发现并修复两项P2。其一，报告和Markdown/JSON输出重合会覆盖证据；有效CLI字节fixture中6项有4项先失败，加入规范化冲突检查和排他创建后6/6通过。初版RED因Windows子进程默认GBK与UTF-8读取不一致而出现6个错误，原日志保留；仅修正测试子进程 `-X utf8` 后才取得有效RED。其二，inventory的 `PackageVerificationError` 未被适配器捕获；两项真实inventory限额负例先抛出异常，显式纳入三个异常边界后返回结构化FAIL，最终稳定性失败也将此前PASS降级。仅fixture临时调整限额，生产阈值未动。

独立显示审查原报告SHA256 `74b9b7f7033ba66f15e9ff96933df55e1a4757db7da3c31015c03e71e13ba79e`；输出保护修复审查 `35c18ab770a75dd038d03503adad73d9c750e932af20942d659838b41914df79`；适配器原审查 `d8662c071671ed455870eef02c7d6fa09184a2bcdef45813a0dd7a8d82b6a90c`；inventory修复审查 `23e061d3935ecfb17d98a86b29bf77fd5d62060669fede51714e5eb9c5eb8494`。四份报告均保留于本工作树 `artifacts/validation/`，原发现没有被覆盖为无发现。

## 实际验证与来源

维护测试最终由受控进程逐套执行：platform_matrix_adversarial **42/42**、platform_evidence **36/36**、capability_closure **10/10**，零失败、零跳过。owned进程全部清理，未超时或强杀，源码首尾稳定。`u28-final-maintenance-01/run.json` SHA256为 `486577a538dc0f82c4fe0fb750aef65379e729ff76e61e50b642e9b7d56514ad`。适配器首次RED31/31失败；早期直接导入旧测试类额外发现23项的RED01也保留，未当成应有用例数。

通过正式CLI重新读取干净c25源码的原始完整Windows Debug/Release收据，两项声明均严格通过；把预期SHA换成cddec或把Debug平台改成Linux后分别退出1、保存FAIL报告且不输出通过Markdown。原C++1477、Lua147/56、CTest62的记录被重解析，没有重新运行这些测试。修复后最终 `u28-current-execution-cli-02/run.json` SHA256为 `6808a22268bdb632f402c71361970ed0d32020b0ef43076dcdd4cb7a4053f36b`；初版01证据也保留。被验证源码仍为 `c25d81ebfb324b85b54252ce937039c6dc790f28`，验证器来自本次U29脏工作树并记录自身指纹，不能声称整合候选已通过原生门禁。

另以U23已经认证和完整下载的f87四平台包执行正式CLI。外部选择固定原run35901948988/attempt1的producer身份和最终文件摘要，四项包字节/收据声明通过；仅替换Windows最终digest的一个字符后整体退出1。原包不改写，不重新联网或执行引擎。`u28-package-cli-01/run.json` SHA256为 `a8075a49ef49e7329a2900ad08c217f9f3f93f281ff7687cb11427ee78838645`。验证保留hosted authentication=NOT_PERFORMED、原始包日志未重放、release_ready=false；先前的GitHub认证属于U23原证据，不冒充本次适配器执行。

新增维护测试注册为 `CaesuraRelease_platform_evidence`，六组native CTest最低发现数70→71，其他profile策略不动；本工作树实际CMake发现/完整执行尚未进行。耦合与API边界静态检查通过，注册检查发现204个Lua、94个C++测试文件均已注册；这是注册完整性，不是行/分支覆盖率。未测覆盖率，不报告百分比。

## 后续出口

按新整合源码冻结候选并执行完整门禁、相应平台/最终包和U29验收，使用本入口重验其真实证据。当前公共矩阵保留逐行历史提交和日期；新一轮未执行的设备、SDK账号、原始包运行日志或发布范围不改为PASS。U28的本次实现与局部验证完成后，最终公开声明仍随整合证据同步；完整U1–U29目标继续。
