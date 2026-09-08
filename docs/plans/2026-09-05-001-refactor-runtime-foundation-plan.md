---
title: "refactor: 运行时可靠性与交付闭环迭代计划"
type: refactor
status: active
date: 2026-09-05
execution: code
baseline_commit: 87a96f3d846caf5ad2aac6e3b8585133e7a5aed5
supersedes: all_previous_iteration_plans
---

# refactor: 运行时可靠性与交付闭环迭代计划

## 1. 计划地位与交付目标

本文从当前源码与可复核证据重新安排后续工作，替代此前所有冲刺、路线图、总任务书和 Studio 实施计划的排期地位。旧文件中的 Phase、round、任务编号、功能冻结、平台排除和执行许可均不继承。历史记录保留用于追溯，不能重新激活旧待办。

仅保留两项产品方向约束：

1. **底层优先**：优先处理运行正确性、资源与状态生命周期、兼容性、真实后端能力、测试和分发可靠性。
2. **Studio 暂停**：不推进 IDE 文件读写、前端认证/LSP 接线、面板功能、Electron 修复、Tauri 壳和桌面编辑器打包。已有接口和随包资源的回归检查继续保留；底层验收完成也不自动恢复 Studio。

其余内容是本轮提出的工程方案，允许依据复现结果、测量结果和资源情况调整。既有模块/API 边界、BackendRegistry 和组合根是当前代码架构，优先沿用；不把旧计划的产品排序继续当作约束。新能力可以因明确底层缺口进入后续切片，不设置继承自旧计划的全局 feature 冻结。

本计划的交付物是：**一条能真实运行、能够恢复状态、具有明确平台能力边界、且最终发布包与验证结果对应的引擎交付链。** 本文是计划，不代表任务已经执行，也不代表已经批准发布或商店上传。

---

## 2. 当前起点与证据分级

以下事实对应 baseline_commit。实施时先检查源码是否变化，不把旧二进制或本文数字当作新基线。

| 观察 | 依据 | 对计划的影响 |
|---|---|---|
| C++20、16 个模块库、34 个接口头、412 个纯虚方法 | CMakeLists.txt、cmake/CaesuraModules.cmake、src/*/api/、scripts/api_stats.py | 复用模块结构；接口改动同步全部实现和普查 |
| 普通游戏通过 SDL 事件消费异步结果；headless/editor 直接 drain | src/resource/AsyncLoader.cpp、src/entry/Engine.cpp | 两条真实消费路径必须分别测试 |
| SDL_PushEvent 成功条件被按非零失败处理 | src/resource/AsyncLoader.cpp 的 poll；仓内 SDL3 头文件 | 高优先级静态缺陷候选，先复现所有权错误 |
| 默认 LocalFile provider 可能绕过设置密钥后的加解密 | src/entry/Engine.cpp、src/storage/SaveManager.cpp | 测试必须覆盖真实组合根配置 |
| RC 生成器直接填写 PASS、1052 用例、固定时间及 RC-GO | scripts/verify_release_candidate.py 的 generate_release_bundle | 先改证据来源；不能重生成旧报告来消除红灯 |
| runner 与 GameState registry 分别创建 ctx | scripts/kag_runner.lua、src/script/state/GameState.cpp、src/debug/HotReload.cpp | 明确活跃会话和热重载的唯一引用 |
| Web 复用 Lua，但运行于独立 JS/DOM/WebAudio 后端 | web/bridge.js、web/audio-engine.js、web/dom-renderer.js | 语言等价性与后端效果验收分开 |
| CLOSED=131、EXPERIMENTAL=3；命令级 Platform=3、Packaged=0 | docs/design/capability-closure-matrix.md，输入指纹 c11d3af2d377632a | 结构闭合不作为运行/平台/包完成率 |
| 同 HEAD 的线上 CI 十个 job 成功 | GitHub Actions run 33891363233 | 可作历史运行证据，仍须看具体 skip 与配置 |
| 该 CI 的 Web 为 342 passed/29 skipped；CTest 两项 skipped | 同 run Windows Debug job | 必需测试与条件测试需显式分类 |
| 已有 C++ 二进制枚举 1137；本地历史 CTest 有 RC mutation 失败 | 上轮枚举与 build/Testing/Temporary 历史日志 | 重新构建并实测，不直接沿用绿色结论 |

RC 生成器的上述源码事实不否定独立 GitHub Actions 的原始执行日志；它意味着由该生成器写出的报告不能自行证明测试运行过。既有证据先保留来源，不覆盖、改日期或贴新 SHA。

已落地的 typewriter 音效、hr、3D-LUT、错误报告链、CLI、Golden 项目、平台构建和打包器都优先复用。本文不把它们重新包装成待实现功能。

---

## 3. 可检验的需求

**验证与交付**

- R1. 所有通过结论来自实际执行，能追溯到源码、配置、二进制、夹具和原始日志。
- R2. 必需检查缺失、失败、取消或跳过时，不能获得对应范围的可发布结论。
- R3. 最终上传/分发的包，就是经过验证的那一份字节产物。
- R4. 旧计划退出当前入口；文档中的能力和平台声明具有真实证据范围。

**运行时与数据**

- R5. 异步请求、取消、完成回调和关停拥有确定的终态及释放责任。
- R6. 存档加密不随 provider 改变；保存失败保留上一个完整版本，旧存档按明确规则恢复。
- R7. 启动、热重载、存读档和回滚操作同一活跃会话；失败不会留下半恢复状态。
- R8. 源脚本、编译缓存和 Web bundle 对共同支持的语言行为保持一致。
- R9. GPU、音频、输入和移动生命周期的关键行为由真实效果验证，降级可辨认。
- R10. CARC 的内容完整性、发行者身份和资源边界分别有明确契约。

**作者路径与平台**

- R11. CLI 能完成创建、校验、构建、仓外启动、存档重启与到达结局的完整路径。
- R12. 各目标的 supported/approximate/unsupported 与实际后端一致，必需能力缺失不能静默成功。
- R13. 六个平台及可选 SDK 分别推进自动检查和条件验证；外部条件只阻塞对应范围。
- R14. 性能、内存与长跑结论由可比较的测量产生；优化不得破坏正确性。

Studio 暂停适用于全部需求。底层 RPC、CLI、现有调试接口的生命周期回归不等于恢复 Studio 开发。

---

## 4. 关键技术决策

- **先修证据生产，再扩大绿色结论。** 执行器、采集器、验证器分工明确；验证器不能伪造测试记录或自行授予 RC-GO。沿用现有脚本和测试框架，不引入新的测试平台。
- **严重正确性修复与基线整理并行。** U3/U4 可以立即用原始回归日志推进，不等待 U1/U2 全部完成；合并验收再接入可信通路。
- **先覆盖实际组合，再决定重构。** 对静态缺陷候选，先确认进入真实路径并观察生命周期、数据或状态错误。若有效回归证明问题不成立，记录原因，关闭修复分支；不为了完成任务而制造实现改动。
- **provider 传输原始字节，SaveManager 决定格式。** 不创建另一套加密provider。存档策略明确为compatible或require-encrypted，由可信宿主配置选择。compatible按key配置决定写入格式并保持旧明文读取，但不承诺阻止整文件替换为JSON；require-encrypted拒绝明文，缺有效key时也拒绝保存并保留旧档，旧档需要显式导入。已识别CAES的认证失败不得回退解析或使用解密输出，依据 [OpenSSL AEAD 接口](https://docs.openssl.org/3.3/man3/EVP_EncryptInit/#aead-interface)。
- **先修 SDL 返回语义与 ownership，再收敛取消和退出。** SDL3 入队成功为 true、过滤或失败为 false；应用事件类型应避免与其他组件冲突。依据 [SDL_PushEvent 官方接口](https://wiki.libsdl.org/SDL3/SDL_PushEvent)。复制 SDL_Event 不会替应用管理 user.data 指向对象的生命周期。
- **语言契约共用，效果验证按后端分开。** 同一对话/选择/变量事件序列可跨 native/Web 比较；像素、音源和设备行为由各自专项验证。Web 近似效果明确标注。
- **发布范围在验证前确定。** 首次候选的默认目标为 Windows、Linux、Web；这是基于现有环境提出的起始选择，U2 可依据真实可用条件调整。macOS/iOS/Android 的自动检查继续运行；任何新增可发布平台都必须满足自己的设备、包与签名条件。不得在测试失败后缩小范围来把该候选改判为通过。
- **测试替身与真机各司其职。** 故障注入测试证明分支行为，真后端检查证明效果；Null 后端不升级为 GPU/音频/SDK 已验证。
- **暂不全面重写 Engine、scheduler、打包器或 Web bridge。** 只有具体复现、重复状态维护或测量结果证明必要时，才提取局部共享实现。
- **源码内的现有兼容承诺先做特征测试。** 发现语义不明确时，在对应任务中写清契约和迁移行为，再改代码；不把文档旧注释直接当成实现真相。

---

## 5. 迭代安排与依赖

工作量是熟悉仓库后的净工程人日粗估，包含定向回归和审查，不含新发现缺陷的额外修复、SDK 下载、设备等待、商店审核。各任务可提前完成时直接关闭，不按预算凑工作。I0 完成后按实际吞吐量重估日历时间；本文不承诺虚构交付日期。

| 迭代 | 任务 | 交付重点 | 初估人日 | 阶段出口 |
|---|---|---|---:|---|
| I0 | U1–U2 | 可信证据与可复现基线 | 7–11 | fresh checkout 可实跑、采集、拒绝缺测与假绿 |
| I1 | U3–U4 | 异步事件与存档加密 | 3–6 | 两个高风险组合各有有效回归和结论 |
| I2 | U5–U9 | 取消/退出、原子写盘、归档边界 | 10–18 | 无晚回调污染、旧档不被失败写盘破坏、信任可选择 |
| I3 | U10–U14 | 会话、恢复、回滚、编译与缓存 | 11–20 | 同一路线在多种执行/恢复路径下状态一致 |
| I4 | U15–U18 | 原生图像、截图、音频输入与诊断 | 10–18 | 实际后端、实际效果与错误终态有证据 |
| I5 | U19–U21 | 目标能力、Web 音频、CLI 作者旅程 | 8–13 | 支持范围真实，仓外作品可保存并冷启动恢复 |
| I6 | U22–U23、U27–U28 | 包验证、发布门禁、长跑与文档 | 12–20 | 同 SHA/同包验收、性能有基线、声明可追溯 |
| I7 | U24–U26、U29 | 平台/SDK 条件验证与候选演练 | 9–17 | 选定发布范围通过；其余范围保持未验证 |

这些是工作包分组，不是八道全局串行屏障。U3/U4 与 I0 并行；U8/U9 的夹具、U13 的语料、U19 的能力核对、U24/U25/U26 的无设备部分可以提前推进。U27 的测量骨架从 U2 开始，最终长跑在主要修复汇合后执行。完整预算池约 70–123 人日，仅用于容量判断；外部条件任务不纳入承诺完成日。

~~~mermaid
flowchart TB
  A["I0 可信证据与基线"] --> E["I6 包、门禁、长跑与声明"]
  B["I1 异步与加密修复"] --> C["I2 资源/数据生命周期"]
  C --> D["I3 会话与脚本语义"]
  C --> G["I4 原生后端效果"]
  D --> H["I5 Web 与 CLI 作者路径"]
  G --> E
  H --> E
  A --> P["平台与 SDK 自动检查/条件准备"]
  P --> F["I7 所选发布范围的候选演练"]
  E --> F
~~~

**并行与合并纪律**：默认最多三个实现分支。资源/Job/Engine、存档/归档、脚本/Web、构建/发布按实际文件集分配；同一 Engine.cpp、SaveManager.cpp、tests/CMakeLists.txt 或 workflow 的变更由一个集成人串行合并。接口变更先形成小补丁并更新所有实现，再允许依赖任务开始。每个 U 单独交付，不做整份路线图的大合并。

**首个可交付批次**：U1、U2、U3、U4。必须得到“可信 baseline + 异步 ownership 回归 + 默认 provider 加密回归”；不要等全部 29 项完成才交付一次改进。

---

## 6. 验证与状态模型

### 6.1 证据数据流

~~~mermaid
flowchart TB
  S["固定源码 SHA / 配置 / 夹具指纹"] --> B["真实构建"]
  B --> T["以本轮二进制运行测试"]
  T --> L["原始日志 / XML / 退出码 / 起止时间"]
  B --> F["包组装及适用的签名/zipalign/公证变换"]
  F --> Q["最终包检查 / 最终 digest"]
  Q --> L
  L --> C["采集与归一化"]
  C --> V["只读验证必需检查及 provenance"]
  V --> K["测试过的包 digest + 可发布范围"]
  K --> P["发布候选判定 / 后续发布动作"]
~~~

证据至少包含：source_sha、dirty 状态/工作树指纹、run_id、平台/编译器/SDK、配置开关、binary/package SHA256、fixture 指纹、实际起止时间、执行参数、原始日志 digest、发现/执行/通过/失败/跳过计数和结果原因。开发 dirty 结果可以保存作诊断，但不能改写为某个干净 commit 的发布证据。

执行result值为 PASS、FAIL、SKIP、NOT_RUN、NOT_APPLICABLE。测试注册数不是通过数；SKIP 不是 NOT_APPLICABLE；缺 SDK/设备通常是 NOT_RUN。必需检查发生 SKIP/NOT_RUN 就未通过相应gate。NOT_APPLICABLE必须来自运行前确定的配置适用性规则。首次失败和重试结果都保留；未解释的重试通过不能掩盖不稳定性。

性能记录另有measurement_status：MEASURED、NOT_MEASURED、INCONCLUSIVE。必需性能gate同时要求执行result为PASS、测量可判定且满足事先锁定的阈值；采样程序正常退出不自动等于性能通过。首次建立基线只验数据完整性，尚无阈值时不能宣称已通过性能防退化检查。

信任边界是受控执行器、指定CI仓库及验证工作流，不包含已攻陷runner或恶意修改测试代码的维护者。required profile及其digest在运行前从受信任仓库配置锁定，待验报告不能自行缩减required集合。发布聚合从指定workflow/run/run_attempt取得job结论与固定artifact ID；digest由真实构建、执行和上传边界计算并复核。自报SHA和一组彼此匹配的摘要只能证明内部一致，不能单独授予发布通过。本轮不建设独立签名证据平台。

每次记录写入独立 artifacts/validation/<source-sha>/<run-id>/<platform-config>/，发布证据聚合引用它们，不覆盖历史。若 artifacts 不受版本管理，仍需上传或保存在可靠位置，避免链接只存在于某台机器。

### 6.2 请求终态与会话回调

下图说明异步请求的方向性生命周期，不规定具体类结构。终态记录和调用Lua闭包是两层：旧请求仍须结束和释放，但owner/session失效后不得调用其成功或失败闭包。

~~~mermaid
stateDiagram-v2
  [*] --> Queued: 请求接受
  Queued --> Running: 开始执行
  Queued --> Cancelled: 开始前取消
  Running --> CompletionQueued: 结果就绪
  Running --> Cancelled: 请求代次失效
  CompletionQueued --> Delivered: owner与代次仍有效
  CompletionQueued --> Cancelled: owner或代次失效
  Delivered --> Released
  Cancelled --> Released
  Released --> [*]
~~~

Cancelled是逻辑终态，不能立即销毁worker仍在读写的缓冲。Released只有在worker停止访问、队列及回调所有权全部释放后才成立；运行中取消通过等待完成或现有共享所有权延迟物理释放，不引入新任务框架。

### 6.3 行为验收例子

- AE1：真实 SDL EVENTS 环境中加载资源，主线程在下一轮消费结果；payload 在消费后释放一次，过滤失败也恰好释放一次。
- AE2：默认 LocalFile provider 设置密钥后保存，磁盘无目标明文片段；退出并重启后同 key 可读，错 key 拒绝且当前会话不变。
- AE3：保存路线 A 后走路线 B 并创建新变量；读 A 后，存档拥有的状态恢复为 A，下一次选择和结局与冷启动读 A 相同。
- AE4：同一场景直接运行、AST 编译、磁盘缓存和 Web bundle 运行，在共同支持语义上得到相同事件序列。
- AE5：音频解码未完成时停止或读档，旧请求之后完成也不会再次播放。
- AE6：候选包复制到仓库外的中文/空格目录，使用包内依赖完成启动、分支、保存退出和新进程恢复。
- AE7：更换一个 package digest，或让一个 required job 被取消，发布输入验证必须失败。
- AE8：当前没有 iPhone 或 Steam 测试账号，相关结果保持 NOT_RUN；其他底层检查继续，不能生成该范围已通过的声明。

### 6.4 执行时的统一验收

每个代码任务均需要定向回归、相关模块检查、独立代码审查和集成构建。C++ 完整套件从正确资源 CWD 运行，所有发现用例通过且 0 failed/0 skipped；分类过滤产生的未选中项另记。Lua 主/孤儿套件继续分开，避免沙箱顺序污染。新测试注册进入实际执行入口，不能只增加一个从未运行的文件。

公共接口、生命周期和存档变更触发完整 Debug 构建及 C++/Lua/CTest；发布候选另外验证 Release、目标平台和真实包。既有 Studio 回归可继续执行，但不以补齐其功能为本计划交付前提。真实覆盖率只有工具产出才报告，测试注册完整性单独命名。

---

## 7. 实施任务

本节的“新增”文件是计划产物，其余为现有入口。修复任务若被有效回归反证，不改变实现；验收记录写明原因。任务内提及文档更新随行为一起交付，避免统一留到最后再修声明。

### U1. 真实执行结果驱动验证证据

- **目标/需求**：消除硬编码测试通过和自动 RC-GO；满足 R1/R2。
- **依赖/估算**：无；4–6 人日。
- **文件**：scripts/verify_release_candidate.py；tests/scripts/test_rc_adversarial_mutations.py；tests/CMakeLists.txt；Lua 两个 runner。新增 scripts/collect_validation_evidence.py、scripts/schemas/validation-evidence.schema.json、tests/scripts/test_validation_evidence.py。
- **方案与模式**：复用 doctest/CTest/Vitest 真实报告，Lua runner输出实际执行计数；collector只解析，不编造缺失字段。RC mutation 使用明确的隔离夹具，真正候选验证在采集之后执行，解除依赖旧 bundle 的循环。
- **执行姿态/测试**：先锁定“runner退出失败却打印PASS”、零用例、日志截断、错误binary digest、缺required suite、异SHA、篡改、重复run-id；同时改报告/日志digest/required列表仍不能越过可信配置；同SHA但错误workflow/run_attempt也拒绝。测试夹具不得作为发布证据消费。
- **验收**：fresh checkout 的 verifier/mutation 测试真实执行；没有运行数据就不能生成PASS或GO；历史bundle被标记来源不足并保留，不重贴当前身份。

### U2. 可复现构建和分层测试基线

- **目标/需求**：定义实际执行配置及每种配置的必需检查；满足 R1/R2/R13/R14。
- **依赖/估算**：配置清点可立即开始，证据出口依赖U1；3–5 人日。
- **文件**：CMakeLists.txt；cmake/CaesuraModules.cmake；tests/CMakeLists.txt；.github/workflows/ci.yml；tests/scripts/check_test_coverage.py。新增 CMakePresets.json、scripts/validation_profiles.json。
- **方案与模式**：复用生产静态库测试架构；固定基础Debug/Release、FFmpeg开关、可选SDK关闭、iOS/Android编译等代表配置。加入Linux Clang ASan/UBSan第一方代码检查，TSan先验证可行性。记录修改前的性能样本供U27比较。
- **2026-09-08 用户指定的 Apple 执行渠道**：U2的iOS与macOS测试使用Expo EAS Workflows的macOS自定义job。按精确源码SHA拉取Caesura，直接运行现有原生构建、CaesuraTests/CTest和可信证据工具；iOS分别记录设备架构编译与实际模拟器测试。EAS任务成功、构建成功、模拟器原生测试、完整app/Metal/设备签名证据分开，不用JavaScript测试结果替代C++引擎结果。U25的真实窗口、设备及签名交付继续保留。
- **测试**：干净配置/构建；缺Python/Bash导致测试未注册；必需资产缺失；测试发现数意外下降；sanitizer诊断使任务失败；Web完整包模式缺资产不得skip。清点29个Web skipped的真实原因，逐项分类。
- **验收**：完整与快速开发配置清楚分开；required集合由运行前锁定的可信配置决定；全部运行用本轮构建，不捡旧Release/Lua；配置不支持的sanitizer有具体原因与替代验证，不能整模块静默关闭。

### U3. SDL 异步事件结果的所有权

- **目标/需求**：确认并修复入队成功时提前释放问题；满足 R5、AE1。
- **依赖/估算**：无，可与U1并行；1–2 人日。
- **文件**：src/resource/AsyncLoader.cpp/.h；src/resource/api/IAsyncLoader.h；必要时src/entry/Engine.cpp；tests/cpp/test_async.cpp、test_entry.cpp。
- **方案与模式**：先保持SDL投递与drain两条现有路径，修正bool返回处理并明确入队前/成功后/拒绝/关停的释放方；核对事件类型冲突，不顺带重写整个事件系统。
- **测试**：真实EVENTS初始化、Job完成、投递、下一轮取出并校验数据；filter拒绝；队列残留时shutdown；以生命周期计数或ASan观测重复释放，不以“没崩溃”作为反证。
- **验收**：两条消费路径一次终态、一次释放；无悬空指针或遗留owner引用。

### U4. 默认 provider 的存档加密与兼容读取

- **目标/需求**：设置key后实际落密文，provider只传原始字节；满足 R6、AE2。
- **依赖/估算**：无，可与U3并行；2–4 人日。
- **文件**：src/storage/SaveManager.cpp/.h；src/storage/api/ISaveProvider.h；src/storage/HttpCloudSaveProvider.cpp、CloudSaveProvider.cpp；src/entry/EngineConfig.h及存档配置接线；tests/cpp/test_storage.cpp、test_save_roundtrip.cpp、test_cloud_save.cpp。
- **方案与模式**：在provider调用之前编码、之后解码，复用现有CAES/加密实现，不新增密码算法。默认compatible保留既有明文读取；可显式配置require-encrypted拒绝明文，策略不得由待验文件自报。旧明文导入需显式允许并保留原档，只在成功后显式保存为密文，不批量重写。
- **测试**：Engine默认配置+key的磁盘字节；正确/错误/缺key；require-encrypted未设key和设key后clear再保存；tag、头部、未知版本、截断；密文整文件换成有效JSON在两种策略下分别接受并报告兼容范围/拒绝；云替身透明传输；旧明文显式导入；不重复加密。
- **验收**：写入格式不受provider影响；已识别CAES失败直接终止，绝不回退JSON；require-encrypted对非CAES也拒绝，缺有效key保存失败且不破坏旧档。compatible不宣称防整文件明文替换或防旧档重放；读取失败无会话变更，兼容政策和迁移样本同步。

### U5. 取消与热重载的异步代际

- **目标/需求**：旧请求不能因新enqueue而重新有效或污染缓存；满足 R5。
- **依赖/估算**：U3；2–3 人日。
- **文件**：src/resource/AsyncLoader.cpp/.h；必要时src/resource/api/IAsyncLoader.h；tests/cpp/test_async.cpp、test_resource.cpp。
- **方案与模式**：在现有InFlightEntry内记录请求代次，明确取消后的终态；复用缓存及资源代际概念，不新增任务框架。
- **测试**：barrier固定旧读取→cancel→改文件→同路径新读取→旧读取晚完成；成功、失败、取消结果分别晚于新会话或shutdown；多个waiter；两次cancel；缓存与pending计数。
- **验收**：旧任务不回填新代缓存；每个已接受请求有记录/计数层面的终态并释放。owner/session失效后禁止调用其成功或失败Lua闭包；取消通知仅在owner仍有效时交付。测试不依赖随机sleep。

### U6. Job 完成屏障与 Engine 关停

- **目标/需求**：区分worker完成与主线程callback完成，防止关闭Lua/GPU后回调；满足 R5。
- **依赖/估算**：U3/U5，Job定向测试可先做；2–4 人日。
- **文件**：src/job/api/IJobSystem.h；src/job/JobSystem.cpp/.h；src/resource/AsyncLoader.cpp；src/entry/Engine.cpp；tests/cpp/test_job_system.cpp、test_null_jobsystem.cpp、test_engine_lifecycle.cpp。
- **方案与模式**：明确现有waitIdle/poll/shutdown语义，必要时增加窄的排空操作；沿用逆序销毁，不把Registry改为通用并发容器。
- **测试**：最后worker和waitIdle竞态；callback再提交；callback抛异常；shutdown后submit；载入中退出；两个Engine先后运行，设备监听与错误回调无残留。
- **验收**：worker已join；callback仅在owner/session代次仍匹配时合法，最后合法callback先于owner销毁；重入行为有界；NullJobSystem与真实Job差异有回归。

### U7. 本地存档失败安全与原子替换

- **目标/需求**：写盘失败不删除上一个完整版本；满足 R6。
- **依赖/估算**：U4后合并，provider局部改动可提前；2–4 人日。
- **文件**：src/storage/ISaveProvider.cpp、LocalFileSaveProvider.h、SaveManager.cpp；tests/cpp/test_storage.cpp、test_cloud_save.cpp。需要子进程夹具时新增 tests/cpp/test_save_atomicity.cpp。
- **方案与模式**：收敛重复写盘路径，避免remove旧档再rename；使用平台适当的替换语义。明确原子可见性和断电持久性区别，不承诺未验证文件系统保证。
- **测试**：temp写入、flush/close、替换失败；文件占用；进程在各阶段中断；云拉取替换；重启读到旧完整档或新完整档；密文同样覆盖。
- **验收**：失败时旧文件字节保留；残留temp有确定恢复/清理策略；不能拿空文件或半档冒充保存成功。

### U8. CARC 发行者信任接线

- **目标/需求**：区分内容自洽与来自指定发行者；满足 R10。
- **依赖/估算**：无，可提前；2–4 人日。
- **文件**：src/entry/EngineConfig.h、Engine_Assets.cpp、StartupValidation.cpp；src/main.cpp；src/archive/CARCReader.cpp；tests/cpp/test_carc.cpp、test_archive.cpp、test_entry.cpp。
- **方案与模式**：复用已有指定公钥open能力，提供兼容自签与固定可信公钥两种显式模式。期望公钥和模式由宿主受控配置在首次挂载前提供，不能从待验归档或其可替换manifest自授信任，也不能依赖init后才加载的Lua配置。
- **测试**：A签名在pinned-A成功；攻击者自签在strict拒绝；同时替换归档与自报公钥仍拒绝；不可信patch不得覆盖base；缺/坏公钥；所有CARC自动挂载层；松散文件补齐缺资源时明确来源未认证；兼容模式旧包。
- **验收**：strict覆盖全部CARC挂载层且失败不降级；manifest指纹只作声明。该模式只认证CARC，不认证松散资源、入口Lua或可被替换的宿主本身，日志与文档明确范围；最终分发包整体身份由U22/U23约束。轮换、多发行者授权与完整签名资源封闭包另作后续需求。

### U9. CARC 边界、压缩与并发读取

- **目标/需求**：验证现有上限和异常路径，不预设解析器有漏洞；满足 R10/R14。
- **依赖/估算**：无，与U8共享源码修改时串行；2–3 人日。
- **文件**：src/archive/CARCReader.cpp/.h、CARCFormat.h；tests/cpp/test_carc.cpp、test_archive.cpp。需要持续fuzz时新增 tests/fuzz/carc_reader_fuzz.cpp 及独立可选CMake目标。
- **方案与模式**：复用已有checkedAdd和尺寸上限，先扩展合法签名的畸形包语料；仅失败处改实现。以缩小阈值覆盖边界，不在普通单测分配GiB。
- **测试**：offset溢出、区域越界、index/entry不一致、错误tag、解压大小不符、重复索引、close/reopen、并发读取、分配/读取失败。
- **验收**：精确成功或有界拒绝，无越界、泄漏、失控分配；真实包峰值内存/耗时留原始样本，只有超预算才考虑流式重构。

### U10. 唯一活跃脚本会话

- **目标/需求**：runner、热重载与C++查询引用同一活跃状态；满足 R7。
- **依赖/估算**：U5/U6的取消边界；2–4 人日。
- **文件**：scripts/kag_runner.lua；src/script/state/GameState.cpp/.h；src/script/vm/LuaManager.cpp；src/debug/HotReload.cpp；现有script API/绑定；tests/cpp/test_game_state.cpp、test_hotreload_integration.cpp；tests/scripts/test_operation.lua。
- **方案与模式**：Lua runner持有会话，C++ registry引用活跃表；明确start/stop/reload/failure更新点；跨模块仍走接口。不要把前端调试功能扩展混进本任务。
- **测试**：wait/tween中reload；失败start后再次start；A停止后B启动；A旧回调到达；close清理；C++与Lua读取相同会话身份。
- **验收**：无第二份分叉状态、无旧协程复活；更新Engine.h等陈旧注释。

### U11. 存档状态契约与真实冷启动恢复

- **目标/需求**：明确恢复提交点和失败状态，真实旧格式经当前C++读取；满足 R6/R7、AE2/AE3。
- **依赖/估算**：U4/U7/U10；3–5 人日。
- **文件**：scripts/kag/commands/save.lua；src/storage/SaveManager.cpp；tests/scripts/test_saveflow.lua、test_save_load_samescene.lua、test_golden_save_migration.lua；tests/cpp/test_save_roundtrip.cpp、test_save_migration.cpp、test_golden_saves.cpp；tests/projects/golden_vn/。
- **方案与模式**：f精确恢复、tf重建、lf/call/control stacks与恢复位置一致。sf及解锁数据先明确“全局累积/存档拥有”的字段清单。恢复依次解析/迁移/校验、准备场景与必要资源、提交会话和请求代次、释放旧资源。提交前不写全局解锁、不启动新音源；提交前失败保留旧会话。提交后无法恢复的GPU/音频失败进入一致的失败停止态，取消新代请求并清理部分资源，不能继续混合会话。真实旧版本产出与人工构造样本分别标记。
- **测试**：读档移除未来f变量；call/for/if内保存；同/跨场景；语言/图层/文本；坏档；准备图层失败、提交后音频/GPU恢复失败、旧回调恰在提交点到达；新进程读取；真实C++旧版本迁移。
- **验收**：成功时立即读取和冷启动读取的状态、分支、可见内容一致；提交前失败旧会话不变，提交后严重失败安全停止并可重新启动，不承诺任意外部副作用的事务回滚。纯Lua镜像不计作C++迁移证据；旧样本来源及digest可追溯。

### U12. 回滚安全点与副作用取消

- **目标/需求**：回滚后继续执行等价、不重复副作用；满足 R5/R7。
- **依赖/估算**：U10；字段语义与U11先对齐；2–4 人日。
- **文件**：scripts/kag/snapshot.lua、operation.lua、cancel_token.lua、kag_runner.lua；tests/scripts/test_rollback.lua、test_rollback_memory.lua、test_operation.lua、test_macro_nested.lua；web/backlog.rollback.integration.test.js。
- **方案与模式**：明确可恢复稳定点，补必要局部/控制状态；不序列化活跃C++资源或协程。不支持的边界返回可解释结果。复用CancelToken和Lua <close>。
- **测试**：call/loop内回滚；第一次点击揭示全文/第二次推进；voice/tween中回滚；选择边界；旧completion/AI回复晚到；多次历史回退。
- **验收**：无重复奖励/选择结算；旧操作清理；允许点下一步与原路径等价；历史上限内存有测量。

### U13. 多执行路径的语言等价语料

- **目标/需求**：源文本、AST、缓存、Web bundle行为一致；满足 R8、AE4。
- **依赖/估算**：语料可先建，恢复案例依赖U11/U12；3–5 人日。
- **文件**：scripts/flow.lua、kag/compiler.lua、semantic.lua、scheduler.lua、expr.lua；tests/scripts/test_compiler.lua、test_kag_semantic.lua、test_expr_lang.lua、test_control_flow.lua；web/flow.integration.test.js、bundle.cross-scene.integration.test.js。新增 tests/projects/runtime_contracts/。
- **方案与模式**：记录规范化对话、选择、变量、调用和结束事件；排除句柄/绝对路径/帧率。差异驱动小修，不先重写scheduler；复用现有parity comparator但必须有实际运行输入。
- **测试**：CJK/转义/多行；短路/三目/插值；静态与动态宏；跨场景call/return；条件选择；不同dt的wait/tween；存档重放。
- **验收**：共同支持语义的事件和终态一致；每个差异有最小场景，不能只比较预填JSON。

### U14. 编译缓存与 bundle 兼容标识

- **目标/需求**：旧/坏缓存不改变语义，包与runtime兼容可检查；满足 R8。
- **依赖/估算**：U13最小语料；1–2 人日。
- **文件**：scripts/kag/compiler.lua、flow.lua、ks_bake.lua、caesura_build.py；tests/scripts/test_compiler.lua、test_caesura_build_cli.py；web/perf-bundle.test.js、story.bundle.sweep.test.js。
- **方案与模式**：区分源hash、缓存格式版本、编译语义/契约标识。cache可丢弃回编译；无源码bundle不兼容时明确拒绝，避免静默执行旧含义。
- **测试**：同长度改源；只改schema默认值；未知版本/截断；mod同名替换；cache目录只读；正常缓存命中。
- **验收**：坏缓存不损坏游戏状态；可回退时回退；有效缓存仍有性能价值；发布包在制作时验证runtime匹配。

### U15. GPU 恢复与截图请求生命周期

- **目标/需求**：GPU状态由renderer定义，截图对应明确帧和请求；满足 R5/R9。
- **依赖/估算**：U6/U7；3–6 人日。
- **文件**：src/storage/SaveManager.cpp/.h；src/entry/Engine.cpp；src/render/api/IRenderDevice.h；BgfxRenderDevice与NullRenderDevice实现；tests/cpp/test_render_device.cpp、test_storage.cpp、test_gpu_monitor.cpp、test_render_integration.cpp。
- **方案与模式**：消除存档侧静态gfx-ready与直接bgfx调用；复用现有截图API，不足时增加引擎自有完成/取消标识。沿用renderer初始化守卫；不以init前getCaps探测安全性。
- **测试**：init前/headless/退出后拒绝；连续两次保存对应帧；丢失时取消截图；恢复后的字体/RTT/纹理；恢复失败；输出尺寸。
- **验收**：完成、失败、取消均明确；无双present；旧GPU资源不再提交；真GPU像素与请求身份可对应。

### U16. 原生渲染的实际效果回归

- **目标/需求**：验证现有渲染能力而不是再次添加同名功能；满足 R9。
- **依赖/估算**：U15，静态场景可提前；3–5 人日。
- **文件**：src/render/BgfxShaderManager.cpp、TextRenderer.cpp、BgfxRenderDevice.cpp、EmbeddedShaders*；tests/cpp/test_render_pipeline.cpp、test_render_postfx.cpp、test_quad_batch.cpp、test_render_metal_contract.cpp。新增 tests/projects/render_contracts/ 与 scripts/verify_render_contracts.py。
- **方案与模式**：固定小场景验证文字/alpha/多纹理/RTT/转场/3D-LUT；backend类型和像素两者都检查。shader生成记录工具版本与输入，不手工复制未经验证的数组。
- **测试**：D3D11/GL实际backend；CJK/ruby/透明边缘；层叠与分辨率变化；identity/非identity LUT；核心shader故障与非核心降级。Metal/GLES按可用设备执行。
- **验收**：预定义关键像素或有容差的参考图通过；Noop/IFH不能冒充成功。字体/驱动差异的容差由基线先确定。

### U17. 输入、音频与前后台生命周期

- **目标/需求**：平台事件到游戏语义准确，恢复后无重复输入或音频；满足 R9/R13。
- **依赖/估算**：U6/U10；2–4 人日。
- **文件**：src/input/、src/platform/、src/audio/AudioFocusService.h；src/entry/Engine.cpp；tests/cpp/test_input.cpp、test_gesture_detector.cpp、test_mobile_adapter.cpp、test_audio_integration.cpp。
- **方案与模式**：沿用物理/逻辑坐标换算及Lifecycle/AudioFocus服务；统一焦点、暂停、恢复、音源结束的可观察状态。不引入新的输入框架。
- **测试**：高DPI/resize后点选；触摸抬起/取消；长按与普通点击不重复；IME提交/取消；失焦音频暂停、恢复；停止后旧音源结束事件；真实voice_wait。
- **验收**：模拟事件回归和真设备行为分别记录；缺设备不提升通过范围；游戏逻辑坐标与实际点击一致。

### U18. 运行错误与底层 RPC 退出/超时语义

- **目标/需求**：错误可定位、失败后能安全退出或恢复，RPC不越线程；满足 R5/R7/R9。
- **依赖/估算**：U6/U10；2–3 人日。
- **文件**：src/entry/Engine.cpp、ErrorUI.cpp；src/main.cpp的owner dispatcher；src/rpc/api/IRpcDispatcher.h；src/rpc/EditorServer.cpp、RpcServer.cpp；scripts/scheduler.lua；tests/cpp/test_error_ui.cpp、test_rpc.cpp；tests/headless_rpc_smoke.py、headless_http_smoke.py。
- **方案与模式**：复用已有report_command_error和结构化日志；Queued取消与转入Running由同一同步判定保证。已开始操作遇超时不伪称取消成功，结果可能未知，终态写入内部请求ID关联的诊断。此切片不新增外部终态查询、持久操作账本或幂等协议，不承诺回滚Running操作；现有客户端不得对未知结果的变更自动重试。仅维护现有RPC，不新增Studio读写/LSP。
- **测试**：坏场景/缺资源/Lua异常；frame错误不刷屏失控；超时与出队同时发生；Running后回复丢失；客户端不自动重复提交；owner忙、断开、关停；worker不碰Lua/GPU；诊断无key/token。
- **验收**：Queued取消后不执行；Running超时明确未知而非已撤销，终态可由日志关联；严重错误有安全停止边界且不挂死。现有HTTP/stdio兼容回归通过；可靠断线重试查询若后续有需求另立协议任务。

### U19. 目标平台能力描述与必需能力校验

- **目标/需求**：作者能在运行/打包前发现不支持能力；满足 R12。
- **依赖/估算**：无，原生真实状态随U16/U17更新；2–3 人日。
- **文件**：scripts/caesura.py、ks_check.lua、package_game.mjs；web/bridge.js、dom-renderer.js；相关Lua backend shim；tests/scripts/test_contracts_runtime.lua、test_contract_runtime_gaps.lua；web/adapter.test.js、dom-renderer.test.js。新增 config/runtime-capabilities.json 与 tests/scripts/test_target_capabilities.py。
- **方案与模式**：最小能力描述覆盖必要的feature维度，区分supported/approximate/unsupported；构建配置/SDK存在性参与判断。复用现有schema解析项目需求，允许作者声明必需/可选，不建通用插件框架。
- **测试**：Web video/particles/复杂postfx无实现；CSS tint仅近似；原生SDK关闭；必需能力缺失；可选降级；拼错键；动态Lua无法静态判定时运行期再次检查；未执行的动态分支不能记为已覆盖。
- **验收**：不支持不能返回supported；包验收检查作者声明的必需能力和可静态确认的调用。无法分析/尚未执行的动态路径列为未证明范围，不授予全路径能力完整性的结论，也不因使用动态Lua就拒绝整个包。运行期实际能力调用仍必须返回明确成功、近似或不支持；诊断能定位目标/场景/命令。

### U20. Web 音频异步完成与取消

- **目标/需求**：旧decode不复活播放，等待依据真实音源状态；满足 R5/R9/R12、AE5。
- **依赖/估算**：U12/U19；3–5 人日。
- **文件**：web/audio-engine.js、bridge.js；web/audio-engine.test.js、audio-unlock.e2e.test.js、audio-ui.e2e.test.js；tests/scripts/test_wait_audio.lua、test_audio_fade.lua。
- **方案与模式**：每bus请求代次，处理fetch/decode晚完成和AudioContext解锁；先确保voice自然结束、stop、BGM loop及基本gain fade。暂未实做的crossfade依U19明确状态。
- **测试**：旧请求晚于新请求；decode前stop；404/解码失败；未解锁；读档中fade；重复stop；loop多周期；真实AudioContext。
- **验收**：过期声音不播放、无未处理Promise错误；wait不死等或提前完成；jsdom通过与实浏览器通过分别记录。

### U21. 非 Studio CLI 的完整作者路径

- **目标/需求**：用现有CLI制作一个能独立运行并恢复存档的作品；满足 R11/R12、AE6。
- **依赖/估算**：创建/启动骨架可先做；最终依赖U11/U13/U14/U19，Web音频依U20；3–5 人日。
- **文件**：scripts/caesura.py、caesura_build.py、ks_check.lua、ks_bake.lua、package_game.mjs；tools/project_templates/；tests/scripts/test_caesura_build_cli.py；tests/projects/runtime_contracts/。
- **方案与模式**：复用五模板和Node打包器；运行create→check→build/package→新目录启动→保存退出→新进程加载。没有Studio参与。
- **测试**：中文/空格路径；自定义entry；至少基础与另一种模板；子目录跨场景；缺资源；构建中断；已有输出含无关文件；Web根/子路径和离线重载。
- **验收**：包内依赖闭合、无源码仓库兜底；失败不留下标记成功的成品；两条分支和冷启动恢复通过；输出清理不删除用户无关文件。

### U22. 三桌面与 Web 最终包隔离验证

- **目标/需求**：以本轮明确包路径验证可搬迁产物，补Windows缺口；满足 R1/R3/R11。
- **依赖/估算**：U1/U2/U21，能力检查用U19；3–5 人日。
- **文件**：scripts/verify_release_package.sh、verify_web_package.sh、web_browser_smoke.mjs；CMakeLists.txt安装规则；.github/workflows/ci.yml、release.yml、deploy-web.yml；web/resources.test.js。新增 tests/scripts/test_release_package_contract.py。
- **方案与模式**：不按mtime挑包；所有适用的签名、zipalign、公证staple、重压缩完成后，计算最终digest并检查最终包。必须后置的变换记录输入/输出digest并重新检查输出。解压到仓外受控PATH目录，用本轮PID/端口验证身份；Web完整lane先bake/build/package再跑资源/浏览器检查。保留现有Studio静态产物兼容检查，不补功能。
- **测试**：删DLL/Lua/模板/WASM/chunk/资源；开发机路径泄漏；旧进程占端口；损坏包；root与subpath；用户手势音频解锁；失败后的进程清理。
- **验收**：三个桌面与Web包按平台配置分别产出digest、原始日志和完成结果。U23/U29只消费该候选在运行前指定为required的平台结果；其他平台继续保留任务和未完成状态，不阻塞范围外候选。required资源缺失直接FAIL；不能误连其他进程；重试不抹首次失败。

### U23. 统一 CI 与发布的必需门禁

- **目标/需求**：发布只消费同一次验证产生的通过产物；满足 R2/R3、AE7。
- **依赖/估算**：U1/U2，以及U22中该候选required的平台结果；3–5 人日。
- **文件**：.github/workflows/ci.yml、release.yml、deploy-web.yml。新增 .github/workflows/validate-engine.yml、scripts/verify_release_inputs.py、tests/scripts/test_release_inputs.py。
- **方案与模式**：共用workflow_call验证入口，固定完整SHA和预锁定profile；aggregate从指定workflow/run/run_attempt取得required job结论与artifact ID；tag/CMake/manifest版本对应。按固定artifact ID下载，上传前复算最终digest，不能重新glob挑包或变换未经复验的字节。服务端branch protection设置和权限结果单独验收。
- **测试**：required失败/skip/cancel/missing；其他SHA/run_attempt/workflow全绿；调换包或required集合；tag/版本不符；验证后签名/重压缩而未复验；聚合后本地包被替换；首次失败后只取重试结果。
- **验收**：不符合声明范围的输入无法到publish；验证与发布身份一致；本任务可完成到发布dry-run，不因计划存在而实际上传/发版。

### U24. Android 自动构建和设备验证

- **目标/需求**：将可自动执行部分做成硬检查，设备行为按真实产物验证；满足 R9/R13。
- **依赖/估算**：准备可提前；证据依U1，运行依U16/U17/U22；2–4 人日，不含设备等待。
- **文件**：android/；scripts/build_android.sh、build_android_release.sh、android_device_smoke.sh；.github/workflows/ci.yml；tests/cpp/test_mobile_adapter.cpp、test_mobile_stress_validation.cpp。
- **方案与模式**：核实NDK/SDK/签名配置；稳定通过后移除compile continue-on-error；APK/AAB结构、测试签名与zipalign独立验证。实际设备按查询结果登记，不假设旧网络地址仍可达。
- **测试**：clean交叉编译；缺依赖失败；安装/升级；CJK、IME、点击、音频焦点、前后台、存档重启；被系统终止后的恢复。
- **验收**：compile/package/install/runtime分别出结果；无设备为NOT_RUN；测试签名不能升级成正式商店签名结论。

### U25. Apple 编译、包与设备运行分层

- **目标/需求**：持续验证macOS/iOS自动路径，独立承接Metal和签名证据；满足 R9/R13。
- **依赖/估算**：准备可提前；证据依U1，运行依U15/U16/U17/U22；2–4 人日，不含硬件/账号等待。
- **文件**：CMakeLists.txt、cmake/；.github/workflows/ci.yml；scripts/verify_metal_shaders.py；tests/cpp/test_render_metal_contract.cpp、test_platform.cpp；docs/platform/中的现有设备执行指南。
- **方案与模式**：macOS构建/可搬迁包和iOS arm64未签名产物持续硬检查；先验证实际产物形态，不预设必须.app或更换发行格式。Mac窗口化和iPhone安装作为独立执行配置。
- **测试**：实际Metal backend/图像；Retina/resize；音频中断；safe area/触摸；前后台；有凭据时签名/公证/TestFlight包身份。
- **验收**：无真机不宣称Gameplay；未签名构建不等于安装成功；任何签名或商店上传需另行实际执行并留证，不从编译推出。

### U26. 可选 SDK 与云存档边界

- **目标/需求**：SDK缺失、离线、失败和真实能力状态明确；满足 R6/R9/R12/R13。
- **依赖/估算**：U4/U7/U19；3–5 人日，真实账号/SDK验收另估。
- **文件**：src/steam/、src/live2d/、src/storage/CloudSaveProvider.cpp、HttpCloudSaveProvider.cpp；tests/cpp/test_steam.cpp、test_live2d.cpp、test_cloud_save.cpp；scripts/steam/。
- **方案与模式**：OFF/mock/fallback始终可验；已获SDK时ON编译；有合法测试模型/账号后才验证Live2D动作、Steam成就/统计/云冲突。云冲突保留双方版本，不以时间戳猜测并静默覆盖。
- **测试**：缺SDK/客户端；离线/超时；加密字节往返；冲突/重试/重复回调；模型失败释放；可用模型的动作和lip sync；测试AppID的实际功能。
- **验收**：fallback不能报告SDK功能已支持；未取得资源的项有具体NOT_RUN条件；真实SDK证据绑定产物。depot/商店发布不自动执行。

### U27. 可重放性能与长跑基线

- **目标/需求**：测量真实退化、资源未回收和挂死；满足 R5/R14。
- **依赖/估算**：U2先建采样骨架；最终依U5/U6/U11/U15/U20；5–8 人日，另加长跑时间。
- **文件**：scripts/run_benchmarks.sh；tests/cpp/test_perf_bench.cpp、test_mobile_stress_validation.cpp；Lua现有benchmark/stress测试；web/perf-baseline.test.js、perf-bundle.test.js。新增 scripts/compare_benchmarks.py、run_engine_soak.py、tests/projects/engine_soak/。
- **方案与模式**：固定Release、工作负载、seed、机器与二进制；预热后至少10样本、3次独立进程，交错比较base/candidate。长跑以静止点采样live handles、pending jobs、缓存、Lua heap和RSS；PR短版与候选至少一小时分开。
- **测试**：场景/资源循环、取消、voice打断、存读档/回滚、重启；人为遗漏释放/永不完成/坏档能被拦截；超时记录最后checkpoint和事件序列，只清理本轮进程。
- **验收**：性能数来自原始样本；未测为NOT_MEASURED，噪声大为INCONCLUSIVE；先锁基线容差再评价candidate，不事后放宽；静止点非缓存句柄/任务回基线；真实后端长跑独立于Null结果。只有可重复退化才启动专项优化切片。

### U28. 能力与平台声明由证据更新

- **目标/需求**：让公开文档能准确说明支持到哪一层；满足 R4/R12/R13。
- **依赖/估算**：U1/U19/U22；平台设备格随U24–U26补充；1–2 人日。
- **文件**：scripts/api_stats.py、capability_closure.py及平台状态生成器；docs/design/capability-closure-matrix.md与overrides；docs/status/platform-matrix.yaml；README.md、docs/team/development-guide.md、docs/compatibility.md；tests/scripts/test_capability_closure.py、test_platform_matrix_adversarial.py。
- **方案与模式**：结构扫描、运行记录、平台记录、包记录分层显示；生成器验证证据身份和存在性，不凭源码关键词给运行PASS。即时修正被本轮确认不准确的描述，不等待所有平台齐备。
- **测试**：旧SHA/缺日志/错误package digest/错误平台；实验能力和SDK关闭；接口计数变化；生成文档过期。
- **验收**：任何运行/平台/发布声明可追溯；旧百分比与硬编码测试数量不冒充当前状态；没有设备的格子保持未测。

### U29. 底层候选包验收演练

- **目标/需求**：用一个固定候选与明确范围验证完整交付链；满足 R1–R14。
- **依赖/估算**：U1–U23、U27/U28中该候选required项；U24–U26按运行前选定适用范围；2–4 人日。
- **文件**：scripts/verify_release_candidate.py、verify_release_inputs.py；tests/projects/runtime_contracts/、golden_vn/；docs/guides/release-process.md。执行证据进入artifacts/validation/，阶段记录按日期进入docs/plans/。
- **方案与模式**：冻结候选SHA及scope；构建、测试、适用分发变换、最终包验收、冷启动恢复、真实后端和长跑；聚合机器检查结论及未测范围。使用最终digest对应的已验证包，不做验收后重建、签名或重新压缩。
- **测试**：AE1–AE8；用户档跨候选读取；坏候选退回上一可验证包；缺凭据、缺required检查和调包时拒绝。
- **验收**：所有required项真实PASS，故障/skip/未测没有藏入百分比；余项列具体外部条件；产物与报告可复查。输出“该范围候选通过/未通过”，不自动发布，也不恢复Studio。

---

## 8. 风险、回退与执行时待决事项

| 风险/待决项 | 在何处解决 | 处理原则 |
|---|---|---|
| 静态缺陷在真实路径不复现 | U3/U4/U5等任务开始时 | 确认路径与观测量；反证后关闭修复，不强行重构 |
| sf和解锁的历史语义不统一 | U11合入前 | 字段归属表与特征测试先行；破坏兼容时显式版本迁移 |
| 取消回调、waitIdle重入的现有调用者假设 | U5/U6接口变更前 | 所有实现/调用者同步；保留一次终态、不允许无限排空 |
| SDK许可、设备和账号未就绪 | U24–U26 | 自动部分先做；仅对应验证NOT_RUN，单列获取条件 |
| sanitizer与第三方/GPU驱动不兼容 | U2 | 保留具体诊断；缩小到可验证的第一方目标，不能静默排除整个业务模块 |
| GPU图像比较受字体/驱动影响 | U16 | 固定字体/场景/分辨率，先确定容差；同时检查actual backend |
| 性能噪声或缺旧二进制 | U27 | 不给确定改善结论；使用固定机器和可构建base重测 |
| 工作量因真实缺陷扩大 | 每个迭代出口 | 先交付已完成U，再追加有证据的新U；不重新编号旧U |
| 多分支同时修改组合根/注册文件 | 每次派发 | 明确文件所有者；接口先合，注册/CI由集成人串行 |
| 存档/归档格式回退 | U4/U7/U8/U11 | 保留原始档、格式说明与旧样本；不对用户数据批量不可逆迁移 |

默认配置、支持范围或兼容策略发生改变时，在对应U的实际实现之前更新该处决策和验收例子。无需设备才能回答的问题不留到设备到位后才处理。

---

## 9. 完成判据与下一轮入口

每个U以实现差异、真实测试和证据共同完成；状态在提交/执行记录中追踪，本文不塞入持续变化的勾选进度。旧t编号和round不作为依赖。新增任务继续分配新U编号，不能重新编号已有项。

本轮底层收敛完成意味着：

- 所选发布范围的真实执行、状态恢复、资源生命周期和最终包全部满足required检查。
- 验证系统在受控执行器及指定CI信任边界内拒绝失败、缺测、错版本、调包和仅靠自报字段制造的通过；不宣称能识别已攻陷runner或恶意测试实现。
- 运行/平台/包声明都具有可复查来源，未测项有明确条件。
- 性能及长跑拥有固定方法和可重复样本，没有未解释的资源增长或挂死。
- Studio仍暂停；后续是否恢复、扩展哪个底层能力，以新的用户指令和已测缺口决定。

计划入口为 docs/plans/README.md。历史计划仅用于背景与证据追溯，不提供当前优先级或执行授权。

---

## 10. 研究依据

- 当前源码：src/entry/Engine.cpp、src/resource/AsyncLoader.cpp、src/job/JobSystem.cpp、src/storage/SaveManager.cpp、src/storage/ISaveProvider.cpp、src/archive/CARCReader.cpp。
- 脚本与Web：scripts/kag_runner.lua、scripts/kag/commands/save.lua、scripts/kag/snapshot.lua、scripts/kag/compiler.lua、scripts/flow.lua；web/bridge.js、web/audio-engine.js。
- 验证与发布：scripts/verify_release_candidate.py、tests/CMakeLists.txt、.github/workflows/ci.yml、release.yml、deploy-web.yml。
- 经验：docs/solutions/architecture-patterns/gpu-api-guard-before-bgfx-init.md、docs/solutions/runtime-crashes/bgfx-noop-renderer-platform-gap.md。采用其中可由现有代码验证的教训，不继承旧排期。
- 既有设计观察：docs/design/save-security-audit.md。provider加密行为以当前组合根回归为准。
- 独立历史CI：[run 33891363233](https://github.com/ailiasdesu/Caesura-AmeKAG/actions/runs/33891363233)。只能证明该run实际执行的检查。
- 外部接口依据已在关键决策中链接：SDL3成功/失败语义与OpenSSL认证失败边界；其余方案主要基于仓内实现。
