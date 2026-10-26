# I10 外部条件平台 lane 记录（U37/U38）

日期：2026-10-26。本记录对应[当前唯一计划](2026-09-05-001-refactor-runtime-foundation-plan.md)的 [§5.1](2026-09-05-001-refactor-runtime-foundation-plan.md#51-2026-10-26-续排起点证据与下一阶段方向)与 U37/U38，并接续[2026-10-26 状态同步](2026-09-05-003-runtime-foundation-todo.md#2026-10-26-状态同步与续排)。I10 的出口要求“U37/U38 逐项 PASS 或 NOT_RUN；外部条件、账号、设备与签名范围不得因 CI 绿色升级”（计划 §9.1）。

本记录只做**只读条件核查**：不安装、不签名、不上传商店、不付费、不登录外部账号、不写设备，不 push/合并/改源码。它不授予发布、商店、签名、账号或设备操作，也不把历史 K40、旧 EAS 或旧托管包证据迁移到当前候选。

## 0. 记录身份与边界

- **候选源码身份沿用 U31**：`e4e40be2f1a0bc001258d4cd30dd449e604ba5e8`（PR #27 head；其最后一个代码提交为 `0ae83992`），与[I8 候选范围验收记录](2026-10-26-002-i8-candidate-scope-acceptance.md)一致；`release_ready=false`，U29 未完成。
- **所在 worktree**：`artifacts/validation/i10-probe-worktree`，分支 `codex/i10-platform-conditions`，探测前 HEAD `62228e56c6660dac67be76f64a36fde647f84322`，`git status --porcelain` 为空（本记录是唯一新增文件）。
- **探测主机与时间窗**：Windows NT 10.0.26200 / AMD64 工作机；本机只读探测发生在 **2026-10-26T13:29–13:33 +08:00**。本机无 Mac、无可用 Android 设备、无 Xcode/Apple 工具链。
- **只读输出归档**：命令原始输出保存在主工作区 gitignored 的 `tmp/i10-probes/`（命中 `.gitignore` 的 `tmp/` 规则，未提交、不随仓库分发），文件与 SHA256 见 §9。E 盘恢复区只读取索引/元数据与文件摘要，未复制大文件（§9）。
- **本轮未执行的动作**：`adb install`/`adb push`/`adb shell`/`am`/`logcat`/`su`/`input` 等任何设备写入或安装；EAS 派发、额度购买或 Expo 登录；Apple 签名/公证/TestFlight；Steam 登录、steamcmd 或 depot 上传；任何 GitHub 写操作（仅 `gh api`/`gh run list` 只读）；任何 `src/`、`.github/` 或其他 worktree 修改；未 push/merge/PR。
- **结论口径**：`PASS` 只用于**已有可复核执行记录**（本次仅只读回读确认，不是新执行）；未执行保持 `NOT_RUN`；无可解释测量保持 `NOT_MEASURED`。三态均不能因 CI 绿色、端点存在或客户端存在而升级。

## 1. 结论总表

| 平台/条件 | 结论 | 依据（文档 / 命令 / run id） | 未执行原因 | 下一次执行需要的具体资源 |
|---|---|---|---|---|
| Android 当前候选真机（U37） | **NOT_RUN** | 本次 `adb devices -l` 仅 offline、`adb connect` 10 s 失败（§2）；[U31 §6](2026-10-26-002-i8-candidate-scope-acceptance.md)；[2026-09-20-015](2026-09-20-015-android-package-validation-execution.md)（2026-09-20 空设备列表） | 设备不可达：历史网络地址 192.168.8.11:5555 为 offline，重新连接 10,030 ms 后失败；无 USB 设备；本记录不安装/不推送 | 一台已授权 ARM64 Android 设备（`adb devices -l` 显示 `device`）＋当前候选 TEST 签名 APK（CI artifact `android-release-artifacts` id 11300862395，或按 `e4e40be2` 本地重建）＋设备日；逐项安装/启动/CJK/IME/触摸/音频焦点/前后台/存读档/重启并记录机型、OS、SHA、APK 摘要 |
| iOS 未签名编译探针（hosted CI gate） | **PASS**（既有托管证据，本次仅只读回读） | 本次 `gh api` 只读回读 run 37195162558/attempt 1：job `111415436805`（Validate engine / iOS CMake probe (gate)）completed/success；`.github/workflows/validate-engine.yml` 的 `ios-compile` 仅在 `macos-latest` 构建未签名目标图；[U31 §1.1](2026-10-26-002-i8-candidate-scope-acceptance.md) | 非本轮新执行；该 job 是编译探针，不是设备/签名/运行证据 | 无需新增资源即可维持该 PASS；若要对新候选复验，需触发同一 gate 的 CI（本记录不 push） |
| iOS 真机/模拟器运行与签名、TestFlight | **NOT_RUN** | 本机无 Xcode/Apple 凭据/EAS CLI（§3）；EAS 第七次与 b533 均在 worker 分配前被额度拒绝（[2026-09-08-003](2026-09-08-003-u2-expo-apple-validation.md)、[b533 §EAS](2026-10-01-004-b533-final-package-validation.md#eas-当前额度条件三-lane-未执行)）；[平台矩阵 §2/§4](../design/platform-support-matrix.md) | 硬件/凭据/额度门：Free CI/CD 60 分钟额度耗尽（最近报告重置 2026-11-01T00:00Z，非容量保证）；未升级付费、未重试、未登录 | Apple 硬件 + Xcode + Apple Developer Program 证书/描述文件/设备 UDID；或 G4 批准的 EAS 额度/付费与 Expo 账号；模拟器/真机运行、签名与 TestFlight 属 G3 范围 |
| macOS 窗口化 Metal（Boot/Gameplay/First-VN） | **NOT_RUN** | 本机无 Mac；候选 run 的 macOS Debug/Release&Package 是 hosted `macos-15` 无窗口作业；包运行时步骤带 `--audio-output software`（`.github/workflows/validate-engine.yml`）；[平台矩阵 §2/§4](../design/platform-support-matrix.md) 的 [M-C/D/E] 仍 hardware-gated | 无 Mac 桌面/显示会话，托管 runner 无窗口 GPU 会话；本记录不启动远程 Mac | Mac（Xcode、Metal GPU、物理显示）上的窗口化 boot/gameplay/first-VN 运行，含截图、日志、帧时间与后端确认；或经批准的外部硬件实验室 |
| macOS 托管 Debug 与 Release&Package | **PASS**（既有托管证据，本次仅只读回读） | 本次只读回读 job `111415436779`（macOS · Clang · Debug）、`111419237701`（macOS · Clang · Release and Package）均 success；包收据与执行 manifest 见 [U31 §2–§3](2026-10-26-002-i8-candidate-scope-acceptance.md) | 非本轮新执行；属 headless 构建/测试/包校验范围 | 无需新增资源即可维持该 PASS；窗口化 Metal 仍按上一行单独 NOT_RUN |
| Steam 账号写入、成就/统计、云冲突、depot 发布 | **NOT_RUN** | 本机检查：Steam 客户端默认目录存在、账号配置标记存在（仅存在性，未读内容），steam 进程未运行，无 `STEAM_*` 环境凭据，steamcmd 未安装；仓库无 `steam_appid.txt`、无 depot 凭据（仅占位模板 `scripts/steam/depot_build_windows_template.vdf`）；[2026-09-20-016](2026-09-20-016-optional-sdk-cloud-boundaries-execution.md) 记录真实账号与云协作未验；[2026-10-03-001](2026-10-03-001-command-native-audit-results.md) 明确 Steam 账号写入未被升级 | 无已授权的 Steam 合作伙伴账号/depot 凭据/steamcmd；本会话未登录、未调用任何 Steam 账号接口 | Steam 合作伙伴账号 + 测试 app/depot ID + depot 写入凭据（经宿主 secret/env 注入，不入库；需 G4）＋steamcmd/Steamworks 客户端 + 测试账号；成就/统计/云冲突脚本与回滚说明；Store 分发仍为 G3 |
| Steam SDK-off 本地 provider 拒绝合同 | **PASS**（既有执行记录，本次未重跑） | [2026-09-20-016](2026-09-20-016-optional-sdk-cloud-boundaries-execution.md)（NullSteamBackend 三个别名拒绝且保留本地 provider）；[2026-10-03-001](2026-10-03-001-command-native-audit-results.md)（SDK-off `steam_achievement` 保持明确拒绝） | 非本轮新执行；属于离线/不可用后端合同，不是账号能力 | 无需新增资源；该 PASS 不得外推为 Steam 账号、成就、云同步或分发能力 |
| 物理音频可听性 | **NOT_MEASURED** | [2026-09-28-001](2026-09-28-001-u26-voice-lipsync-validation.md) 明确 `physical_audibility=NOT_MEASURED`；[U31 §6](2026-10-26-002-i8-candidate-scope-acceptance.md) 保留；本机仅有音频端点枚举（§6） | 端点存在不等于可听性；本机无经校准声压计、无校准麦克风测量装置、无固定回环采样协议；本会话未运行候选 VOICE/口型场景 | 经校准声压计或固定 WASAPI loopback 采样协议 + 候选构建实际运行 VOICE/口型场景 + 精确硬件回调时间与音量/距离/设备记录；单独测量报告 |
| （相关上下文，非 U37/U38 新增）hosted tag 触发 release.yml dry-run | **NOT_RUN** | 本次 `gh run list --workflow release.yml --limit 5` 返回空列表；[U31 §6](2026-10-26-002-i8-candidate-scope-acceptance.md)；计划 §5.1.5 G2 | 无与候选匹配的新 tag；G2 未批准，本记录不创建/推送 tag | 用户 G2 批准后创建并推送与源码匹配的新 tag；属 U33/G2 范围，不在本 lane 执行 |

## 2. Android 当前候选真机 lane（U37）

### 2.1 实际执行的只读探测

探测使用本机既有 `D:\green\platform-tools\adb.exe`（未使用 Android SDK 目录中的第二份 adb，也未安装任何工具）。关键实际输出如下（完整记录见 `tmp/i10-probes/android-adb.txt`）：

| 时间（+08:00） | 只读命令 | 实际输出（关键行） |
|---|---|---|
| 13:29:39 | `where adb` | exit 0，`D:\green\platform-tools\adb.exe` |
| 13:29:40 | `adb version` | exit 0；`Android Debug Bridge version 1.0.41`；`Version 35.0.2-12147458`；`Running on Windows 10.0.26200` |
| 13:29:40 | `adb devices -l` | exit 0；列表已有 `192.168.8.11:5555  offline transport_id:1`。探测时 adb server 已存在（pid 16180，启动于 13:09:30），该 offline 条目的创建者本记录未追溯 |
| 13:29:40 | `adb connect 192.168.8.11:5555` | exit 0、30 ms；`already connected to 192.168.8.11:5555`（adb 对 offline 连接也会返回 0，退出码不能当作连接成功） |
| 13:30:04 | `adb -s 192.168.8.11:5555 get-state` | exit 1；`error: device offline` |
| 13:30:27 | `Test-NetConnection 192.168.8.11 -Port 5555` | `TcpTestSucceeded=True`、`PingSucceeded=False`；本机 IPv4 列表无 192.168.8.0/24 网段地址（仅以太网、VPN、WSL/Hyper-V 虚拟网段与代理网段）。该 TCP 结果与 adb offline 同时存在，不能据此认定设备在线 |
| 13:30:27 | `adb disconnect 192.168.8.11:5555` → `adb connect 192.168.8.11:5555` → `adb devices -l` | disconnect exit 0；重连 exit 0、**10,030 ms**；`failed to connect to 192.168.8.11:5555`；随后设备仍为 `offline transport_id:3` |

- 全程未执行 `adb install`、`adb push`、`adb shell`、`am start`、`logcat`、`su`、`input` 或任何设备写入；未尝试修复授权、重启设备或修改网络设置。
- 历史 K40 网络地址 192.168.8.11:5555 的来源见[平台矩阵 §4](../design/platform-support-matrix.md)（设备档案唯一权威为 [project-memory §4.2](../team/project-memory.md)）；该地址在本次探测中不可作为在线设备使用。

### 2.2 结论与依据

- **结论：NOT_RUN。** 当前候选 `e4e40be2` 没有绑定任何可运行的 Android 设备；U31 记录、2026-09-20 的空 `adb devices -l` 与本次 offline/连接失败一致。
- 既有 `scripts/android_device_smoke.sh` 在仓库中存在，但它的前置条件是“`adb devices -l` 显示 `device` 且 APK 已安装”（脚本头注释），本次条件不满足，未执行脚本。
- MIUI/root 输入注入（历史 K40 的 `su -c input tap`）属于历史设备经验，[project-memory §4.2](../team/project-memory.md) 明确重新安装后 UID/环境不能照抄；**不迁移为本候选真机证据**。
- 历史 K40 全链路闭环见 [2026-08-24-028](2026-08-24-028-android-full-closure.md)，只作背景；当前候选真机仍 NOT_RUN（[平台矩阵 §1/§4](../design/platform-support-matrix.md)）。

### 2.3 下一次执行需要的具体资源

1. 一台已授权、可连接的 ARM64 Android 设备（USB 优先；网络 adb 需设备端开启并授权），`adb devices -l` 必须显示 `device` 而非 `offline`。
2. 与候选源码绑定的 TEST 签名 APK：CI artifact `android-release-artifacts`（id 11300862395，U31 §3）或按 `e4e40be2` 本地重建；记录 APK SHA256、机型、OS、构建 SHA。
3. 设备日执行 U37 清单：安装/升级、启动、CJK/IME、触摸、音频焦点、前后台、存读档与重启；保留 logcat、截图、进程退出与清理记录；测试签名与正式签名结论分开写。

## 3. Apple / iOS lane

### 3.1 当前主机条件（只读检查）

- 主机为 Windows NT 10.0.26200 / AMD64；`xcodebuild`、`xcrun`、`codesign`、`security`、`altool`、`xcbeautify` 均 **NOT_FOUND**；Xcode 默认目录不存在。
- 本机 `eas`（Expo CLI）**NOT_FOUND**；`APPLE_ID`、`APPLE_APP_SPECIFIC_PASSWORD`、`APPLE_TEAM_ID`、`EXPO_TOKEN` 环境变量均未设置。
- 仓库内未发现 `*.p12`、`*.pfx`、`*.mobileprovision`、`*.cer` 等签名凭据文件（只做存在性扫描，不读取任何凭据内容）。
- 现有 Apple 工作流文件存在：`.eas/workflows/u2-apple-validation.yml`（3 个 custom `macos-medium` job：`macos_debug`、`ios_device_compile`、`ios_simulator_cpp`，默认镜像 `macos-tahoe-26.5-xcode-26.6`）。

### 3.2 既有 EAS lane 记录（额度失败）

- 历史成功只绑定旧源码：[2026-09-08-003](2026-09-08-003-u2-expo-apple-validation.md) 第六次 EAS 运行 `01a0810e-bf2f-7590-8999-c1bf4fc87005` 三 job 成功，统一源码 `74c07ea5ac5d618652ed1b072519b2899b044529`（macOS 完整 Debug profile；iOS Simulator 1281/1281 断言通过；iOS device 未签名编译）。**该源码与当前候选 `e4e40be2` 不同，不迁移。**
- 后续 EAS 运行均未进入构建：第七次 `01a08139-57a7-7143-884c-d14d3e367d31` 在 worker 分配前因 Free CI/CD 60 分钟额度耗尽失败（服务报告重置 2026-10-01T00:00Z）；b533 的用户指定运行 `01a0f640-2601-7af4-944e-5afdd56bc0e3` 同样在 worker 分配前被拒，报告重置 **2026-11-01T00:00Z**（非未来容量保证）。三 lane 取源/configure/compile/tests 均 NOT_RUN（[b533 §EAS](2026-10-01-004-b533-final-package-validation.md#eas-当前额度条件三-lane-未执行)）。
- 2026-10-26 只读复核：E 盘恢复区原件 `u2-b533-eas-detail-01/diagnosis-02.json` 存在，重新计算 SHA256 为 `522F20DE28B5B796FB982EFB84E62606C9FD843EF799C567897A728FB9DAE507`，与 b533 记录引用一致（原件索引见 §9）。
- 本次未派发 EAS、未升级计费、未登录 Expo、未重复提交已知额度条件；Free 套餐剩余额度不由历史身份或重置时刻推定。

### 3.3 iOS 探针 CI 状态（本次只读回读）

- 2026-10-26T13:31:41 +08:00 通过 `gh api`（只读）回读 run `37195162558`（attempt 1）：`status=completed`、`conclusion=success`、`head_sha=e4e40be2…`、event=`pull_request`，12 个 job 均 success。
- 其中 iOS job：`111415436805`，name `Validate engine / iOS CMake probe (gate)`，success，2026-10-04T10:22:55Z → 10:26:54Z（3 m 59 s）。该 job 在 `macos-latest` 上执行 `verify_metal_shaders.py`、iOS CMake 配置与模块图构建，构建参数显式 `CODE_SIGNING_ALLOWED=NO`、`CODE_SIGNING_REQUIRED=NO`（`.github/workflows/validate-engine.yml`）。
- 因此该 lane 的 **PASS 仅限“未签名编译探针”**；它不产生模拟器/真机运行、UIKit 启动、真实 Metal 像素、音频或签名证据，也不能替代 EAS 三 lane。

### 3.4 结论与下一次资源

- **结论：iOS 未签名编译探针 PASS（既有 run 只读回读）；iOS 真机/模拟器运行与签名、TestFlight、EAS 三 lane 均 NOT_RUN（hardware/credential/quota-gated）。**
- 下一次执行需要：① Apple 硬件（Mac 或经批准的外部设备）+ Xcode；② Apple Developer Program 团队/证书/描述文件/设备 UDID（签名与 TestFlight 属 G3）；③ 若走 EAS，需要可用额度/G4 决策 + Expo 账号（凭据不入库）；④ 运行后保留模拟器/真机日志与截图，并明确记录绑定源码 SHA。

## 4. macOS 窗口化 Metal lane

- 本机为 Windows 工作机；本轮没有取得任何 Mac 桌面窗口或显示会话。候选 run 的 macOS Debug 与 Release&Package 分别在 hosted `macos-15` 上运行（job `111415436779`、`111419237701`，本次只读回读均 success），属于无窗口构建/测试/包校验；包运行时步骤带 `--audio-output software`，不涉及真实显示或窗口化 Metal 呈现。
- [平台矩阵 §1/§2/§4](../design/platform-support-matrix.md) 明确 [M-C/M-D/M-E]（Boot/Gameplay/First-VN）为 ⏳ hardware-gated；[U31 §6](2026-10-26-002-i8-candidate-scope-acceptance.md) 也把窗口化 Metal 列为 NOT_RUN。托管 macOS 包收据 PASS/RUNTIME_PASS 不能升级为窗口化桌面证据。
- **结论：NOT_RUN。** 下一次执行需要 Mac（Xcode、Metal GPU、物理显示）上的窗口化 boot/gameplay/first-VN 运行，记录实际后端、截图、帧时间、退出码与资源清理；或经批准的外部硬件实验室与可复核的屏幕/日志采集。

## 5. Steam 账号 / depot lane

### 5.1 实际只读检查

- 本机默认 Steam 客户端目录（`C:\Program Files (x86)\Steam`）存在；仅做存在性检查，未读取任何账号配置内容：`config\loginusers.vdf`、`config\config.vdf`、`steamapps\libraryfolders.vdf` 存在，`config\depotcache` 不存在。Steam 客户端进程（steam/steamwebhelper/steamservice）未运行。
- `STEAM_PATH`、`STEAM_API_KEY`、`STEAM_APP_ID` 等环境变量未设置；`steamcmd` 未安装（PATH 与常见目录均无）。
- 仓库内未发现 `steam_appid.txt`、`loginusers.vdf`、`sdkredist` 等凭据/账号文件；与 depot 相关的仅有占位模板 `scripts/steam/depot_build_windows_template.vdf`（`DepotID` 为 `<DEPOTID_WINDOWS>` 占位，不构成 depot 凭据）。
- 本次未登录 Steam、未调用 Steamworks/steamcmd、未读取或写入任何 depot。

### 5.2 结论与依据

- **Steam 账号写入、成就/统计、云冲突、depot 发布：NOT_RUN。** 真实账号与云协作在 [2026-09-20-016](2026-09-20-016-optional-sdk-cloud-boundaries-execution.md) 中仍未验收；SDK ON 仅完成两个 Debug 配置的编译链接与未初始化查询，未做账号功能；[2026-10-03-001](2026-10-03-001-command-native-audit-results.md) 明确 Steam 账号写入未被升级为支持。
- **既有离线/拒绝合同 PASS**：[2026-09-20-016](2026-09-20-016-optional-sdk-cloud-boundaries-execution.md) 的 NullSteamBackend 在 `steam`、`steam://`、`steamcloud` 三个别名下拒绝配置并保留本地 provider；SDK-off 的 `steam_achievement` 保持明确拒绝。该 PASS 仅覆盖离线拒绝合同，不证明账号、云同步或分发能力。
- **Store 级别仍为 `?`**（[平台矩阵 §1](../design/platform-support-matrix.md)）；Steam 分发属 G3。

### 5.3 下一次执行需要的具体资源

Steam 合作伙伴账号与测试 app/depot ID、depot 写入凭据（经宿主 secret/env 注入，不进仓库；需 G4 授权）、steamcmd 或 Steamworks 客户端与测试账号；成就/统计/云冲突的可回滚测试脚本与场景说明；上传前固定候选包摘要，上传与商店动作按 G3 单独授权。

## 6. 物理音频 lane

- 本机只读枚举到物理与虚拟音频端点（Realtek 扬声器/耳机、USB Audio Device、HDMI 显示器音频、虚拟音频设备、USB 麦克风等），这**只证明端点存在**，不是物理可听性测量。
- 本机未发现经校准的声压计/测量麦克风装置或固定回环采样协议；`ffmpeg` 在 PATH 中存在（通用采集工具），但没有与其配合的校准、距离/音量记录或候选 VOICE/口型场景运行；本会话未运行候选构建的音频场景。
- 既有记录 [2026-09-28-001](2026-09-28-001-u26-voice-lipsync-validation.md) 明确 `physical_audibility=NOT_MEASURED`（WinMM 后端与源总线 PCM/像素已核对，但无扬声器声压、回环采样或精确硬件回调时间证据）；[U31 §6](2026-10-26-002-i8-candidate-scope-acceptance.md) 保留该状态。
- **结论：NOT_MEASURED。** 下一次执行需要：经校准声压计或固定 WASAPI loopback 采样协议（含设备、音量、距离、采样率与时长记录）＋候选构建实际运行 VOICE/口型场景＋精确硬件回调时间；单独形成测量报告，不能用端点枚举、软件混音或源总线 PCM 代替。

## 7. 其他既有未测项（仅列有可靠依据者，不得升级）

| 项目 | 状态 | 依据 |
|---|---|---|
| Android 真机物理多指/捏合注入 | NOT_RUN（单测有覆盖，真机物理注入未实测） | [android-device-validation.md](../platform/android-device-validation.md) #4/#41（adb 无多指注入工具）；[project-memory §5.2](../team/project-memory.md) |
| 物理游戏手柄输入 | NOT_RUN（未见物理手柄运行记录；命令审计未把手柄升级为支持） | [2026-10-03-001](2026-10-03-001-command-native-audit-results.md) 的边界面声明 |
| Android 真机 IME 实体输入法打字 | NOT_RUN（claimed / device-unverified） | [project-memory §5.2](../team/project-memory.md) |
| Android 低内存 onLowMemory 物理触发 | NOT_RUN（真机物理触发未覆盖） | [project-memory §5.2](../team/project-memory.md) |

以上项目均不因本记录的 CI 回读、端点存在或历史 K40 证据升级；需要对应设备/手柄/内存压力工具与用户授权的设备日。

## 8. 不授予与复用边界

- 本记录**不**授予发布、商店上传、正式签名、TestFlight/公证、Steam 账号/depot、EAS 付费或设备操作；G2/G3/G4 决策仍由用户保留。
- 本记录**不**把 PR #27 托管 run 的绿色、macOS/Windows/Linux 包收据、iOS 编译探针、历史 K40 真机、旧 EAS 第六次成功或本机端点/客户端存在性升级为设备、窗口化、商店或账号声明。
- 当前候选 `e4e40be2` 的 `release_ready=false` 与 U29 未完成状态不变；U1–U29 其余未闭合项（sanitizer strict FAIL、CPU INCONCLUSIVE、能力闭包结构扫描、EAS 三 lane、真实设备、Steam 账号、物理音频、hosted tag dry-run）保持各自原记录。
- 下游如需在公开平台声明中引用本记录，只能引用对应的 NOT_RUN/NOT_MEASURED 条件，不得改写成 PASS。

## 9. 复核线索

### 9.1 本次只读输出（主工作区 gitignored `tmp/i10-probes/`，未提交）

| 文件 | 字节 | SHA256 |
|---|---|---|
| android-adb.txt | 2,091 | `696410aec49fce8418f4cce4520aee47c03133e6781bbf3083fb746f25995dda` |
| apple-macos.txt | 544 | `4738a80eab0a0a460c2a46b4f6bb65dd3c631d1cd85fa17c3ceb269161b3d9f0` |
| audio.txt | 1,339 | `dd4c3d0de728e1829fbe7efbb61e6d89cbb28dd3f4e7b9ce308c992fc08c04f6` |
| eas-evidence.txt | 3,110 | `7a713f805d691d87694d1e501377c10cffe2e939f1297ff767a1ea39f6db020c` |
| e-index.txt | 1,020 | `98e1dab249239f33b0b4f4ce3180b9a8768ffb23460a7c00a99bdb20a063d028` |
| e-recovery.txt | 36,058 | `9513b76c519b43e959faba1c4ec887b9e8fb245d5e98d463d55be4f672ef3794` |
| gh.txt | 416 | `b386253ed803faad2e233cf682bd7890f4c6cc2605fbc6d6496f0b9653f18516` |
| gh-runs.txt | 1,872 | `4eb020f53aa5f62358c11d844072142e4d2145006733c59c406a231777034846` |
| git-state.txt | 282 | `80297e34793ae6784b8c802e0b653595aef8e903076f4aa55b995f4134100f9a` |
| steam.txt | 989 | `7d4229338acc037133c802d75ca18f867bba9b46289d66a5f750c204be952bcb` |
| steam-signing.txt | 1,507 | `6bcda229477a9f4a12e3ce62c827dba385f672226210a7c6a9dcd24ab45e3dca` |

### 9.2 外部原件索引（E 盘恢复区，只读引用，未复制大文件）

E 盘恢复区 `E:\CaesuraRecovery\20260924-1446` 的索引与来源说明：

| 原件 | 字节 | SHA256（本次只读重算） |
|---|---|---|
| `README.md`（恢复范围说明） | 7,645 | `F122C6EF50BD22B6A820D9ABF2CC9A99E88F3E6C77D1B5D11007A5D7BAC5B617` |
| `recovery-summary.json` | 3,834 | `701FA9D6CAB7AF311BB2C2A8482C5391FC8DD567DF8F9EA6248441DBF6B90CAF` |
| `RECOVERY-RESULT-20260927.md` | 3,739 | `9F12F78C5A7AD275C40BEF936D8F9ED777011281BF4AEC22DB0C5341A2C6E288` |
| `u2-b533-eas-detail-01/diagnosis-02.json`（EAS 额度拒绝原件） | 7,669 | `522F20DE28B5B796FB982EFB84E62606C9FD843EF799C567897A728FB9DAE507` |

- 顶层共 1,609 项（831 目录 / 778 文件），其中包含 30 个恢复工作树与历史原件；本记录只引用索引与上表原件，未复制或改写任何大文件/原失败回执。
- 该目录的原记录见 [2026-09-24-017 已恢复原件与当前缺口](2026-09-24-017-foundation-integration-execution.md#已恢复原件与当前缺口)。

### 9.3 Git 状态

- worktree：`codex/i10-platform-conditions`，探测时 HEAD `62228e56`，工作区干净。
- 主工作区：仅本 teammate 的 `tmp/`（gitignored）写入；未修改任何 tracked 文件。
