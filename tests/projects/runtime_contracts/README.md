# 运行时语言等价语料

`corpus.lua` 定义六组输入与独立的终态/事件数量要求；`.ks` 是真正被执行的语料。没有提交预先填写的事件 JSON。

`tests/scripts/test_runtime_contracts.lua` 以真实 tokenizer、semantic、compiler、scheduler、kag_runner 和存档准备/恢复流程执行 source、AST 和实际磁盘 `.ksc` 读回三条路径。每条分别输入7ms和31ms帧时间，共36次运行。仅宿主绘制/存储绑定和场景内容提供器使用替身；此处缓存读回证明序列化语义，不代替 U14 的 flow 缓存新鲜度和失效验证。

`web/runtime-contracts.integration.test.js` 启动真正的 Lua 5.4 原生语料，再调用 `ks_bake.lua --web` 生成 `story.lua`，通过 Wasmoon 的实际 `createPlayer.runScene/runFromBundle` 入口运行同一组场景。bundle 不提供源文本 fallback。Web 驱动器使用其原有16ms更新；两条 Web 路径共12次运行，与原生事件逐项比较。场景、对话、分页、可见选择、变量、调用栈中的场景/局部变量和正常结束被记录；句柄、绝对路径和帧计数不进入比较。阻塞 tween 必须到达精确终点，存档重放必须等于原执行的后缀，奖励只能得到一次。

语料包括 CJK/反斜线与引号/多行文本、短路/三目/插值、静态与重定义宏、本场景及跨场景 call/return、带条件的选择、wait/tween、callee 内保存后的跨场景重放。选择使用现有 deferred-jump 合同：`endbutton` 后的 `end` 提交待选目标，再进入分支；不据此宣称无终止屏障的旧选择行为已重构。

原生输出的 `RUNTIME_CONTRACTS_JSON:` 行和 Web 的测量保存在新建的 `artifacts/validation/runtime-contracts-*` 目录，包含本次 bake、原始日志和比较结果。比较器复用 `compare_platform_parity.py` 的比较与平台值泄漏规则，并检查六组语料和必需执行路径是否齐全。六个负控制从本次真实输出复制后改变变量、对话顺序、调用帧、结束、bundle路径或重放；原数据保留。

执行：在仓库根目录运行 `build/lua/Debug/lua.exe tests/scripts/test_runtime_contracts.lua`；Web从 `web/` 执行 `npm test -- runtime-contracts.integration.test.js`。跨目录工作树可设置 `CAESURA_LUA_BIN` 指向明确的 Lua 5.4 解释器。缺解释器直接失败；不把缺依赖记成测试通过。这是原生宿主绑定替身及 jsdom/Wasmoon 的行为证据，不包含实际 GPU、设备音频或渲染截图证明。
