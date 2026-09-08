# U15 截图与帧生命周期执行记录

本记录接续[唯一 U1–U29 计划](2026-09-05-001-refactor-runtime-foundation-plan.md)。U15 在 `codex/u15-gpu-lifecycle` 隔离分支实施；初始基线为 `56117a87`，实现检查点 `6fee4957` 后与主分支 `eb3aa8a8` 正常合流为 `2c059a04489672e1b7a997e85ba1061cb5834f2f`。后续保存边界、探针和文字回归仍在整合，尚未创建或合并 U15 PR。完整路线图未完成，Studio 继续暂停。

## 当前实现

- `IRenderDevice` 持有截图请求、设备代次、提交帧号、完成/失败/取消状态与独立 PNG 字节。查询 Pending 不消费，终态只领取一次；取消后仍须领取或丢弃终态。队列限制请求数、尺寸与保留字节量，回调验证格式、pitch 和可读尺寸，同帧请求共享 backbuffer readback 后分别输出所需尺寸。
- bgfx callback、设备丢失标记和截图内容归设备实例所有。未初始化、Null/headless、恢复、关闭或恢复失败时拒绝新截图。成功恢复开启新代次；关闭仍保留可用物理上下文的资源排空，恢复失败不重新开放正常绘制。
- `SaveManager` 移除 GPU 就绪静态值、bgfx 调用和固定截图文件。Lua 保存先冻结槽、状态、场景、游标和描述，再用 Operation 与 Lua 5.4 关闭守卫等待本请求图片，最后沿既有原子/加密保存路径提交一次。普通缺图可保存状态，取消或 owner 失效则不写盘。
- 主机新发起的最终状态保存保持兼容。runner 只在成功完成退役后授予私有 retained owner 资格；新截图等待共享该 owner 的取消列表，旧 scheduler 不复活。失败清理、重载、替换及旧请求不得借保留状态重新提交。
- `commit_frame` 完成场景与后处理提交，正常推进统一由 `advanceFrame` 执行；Engine 的正常循环、`renderOneFrame`、RPC 和导出共用完整绘制后的呈现边界，平台 `postFrame` 随后执行。RPC/导出消费自己的 PNG，不读取轮换文件或额外推进空帧来假定完成。
- native managed coroutine 使用真正 registry 引用。正常结束、错误和 abort 在解除引用前关闭协程；yield 只移除实际返回值，保留零值暂停下的 native 关闭守卫。

## 原始失败与修复证据

初始真实 C++ RED 使用隔离子进程调用未初始化/退出后的 renderer、headless 保存和 KAG 缩略图，并植入旧 `save_thumb.png`。4 个选中用例中 3 个失败，证明不安全 GPU 入口及旧文件消费；原始二进制和日志保留在 `artifacts/validation/u15-initial-01/`，没有通过创建 GPU 来掩盖无 GPU 用例。

整合时新增 Engine 测试暴露默认 TextureManager 把任何 editor renderer 都当作 bgfx 的假设。实际进程在 `bgfx::copy` 路径崩溃；组合根现与既有 LayerManager 工厂一致，仅对真实 BgfxRenderDevice 启用该默认纹理服务的 GPU 路径。另修复新测试的 Lua 搜索路径、必需 audio 夹具及中心最近邻采样的像素预期。

完整 Web 首轮发现 3 个既有 `saveCurrent` 用例失败：新守卫错误拒绝自然结束后保留的最终表。真实 runner 新回归先为 29 通过/3 失败，修复后 32/0；随后独立审查指出“结构上 inactive”仍可能来自失败清理。用实际 scheduler 的 Lua `<close>` 抛错建立有效 RED，35 通过/2 失败；私有成功退役标记修复后为 37/0，同时覆盖最终状态异步截图成功与替换取消。早期 CancelToken 回调被内部吞错及未进入预期 hook 的两份夹具结果被保留，不能作为该缺陷的有效原因证据。

## 已执行的本机验证

| 执行 | 实际结果 | 边界 |
|---|---|---|
| 完整 Debug 构建及 C++ preflight | 1324/1324，399871 断言，0 失败/0 跳过 | `u15-preflight-01`；后续新增 GPU 探针目标及最终 Lua 清理资格增量仍须最终冻结门禁 |
| Lua preflight | 主 147/147、孤立 46/46，0 失败 | 后续单独的清理资格回归为 37/0；不拼成最终统一 receipt |
| 原生 headless stdio | 正常返回、错误终止、stop 响应、abort 关闭四项通过 | 真正引擎进程，未创建 GPU |
| 定向 Web | 原有存档/语言/持久化与跨语言语料 77/77 | 显式指定本轮 Lua；Node 24 |
| 完整 Web，Node 22.23.2 | 505/506，1 失败、0 跳过 | 唯一失败是原合成 1000 行调度吞吐门槛；不是文字渲染 FPS |
| 真实 D3D11 截图与 Engine RPC | renderer 56 项、RPC 70 项均通过，两个进程均退出 0 | `u15-gpu-01`；独立图像复核发现既有缓存文字缺损，见下一节 |
| 冻结候选第一次原生 profile | C++ 1325/1325、399990 断言；Lua 147/147 与 46/46；前十项检查退出 0 | `u15-native-candidate-01`，完整 receipt 为 FAIL：CTest 22 通过、3 失败、1 项预先允许的 AI 服务跳过 |

第一次冻结候选为 `cc6c0121c8de34c1514a4569bd3a54c6774850bf`，run ID `6c1862c4-5620-43e4-84e2-032158a0a2bb`，执行期间源码/夹具稳定。collector 和严格 verifier 均保留 FAIL；不能把前十项通过改写为候选通过。三项 CTest 失败中，GoldenVn 与 ValidationOutputPaths 使用了本机缓存中的 WSL `System32/bash.exe`，不能解析该 Windows 中文路径；在当前隔离构建显式配置 Git Bash 后，两项定向检查通过（`u15-config-fix-01`），没有安装或修改全局 PATH。

HeadlessHttpSmoke 初始为 73/75：Node 打包脚本忽略当前构建的 Lua。CTest 现通过 `cmake -E env` 传入当前配置的 `lua_cli`，脚本把显式 `CAESURA_LUA` 作为权威选择，空/缺失/目录配置直接失败，不回退到旧 packaged/Release/PATH 解释器。真实 Node/Lua 回归先有两方法失败（五个子测试），修复后新增 3/3、完整 14/14，0 跳过，记录在 `u2-package-lua-01`。

随后实际 HTTP 为 74/75，确认打包成功但旧测试根定位只覆盖三层，漏掉更深的 preset 目录。测试改用自身源码锚点并保留独立宿主回退，每次生成 UUID 包名，避免旧成品误通过。`u15-http-lua-fix-02` 的真实 HTTP 75/75 及资源同步通过；首次 74/75 日志原样保留在 `u15-http-lua-fix-01`。这些是失败原因修复后的定向证据，下一次完整 profile 仍须绑定新冻结提交与配置。

Node 22 的合成 1000 行三样本为 2477.4/2459.9/2418.4 ms，中位数 2459.9 ms，调度吞吐 1.626085 帧/ms 未达到原大于 2 的门槛。原 12 项性能用例、预热、三样本、64 个历史点和 2000 个已读标记检查均保留。此前 U13/U14 固定配对未建立版本回退关联；本次也不宣称由 U15 或宿主环境造成。共同成本诊断列入 U2/U27，不重试挑绿、不放宽预算。

真实 GPU 探针在隐藏 SDL 窗口使用实际 Direct3D 11 和生产 Engine/renderer/PNG decoder。640×360 与 320×180 的票据同属 frame 1；显式恢复后代次从 1 变 2，TextureManager 保留的颜色纹理 ID 和 TTF 描述恢复，RTT 旧句柄失效并重新创建。恢复前后整张 PNG 的 SHA256 相同。RPC 两次分别呈现一次，返回耗时为本次实测 89/140 ms，等待期间没有额外推进帧。最后以无效新尺寸触发实际 core 重建失败，确认新截图拒绝、旧票据取消、帧入口安全返回和关闭完成。

本次 GPU probe SHA256 为 `4d6869032c2022bf2ac5dc39abc6dd5349517cbf4d8777d1f74f4d0a9a08c4fd`；原始 PNG、PID、日志、枚举输入及工件散列保留在 `artifacts/validation/u15-gpu-01/`。该报告验证的是列举的输入，不能代替完整工作树与构建配置 receipt。操作系统主动移除 GPU、core 成功后字体分配失败、Android EGL、Metal/GLES 及真实音频不在此探针的通过范围内。

## U16 接续发现与尚待证明

看图确认 `GPU 15` 字形缺损。源码的缓存文字路径为六个展开顶点建立了四顶点四边形的索引，第二个三角形退化为零面积；原几何测试还固定了错误的末项索引。最初 GPU 探针只要求白色像素存在且恢复前后相同，因此那次 PASS 证明了截图/恢复一致性，**不能证明字形完整**。

新增几何回归检查两个三角形的非零面积、绕序、完整矩形覆盖和非零追加起点。旧实现的 11 个选中用例为 9 通过/2 失败，171 条断言中 28 条失败。GPU 探针增加独立 FreeType 参考：直接读取固定字体，在 28px 下按字符 bitmap/ascender/advance 建立预期位置，不调用生产 layout/索引生成器；预期不透明像素必须全部命中白色±2，阈值在修复前固定。原生 334 个参考像素只命中 168 个；原有126项检查仍通过，新增10项全部失败。

生产修复仅将缓存几何的第二组三角形索引改为展开顶点的 3/4/5。未修改回归或阈值，之后几何 11/11、171/171 断言全部通过；真实 D3D11 renderer59/RPC77共136项全部通过，原生334/334、缩略图94/94参考像素全部命中。主代理再次看图确认字形完整，恢复前后 PNG SHA256 同为 `ebd10f535a2bbd95987ae69f397c91f395bc81bddb199e858c6f93f4d25257d9`。新 probe 二进制 SHA256 为 `d982c4da6a90b6f24185e29df63c4cca58a6979ea7677c69eb9033eaa277f62f`；原始红绿日志和图像在 `artifacts/validation/u16-{red-01,green-01,gpu-red-01,gpu-green-01}/`。本次只完成缓存字形缺损修复，其他 U16 图像能力仍须逐项验收。

2026-09-09 补充：`68a6e74db3317ea9610fa1767626cdeaceadfb35` 的完整 Debug profile 已通过，run ID `231f973e-23af-47bc-8f98-e1d3bb976ff0`。C++1325/1325、399990断言，Lua147/46，CTest25通过、1项预先允许AI服务跳过、0失败；collector与严格verifier均PASS，源码/夹具稳定。随后共同Web成本修复取得 `a699bc5c` 完整38文件511/511、原十二项性能预算与严格核验通过，原始失败及完整过程见[U27执行记录](2026-09-09-006-u27-web-cost-execution.md)。

后续U27增量的最新完整原生整合及候选CI仍须独立完成，不将旧profile改绑为新源码。其他 U16 图像能力及 U17–U29 仍按唯一计划逐项推进；本切片不替代它们的验收。
