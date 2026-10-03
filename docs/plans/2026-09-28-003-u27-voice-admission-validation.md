# U27 语音接纳观测修复与 8d3 候选终态

完整目标仍为 U1–U29，底层优先、Studio 暂停。语音探针修复提交为 `7b5657670d94ac10bf1dba1aa85994fbeb95d3be`；Android 依赖归档路径修正为 `c6cf2e1074b0b5ca832fc3b6985f49419a71a58f`。本记录不宣告 U27/U29 完成。原始证据均保留在 `E:/CaesuraRecovery/20260924-1446`（下文为相对路径）。

## 8d3 完整门禁与长跑的不同结论

文档候选 `8d3dea6b71bd5067351f44c060d3bbe6c7f4bf6e` 的 Foundation Windows Release 完整 11 项检查实际退出 0：C++ 1549/1549、453533 断言、零失败零跳过；Lua 147/147 + 57/57；CTest 72 项发现、71 通过、1 个预准可选 AI 跳过，1235.39 秒。原运行 `u27-8d3-release-gate-01/release-raw-01/run.json` 的 SHA256 为 `44e1609dc18ca3f1d26d03a40603eb7d1ce2105d9cf3e47fb240e13a8fe8f76f`；严格复核 `release-verification-02/report.json` 的 SHA256 为 `811ed5b4927f214c71b4e36d17ec66901d076349f8d29e05e95bc4c1e6641480`。前次整理拒绝保留，只修正收据整理，没有重跑原生门禁。

该候选的诊断、冷启动、三项故障对照和短跑均完成；短跑有 200 个测量循环、152.9464467 秒、2 个上下文及 720 张 PNG。随后长跑在 epoch 20（第 21 个上下文）的第 88 轮活动接纳处失败，原生实际退出 1，受控清理 COMPLETE，无超时、强杀或停止请求。成功上下文 0–19 只证明 2000 个测量循环、1647.0876191 秒；失败上下文另外完成 87 轮、其中测量 67 轮，不能把它追加为成功的整体验收。

- 长跑外层：`u27-8d3-soak-01/long-launch-01/report.json`，SHA256 `6aafded3d7e0b0172fd81f1d14833c9f81b7fa5b53fcc58326bb5bb64686fe2f`。
- 失败上下文：`u27-8d3-soak-01/long-01/probe/epoch-20/result.json`，SHA256 `943269e6fd2611d16de37d45c1b9b0ba25f771fc4d150e08c3f34e45c139e05b`。
- 本次失败为 `Real async/voice admissions failed`；不是内存预算判定。没有获得完整一小时、全局内存或全部 PNG 验收。

## 已定位的断言与真实机制对照

原始 stdout 证明本轮 async request #784 和三个非零且互异句柄 `1081348/1085445/1089540` 已成功返回。原断言最后要求 `isVoicePlaying()` 为真；失败快照有一次后端完成待交付。句柄数组只在断言之后保存，导致失败报告错误地保留上一轮句柄。本次先保存本轮诊断，再检查接纳。

实际短 WAV 为 3840 帧、48000 Hz、单声道 16-bit，即 80 ms。ManualMix 对照使用相同 WAV 和生产 SoLoud 库：8192 帧真实混音在 0.3077 ms 墙钟时间内产生 7666 个非零样本，状态从 true/0 转为 false/1；公开 stop 仍为 false/0，缺文件句柄为 0。`u27-voice-admission-control-01/manual-01/report.json` 的 SHA256 为 `2c3bf569a79e92c8c9f49eb09f77ec4f778a4f3b5c4616a2bfeacedfd3133767`。

WinMM 有界对照的 64 次首次查询全部为 true/0；等待 20 ms 后，21 次查询在少于源 80 ms 时长的墙钟间隔内为 false/1。`device-01/report.json` 的 SHA256 为 `69926d9f9e6285e0d78ecc01258bbf7b3b484e715fa8c377bc152092f0fe2935`。这证明播放状态不是接纳成功的持久条件，但没有重现原失败约 2.46 ms 窗口中的精确线程交错，也不能仅凭完成计数把原事件的停止原因确定为 EOF。物理可听性未测。

## 严格接纳合同与实际红绿

接纳前必须观察到后端与宿主完成追踪均受支持，后端 pending、宿主 pending/active/owner refs 均为 0。不得通过消费或清空通知满足前提。随后要求 async ID 为正、三个本轮句柄非零且三者互异，并且只接受以下两种状态：

| 正在播放 | 待交付完成数 | 判定 |
|---|---:|---|
| true | 0 | 接纳且仍在播放 |
| false | 1 | 接纳且已完成，等待真实宿主交付 |
| false | 0 | 拒绝 |
| true | 1 | 拒绝旧通知或来源歧义 |
| 任意 | 大于 1 | 拒绝额外完成 |

phase 7–9 原文未变：继续要求恰好一次异步完成与一次自然语音回调、8 个取消接纳、无取消回调及连续静止点。原 20 轮预热、80 ms 工作负载、长跑时长与 64 MiB 内存增长预算均未变。trace 验证器独立校验观测值和布尔/整数类型，不信任报告中的 accepted 标志。

四个维护 C++ 回归使用实际 SoLoud ManualMix 和 PCM，涵盖 EOF、stop、缺文件、真实构造的 true/1 和 false/2、异步 ID 和三对句柄唯一性。初次外部 RED 同时暴露测试前提错误：生产 `getLength("voice")` 返回流播放时间，不是源总时长；原结果保留为未通过。v02 改为同一 WAV 的实际 `SoLoud::Wav.load()` 与解码时长检查，产品 API 未改。外部控制器的 doctest XML 字段解析错误也单独修正，原报告不覆盖。

| 同一 v02 测试 / 同一生产静态库 | 实际退出 | 测试 / 断言 | 证据 SHA256 |
|---|---:|---|---|
| 旧 playing-only RED | 1 | 4 用例中 2 失败；108 断言中仅 after/repeated/stale 三项按预期失败 | `f1dec1068ff1c30878136f1f6c94bb5f00ea55b72de11737fef4aad31734b61b` |
| 严格两状态 GREEN | 0 | 4 用例、108 断言全通过，零跳过 | `66a67fcd92d91106935e45795b9dad96bc652b2482d02ce2faf9dcdefd51035f` |

对应 `u27-voice-admission-regression-01/red-02/report.json` 与 `green-01/report.json`；受控进程清理完成，源码、测试输入与生产库首尾稳定。合入测试 SHA256 为 `4c2684b8423156739ca87e37afdcceabb083bd5684878f646dccaf044bc418de`。Python trace 原 43 项加 6 项新增回归共 49/49 通过；其余五文件在 v02 未变，故没有重复执行同一套 Python。独立实现审查 `u27-voice-admission-implementation-review-01/review-02.json`（SHA256 `d528eae5355958c9efa770701ae3993fe1c80661de720a95f6b48de0b982d3d7`）未发现剩余阻断。这些定向结果不替代新候选的完整 Debug/Release 和真实新探针长跑。

## 8d3 托管与移动端边界

[CI run 36418012531 attempt 1](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/36418012531) 的全部 12 个作业成功。聚合原件为 `DRY_RUN_INPUTS_VERIFIED` / `INPUTS_VERIFIED`、`release_ready=false`，绑定 9 个 required jobs、11 个 artifact roles 和 45 个 producer outputs。原聚合 artifact `10970524320` 的 1,409,267 字节已下载、SHA256 `81bf1ce17bb3b2a84ab46c3d82681d783f2f8d7b13bc4c97082a8486b9314158` 与上传日志及 API 对上；本地包中仅含聚合证据，不含全部大 ZIP/执行归档，不能称本地重新验证了所有最终包字节。独立聚合复核 `u29-8d3-aggregate-independent-01/review-01.json`（SHA256 `af3cb64d7ee3bbb137d5715a85afc2f303903afd8b9918c72fb95e26fcceace1`）未发现限定证据链矛盾。Web 最终产物本地下载在 900 秒达到截止时间，仅留 34,783,232/40,866,517 字节，仍未准入。

Android 实际完成 arm64-v8a/API24 Release 原生编译和 Gradle Debug/Release/AAB、APK zipalign 与签名验证；没有执行 C++ 测试或设备安装/升级/IME/生命周期。依赖 slices 上传使用了 literal `$RUNNER_TEMP`，实际没有上传且 warn 掩盖该缺失。本次仅把两个路径改为 `${{ runner.temp }}` 并对缺文件报错，保留 audit job 既有 optional 属性；修正后的托管上传尚待验证。iOS 证据仅为 iPhoneOS 26.5/arm64 的真实编译链接，不证明最低部署版本、Simulator/实机、签名或 TestFlight。

## 后续仍需完成

新候选完整 Windows Debug/Release、托管 CI、匹配探针的前置对照与一小时长跑继续执行。正式 CPU 原结果保持 INCONCLUSIVE；本机 Linux sanitizer 原失败和磁盘前提、U24/U25 设备/配额、U26 其余 SDK/账号/物理音频、AE3 最终包作者场景及跨候选坏包拒绝/旧包回退、U28/U29 其余验收继续开放。master 最小保护设置已按先前批准执行；单次 UAC 跟踪已结束，均不重复。本记录不授权合并、发布或标签变更。
