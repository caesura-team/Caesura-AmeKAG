---
module: render
tags: [sanitizer, lsan, mesa, egl, llvmpipe, evidence]
problem_type: validation-diagnostics
date: 2026-09-24
---

# Mesa EGL 退出泄漏的独立复现

## 现象与结论边界

冻结候选 `c01b2aa940dfbfadd3b7089d83a268bcd6da41b1` 的完整 Linux Clang ASan/UBSan 验证实际失败。除后来已修复的 LuaManager 和 SoLoud 生命周期问题外，五个 HTTP 子进程各留下 128+56+56 字节、共 240 字节的 LSan 报告；其中一个 CTest 功能检查仍判 PASS，完整证据采集器正确拒绝其诊断。

没有链接 Caesura 或 bgfx 的单文件 EGL 控制也能复现相同 Mesa 库构建、相同三个分配调用点、相同大小的诊断。这是第三方分配的独立复现，不是引擎 sanitizer 通过，也没有修复或豁免原完整门禁。

## 复现与定位

证据保存在 `artifacts/validation/u2-lua-lifetime-worktree/artifacts/validation/u2-gpu-leak-01/`。独立控制加载 EGL，创建 OpenGL 上下文和 16×16 pbuffer，编译着色器、绘制三角形并实际读回中心红色像素。它随后删除 VAO、program、shader，检查 `GL_NO_ERROR`，解除当前上下文，销毁 context/surface，检查 `eglTerminate`、`eglReleaseThread` 和 `dlclose` 全部成功；进程最终仍因 LSan 退出 1，owned 清理完成。

实际后端为 llvmpipe（LLVM 21.1.8）、OpenGL 4.5 Core、Mesa 26.0.8-1ubuntu0.3。程序请求最低 4.3；这里没有硬件 GPU、D3D12 或物理显示设备验证。

控制源码在库卸载前保存 `/proc/self/maps`。将诊断 PC 按真实映射换算为文件偏移，再核对原引擎运行保存的映射，两次均得到：

| 分配 | libgallium 文件偏移 | 匹配 build ID 的符号结果 |
|---|---|---|
| direct 128 B | `0x656842` | `get_cpu_topology`，`src/util/u_cpu_detect.c:566` |
| indirect 56 B | `0xef1ba0` | `u_mmInit`，`src/util/u_mm.c:81` |
| indirect 56 B | `0xef1b89` | `u_mmInit`，`src/util/u_mm.c:77` |

安装库与解包调试文件的 build ID 均为 `b089ec2f62ded38e9327502246aa4ccfb7929b24`；所用可执行段的文件偏移与虚拟地址一致。调试信息来自 Ubuntu 官方同版本 dbgsym 包，仅解压到私有目录，没有安装包或修改全局驱动。精确源包摘要按官方 HTTPS 获取的 DSC 核对；没有另行验证 DSC 的 PGP 签名。源码可见静态 CPU 拓扑状态和 rtasm 执行堆，但不能仅据这些静态结构将所有泄漏统称为无害缓存。

## 留存证据

- 原引擎 PC 映射：`default-01/frame-mapping.json`，SHA256 `7198a7f1a278045d86f076cca11df88979e8b0fb7291ece448cad646fd268329`。
- 独立绘制控制：`egl-draw-control-01/review.json`，SHA256 `06202670a9e1f6249bfabb71be7ce5a46c2abce0428e474e5a9ca9ad7879a66a`；原始持久目录 `/home/ailias/.local/share/caesura-validation/egl-draw-independent-n4hzlxl4`。
- 控制实际 maps：SHA256 `999303055e2e8d4bacd45230ba5c6989ee4abded524e47d863e73f06ad66cddf`。原控制 review 未包含该引用，后续以独立补充映射报告绑定，原 review 不改写。
- 不可变补充映射：`egl-draw-control-01/mapping-supplement-01.json`，SHA256 `2a6c06fa2d05ba8a592c87a86c7d566b84b3936585454eb9fa4944fbf556eee0`；绑定原控制 review、maps、sidecar、模块、debug 与原映射，并用一次工具调用完成24个不同Mesa地址的离线符号化。
- 调试信息获取与三处符号：`symbols-02/review.json`，SHA256 `e066e8ecde0753119ae1bf6de9ecca1deaebcf51a817007497f2d079dcab331a`。
- 源包与选取文件：`mesa-source-01/review.json`，SHA256 `49c981d2cd867685a23b12c60aaed43758d7daa551e642457252bc702ecfcc78`。

独立审查重算 13 个控制输入、25 个原始引用，并重读 ELF build ID、三个 PC 映射和符号输出，支持上述有界结论。只清屏的首份 EGL 控制仅复现 128 B，不能据它推断另外两项；加入真实绘制后才获得完整三项的证据。

## 后续处理规则

2026-09-24 又对冻结候选 `e0c019359b193d9504ca695706263bb84f36dd13` 的现存 sanitizer 编辑器二进制做了一次独立两帧重放。实际 owned 进程退出1、清理完成；新观察器捕获128份实时 maps，且以原始 `run.json`、`process.json` 及前后进程身份检查绑定。新诊断仍为128+56+56字节，三处调用偏移与上表及独立EGL控制完全相同。证据目录为 `e0-probe-01`，原Linux持久目录 `e0-gpu-maps-bgpjxc_a`；review SHA256 `ad7606bdf9cdceaaf4e9312ef07916827eb7c00b602e25dd6146f7d12d5b26b9`，frame-mapping SHA256 `261abe8a3c070519cb06123616a64f05fe89320ca44998c41df5f355970ac34a`。

上述新重放不补写e0先前完整运行没有取得的进程maps。其完整profile仍为FAIL：C++1505项零失败/零跳过、实际退出0且诊断为空，CTest68通过/2失败/1既定可选AI跳过，五个图形子进程各有240字节诊断。第一方测试结果、这次定向PC归属与完整门禁结论分别保留。

新的独立复核重算11项锁输入、23项引用、31个文件及128份快照摘要，并从实际可执行映射重新反算PC，按分配大小与文件偏移逐项匹配独立EGL控制。PID487的创建身份同时对应receipt、process.json与唯一maps identity；17条观察错误仍保留，有效证据只来自前后身份检查通过的快照。`e0-probe-01/independent-review-01.json` SHA256为 `b5dbc322a2a97e1fb97e48a9ce4b295cd9a2730ce545a48a70e8296c8bec4e09`。审查未重跑引擎，原完整失败记录摘要也再次确认未变。

保留原完整 Linux profile 的失败结果和全部诊断，继续验证修复后的候选。当前计划 U2 允许在第三方/GPU 不兼容时明确限定可验证的第一方目标，但不能静默排除整个业务模块，也不能缩小旧运行的 required 范围后重新宣称通过。真实图形后端功能验证、第一方 sanitizer 结果和第三方退出诊断应分别给出结论。

符号化时优先将多个地址交给一个工具进程，避免对同一大型调试文件逐帧重复加载。本次逐帧辅助脚本在单帧解析时超时，未产出完整栈报告；该工具失败不改写已经独立核验的三处分配点。
