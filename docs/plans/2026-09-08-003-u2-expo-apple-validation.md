# U2：通过 Expo EAS Workflows 验证 Apple 原生目标

日期：2026-09-08。该记录落实用户“U2 的 iOS 和 macOS 测试用 Expo 来做”的要求，是 [运行时可靠性与交付闭环计划](2026-09-05-001-refactor-runtime-foundation-plan.md) 的 U2 执行接入，不替代 U1–U29，也不恢复 Studio 开发。

当前状态：工作流已真实执行。第三次EAS run已证明macOS完整原生profile和iOS arm64未签名编译通过；iOS Simulator在Metal链接处失败，修复准备后仍需真实重跑。本记录不把接入完成或两个lane通过等同于U2完成。U25的完整iOS app、UIKit生命周期、真实Metal/音频、真机及签名交付合同仍需单独完成。

## 运行合同

`.eas/workflows/u2-apple-validation.yml` 使用三个独立的 `custom` / `macos-medium` job。上传到 EAS 的只是仓库 allowlist 中的工作流、驱动及项目元数据。驱动从固定公开地址 `https://github.com/caesura-team/Caesura-AmeKAG.git` 取得输入指定的完整 40 位 commit SHA，在新的 detached clean checkout 中构建。没有 React Native 展示壳，也没有用 JavaScript 测试替代 C++ 引擎测试。

| Job | 实际入口及通过条件 | 不代表的能力 |
|---|---|---|
| `macos_debug` | `macos-foundation` CMake preset；原仓库 `run_validation.py --profile-name macos-debug`；原 collector 和 strict verifier 都必须通过 | 真实显示/GPU、音频、macOS 签名/公证与可分发包 |
| `ios_device_compile` | `iphoneos`、arm64、Debug，构建 SDL3/OpenSSL 和引擎完整目标图；`file`、`lipo`、`vtool` 确认两个原生产物为 IOS/arm64 | 任何设备运行；签名关闭，不产生真机/TestFlight 通过结论 |
| `ios_simulator_cpp` | 单独构建 `iphonesimulator`、arm64、Debug；创建本次 job 专属 iPhone 模拟器；确认实际 CWD 与夹具可读，再 `simctl spawn` 执行未过滤 `CaesuraTests --no-colors` | 完整 iOS app/UIKit 启动、交互、Metal、音频；iOS Lua 与 CTest 整体套件；物理 iPhone/iPad |

macOS profile 的用例数、最低发现门槛和允许的可选跳过继续来自原 `scripts/validation_profiles.json`，驱动不重写 profile。`run_validation.py` 拒绝把交叉编译目录声明为主机原生测试，所以 iOS 的编译/模拟器证据单独记录，不能拿 `macos-debug` 的 native receipt 声称 iOS 全绿。

iOS Simulator C++ 使用原 profile 的 `macos-debug/cpp` 最低发现门槛作为保守检查，读取当前源码中的值并记录 profile SHA256；实际数量来自同一源码 collector 的 doctest 解析器，必须退出 0、发现数达到门槛、失败 0、跳过 0，且运行前后 binary SHA256 相同。该门槛的来源被明确标记，不伪称已有独立完整 iOS profile。若平台条件编译、模拟器 API、夹具映射或运行入口仍有问题，job 失败并保存具体错误；不会退回“编译通过”覆盖运行失败。

## 源码、依赖与运行目录

上传工作流/驱动的 SHA256 与实际下载源码 SHA 分别记录在 `eas-execution.json`。操作者需要同时保存 EAS run/job URL 和上传源码版本，不能仅凭工件内自报的 provider 字段证明其云端来源。

- 原生源码：固定仓库、完整 SHA，拒绝分支名和非 SHA 输入。保留完整 Git 历史（只过滤未使用的 blobs，不做 shallow clone），让平台矩阵可定位文档提交之前的实际 evidence head；存在 `.gitmodules` 时递归初始化。执行前后用原仓库函数核对 Git SHA、dirty 状态、源码内容 fingerprint 以及 fixture fingerprint。
- macOS：`build/presets/macos-foundation`，`Debug`；Lua 使用该构建生成的真实解释器，Python 使用独立 Python 3.12 venv，PyYAML 固定为 `6.0.2`。Homebrew、Xcode、SDK、Clang、CMake 实际版本记录在日志中。
- iOS SDL3：沿用现有 CI 的 `release-3.2.4`，固定 commit `b5c3eab6b447111d3c7879bb547b80fb4abd9063`，分别针对 device/simulator 构建静态库。
- iOS OpenSSL：沿用现有 CI 的 `3.3.2`，固定 commit `fb7fab9fa6f4869eaa8fbb97e0d593159f03ffe4`。device 使用 `ios64-xcrun`；simulator 使用上游该版本明确提供的 `iossimulator-arm64-xcrun`。
- iOS 构建目录为 `build/eas-ios-device` 与 `build/eas-ios-simulator`，SDK/架构明确传给 CMake，不复用主机 macOS binary。
- 三个 lane 都显式关闭本接入未提供 SDK 的 Live2D、FFmpeg、Steam；macOS 沿用 foundation preset，iOS 使用同样边界。真实后端验证不能由本次禁用配置推定。
- 构建后按已核验 Mach-O 的实际目标目录，显式调用原 `tests/SyncTestAssets.cmake` 同步夹具，避免 Xcode 生成路径中未展开的变量。驱动读取原脚本的完整夹具清单，并核对源与副本的目录/文件 inventory 散列。模拟器启动使用本次新建 UDID 的真实 `dataPath`：仅向这个专属目录复制夹具，不覆盖已有目录；先执行真正的 IOSSIMULATOR/arm64 CWD 探针，核对 `getcwd()` 和资源可读性，再运行原 C++ 测试。这是 EAS 运行适配，未修改生产 Xcode 夹具生成规则。
- 模拟器 UDID、runtime、device type、测试 CWD 和 Mach-O platform/architecture 留在证据内；只关停/删除本 job 新建的专属 UDID。命令超时由已有 `validation_process.py` 管理其进程树，模拟器另在结束处理里关停，避免遗留通过 CoreSimulator 启动的测试进程。

## 工件与失败处理

每个 lane 在上传项目下独占 `artifacts/eas/<lane>/`；目录已存在则拒绝覆盖。`evidence/` 是唯一上传的工件范围，构建 checkout 和依赖缓存不上传。EAS `eas/upload_artifact` 使用 `type: other` 且 `if: ${{ always() }}`，失败也上传已经落盘的内容。

工件内容：

- `eas-execution.json`：逐条真实 argv/CWD、开始/结束时间、退出码、原始 stdout/stderr 路径及 SHA256；不作为发布批准或通用 release receipt。
- 每个命令自己的 `*.stdout.log`、`*.stderr.log`，包括依赖构建、CMake、SDK inventory、模拟器诊断与真实测试输出。
- macOS 的 `native-run/run.json`、原始日志、CTest XML，以及 collector 生成的 `native-evidence/<sourceSHA>/<runID>/macos-debug/`。`sourceSHA` 与 `runID` 直接读取原 executor 的 receipt，满足 collector 规定的身份路径；verifier 指向同一目录。collector/verifier 即使遇到失败也保留诊断，原始失败状态不被隐藏。
- iOS 的 `native-products.tar.gz` 包含本次构建的 `.app`/测试产物及目标目录内夹具；`CMakeCache.txt` 和可用的 `CMakeConfigureLog.yaml` 在失败路径同样保留。
- Simulator 成功得到 doctest 总结后保存真实 counts、门槛来源、binary 前后散列及 `simulator-discovery-profile.json`。若启动失败或没有完整总结，job 直接失败，原始退出码和诊断仍然存在。

没有 `continue-on-error` 或忽略失败的最终成功路径。`ios_device_compile` 的成功状态只表示它明确命名的编译检查成功。其他层级在记录中保持 `unverified`，不会因该 job 绿色而改变平台支持矩阵。

## 执行方式

根目录 `app.json` / `eas.json` 定义主代理已绑定的 Expo 项目与 CLI 版本；账户/云项目管理不属于驱动。待验证的完整 SHA 必须已推送到固定公开源、能由 `git fetch` 获取。检查当前工作流上传内容后运行：

```sh
eas workflow:run .eas/workflows/u2-apple-validation.yml -F source_sha=<完整40位提交SHA>
```

EAS 提供机器/调度/工件保存，不代替原生测试的通过条件。云端首次执行后，应追加实际 run/job URL、对应 source SHA、各 lane 状态、macOS profile 真实 counts、Simulator 实际发现数或失败入口。没有这些回执前不得写“Apple 测试已通过”。工作流运行不执行发布、签名、App Store/TestFlight 上传。

## 已完成的本地工具检查

1. 已按 Expo 技能通过 fetch 脚本取得当前官方 schema、syntax 与 pre-packaged jobs 文档。工作流 `1856` bytes，小于官方 `16 KiB` 上限。
2. 已真实运行技能的原版 `validate.js`。首次在编译官方 schema 阶段因 Ajv `strictTypes` 报错：当前官方 schema 含合法的多类型 `type` 数组，而技能未设置 `allowUnionTypes`。随后仅在该次 Node 运行中给 Ajv 加 `allowUnionTypes=true`，原 `validate.js`、当前 schema 和字段校验保持原样，得到 `✓ .eas/workflows/u2-apple-validation.yml`、exit 0。未修改安装的技能文件。
3. 初始 `python scripts/eas/test_run_apple_validation.py` 的7个工具回归通过；覆盖真实非零进程退出/原始日志、失败回执落盘、实际超时，以及不合法SHA、dirty checkout、错误Mach-O平台和错误架构的拒绝。第四次云执行后追加资源同步与模拟器路径回归，现为12/12；这些是驱动工具测试，不是Apple引擎测试。
4. Python 语法编译与 Git whitespace 检查通过。本地没有启动 Apple/其他原生引擎构建，也没有把 Windows 执行结果当 Apple 证明。

## 首次云执行与输入传递修正

已创建并绑定`@ailiasdesus-team/caesura-native-validation`。首次运行[01a08072-84a2-7638-85e7-f3efe8f6fc33](https://expo.dev/accounts/ailiasdesus-team/projects/caesura-native-validation/workflows/01a08072-84a2-7638-85e7-f3efe8f6fc33)上传22.4KB编排归档，明确请求源码`9e9cfc07c4455b498ea99d465cc6f560bf1eed32`。

三个job均在构建前的SHA验证阶段失败；原生验证、iOS编译和模拟器测试尚未执行。CLI原始日志显示没有收到有效40位`CAESURA_SOURCE_SHA`。工作流现改为官方文档指定的job级`env`传递手动输入，并在run_name显示源码SHA；保持相同原生检查和失败条件。修正版经相同官方schema严格字段校验通过，下一次云运行需证明输入确已到达。首次失败记录保留，不被后续结果覆盖。

第二次运行[01a08082-9af3-7b68-99d0-57bfcb3037b6](https://expo.dev/accounts/ailiasdesus-team/projects/caesura-native-validation/workflows/01a08082-9af3-7b68-99d0-57bfcb3037b6)已从job环境取得正确SHA，完成clone/fetch/checkout和clean校验，随后三个lane在Homebrew安装处失败：自动镜像中没有sdl3配方。现固定官方列出的`macos-tahoe-26.5-xcode-26.6`镜像；macOS安装本机SDL/OpenSSL，iOS只安装必要主机工具，继续从固定提交构建目标SDL/OpenSSL。新运行继续验证同一原生源码SHA，旧失败及原始日志保留。

## 第三次云执行与 Simulator 链接修复

[第三次EAS运行01a0808e-5336-786d-8364-c7a01bb36a00](https://expo.dev/accounts/ailiasdesus-team/projects/caesura-native-validation/workflows/01a0808e-5336-786d-8364-c7a01bb36a00)继续验证源码`9e9cfc07c4455b498ea99d465cc6f560bf1eed32`。三个真实工件已下载并检查路径安全后解包，保存在本地`artifacts/validation/eas-third-results/`，原始archive未改写。

- macOS：完整`macos-debug` profile PASS，原生run ID `029c7068-4f92-4585-9edc-19c9fd8d5af8`；C++1281/1281、Lua147/44、Python17/6/53/57、耦合、注册、CTest25通过/0失败/1预声明外部AI跳过；executor、collector、严格verifier均退出0，源码clean及fingerprint前后相同。
- iOS device：SDL/OpenSSL及引擎、CaesuraTests完整目标构建成功；确认IOS/arm64、Debug、未签名，源码前后clean。它是编译证明，未运行真机。
- iOS Simulator：交叉依赖和配置完成，实际引擎链接exit65。`libbgfx.a(renderer_mtl.o)`引用的`_MTLIOErrorDomain`、`_MTLTensorDomain`在该Simulator SDK中不存在；完整构建未完成，故没有Simulator C++通过结论。

最小修复把vendored Metal-cpp头中这两个ErrorDomain改用该头已有的动态弱符号宏；有真实符号时读取它，缺失时为nullptr，不关闭Metal、不自行定义替代常量。Apple官方Metal-cpp说明的ErrorDomain弱链接合同提供依据；当前bgfx上游尚未对这两处采用该宏，因此这是本仓库修复，不标为已合入上游。

同一次构建还发现OpenSSL对象默认使用SDK26.5最低版本而引擎实际最低版本为14.0。驱动现将SDL、引擎、CWD探针和OpenSSL的显式target统一到iOS14.0，并检查实际Mach-O minos。Windows Clang18提取真实vendor宏的IR对照确认两个强外部引用消失而dlsym保留；驱动7项检查和minos正负控制通过。这些本地检查不能代替Apple SDK链接与Simulator复验，下一次EAS改为新的修复源码SHA。

## 第四次云执行与模拟器资源目录修复

[第四次EAS运行01a080c6-6939-7d45-a905-8b721b130764](https://expo.dev/accounts/ailiasdesus-team/projects/caesura-native-validation/workflows/01a080c6-6939-7d45-a905-8b721b130764)验证源码`e56e55a182781be8094fc03f4d5d6ee7dff7f234`。macOS完整profile再次通过：C++1281/1281、Lua147/44、Python17/6/53/57、CTest25通过及唯一预声明AI跳过；源/夹具稳定，云端executor/collector/verifier及下载后的本地strict verifier均退出0。

iOS device和Simulator的完整编译/链接均成功；独立解析工件中的两目标Mach-O，分别为arm64/IOS和arm64/IOSSIMULATOR，minos均14.0.0，原始binary SHA与receipt一致。两lane各1093条OpenSSL编译命令带显式target14.0，旧的最低系统版本警告均为0。Metal弱符号修复在此次实际Simulator链接中生效，但不表示已验证Metal像素。

Simulator在原生CWD探针退出3处失败，C++测试尚未启动。证据显示生成的sync脚本保留字面`Debug${EFFECTIVE_PLATFORM_NAME}`，真实目标目录没有四个夹具树；`simctl spawn`又使用新建模拟器的data目录，不继承宿主subprocess CWD。驱动现按上述实际产物/专属dataPath同步，所有复制命令和源/副本散列写入receipt，保留原生探针及全量C++出口条件。

五项新适配回归先RED；修复后完整12/12 GREEN，包含真实CMake生成路径复现与复制、缺夹具拒绝、错误UDID、已有目标保护、无关模拟器内容保留。原始RUN04、工件散列审查及RED/GREEN在`artifacts/validation/eas-fourth-*`；第四次仍为FAIL，修复后的Simulator C++实际执行待下一次云端验证。下一次保持相同原生源码SHA，上传驱动的独立摘要另行记录，以区分原生源码和编排适配身份。

## 第五次云执行与测试入口修正

[第五次EAS运行01a080e8-8333-73e5-abe7-ba28dc310f59](https://expo.dev/accounts/ailiasdesus-team/projects/caesura-native-validation/workflows/01a080e8-8333-73e5-abe7-ba28dc310f59)仍验证原生源码`e56e55a182781be8094fc03f4d5d6ee7dff7f234`，上传编排来自`509f3cb8`。macOS再次通过完整profile及strict核验。iOS device的42条命令全部退出0，真实IOS/arm64/minos14.0.0产物与receipt一致，包内四组夹具共327文件/34目录，内容散列与源完全一致。

Simulator的CWD探针、资源部署及真实C++启动成功；源夹具、实际目标包与专属设备data目录副本的inventory散列均为`986c8037…`，二进制前后摘要一致。完整1281项C++实际执行结果为**1273通过、8失败、0跳过**，61条命令中仅`simulator-cpp`退出1；该次运行保持FAIL。上传helper/workflow的实际字节散列精确匹配提交内容的CRLF表示，原LF与CRLF散列及对应关系均保留，未泛化忽略内容差异。

七个失败发生在SDL事件初始化。独立控制台doctest入口没有经过SDL平台main包装；固定SDL3.2.4的iOS初始化先检查MainIsReady。测试入口现使用显式标准main，在原doctest Context运行前调用官方`SDL_SetMainReady()`，并保留CRT/主线程setup及命令行参数。七项原断言保持必跑，仅补充SDL错误文字；没有关闭iOS事件检查。

另一个失败是Live2D路径测试假定临时目录一定在CWD外，实际Simulator临时目录位于data CWD中。测试现建立明确的model-root与外部sibling，并分别验证真实越界symlink拒绝、root内symlink接受。使用原生产`PathConfinement.cpp`和真实testcase的MSVC组件回归，原前提在TMP位于CWD内时先RED；更新后1用例、8断言全部通过，两条symlink实际执行。test_async与test_live2d声明数分别保持22和27，注册审计通过。

本次只修改测试入口/夹具/诊断，生产runtime及CMake未改。完整报告及原始工件在`artifacts/validation/eas-fifth-*`。Windows组件通过不能替代Apple验收；下一次EAS必须使用包含测试修正的新源码SHA，再执行完整三lane。

## 第六次完整云验收

[第六次EAS运行01a0810e-bf2f-7590-8999-c1bf4fc87005](https://expo.dev/accounts/ailiasdesus-team/projects/caesura-native-validation/workflows/01a0810e-bf2f-7590-8999-c1bf4fc87005)的三个job全部成功，统一原生源码为`74c07ea5ac5d618652ed1b072519b2899b044529`。原始工件已下载；125条命令的stdout/stderr散列、实际二进制、源/夹具及执行结果逐项核对。

- macOS完整Debug profile：C++1281/1281、0失败/0跳过；Lua主147/147、孤立44/44；Python17/6/53/57，CTest25通过、仅唯一预声明外部AI smoke跳过。云端executor/collector/verifier和下载后的strict verifier均退出0。
- iOS Simulator：1281项未过滤C++用例与398972条断言全部通过，0失败/0跳过，exit0；七项SDL事件回归和Live2D symlink用例仍在完整套件中，前次8项失败均已消除。原生CWD探针命中本次新UDID的data目录，模拟器关停与删除成功。
- iOS device：完整目标编译通过，两个真实Mach-O均为arm64/IOS/minos14.0.0。Simulator对应产物为arm64/IOSSIMULATOR/minos14.0.0。二进制散列与工件一致，运行前后稳定；OpenSSL较新最低OS警告为0。

源夹具、实际目标目录、归档及模拟器副本共327文件/34目录，inventory散列一致。完整核验报告在`artifacts/validation/eas-sixth-report.md`及`eas-sixth-evidence-audit.json`，前五次失败原样保留。驱动的12项工具回归随后注册进Windows Debug和macOS的GitHub CI，以持续校验编排；此CI接线不改变上述已验证的原生源码或驱动行为。

本次已完成用户指定的Expo三lane接入与实际验收。iOS验证范围为未签名device编译和Simulator原生C++，macOS为上述foundation profile；UIKit完整应用、真实Metal/音频效果、iOS Lua/CTest整套、物理设备、签名交付，以及U2其余配置/sanitizer/性能基线仍按唯一计划分别推进。

## 官方来源

- [EAS Workflows 当前 JSON Schema](https://api.expo.dev/v2/workflows/schema)：API envelope 的 `data` 才是 JSON Schema。
- [Expo Workflows syntax](https://github.com/expo/expo/blob/main/docs/pages/eas/workflows/syntax.mdx)：custom Mac workers、手动输入、checkout、失败工件上传及 16 KiB 限制。
- [Expo pre-packaged jobs](https://github.com/expo/expo/blob/main/docs/pages/eas/workflows/pre-packaged-jobs.mdx)：用于区分本次 custom jobs 与标准 Expo app build jobs。
- [OpenSSL 3.3.2 iOS targets](https://github.com/openssl/openssl/blob/openssl-3.3.2/Configurations/15-ios.conf)：device/simulator 编译目标的直接来源。
- [Apple Metal-cpp](https://developer.apple.com/metal/cpp/)：ErrorDomain弱链接及缺失符号为nullptr的约定。
- [SDL_SetMainReady](https://wiki.libsdl.org/SDL3/SDL_SetMainReady) 与 [固定SDL3.2.4初始化源码](https://github.com/libsdl-org/SDL/blob/b5c3eab6b447111d3c7879bb547b80fb4abd9063/src/SDL.c)：自有main的ready接线及iOS初始化守卫。
