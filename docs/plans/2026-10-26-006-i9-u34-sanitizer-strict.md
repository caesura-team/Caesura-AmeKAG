# I9 / U34：Linux sanitizer strict 复现与技术结论

日期：2026-10-26。对应[计划 §5.1 与 U34](2026-09-05-001-refactor-runtime-foundation-plan.md#54-2026-10-26-续排)。完整目标仍为 U1–U29；本记录只覆盖本次在固定源码与固定环境上的 sanitizer strict 复现，不把 FAIL 改写成 PASS，也不自行批准范围调整。

## 1. 结论

**Linux sanitizer strict 仍为 FAIL（本机完整复现，未修复）。** 失败来源是第三方 Mesa/libgallium 在真实 EGL/llvmpipe 上下文进程退出时的 240 B/3 分配 LeakSanitizer 报告；第一方 ASan/UBSan 插桩 C++ 套件零诊断。建议处置 **B：带证据的范围调整，需 G5 用户批准**；未获批准前 strict 继续记录 FAIL，不抑制、不缩减 required 集合。

## 2. 环境与源码身份

| 项目 | 实际值 |
|---|---|
| 宿主 | WSL2 Ubuntu 26.04，kernel 6.6.87.2-microsoft-standard-WSL2，x86_64 |
| 资源 | 16 vCPU / 15 GiB RAM / 约 932 GB 空闲 |
| 编译器 | clang 21.1.8（私有 root：/home/ailias/.local/share/caesura-toolchains/clang21-u2-aldfnq_g；未 sudo、未 apt 变更） |
| CMake / Python | CMake 3.28.3（Unix Makefiles）/ Python 3.14.4 |
| 图形栈 | Mesa 26.0.8-1ubuntu0.3；libgallium build-id b089ec2f62ded38e9327502246aa4ccfb7929b24；llvmpipe (LLVM 21.1.8)，OpenGL 4.5 Core |
| SDL3 | 3.4.2（系统 libsdl3-dev） |
| 源码 | 独立 clone /home/ailias/caesura-i9-sanitizer，HEAD 01c52fad9928c1ca43bcee410e719efb68515a47，git clean |
| 构建配置 | CMakePresets linux-sanitized：Debug，FFmpeg/Live2D/Steam OFF，CAESURA_REQUIRE_TEST_PREREQUISITES=ON，CAESURA_SANITIZERS=address-undefined |

## 3. 构建与测试结果

- configure：exit 0（166 s）；build exit 0（213 s）。17 个第一方 target 的 flags.make 含 -fsanitize=address,undefined；CaesuraAmeKAG 与 CaesuraTests 各含 372 个 __asan_/__ubsan_ 符号。
- C++：CaesuraTests 1,601/1,601 用例、460,583/460,583 断言通过，0 failed / 0 skipped，exit 0，零 sanitizer 诊断。
- Lua：主套件 153/153、隔离套件 60/60 通过；项目自建 Lua 解释器未插桩（0 个 sanitizer 符号），仅功能证据。
- CTest：74 发现、71 通过、2 失败、1 跳过（预先允许的 CaesuraHeadlessAiSmoke），exit 8，822.03 s。失败项为 #22 `CaesuraRpcTimeoutSmoke` 与 #23 `CaesuraManagedRpcSmoke`；功能侧检查项（own_transport_ready、timeout_envelope、stop_acknowledged 等）为 true，失败源于引擎子进程被 LSan 置为 exit 1（natural_exit=false）导致 required regex 未满足。
- 严格 gate 判定：FAIL。即使 CTest #25 的 HTTP smoke 功能 76/76 通过，其引擎子进程仍留下 LSan 诊断，不能用 CTest PASS 抵消。

## 4. LSan 诊断（5 份，全部为 Mesa EGL 退出泄漏）

| 文件 | 产生测试 | 分配 |
|---|---|---|
| sanitizer.23347 | #22 / http-queued | 128 B direct + 56 B + 56 B indirect |
| sanitizer.23419 | #22 / http-late-success | 同型 |
| sanitizer.23494 | #22 / http-late-failure | 同型 |
| sanitizer.23741 | #23 / http-run-submitted | 同型 |
| sanitizer.23863 | #25 CaesuraHeadlessHttpSmoke（mtime+启动参数推断；该测试未回显 PID） | 同型 |

合计 15 次分配 / 1,200 B。三处分配偏移精确为 `0x656842`、`0xef1ba0`、`0xef1b89`；符号化结果（匹配 build-id 的私有 dbgsym，未安装/未改全局）：get_cpu_topology（src/util/u_cpu_detect.c:566）、u_mmInit（src/util/u_mm.c:81/77）。

调用链：`--editor` 引擎子进程 → bgfx `gl::GlContext::create`（external/bgfx/bgfx/src/glcontext_egl.cpp:283）→ `Caesura::BgfxDeviceCore::init`（src/render/BgfxDeviceCore.cpp:130）→ `BgfxRenderDevice::init`（:132）→ `Engine::initPlatformPhase`（src/entry/Engine.cpp:457）→ `Engine::init`（:347）。触发脚本：tests/headless_rpc_timeout_smoke.py:52、tests/headless_managed_rpc_smoke.py:50、tests/headless_http_smoke.py:759。`--headless` stdio 路径不创建 render backend，10 个 stdio 引擎子进程 exit 0 且无诊断，可作交叉对照。

## 5. 第三方归属证据

1. 三处分配点均在 libgallium（按 build-id 与偏移匹配），项目帧只出现在调用路径，不存在从 src/、scripts/ 或 Lua 发起的分配。
2. 无 Caesura/bgfx 的独立 EGL 控制（源码 SHA256 2d43a49738d9e767fc68c62d6cd947a70f99e14111695c27b70509d953dacb5f，clang 21.1.8 -fsanitize=address,undefined）在 llvmpipe/Mesa 26.0.8 完成真实绘制、读回与完整 teardown 后仍 exit 1，得到同样的 240 B/3 与同三处偏移；历史二进制重跑同型。
3. apt-cache policy 显示 Mesa 安装版与候选均为 26.0.8-1ubuntu0.3（无更新候选），sudo 不可用，无法通过升级 Mesa 尝试 strict PASS。
4. 三种独立形式（历史二进制、新编译控制、真实引擎子进程）确定性复现，不是环境偶发（处置 C 不成立）。

## 6. 第一方与处置

- 完整插桩 C++ 套件零 LSan/ASan/UBSan 诊断；历史 LuaManager（8b1312de）与 SoLoud（8eba52fc）第一方泄漏修复未复发。本环境/源码下无第一方可归因的泄漏分配点，因此没有 src/ 级最小修复。
- A（修复/升级后 strict PASS）未完成：需要更新 Mesa（本机无候选、无 sudo）或修改引擎/测试使 `--editor` HTTP smoke 不创建真实 EGL 上下文（产品行为变更，需评估 RPC 渲染语义并全量重验）。拒绝“仅保留 EGL 句柄不卸载”的掩蔽式改动。
- B（带证据范围调整）为建议：仅豁免“真实 EGL/llvmpipe 初始化后退出”场景中 libgallium build-id b089ec2f… 的精确签名（128 direct @0x656842 + 2×56 indirect @0xef1ba0/0xef1b89，同进程同型出现），其他 LSan/ASan/UBSan 诊断仍 fatal；第一方诊断必须保持 0；CTest 功能失败仍按功能契约记录；不得使用 suppressions、detect_leaks=0 或缩减 required 集合后宣称 strict PASS。**该范围调整需 G5 用户批准，本记录不自行豁免。**
- C（NOT_MEASURED）不成立。

## 7. 原始日志与证据路径

WSL：/home/ailias/caesura-i9-sanitizer/；Windows：\\wsl.localhost\Ubuntu\home\ailias\caesura-i9-sanitizer\

- 配置/构建：logs/configure.log、logs/configure-env.log、logs/clang-version.log、logs/cmake-version.log、logs/build.log、logs/build-status.log
- 单测：logs/i9-evidence/env.log、status.log、cpp.log、lua-main.log、lua-orphan.log
- CTest：logs/i9-evidence/ctest.log、ctest-junit.xml、ctest-LastTest.log、ctest-list.txt、ctest-status.log、ctest-LastTestsFailed.log
- 引擎子进程 sanitizer：logs/i9-evidence/sanitizer-ctest/sanitizer.{23347,23419,23494,23741,23863}
- 独立 EGL 控制：logs/i9-evidence/egl-control-fresh/{egl-draw-control.c, source.sha256, compile.log, run.stdout, run.stderr, sanitizer/sanitizer.14149, symbolized-offsets.txt}；历史对照 logs/i9-evidence/egl-experiments/
- 汇总：logs/i9-evidence/summary.txt
- 过程报告（主工作区 gitignored）：tmp/i9-sanitizer-report.md

## 8. 边界

本记录绑定 01c52fad 源码与上述固定环境；不把旧 e0/c41 的 FAIL 改写成通过，也不把本次复现外推到其他 Mesa 版本或平台。CTest #22/#23 的功能判定与 sanitizer 退出码污染需在 G5 决策后按证据另行处理。未运行 TSan 或其他 profile，未重复运行追求绿色，未修改主工作区源码、未 push/merge。
