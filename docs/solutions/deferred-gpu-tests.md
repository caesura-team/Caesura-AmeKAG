# 待实际运行检查项

这里区分可以在无 GPU 环境下验证的所有权合同与必须使用真实窗口/GPU 的呈现效果。2026-06-16 的原始记录曾把存档缩略图视为 `SaveManager` 内的 GPU 调用；该入口已移除，现行截图属于 `IRenderDevice`，不再以“先设置 GPU 就绪标志”作为测试前提。

| 范围 | 所需上下文 | 验证内容 |
|------|------------|----------|
| Renderer 截图与尺寸 | 实际 bgfx + SDL3 窗口，明确记录真实后端 | A/B 不同完整帧产生对应像素；同帧多票据、不同输出尺寸、原生尺寸固定及表面 resize；PNG IHDR 与像素/方向符合本次票据，不能只断言 Base64 非空 |
| 完整帧呈现 | 真实后端与后处理/小游戏绘制 | 场景、小游戏绘制、后处理全部发生在唯一正常帧推进之前；截图包含这些绘制，外部 surface swap 位于后端提交之后 |
| 设备恢复 | 真实设备恢复及可控失败注入 | 旧代 Pending 取消且终态可领取，迟到回调不能发布旧图；成功恢复后字体、纹理、RTT 可重新呈现；失败后禁止继续绘制/接收截图 |
| 平台 surface 生命周期 | 对应 Android EGL、Apple Metal 或其他目标设备 | 在各自实际设备验证 swap、恢复、暂停/重入等合同；Windows 结果不能代替这些平台证据 |

无需真实 GPU 即可检查的范围包括：未初始化/headless/退出后的安全拒绝、票据一次消费、取消与完成竞态、编码输入边界，以及实际 Lua SaveBinding/SaveManager 的冻结状态、取消后不写盘和旧截图文件不受影响。此类测试使用真实生产保存实现和受控 renderer 完成边界；它们不证明真实 GPU 像素或平台呈现效果。

2026-09-08 的本机记录包括完整 C++ preflight1324/1324（0失败/0跳过）、Lua147/46，以及 native headless stdio4项关闭检查。随后新增缓存文字几何回归先RED，修复后三角形检查11/11与实际D3D11 renderer59/RPC77共136项通过；独立FreeType参考在原生图命中334/334个不透明字形像素，在320×180图命中94/94个。恢复前后图像一致、截图身份、单次呈现及无效重建尺寸的安全失败均有实际记录。完整候选门禁仍待冻结执行，其他平台、其他U16图像能力与core重建成功后的字体分配失败仍未据此通过。具体源码/二进制/原始红绿证据见[U15/U16执行记录](../plans/2026-09-08-005-u15-screenshot-lifecycle-execution.md)，本页不由测试数量推断覆盖率。

当前合同见 [C++ screenshot API](../api/cpp-interfaces.md#111-irenderdevice) 与 [Lua Save](../api/lua-modules.md#save-registered-on-the-kag-module)；具体回归见 [test_save_binding.cpp](../../tests/cpp/test_save_binding.cpp) 和 [test_screenshot_lifecycle.cpp](../../tests/cpp/test_screenshot_lifecycle.cpp)。无窗口测试仍遵守 AGENTS.md 的资源约束，真实 GPU 验证必须明确其运行环境。

*原始记录日期：2026-06-16 · 当时分支 codex/archive-expanded-tests；职责与验证边界于 2026-09-08 同步。*
