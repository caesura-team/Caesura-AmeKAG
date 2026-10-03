# U27 内存观测顺序修复与有界诊断

本记录接续 `7c23b22a0d0019bbce97490e339b12150ae674a8`。完整目标仍为 U1–U29；本次仅修正 soak 探针已复现的观测顺序缺陷，不宣告 U27 或 U29 完成。所有原始证据保存在用户指定的 `E:/CaesuraRecovery/20260924-1446`（下文称恢复根目录）。

## 原长跑结论保持 FAIL

`u27-7c23-foundation-01/long-01/run.json`（SHA256 `68085c5f08090c563593fd6dc745db2ebb0093fd025ecd96ed0073057a500b1a`）记录约 3647.76 秒、5200 个测量循环、52 个上下文。原生进程退出 0，验证器因初始预热基线上的私有提交量超限退出 1。

原固定基线为 383,823,872 字节，预算仍为 67,108,864 字节；epoch 1/cycle 53 报告 462,561,280 字节，超过阈值 11,628,544 字节。没有修改预算、预热、静默点或时长要求；失败后的图片未完成全部解码，不能报告完整图像验收。

## 单次授权的分配栈诊断

用户明确批准一次有界 UAC 跟踪，已消费该授权。专用实例 `CaesuraU27Alloc7c23_01` 由本轮 helper 启停，普通权限运行引擎探针。记录 268.6750551 秒，ETL 为 152,567,808 字节，丢失事件和缓冲区均为 0。停止后读回 `WPR is not recording`；没有再次发起跟踪。

- ETL：`u27-7c23-allocation-stack-01/allocations.etl`，SHA256 `63335733bcf76d4362ed0cf6b972494b278905d1fb3e059f3094497b629a8616`。
- 根复核：同目录 `diagnostic-review-01.json`，SHA256 `71187c618253431880ee91b7a8507b286e2e13b217c3229839e320c57a0f53e6`。
- 原生 PID 34684、创建标识 `134350648149908751` 与 ETL 单一生命周期对应；普通进程正常退出，所有权清理完成。
- 原 Release 没有 PDB。离线重链接只用于生成 MAP；原二进制未替换或运行离线副本。`.text` 与 `.pdata` 的字节、RVA 及 ImageBase 匹配，可映射 COFF 符号，不提供源代码行或内联栈保证。
- 栈和源代码提供字体准备、字形图集反复分配的线索。累计分配量不是保留量；进程退出时 outstanding 为零不是静默点无泄漏证明。未采集 VirtualFree 栈，不能把释放事件直接归因于 Lua GC。
- 本次有界诊断没有复现原一小时峰值，因此不能完整归因原 FAIL。

## 真实回归与最小修复

原 `observe()` 在 full Lua GC 前读取 Windows 私有提交量，在 GC 后读取 Lua 字节数；Lua userdata 的 `__gc` 可以释放原生内存，因此两组值可能来自清理前后不同状态。

外部对照调用实际探针 `observe(Engine&)`，使用真实 Lua VM、80 MiB 的 `VirtualAlloc` 块及 `__gc` 中的 `VirtualFree`；Engine 使用 Null 后端，证据范围不含 GPU。旧实现和修复实现使用字节相同的对照源文件，保留引用的第二个 80 MiB 块作为负控制。

| 对照 | 实际退出 | 不可达块：报告 / 调用后私有提交量 | 保留块仍超过原 64 MiB 预算 |
|---|---:|---:|---|
| 旧实现 RED | 1 | 87,134,208 / 3,084,288 字节 | 是 |
| 修复实现 GREEN | 0 | 3,112,960 / 3,112,960 字节 | 是 |

RED 收据 `u27-gc-observation-control-01/run-01/report.json` 的 SHA256 为 `445ae66a90ff09ae930ceaab1ba397067ac77848b8dd5d1ae5dbb9e749f8b48b`；GREEN 收据 `u27-gc-observation-control-02/run-01/report.json` 的 SHA256 为 `677184916598e9be0b1067e1f6afc8ca2f2ae1276023a4fa5d38a953f781b2c4`。两者输入稳定、受控进程清理完成。

修复把 full GC 提到 Lua/Windows 内存计数与运行时债务快照之前。它们仍是连续读取，不是全进程原子快照，也不代表 GPU 自动静默。生产引擎、增长预算和 soak 验收规则未修改。

维护中的 Windows 回归 `tests/cpp/test_soak_memory_observation.cpp` 已注册到 `CaesuraTests`，使用相同真实分配/释放机制，并验证释放计数、实际区域状态及保留块。单独编译运行该原始测试源码得到 1/1 用例、20/20 断言、零失败、零跳过。相关脚本：contract 14/14、trace 43/43、driver 18/18；注册检查为 205 Lua + 96 C++ 文件全部登记，注册数不是测试覆盖率。

定向证据 `u27-maintained-gc-regression-01/run-01/report.json` 的 SHA256 为 `07ab071008bd164135e1dc829836b6fac668d8dc718947c629e2cc76721d5c46`。本次四文件补丁的独立只读审查未发现可行动问题；实际 `CaesuraTests` 整体编译链接与完整门禁仍需执行。

## 当前验收边界

父提交 7c23 的 Foundation Release 完整门禁已通过：C++ 1548/1548、453513 条断言、零失败零跳过；Lua 147/147 + 57/57；CTest 71 通过、1 个预先允许的可选 AI 跳过。严格 verifier 的 manifest SHA256 为 `b1a8908f40a1dc04f2bc1d9261daeb664b034983215dbd4bc0b7be21b8a2f409`。该证据先于本次修复，不能冒充新候选完整通过。

新候选完整 Debug/Release、匹配新探针的诊断/故障/冷启动/短跑/一小时长跑以及正式 CPU 基线比较继续执行。原 CPU 噪声结论保持 INCONCLUSIVE。U24/U25 的设备及配额条件、U26 其余 SDK/账号范围、U28 声明与 U29 最终候选交付继续保持各自未完成项；本记录不授权合并、发布或标签变更。

## 908e 完整 Debug 与文档同步续记

修复提交为 `908e661c91d4e2eae5bdce22a45c601760d97284`。该干净提交在独立 Foundation OFF 构建目录完成完整 Windows Debug：11 项 required 检查全部实际退出 0；C++ 1549/1549、453544 条断言、零失败零跳过；Lua 147/147 + 57/57；CTest 72 项发现、71 通过、1 个预准 AI 跳过、零失败，总时长 1219.69 秒。源码及夹具身份稳定，受控进程清理完成。

- 原运行：`u27-908e-observation-gates-01/debug-raw-01/run.json`，run ID `0e5b8030-434c-4d60-82e8-bb3fa8bfe7cd`，SHA256 `2c6665a0833889405859807699cc236fd04118382c787f536b3d77e741d7ecb5`。
- 严格 manifest：`u27-908e-observation-gates-01/debug-bundle-02/908e661c91d4e2eae5bdce22a45c601760d97284/0e5b8030-434c-4d60-82e8-bb3fa8bfe7cd/windows-debug/manifest.json`，SHA256 `970a95dc1b8a00a4d4761151fd82ee4217fb1718870d8372a1d55658434d0cf0`。
- 首次 collector 调用因输出目录缺少 `<source_sha>/<run_id>/<profile_name>` 后缀而拒绝；原失败保留。只修正外层整理目录，使用同一原始执行收据通过严格 verifier，没有重复构建或测试，也没有使用 diagnostic 放宽。

该提交的托管 CI `36415081654/attempt1` 已主动取消：本机按同一 CI 命令实际发现平台同步锚点仍为 e7e9b1a，且旧计划自动事实块闭合计数过期。托管终态为 4 个作业成功、7 个取消、聚合门禁失败；没有候选通过或最终包准入结论。原始 API、作业日志、取消原因及本地失败见 `u29-908e-hosted-ci-01/terminal-review-01/report.json`，SHA256 `4a0a9f9660d96f7d391f6a23a5619875e76bbe7665b9efddace5ac77381fdb2d`。

随后仅文档同步至 908e 代码锚：YAML 和生成页保留原平台逐项状态、执行提交、日期及证据；历史计划只更新自动事实块，不恢复旧排期。新的文档提交仍需自己的托管门禁和 Release/最终包绑定，908e 本机通过不被改写为别的提交通过。旧 e51 四条加密配对已接受，可作为新候选受损后回退测试的已知旧包；选择收据 `u29-908e-prior-package-selection-01/selection-01.json` 的 SHA256 为 `efb605a19d2e7231be514810b6cdd294f8675ea890bba46bffa11695585a12af`，独审无阻断项，未来实际回退尚未执行。原 f87 回退 RED 继续保留。
