# GitHub CI 接续：真实浏览器基准与验证前提

用户已授权将全部改动推送 GitHub，并在 CI 全绿后合并 master。PR 为 [#27](https://github.com/caesura-team/Caesura-AmeKAG/pull/27)。本记录提交时托管复验尚待完成；最终结果以该 PR 最新 head 的检查和合并记录为准。

首轮 [CI 37109375408](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/37109375408) 在 `4e095830` 上保留了三个失败：

1. macOS 完整 C++ 1601/1601 通过，CTest 的 cleanup-failure 反证未先建立可信 terminal 前提，合法的 0.4 秒进度超时可能抢先成为首错。测试现复用真实 child/launcher/cleanup 与已有 publication barrier，再让观察器跨原期限并注入清理返回失败；原期限及失败断言不变。本地完整24项通过，独立审查通过。
2. Linux 原生及完整 Web 通过，平台矩阵生成器拒绝旧代码锚点。只同步 review anchor 和生成文档，保留每个平台原有执行提交、日期和 NOT_REVERIFIED 范围。
3. Windows 严格原生验证通过，Web 主故事吞吐约0.0615 tokens/ms，低于原0.08。首轮其他结果不被后续修复覆盖。

## 故事吞吐的测量宿主修正

独立Windows对照中，Node timeout16、interval首触发、原始jsdom rAF的中位等待分别约30.802、30.826、30.871 ms；150次等待本身约4.29秒，已经超过339 token在原门槛下的总预算。真实Chrome的150次未替换rAF对照约1.08秒，中位回调间隔5.6ms；不将原本假设的16.7ms当作实测值，也不据此认定唯一系统设置原因。

保留同名必跑故事用例、同一源码/资源、一次预热和三次完整采样、原0.8 ticks/ms与0.08 tokens/ms门槛，将测量移入真实Chromium的生产Wasmoon/bridge/DomRenderer。所有平台使用同一路径；缺少已有浏览器或Python即失败，无jsdom回退。未替换rAF、未改浏览器时钟、未安装新依赖或更改系统计时器。

原jsdom基线的音频能力不可用。真实浏览器默认有音频，直接运行会正常返回WAIT_AUDIO，且后续audio-tick会改变原单次pump计数。因此基准通过公开audioContext参数注入创建后已关闭的真实AudioContext，并逐轮检查同一实例、closed状态和真实公开availability=false，保持原基线前提。没有修改场景/音频命令、合成恢复结果或伪造计数；这不是音频正向或完整有声故事吞吐证明。

每个实测样本保持DONE:339:193、5826个调度tick、150次真实呈现。本地定向中位2640ms，约0.128 tokens/ms。新的39项报告/路径反证与完整Web复验均通过；完整结果为57文件、711测试通过、0失败。旧Windows Node/jsdom失败仍保留，本次是测量宿主修正，不是宣称产品代码获得同等倍数提速。

浏览器使用独有profile/CDP和Vite缓存；HTTP路径受限，允许Vite等待尚未发布的自有cache依赖但拒绝越界及链接逃逸。源码manifest包含实际Wasmoon JavaScript/WASM。Python薄入口复用维护中的Windows Job/POSIX进程组所有权；报告要求真实浏览器/launcher退出、endpoint退休、源码稳定、无fallback及完整owner清理，不只接受摘要PASS。

原件留在恢复根的 `ci-pr27-macos-small-01`、`ci-pr27-windows-first-failure.log`、`ci-pr27-targeted-preflight-02`、`ci-web-throughput-readonly-01`、`ci-pr27-real-browser-story-01..04`、`ci-pr27-web-full-green-01`。初次浏览器bootstrap、WAIT_AUDIO等失败也保留；没有重写成通过。

## Linux Vite 路径复验

第二轮 CI `37114535328` 的 macOS Debug 已通过；Linux 的新浏览器故事测试在 bootstrap 阶段失败。Vite 在 POSIX 上通过 `posix.join('/@fs/', absoluteId)` 生成 `/@fs/tmp/...`，其 `fsPathFromId` 会恢复前导斜杠。协调器此前直接对截取后路径调用 `resolve`，误将 Linux 绝对路径解释为仓库相对路径。现按 Vite 规则解码，保留 Windows 绝对盘符和独占缓存的词法、真实路径边界；未扩大任意文件访问范围。

路径夹具改用 Vite 实际的 URL 生成方式，报告新增源码清单摘要校验，失败 stderr 保留有界原始诊断，成功日志记录三个真实样本。最新本地检查为46项通过；真实浏览器故事三个样本均为 DONE:339:193、5826 ticks、150次呈现，中位2875.8ms，原阈值通过，源码稳定且进程树完整清理。原件为 `ci-pr27-real-browser-story-05`。这些是定向验证，最终托管全量结果仍须以最新提交 CI 为准。

## 字体重复解码与身份故障注入修复

第二轮 Windows 的真实浏览器故事仍未通过原吞吐门槛：三个样本5007.7/5062.9/4694.8ms，中位约0.0677 tokens/ms，要求0.08。没有降低阈值或改变故事工作量。只读观测排除了每帧双rAF和文字DOM重建：本地每个测量样本确有150次呈现，绘制列表均无文字；DOM处理约50–58ms，帧等待约765–772ms。剩余耗时不能直接称为Lua CPU时间。

资源计时显示每个样本重复读取相同约16MiB字体6–7次。随后Chrome CPU采样在原一次预热及三次样本内记录FontFace构造自耗4264.309ms，确认重复字体解码是热点。修复只保留当前活动字体的已解码资源与自有字节；每次准备仍重新读取并验证资产，只有loaded且逐字节一致才复用，尺寸由绘制状态持有。票据持有精确资源，切换、丢弃、清理及销毁不依赖后来活动字体；同face应用不会删除自身，外部移除仍可重新加入。没有URL缓存掩盖资产变化或缺失。

原7项字体测试保留，新增7项验证字节变化、缺失、票据交错、外部字体移除、应用失败、清理和销毁。修改前14项中6项失败，修改后14项全过，独立审查通过。真实故事样本1848.3/1705.8/1739.8ms，中位约0.195 tokens/ms，仍为DONE:339:193、5826 ticks、150次真实呈现，原门槛不变。证据为恢复根的 `ci-pr27-story-timing-01`、`ci-pr27-story-cpu-01`、`ci-pr27-font-reuse-red-01`、`ci-pr27-font-reuse-green-01`。

第三轮Linux在CTest中暴露了身份损坏测试的故障注入竞态：存在性探测认为文件未发布后，实际读取恰好成功，从而未注入非法JSON。生产代码的JSON解码错误不会重试。测试现先保留真实读取的缺失/共享错误，再对成功读取的精确目标注入损坏内容；同时固定原预检查边界，防止再次漏注入。原单次拒绝、监控未执行和完整进程清理断言不变。本地完整native package runtime套件84项通过，证据为 `ci-pr27-linux-third-ctest` 和 `ci-pr27-native-identity-green-01`。

两项修复后的完整Web复验为57文件、725/725通过，源码稳定、实际退出0且进程树清理完成。原件为恢复根的 ci-pr27-web-full-font-green-01；最终合并仍以最新head的托管CI通过为前提。

## 托管复验与浏览器协议就绪

后续托管Linux完整Web 725/725通过，故事中位3753.8ms；Windows Debug也通过了严格原生、编辑器、Web和Unicode打包检查，故事样本3662.6/4244.8/3705.0ms，按原中位数规则约0.0915 tokens/ms。新鲜度检查另发现闭环矩阵的静态测试引用计数未同步，现重新生成；其输入指纹与托管结果一致，不把静态计数提升为运行证明。

再下一轮Windows在故事开始前遇到独立启动故障：DevToolsActivePort已发布，但首次GET /json/version在原1000ms请求限时内没有响应。协调器现于原60秒总deadline内等待同一浏览器的HTTP和初始page就绪；每次请求及正文读取共享不超过剩余预算的1000ms AbortSignal。仅指定临时网络错误、502/503/504和合法空page列表可等待；非法JSON、错误端口/页面身份、永久HTTP错误或子进程退出立即失败。没有浏览器重启、场景重跑或更改性能门槛，退出和进程树清理要求不变。

原一次性读取行为在新增反证中产生11项失败；实现后58项通过，实际浏览器故事中位1810.5ms且仍完成原工作量。随后新增真实loopback HTTP延迟正文测试，直接触发原生fetch/AbortSignal超时并验证同一端点后续就绪，59项全过，服务端连接及拥有进程树完整清理。独立审查通过。原件为 `ci-pr27-windows-fifth-job.log`、`ci-pr27-browser-startup-red-01`、`ci-pr27-browser-startup-green-01`、`ci-pr27-browser-startup-http-green-01`。
