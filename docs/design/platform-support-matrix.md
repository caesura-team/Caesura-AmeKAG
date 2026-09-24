# Caesura (AmeKAG) 平台支持矩阵（七级阶梯）

> **2026-09-24 当前源码同步边界**：`a04784ea32688ebdac69753fa2660d227b52357e` 是当前源码锚；下表仍是各项注明的历史证据快照，原始能力证据为 `NOT_REVERIFIED`，本次文档同步不新增平台通过或发布批准。d61托管终态失败、HTTP本机回归、新音频未验、ed6 SDK终态与Haru原失败分别见 [U29执行记录](../plans/2026-09-24-017-foundation-integration-execution.md#2026-09-24-http-就绪与音频集成前提修复续记)。生成声明及其核验边界见 [平台状态](../status/platform-status.md)。

> 029 §二 工件 B。平台表述一律走本文档七级阶梯（新纪律②）；无证据=「?」，禁止凭印象推断。
> **符号**：✓=有可复核证据；⏳=证据存在但硬件/环境受限（hardware-gated）或修复中；?=无证据（不得推测）。
> **证据快照基线**：GitHub Actions run **33245271845 @ 0fce3311**（round-7，2026-08-29 十 job 全绿=新 CI 绿基线）。
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

## 2. 证据注记

证据码释义（出处=CI job 名 + run id；run 未注者=同一快照 33245271845；本机/文档类证据按条目注明）：

- [S] 全部平台 Store=?：商店/正式分发管线（Steam 等）属 Sprint7，未启动——无证据不推测（Web 虽有手动 GitHub Pages 部署 deploy-web.yml，定义为手动物流，不计入 Store 级）。
- **Windows**：[W-A] CI build-windows（Debug+Release）。[W-B] 同上（Configure+Build 步骤）。[W-C] 同 job「Smoke Test」步骤。[W-D] 本地 demo/editor 运行 + doctest 385877+ 断言门禁（本机，docs/guides/getting-started.md）。[W-E] 本地 verify_first_vn.sh（docs/guides/getting-started.md 快速开始同源命令）。[W-F] Release · Package（CPack ZIP 产物上传，run 33245271845）。
- **Linux**：[L-A/B] Linux · GCC（SDL3 源码构建+Configure+Build）。[L-C] 同 job「Bundled layout boot smoke (xvfb; A3/R6 contract)」。[L-D/E/F] **Linux · Package「Verify release package (30 assertions, serial)」30/30 通过，含 renderdisabled=0（M1：Linux 发布包真渲染首证；五层链 platformDefaultBackend→BGFX_CONFIG_RENDERER_OPENGL=43→gl_FragColor substitute→-msse4.1→libegl1，见 §3）。
- **macOS**：[M-A/B] macOS · Clang（Configure+Build+Lua+Test）。[M-C/D/E] ⏳ windowed Metal 运行 hardware-gated（本机无 Mac 桌面窗口证据；CI 无窗口桌面验证）。[M-F] macOS · Package（CPack TGZ 产物上传 ✓；verify 30/30 ✓ @ round-8 run 33248475888——round-7 §3 红定性为 verify 脚本 ps 探测误报（活进程 ps -args 空返回→活判死；无 .ips、demo 探针 rc=0、GPU degrade WARN 证明帧循环长期运行），修复=launcher 直记 pid/rc（0422c17c，§3/§4 对称；mac 实测 editor ready 3s/token 1s），自 round-9 起 verify 为硬门。
- **Web**：[B-A/B/C/D/E] CI「Web Player unit & integration tests (vitest)」+ web vitest 全量（工件齐备 368/368 基线）+ web_browser_smoke.mjs（first_vn 浏览器冒烟，subpath/jsdom 等断言）+（本地 W0-W7 轮记录）。[B-F] ✓ **Linux CI 硬门**（r26 run 33704740139@a7ff1ce9：bake demo→vite build→package first_vn→`scripts/verify_web_package.sh dist/first_vn` 25/25 文件断言，vitest dist 用例实跑）+ `deploy-web.yml` 发布前 verify 25/25 + headless Chrome smoke（root + `/<repo>/` 子路径）双硬门；ks_bake --dir/--web 自 2026-09-03 起 POSIX 可移植（此前 cmd.exe `dir /s /b` 在 Linux 静默产出 0 场景）。
- **Android**：[D-A/B] Android CMake probe (audit) + Android static contract (no NDK)（探针/契约级，continue-on-error 审计）。[D-C/D/E] K40 真机记录 docs/plans/2026-08-24-028-android-full-closure.md（全链路闭环：启动、渲染、字形、分支、存档、生命周期；另见 project-memory §5.2）。[D-F] ✓ APK 发布链路已落地（2026-09-04 核实）：android/ gradle 工程（SDLActivity + MainActivity + HID/audio 模块 + AndroidManifest + assets 预置）已提交（feat(android) 系列：IME 输入桥、audio-focus JNI、Release 签名管线、A5 E2E 设备入口 + verification zipalign/apksigner CI step + APK/AAB upload artifacts）；引擎侧 R1 MODULE 出 libCaesuraAmeKAG.so + CMake ANDROID 分支 + OpenSSL 路径已落地。真机安装/运行证据保留在 K40 记录（D-C/D/E）。
- **iOS**：[I-A/B] iOS CMake probe (gate)（2026-09-04 起硬门：continue-on-error 移除，job 名 audit→gate；修复链含 SDL3-iOS、OpenSSL、CODE_SIGNING_ALLOWED=NO、BUNDLE DESTINATION、Metal shader 数组）。[I-C/D/E/F] ⏳ 其余全部 hardware-gated（真机/模拟器未验，签名与真机部署属后续轮）。

## 3. 基线与运行时要求

- **CPU 基线（Windows/Linux）**：x86_64 + SSE4.1（commit `142d0dbe`）。MSVC 编 SIMD 内建从不设 /arch 门槛、Windows 二进制本就发射 SSE4.1；Linux GCC 硬拒（__builtin_ia32_dpps needs -msse4.1），按架构门控 `target_compile_options(bgfx PRIVATE -msse4.1)`（CMAKE_SYSTEM_PROCESSOR MATCHES x86_64；ARM 走 NEON 不受影响）。
- **Linux 运行时依赖**：libEGL.so.1（commit `0fce3311`）。vendored bgfx 在 Linux 的 GL 上下文走 glcontext_egl.cpp，运行时 dlopen('libEGL.so.1')——只装 GLX 系包（libgl1/libglx-mesa0/libgl1-mesa-dri）不够；缺 libegl1 时症状=『[bgfx WARN] Failed get egl*』连串后经空函数指针 SEGFAULT（不是干净 init 失败）。CI runner 与 Linux 发行包（AppImage/deb）都必须带 libegl1；xvfb 场景同样适用（llvmpipe 经 EGL 供上下文）。

## 4. 声明边界（诚实标注）

- **Linux**：Build/Boot/Gameplay/First-VN/Package 全 ✓ 基于 round-7（run 33245271845 @ 0fce3311）——此前 round-4 的『窗口化验证』存在 requested=Direct3D 11/actual=Noop 假绿（exit 0 从未渲染），本次是真渲染首证（renderdisabled=0）；后续 CI 红点排查以 0fce3311 为绿参照。
- **macOS**：Package 产物 ✓ 且 verify 30/30 ✓（round-8 run 33248475888；§3 误报已修复 0422c17c，自 round-9 起 mac verify 硬门）；windowed Metal **运行**（Boot/Gameplay/First-VN）仍 hardware-gated 标注 ⏳——引擎侧 12/12 Metal Shader 与 Xcode 流程完备（探针级）。
- **Android**：真机证据来自 K40（Redmi K40 M2012K11AC；网络 adb 192.168.8.11:5555，Magisk root）——设备档案唯一权威见 project-memory §4.2；不允许标注为其他设备。
- **iOS**：仅 Build 探针 ✓（audit 级 continue-on-error），其余 hardware-gated。
- **Store** 全部 ?：Sprint7 统一推进（Steam 管线）；本矩阵不预填 ✓/⏳。