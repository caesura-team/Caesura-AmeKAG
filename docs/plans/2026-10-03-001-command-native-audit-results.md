# 182 个命令名的原生合同审计结果与验收边界

本轮针对合法命令经 Lua → C++ → 实际后端执行时的错位、崩溃、失效及测试假通过。当前工作树是 `codex/command-native-contract-audit`，基线 HEAD 为 `6080057a4cd51f53655800dd095aa8444bef713d`，修改未提交。原 D 盘目录未写入；源码与证据保留在 `E:/CaesuraRecovery/20260924-1446`。

## 命令名清单

145 个 schema 名称、28 个额外流程名、9 个兼容别名，共 182 个，均有选定场景的证据条目：

| 分类 | 名称数 | 含义 |
|---|---:|---|
| 应用、状态或宿主观察 | 176 | 选定参数与场景下的实际原生执行；各条证据范围独立 |
| 明确拒绝 | 5 | `blur`、`xfadebgm`、`live2d_motion`、`live2d_expression`、SDK-disabled 的 `steam_achievement` |
| 离线回退 | 1 | `ai_dialog`，不证明在线服务 |

机器清单：`command-native-audit-01/command-inventory/runtime-evidence-06.json`。保留的 63 份小型历史回执已重新核对文件摘要；新增 SMA 五命令、Live2D 五命令和 Steam 拒绝记录只各计一次。历史场景与当前增量有不同的源码/二进制身份，**不是同一最终二进制重跑全部 182 名称，也不是代码覆盖率或全部参数/平台无缺陷的证明**。Steam 正向账号操作始终为 NOT_RUN。

## 主要修复

- 转场：对齐 Lua/native 参数合同，使用真实场景快照、句柄与生命周期，处理首帧尚不可捕获及目标画面呈现；移除只靠宽松 mock 得出成功的依据。
- 渲染、输入、音视频及脚本边界：修正此前审计中确认的 blt 字段、参数别名优先级、播放/淡出生命周期、SE 混音饱和原生异常、视频实际资源入口、缓存事务与流程恢复等缺陷。详细递进证据见前一份执行记录及逐命令机器清单，不以本表代替各项实际证明。
- SMA：真实 Lua 的位置数组与 C++ 具名字段合同统一；默认 MAIN 视图、逐 draw 透明度、GPU immutable pose/output、uint16 骨骼域、尾线程、临时索引缓冲与 C++ 异常回 Lua 边界均有对应限定回归。实际五命令场景已执行 wave、IK、eyes 部件替换及 stop，原生网格数 0→5→0，逻辑纹理退役。
- Live2D：修复设备丢失后 bgfx 提前卸载 d3d11.dll、外部 COM 引用稍后释放引发的 execute AV；真实缺件复验可正常清理。验证目录补齐 SDK 两份 FrameworkShaders 后不再 HUNG。产品现在在创建模型 renderer 前检查实际全部 shader/layout，就绪失败明确拒绝并缓存失败。另将错误的 view0 呈现改为 MAIN。
- 验证入口：Golden 驱动改为明确的模拟渲染边界，命令错误不能继续报 DONE；修复声明缺失、注册缺失、Git Bash/PowerShell 前提及实际夹具问题。旧失败不覆盖，不降低断言或性能阈值。

## 本轮实际结果

| 检查 | 实际结果 |
|---|---|
| Windows Debug 固定 profile | 11 项检查退出 0；执行期间源码和夹具均未变化 |
| 完整 C++ | 1,630 passed / 0 failed / 0 skipped；460,880 assertions passed |
| Lua 主 / 隔离 | 153/153 与 60/60 |
| CTest | 74 passed / 0 failed / 1 预声明可选 AI skip，共 75 项 |
| 测试注册 | 215 Lua + 106 C++ 文件全部注册 |
| SMA 实际 Lua/native 合同 | 10 用例、273 断言；CPU Engine 像素原四项失败修复后通过 |
| SMA 实际 D3D11 opacity 子进程 | 60,526 断言通过；源 alpha 255/128 × actor opacity 0/.5/1，同帧多提交，GPU 实际 compute |
| DLL loader 子进程 | 19 断言通过；真实加载器引用与部分初始化清理 |
| Live2D 正向实际像素 | Haru 可见改变 23,483 RGB 像素，嘴型改变 73 像素；hide/unload 精确恢复基线，正常退出 |
| Live2D missing / corrupt shader | 每进程两次真实加载均拒绝，模型数零，编译失败一次，正常退出；未伪报测得 GPU draw count |
| Live2D 维护 observer | 15 项纯判据反证通过，读取正式 Host 的实际正/负原件也通过；不是重新执行引擎 |
| 编辑器受影响的纯 commandLint | 12/12，非完整 Studio 验收 |
| 完整 Web | **671 passed / 1 failed，共 672 项；56 文件中 55 通过** |

完整原生运行：`ca1-sdk/normal-profile-gates-results-02/run.json`。其前后工作树指纹均为 `0754824a04dca471b41c8b91e90f679625a4e785f85c61b65200ec2b92b3ff1c`。配置为 Debug、Live2D/FFmpeg ON、Steam/Sanitizers OFF，普通 Windows 用户目录；输出、TEMP、游戏工作目录在 E 盘。临时 AV CMake hook 已移除。

正式 Host SHA256 为 `4faf8778b54c5785063f504ae2e941f2e49d53363c3f1878d46fc689b52f4640`。最新实际场景原件：`ca1-sdk/native-sma-actor-final-01`、`ca1-sdk/native-live2d-current-final-01`、`ca1-sdk/live2d-shaderguard-controls-03`、`steam-sdk-disabled-refusal-prepared-01/case`。维护 observer 位于 `tests/scripts/verify_live2d_native_contract.py`，其默认无 GPU 判据测试已注册 CTest。

## 尚不能通过的边界

Web 唯一剩余失败是 `perf-baseline.test.js` 的主故事吞吐门槛：median 5289.7 ms，约 0.06409 tokens/ms，低于原 0.08。原件 `ca1/web-full-05` 实际退出 1。旧诊断中真实 rAF 等待占主要时间，但没有证明唯一系统原因；不降低阈值、不减少真实帧、不循环重复相同测试。因此**不能宣称全部门禁通过或允许合并/发布**。

四项已声明 unsupported 的正向功能、Steam 账号写入、在线 AI、物理音频/手柄、所有模型质量及完整跨平台均没有被此清单升级为支持。任意 GPU 设备重建的 Live2D 恢复合同仍有独立风险记录；当前正常命令场景通过不等价于该恢复链路已验。整个 U1–U29 计划不由本轮命令审计宣布完成。

本文件及索引是门禁结束后的文档收尾；未在通过的运行之后修改产品代码或测试实现。所有历史失败和限定通过原件均保留，未合并、发布或操作外部账号。
