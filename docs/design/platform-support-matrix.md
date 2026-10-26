# Caesura (AmeKAG) 平台支持矩阵（七级阶梯）

> **2026-10-26 当前源码同步边界**：候选代码身份为 `e4e40be2f1a0bc001258d4cd30dd449e604ba5e8`（PR #27 head；其最后一个代码提交为 `0ae83992`，`e4e40be2` 只更新文档/生成状态），托管 CI [run 37195162558 attempt 1](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/37195162558) 的 12 个 job 全部成功，PR #27 合并 `b994cfe6`。逐项 job/artifact 与 NOT_RUN 条件见 [I8 候选范围验收记录](../plans/2026-10-26-002-i8-candidate-scope-acceptance.md)：该记录 `release_ready=false`，本次同步不增加平台、设备、商店或发布批准。
>
> **历史同步边界（2026-09-27）**：`a4fdaab98181aad3acaf0ab470d9208ef29c1dfd` 是当时代码审查锚；历史能力字段及 `NOT_REVERIFIED` 不变。a6已恢复门禁与加密加载RED、当前Lua修复和单文件原生诊断的范围见[U29执行记录](../plans/2026-09-24-017-foundation-integration-execution.md#2026-09-27-回放加载事务修复与已恢复终态)；新候选完整门禁/包仍待验。
>
> 029 §二 工件 B。平台表述一律走本文档七级阶梯（新纪律②）；无证据=「?」，禁止凭印象推断。
> **符号**：✓=有可复核证据；⏳=证据存在但硬件/环境受限（hardware-gated）或修复中；?=无证据（不得推测）。
> **历史 CI 绿基线**：GitHub Actions run **33245271845 @ 0fce3311**（round-7，2026-08-29 十 job 全绿=当时的新 CI 绿基线；仅代表该 run 自身范围）。
> **当前候选证据快照（U31）**：GitHub Actions run **37195162558 attempt 1 @ e4e40be2**（2026-10-04，12/12 success；合并 `b994cfe6`）。run 内六配置执行 manifest 与 7 份最终包收据为 PASS/RUNTIME_PASS；设备、窗口化 Metal、物理音频、Steam 与 hosted tag dry-run 仍为 NOT_RUN。
> 本文档与 docs/design/engine-capability-matrix.md（能力矩阵）用途不同：本表回答「平台支持到哪一级」，能力矩阵回答「引擎有哪些能力」。

## 1. 总表（七级 × 六平台）

| 级别 | Windows | Linux | macOS | Web | Android | iOS |
|---|---|---|---|---|---|---|
| **Support**（平台受支持） | ✓ [W-A] | ✓ [L-A] | ✓ [M-A] | ✓ [B-A] | ✓ [D-A] | ✓ [I-A] |
| **Build**（构建通过） | ✓ [W-B] | ✓ [L-B] | ✓ [M-B] | ✓ [B-B] | ✓ [D-B] | ✓ [I-B] |
| **Boot**（引擎启动/跑帧） | ✓ [W-C] | ✓ [L-C] | ⏳ [M-C] | ✓ [B-C] | ✓ [D-C] | ⏳ [I-C] |
| **Gameplay**（游戏可玩） | ✓ [W-D] | ✓ [L-D] | ⏳ [M-D] | ✓ [B-D] | ✓ [D-D] | ⏳ [I-D] |
| **First-VN**（官方示例完整跑） | ✓ [W-E] | ✓ [L-E] | ⏳ [M-E] | ✓ [B-E] | ✓ [D-E] | ⏳ [I-E] |
| **Package**（发布包） | ✓ [W-F] | ✓ [L-F] | ✓ [M-F] | ✓ [B-F] | ✓ [D-F] | ⏳ [I-F] |
| **Store**（商店分发） | ? [S] | ? [S] | ? [S] | ? [S] | ? [S] | ? [S] |

Android 的 [D-C/D-D/D-E] 来自 K40 历史真机记录，不代表当前候选 `e4e40be2` 已在设备运行（当前候选真机 NOT_RUN，见 §2 与 §4）。macOS [M-C/M-D/M-E] 与 iOS [I-C–I-F] 保持硬件/凭据门，不因本次托管 run 改变。

## 2. 证据注记

证据码释义（出处=CI job 名 + run id；run 未注者=同一快照 33245271845；本机/文档类证据按条目注明）：

- [S] 全部平台 Store=?：商店/正式分发管线（Steam 等）属 Sprint7，未启动——无证据不推测（Web 虽有手动 GitHub Pages 部署 deploy-web.yml，定义为手动物流，不计入 Store 级）。
- **Windows**：[W-A] CI build-windows（Debug+Release）。[W-B] 同上（Configure+Build 步骤）。[W-C] 同 job「Smoke Test」步骤。[W-D] 本地 demo/editor 运行 + doctest 385877+ 断言门禁（本机，docs/guides/getting-started.md）。[W-E] 本地 verify_first_vn.sh（docs/guides/getting-started.md 快速开始同源命令）。[W-F] Release · Package（CPack ZIP 产物上传，run 33245271845）。2026-10-04 候选 run 37195162558 的 Windows Debug 与 Release&Package 两个 job success；windows-debug/release 执行 manifest 记录 C++ 1,626/1,626、Lua 153/153 + 60/60、CTest 73 通过 + 1 项预声明 AI 跳过，ZIP 包收据 PASS/RUNTIME_PASS（U31 记录；托管范围，不含发布批准）。
- **Linux**：[L-A/B] Linux · GCC（SDL3 源码构建+Configure+Build）。[L-C] 同 job「Bundled layout boot smoke (xvfb; A3/R6 contract)」。[L-D/E/F] **Linux · Package「Verify release package (30 assertions, serial)」30/30 通过，含 renderdisabled=0（M1：Linux 发布包真渲染首证；五层链 platformDefaultBackend→BGFX_CONFIG_RENDERER_OPENGL=43→gl_FragColor substitute→-msse4.1→libegl1，见 §3）。2026-10-04 候选 run 的 Linux Debug 与 Release&Package 两个 job success；执行 manifest 记录 C++ 1,601/1,601、Lua 153/153 + 60/60、CTest 73 通过 + 1 项预声明 AI 跳过，TGZ 与 AppImage 收据 PASS/RUNTIME_PASS（U31 记录）。
- **macOS**：[M-A/B] macOS · Clang（Configure+Build+Lua+Test）。[M-C/D/E] ⏳ windowed Metal 运行 hardware-gated（本机无 Mac 桌面窗口证据；CI 无窗口桌面验证）。[M-F] macOS · Package（CPack TGZ 产物上传 ✓；verify 30/30 ✓ @ round-8 run 33248475888——round-7 §3 红定性为 verify 脚本 ps 探测误报（活进程 ps -args 空返回→活判死；无 .ips、demo 探针 rc=0、GPU degrade WARN 证明帧循环长期运行），修复=launcher 直记 pid/rc（0422c17c，§3/§4 对称；mac 实测 editor ready 3s/token 1s），自 round-9 起 verify 为硬门。2026-10-04 候选 run 的 macOS Debug 与 Release&Package 两个 job success，TGZ/DMG 包收据 PASS/RUNTIME_PASS（U31 记录）；这些 job 是 headless 构建/测试/包校验，不构成 [M-C/D/E] 窗口化 Metal 桌面证据。
- **Web**：[B-A/B/C/D/E] CI「Web Player unit & integration tests (vitest)」+ web vitest 全量（工件齐备 368/368 基线）+ web_browser_smoke.mjs（first_vn 浏览器冒烟，subpath/jsdom 等断言）+（本地 W0-W7 轮记录）。[B-F] ✓ **Linux CI 硬门**（r26 run 33704740139@a7ff1ce9：bake demo→vite build→package first_vn→`scripts/verify_web_package.sh dist/first_vn` 25/25 文件断言，vitest dist 用例实跑）+ `deploy-web.yml` 发布前 verify 25/25 + headless Chrome smoke（root + `/<repo>/` 子路径）双硬门；ks_bake --dir/--web 自 2026-09-03 起 POSIX 可移植（此前 cmd.exe `dir /s /b` 在 Linux 静默产出 0 场景）。2026-10-04 候选 run 的 Web Final Package job success，Web ZIP 与 Pages tar 收据 PASS/RUNTIME_PASS，Pages artifact 为 `PAGES_ARTIFACT_VERIFIED` 且 deployment=`NOT_RUN`（U31 记录）；历史 Chrome 根/子路径、缓存与离线记录保留独立范围。
- **Android**：[D-A/B] Android CMake probe (gate) + Android static contract (no NDK)。原 `continue-on-error` 审计已按 2026-09-30 记录改为必需门禁（与 release-input 的 required_jobs 同步）；2026-10-04 候选 run 的两个 Android job 均 success。[D-C/D/E] K40 真机记录 docs/plans/2026-08-24-028-android-full-closure.md（全链路闭环：启动、渲染、字形、分支、存档、生命周期；另见 project-memory §5.2）。**当前候选 `e4e40be2` 的真机运行 NOT_RUN**（无设备可达，U31 记录）；K40 历史证据保留但不迁移到本候选。[D-F] ✓ APK 发布链路已落地（2026-09-04 核实）：android/ gradle 工程（SDLActivity + MainActivity + HID/audio 模块 + AndroidManifest + assets 预置）已提交（feat(android) 系列：IME 输入桥、audio-focus JNI、Release 签名管线、A5 E2E 设备入口 + verification zipalign/apksigner CI step + APK/AAB upload artifacts）；引擎侧 R1 MODULE 出 libCaesuraAmeKAG.so + CMake ANDROID 分支 + OpenSSL 路径已落地。2026-10-04 候选 run 的“Build controlled unsigned packages and verify final TEST-signed bytes”与 android-package-contract-evidence 收据通过（U31 记录）；真机安装/运行证据仍只保留在 K40 历史记录。
- **iOS**：[I-A/B] iOS CMake probe (gate)（2026-09-04 起硬门：`continue-on-error` 移除，job 名 audit→gate；修复链含 SDL3-iOS、OpenSSL、CODE_SIGNING_ALLOWED=NO、BUNDLE DESTINATION、Metal shader 数组）。2026-10-04 候选 run 的 iOS CMake probe job success（含 Metal shader/fallback 校验与模块图构建），仍只是编译探针（U31 记录）。[I-C/D/E/F] ⏳ 其余全部 hardware/credential-gated（真机/模拟器未验，签名与真机部署属后续轮）。

## 3. 基线与运行时要求

- **CPU 基线（Windows/Linux）**：x86_64 + SSE4.1（commit `142d0dbe`）。MSVC 编 SIMD 内建从不设 /arch 门槛、Windows 二进制本就发射 SSE4.1；Linux GCC 硬拒（__builtin_ia32_dpps needs -msse4.1），按架构门控 `target_compile_options(bgfx PRIVATE -msse4.1)`（CMAKE_SYSTEM_PROCESSOR MATCHES x86_64；ARM 走 NEON 不受影响）。
- **Linux 运行时依赖**：libEGL.so.1（commit `0fce3311`）。vendored bgfx 在 Linux 的 GL 上下文走 glcontext_egl.cpp，运行时 dlopen('libEGL.so.1')——只装 GLX 系包（libgl1/libglx-mesa0/libgl1-mesa-dri）不够；缺 libegl1 时症状=『[bgfx WARN] Failed get egl*』连串后经空函数指针 SEGFAULT（不是干净 init 失败）。CI runner 与 Linux 发行包（AppImage/deb）都必须带 libegl1；xvfb 场景同样适用（llvmpipe 经 EGL 供上下文）。

## 4. 声明边界（诚实标注）

- **Linux**：Build/Boot/Gameplay/First-VN/Package 全 ✓ 基于 round-7（run 33245271845 @ 0fce3311）——此前 round-4 的『窗口化验证』存在 requested=Direct3D 11/actual=Noop 假绿（exit 0 从未渲染），本次是真渲染首证（renderdisabled=0）；后续 CI 红点排查以 0fce3311 为绿参照。2026-10-04 候选 run 另提供同候选的 Linux Debug/Release&Package 与包内运行校验（U31）。
- **macOS**：Package 产物 ✓ 且 verify 30/30 ✓（round-8 run 33248475888；§3 误报已修复 0422c17c，自 round-9 起 mac verify 硬门）；windowed Metal **运行**（Boot/Gameplay/First-VN）仍 hardware-gated 标注 ⏳——引擎侧 12/12 Metal Shader 与 Xcode 流程完备（探针级）。2026-10-04 候选 run 的 macOS 包收据 PASS/RUNTIME_PASS 属托管 headless 范围，不能升级窗口化 Metal 或桌面体验证据。
- **Android**：真机证据来自 K40（Redmi K40 M2012K11AC；网络 adb 192.168.8.11:5555，Magisk root）——设备档案唯一权威见 project-memory §4.2；不允许标注为其他设备。**当前候选 `e4e40be2` 真机运行 NOT_RUN**，只有编译/TEST 签名包与包契约证据（U31）；不得据 CI 绿色升级设备或正式签名声明。
- **iOS**：仅 Build 编译探针 ✓（2026-09-04 起为必需 gate；2026-10-04 候选 run 通过，仍是未签名编译探针）；其余 hardware-gated，真机/模拟器与签名交付未验。
- **当前候选（U31）**：run 37195162558 attempt 1 在 `e4e40be2` 上 12/12 success，六配置执行 manifest 与 7 份最终包收据 PASS/RUNTIME_PASS，dry-run 输入聚合为 `DRY_RUN_INPUTS_VERIFIED`/`INPUTS_VERIFIED`/`HOSTED_INPUTS_VERIFIED`；该证据范围是托管构建/测试/打包/包内运行校验，`release_ready=false`。EAS 三 lane、Android/iOS 真机、macOS 窗口化 Metal、Steam 账号、物理音频与 hosted tag dry-run 均 NOT_RUN，不得据 CI 绿色升级为设备、商店或发布声明。
- **Store** 全部 ?：Sprint7 统一推进（Steam 管线）；本矩阵不预填 ✓/⏳。
