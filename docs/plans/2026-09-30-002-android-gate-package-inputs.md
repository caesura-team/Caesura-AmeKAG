# Android 必需门禁与最终包输入、观测修复

状态：实现、定向证据与增量独立审查已完成，审查没有剩余可行动问题；新候选真实托管执行待完成。完整目标仍为 U1–U29；底层优先、Studio 暂停。本文不提供合并、发布或标签变更授权。

## 原因与最终行为

当前计划 U24 要求稳定编译后取消容错，并分别验证 APK/AAB 结构、测试签名和 zipalign。3a3、5b9、b7 三次实际 Android 作业完成了 arm64 编译与 Gradle 打包；这构成提升为必需检查的工程依据，不代表统计可靠性或设备运行证明。

原 Android CMake 作业使用 `continue-on-error: true`，且未加入发布输入策略的 required jobs。仅移除容错仍可能让聚合门禁忽略它。本次同时将作业改为 gate，在默认策略及策略生成器加入 `android-compile`。必需作业由 9 项变为 10 项；现有 11 个 artifact 角色和 45 个发布输出不变。失败、取消、跳过、超时、缺失、名称错误和不唯一的 Android 必需作业均不能通过聚合输入校验。

新的 `scripts/ci_android_package.py` 消费同一作业已编译的 JNI，复用既有 Android driver 的 staging、受控 Gradle 属性、独立 TEST 签名及 `android_package_contract` v2 验证器。Gradle 同一次调用产生 Debug、Release APK 和 AAB；最终 APK/AAB 必须满足完整业务条目、JNI、签名前后内容闭包与证书合同。上传前重新检查原件和小型 proof 副本的摘要。失败保留原件，仅清理本轮私有签名材料。

普通应用的默认版本不变；CI 从 CMake 提取版本并使用既有五项 Gradle 属性。完整离线 driver 的 `offline=True` 默认不变，CI 显式选择在线依赖解析。此 adapter 不重编 JNI，也不把调用方的编译日志伪装成自身的原生编译来源证明。安装、真机运行与正式商店签名仍为 NOT_RUN。

Bundletool 使用官方 `1.17.1` release 的 asset `181329951`，实际下载 32456876 字节，SHA256 `45881ead13388872d82c4255b195488b7fc33f2cac5a9a977b0afc5e92367592`。该旧 asset metadata 未提供 digest；这是官方 HTTPS 字节的计算锁，不是发布方签名。

## AppImage 确定失败与修复

b7 托管运行 `36685995730/attempt1` 的 Linux Release 在 runtime 下载后严格摘要检查失败。原 URL 指向移动的 `continuous`，旧记录锁定的 asset `456065460` 现返回 404；失败日志没有上传实际错误 runtime 字节，不推测其摘要。

替换为官方日期版 `20251108`：release `260789861`、asset `326011592`、上游 commit `dd6cebedcbddde9c82f89b011e8e1d40b6e43868`，实际文件 944632 字节，SHA256 `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`。URL 和下载／打包两处摘要同步变更；appimagetool 1.9.1、严格 checksum 及失败退出保持不变。GitHub 标示该 release `immutable: false`，因此只称日期来源与摘要锁定，不称平台强制不可变或已验证发布方签名。

新增回归先在 continuous URL 上实际失败，再在固定来源上通过；完整 workflow 套件 27/27，退出 0。下载文件未在本机执行，新 runtime 的 Linux TGZ/AppImage 实际兼容性仍须新 CI 证明。

## Windows 模块路径观测的真实边界

在独立隐藏 helper 中调用当前生产 observer，目标为恢复根内复制并锁定摘要的系统 DLL。稳定加载时观察到 28 个路径；真实 Enum 返回后，控制器通过 IPC 指示 helper 执行 `FreeLibrary`，返回 1，并以 `GetModuleHandleW` 确认卸载。随后对旧 HMODULE 的真实 `GetModuleFileNameExW` 返回 0、lastError 6（ERROR_INVALID_HANDLE），触发原生产错误。helper PID／creation／exe 保持一致，重新加载后的正控再次得到 28 个路径；helper 和 owned 控制器均正常退出 0、清理完整。

wrapper 只安排真实 API 调用间的同步边界并记录原返回值，没有伪造 WinAPI 结果。该实验证明现有一次快照处理会在真实 DLL 卸载边界失败；历史 b7 原件缺少 lastError 和 handle，不能把实验原因强加给历史运行。对应微软 API 文档也明确模块列表变化可能导致失败：[GetModuleFileNameExW](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmodulefilenameexw)。

原件为 `u29-windows-module-observation-diagnosis-01/evidence-summary.json`，完整 API／IPC 在 `inner-report.json`；历史 Windows 包失败与聚合缺输入仍分别保留在 `u29-b7b-hosted-ci-01/windows-release-producer-readback-01/failure-analysis.json` 和 `terminal-review-01/terminal-review.json`。维护回归的首轮 RED 为 5 项中 2 个失败、3 个错误，原件保留；实现后同一轮 5/5 GREEN。修复只对已证实的 Windows 模块路径错误 6 进入恢复，在原 deadline 内最多三次观察尝试；每次重新枚举，不合并部分路径，并重新核对 PID、创建时间和程序身份。连续失效耗尽、永久权限拒绝、路径截断、进程身份变化和已读到的包外同名 DLL 均继续拒绝，失败记录包含 API、handle、返回长度、lastError 和已读路径。

初版完整 83 项虽通过，独审仍发现恢复期间的 pending 分支未计入三次上限。新增真实反例安排首次 Error6、两次缺 DLL、第 4 次才加载，旧实现错误通过；修正后于第 3 次拒绝并保留 NOT_VERIFIED／PENDING／PENDING。首次失效前及非 Windows 的既有 readiness 等待不变。该回归 RED 1 项失败、GREEN 1 项通过，修正后完整直接受影响套件 84/84，0 failed、0 skipped。初版源码、83 项日志和独审 P2 均保留，没有改记历史。

额外包装层套件首次 21/22：负控依赖干净检出中不存在的 `artifacts/validation` 父目录，提前抛 FileNotFoundError，尚未检查拒绝行为。测试改用必然存在的 `tests/` 父目录下独有子路径，并断言前后都没有创建子目录；生产拒绝规则未改，完整包装层复验 22/22。此修正只恢复真实测试前提，不放宽异常或接受仓库内输出。

最新实现与维护测试原件为 `u29-windows-module-observation-tdd-01/handoff02.json`、`green-wrapper02/execution.json`。这些证据不替代新候选实际最终包验证；旧 b7 FAIL 保持。

## 已有证据与限制

| 范围 | 实际结果 |
|---|---|
| 初始 Android required-job 回归 | RED 6 项/13 个失败断言；GREEN 6/6；五套 Python 123/123 |
| 严格 CI adapter 回归 | RED 5 项退出 1；最终定向 GREEN 5/5 |
| 严格包改动七套完整测试 | 209 项，208 通过、1 失败；原件保留 |
| 唯一计数断言修正后 workflow 全套 | 26/26；其余六模块 183 项的生产源码未变 |
| AppImage 改动后 workflow 全套 | 27/27；先前 183 项适用证据复用 |

原 209 项完整执行的失败是新增 proof upload 后旧测试仍预期 11 个诊断上传，实际为 12。只修正该断言后重跑受影响全套；不能把原完整执行改记为全绿。首轮 adapter GREEN 的 Git launcher 夹具选择失败也保留。测试计数不等于覆盖率；未测覆盖率，未安装或运行 actionlint。实际 YAML 合同测试已运行。

b7 的本地完整 Release 有独立证据：C++ 1553、Lua 147＋57、CTest 71 通过及 1 项预允许 AI 跳过。5b9 的本地完整 Debug 属于其自身 SHA。b7 托管终态为 9 成功、3 失败：Linux Release、Windows Release 和聚合 gate 失败；它不是通过候选。Windows 完整 Release 构建成功。最终 ZIP 的 `created_game_frames` 正常完成 60 帧，native／launcher 退出 0、清理完整，但 `GetModuleFileNameExW` 模块路径观测失败，required `SDL3.dll` 来源保持 `NOT_VERIFIED`，验证器因此拒绝。原件没有失败句柄、返回长度或 Win32 错误码，不能认定是退出竞态或某个 DLL 卸载，更不能把正常退出改记为来源验证通过。正在建立有界模块变化复现。

旧性能基线 b008 的新完整 Release 已通过严格原件验证：C++ 1415/427077 断言，Lua 147＋56，CTest 48 发现、47 通过、1 项预允许 AI 跳过，0 失败。该证据只属于固定旧基线，不迁移到新候选，也未给出 CPU 性能结论。

## 原件入口与后续

所有本轮原件位于恢复根 `E:/CaesuraRecovery/20260924-1446`，D 盘原项目不变。

- Android hardgate：`u24-android-required-tdd-01/handoff.json`、`u24-android-required-review-01/review-01.json`。
- 严格包集成：`u24-android-package-ci-tdd-01/handoff.json`，含九份冻结源码和各次原始日志。
- AppImage 官方来源：`u29-appimage-runtime-pin-investigation-01/investigation-report.json`；回归 `u29-appimage-runtime-pin-tdd-01/green.json`。
- b7 原失败与终态：`u29-b7b-hosted-ci-01/observation-04/summary.json`、`linux-release-producer-readback-01/`。
- 旧基线：`u27-baseline2-release-rebuild-01/release-verification-02/report.json`。

Android/AppImage 独立审查报告为 `u24-android-appimage-independent-01/review-01.json`，锁定九份当前源码，当前适用测试证据为 183＋27 项。Windows 观察修复与包装测试前提的三文件增量独审也已通过，报告 `u29-windows-module-observation-independent-01/review-02.json`；原 P2 发现和修正证据保留。提交实现后，以单独 docs-only 提交同步平台矩阵代码锚，提交后运行标准新鲜度检查，不使用 `--head` 绕过。新候选实际 Android 签名／v2、各平台最终包、AE3 热冷与 Web 新进程恢复、Linux sanitizer、固定性能比较、当前长跑、设备／账号条件及 U28/U29 全部验收仍需继续。
