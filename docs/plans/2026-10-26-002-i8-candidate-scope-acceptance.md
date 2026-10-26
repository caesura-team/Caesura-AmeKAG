# I8 候选范围验收记录（U31）

日期：2026-10-26。本记录接续[计划 §5.1](2026-09-05-001-refactor-runtime-foundation-plan.md#511-当前起点)与[2026-10-26 状态同步](2026-09-05-003-runtime-foundation-todo.md#2026-10-26-状态同步与续排)，对应当前唯一计划。完整目标仍为 U1–U29；本记录只把 PR #27 在候选源码上的实际托管执行范围转成可追溯的验收记录，不宣称 U29 完成，也不授予发布、tag、商店或设备操作。

## 源码身份与记录边界

**候选源码**

- 候选提交：`e4e40be2f1a0bc001258d4cd30dd449e604ba5e8`（PR #27 head，分支 `codex/command-native-contract-audit`，提交时间 2026-10-04T18:22:22+08:00）。
- 代码树身份：`e4e40be2` 本身只改 5 个文档/生成状态文件（`docs/design/capability-closure-matrix.md`、`docs/plans/2026-10-04-001-github-ci-finalization.md`、`docs/plans/README.md`、`docs/status/platform-matrix.yaml`、`docs/status/platform-status.md`）；其最后一个代码提交为 `0ae839922d1f4b722d4d1a5fa0b2578b7d0dc905`（`perf: reduce validated layer and backlog transfer overhead`）。
- 合并：PR #27 于 2026-10-04T11:53:11Z 合并为 `b994cfe6b3eb51fcfc92ce67185d56db46aa432b`；其后 master 推送 CI [run 37200233061](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/37200233061) 的 12 个 job 全部 success（§1.2）。PR #28 仅文档，合并 `01c52fad`，不改变代码身份。
- 本记录所在工作树：`codex/i8-delivery-closure`（基于 `codex/next-iteration-plan-20261026`，HEAD `227bad9d`）。本记录是文档写入，不改变被测源码。

**记录对象与方法**

- 唯一被验收的托管运行：CI [run 37195162558 attempt 1](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/37195162558)，workflow `.github/workflows/ci.yml`（workflow id 291151646，pull_request 事件，head 分支 `codex/command-native-contract-audit`），起 2026-10-04T10:22:33Z、终 2026-10-04T11:51:44Z，结论 success。run 内 `expected.json` 记录 `workflow_ref=refs/pull/27/merge`、`workflow_sha=42d1e6b50bcf671dd41c1506cdeeebe8de70628f`。
- 回读方式（2026-10-26，只读）：`gh api` 读取 run、jobs、artifacts、attempt 与 PR 元数据；`gh pr checks 27` 复核 12 项 pass。未触发任何 GitHub 写入。
- 唯一解包核对的 artifact 是 `release-input-gate-evidence`（id 11301783153，API digest `sha256:ff4494612691c3cc0903e5b15dc5bdd6c08bd9a148d6f8fafaf87212fafe843b`）；本地下载字节 SHA256 与 API digest 完全一致（§7）。其余 29 个 artifact 只登记 API 元数据，未下载内文。
- 不重跑构建、测试或打包；不重签名、不重打包、不改写历史回执。历史本地计数按日期引用（§5），不与本候选执行计数混用。
- 时效：30 个 artifact 查询时均 `expired=false`，统一 `expires_at=2027-01-02T10:22:33Z`（90 天保留期，从 run 创建时刻起算）。过期后只能依赖本记录的 run/job/artifact id 与 digest，不能伪造摘要或内文。
- 记录边界：本记录不覆盖设备、商店、物理音频、EAS、Steam 与 tag 触发发布（§6），也不改写既有 FAIL/INCONCLUSIVE（§8）。

## 1. 候选托管运行：12/12 job success

| 字段 | 值 |
|---|---|
| run id | 37195162558（attempt 1，run_attempt=1） |
| workflow | CI / `.github/workflows/ci.yml`（id 291151646） |
| 事件 / 分支 | pull_request / `codex/command-native-contract-audit` |
| head_sha | `e4e40be2f1a0bc001258d4cd30dd449e604ba5e8` |
| 起止（UTC） | 2026-10-04T10:22:33Z → 2026-10-04T11:51:44Z |
| 结论 | success（12 个 job 全部 success） |
| URL | https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/37195162558 |

### 1.1 job 清单（attempt 1）

| # | job 名 | policy key | 结论 | 起（UTC） | 止（UTC） | 耗时 | job id |
|---|---|---|---|---|---|---|---|
| 1 | Lock source and candidate policy | lock-source | success | 2026-10-04T10:22:35Z | 2026-10-04T10:22:48Z | 13s | 111415396368 |
| 2 | Validate engine / macOS · Clang · Debug | macos-debug | success | 2026-10-04T10:22:56Z | 2026-10-04T10:46:18Z | 23m22s | 111415436779 |
| 3 | Validate engine / iOS CMake probe (gate) | ios-compile | success | 2026-10-04T10:22:55Z | 2026-10-04T10:26:54Z | 3m59s | 111415436805 |
| 4 | Validate engine / Windows · MSVC · Debug | windows-debug | success | 2026-10-04T10:22:50Z | 2026-10-04T11:07:20Z | 44m30s | 111415436811 |
| 5 | Validate engine / Linux · GCC · Debug | linux-debug | success | 2026-10-04T10:22:51Z | 2026-10-04T10:45:24Z | 22m33s | 111415436848 |
| 6 | Validate engine / Android static contract (no NDK) | android-static | success | 2026-10-04T10:22:50Z | 2026-10-04T10:23:03Z | 13s | 111415436854 |
| 7 | Validate engine / Android CMake probe (gate) | android-compile | success | 2026-10-04T10:22:51Z | 2026-10-04T10:33:56Z | 11m5s | 111415436884 |
| 8 | Validate engine / Web · Final Package | web-package | success | 2026-10-04T10:45:26Z | 2026-10-04T10:47:16Z | 1m50s | 111419071282 |
| 9 | Validate engine / Linux · GCC · Release and Package | linux-release | success | 2026-10-04T10:45:26Z | 2026-10-04T11:05:59Z | 20m33s | 111419071334 |
| 10 | Validate engine / macOS · Clang · Release and Package | macos-release | success | 2026-10-04T10:46:25Z | 2026-10-04T11:10:20Z | 23m55s | 111419237701 |
| 11 | Validate engine / Windows · MSVC · Release and Package | windows-release | success | 2026-10-04T11:07:22Z | 2026-10-04T11:50:06Z | 42m44s | 111422710287 |
| 12 | Verify release inputs / Verify exact release inputs | release-inputs | success | 2026-10-04T11:50:09Z | 2026-10-04T11:51:43Z | 1m34s | 111429728569 |

- 12 个 job = 10 个 policy required job（Windows/Linux/macOS 各 Debug 与 Release&Package、Web Final Package、iOS 编译探针、Android 静态与编译探针）+ `Lock source and candidate policy` + `Verify release inputs / Verify exact release inputs`。
- 各 job 内 `skipped` 的步骤均为只在失败时运行的“保留首次/失败证据”步骤（如 `Preserve first ... execution attempt`、`Extract ... failure evidence`），成功时正常跳过，不是测试跳过。唯一被跳过的测试是 CTest 内预声明的 `CaesuraHeadlessAiSmoke`（§2.2）。
- `Lock source and candidate policy` 的步骤 4/5 实际执行“Verify the explicit execution head”与“Freeze the source policy before producer fanout”，把执行身份固定在 `e4e40be2`。
- job URL 可在 run 页面按 job id 定位：`https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/37195162558/job/<job id>`。

### 1.2 合并与 master 复验

- PR #27（`fix: harden runtime recovery and native command contracts`，https://github.com/caesura-team/Caesura-AmeKAG/pull/27 ）在 head `e4e40be2` 上 12/12 成功后合并为 `b994cfe6`（2026-10-04T11:53:11Z）。
- 合并后 master 推送运行 CI [run 37200233061](https://github.com/caesura-team/Caesura-AmeKAG/actions/runs/37200233061)：event=push，head_sha=`b994cfe6`，起 2026-10-04T11:53:14Z、终 2026-10-04T13:21:23Z，12 个 job 全部 success。job id：111430240969、111430282208、111430282252、111430282253、111430282286、111430282293、111430282368、111434298927、111434646009、111434646065、111437733128、111444955182。
- 上述绿色只证明这些 run/attempt 实际执行的检查；不自动等于已验收、可发布或全部 U 完成。

## 2. 候选自身执行计数（run 证据 artifact 内文）

以下计数不是历史本地记录，而是从 run 37195162558 产出的 `release-input-gate-evidence` artifact（id 11301783153）内文读取，并已用 API digest 核对下载字节（§7）。artifact 内 6 份执行 manifest 的 `context.source_sha` 均为 `e4e40be2`、`dirty=false`、profile 级 `result=PASS`。

### 2.1 门禁结论字符串（同一 artifact 内）

| 文件（artifact 内相对路径） | status | release_ready | 其他关键字段 |
|---|---|---|---|
| `release-input-gate/gate.json` | `DRY_RUN_INPUTS_VERIFIED` | `false` | stage=complete；11 个下载均 `ARTIFACT_DOWNLOADED`；errors=[] |
| `release-input-gate/aggregate/aggregate.json` | `INPUTS_VERIFIED` | `false` | source_sha=`e4e40be2`；version=`1.0.1`；release_tag=`null`；stability=`STABLE`；errors=[] |
| `release-input-gate/api-final/summary.json` | `HOSTED_INPUTS_VERIFIED` | `false` | 12 个 job 与 artifact id/digest 映射；errors=[]；该阶段声明未自证项：package_payloads、execution_evidence、versions、preupload_bytes、branch_protection 均为 `NOT_VERIFIED` |
| `release-input-gate/aggregate/pages-pages-artifact/pages.json` | `PAGES_ARTIFACT_VERIFIED` | `false` | version=`1.0.1`；deployment=`NOT_RUN` |

聚合阶段输入锁（artifact 内）：policy `7561c75009fd8f00f6bc7726ae01fad6b3d1143d45d066d58603b2b0f05faaa5`、expected `106e0fae4299083731539a383260b6ccc121f1766d1c687f72e6ce33e54a0f46`、producer-outputs `ee21b709c5ccfaf5faf1f0ad2b61f2010931e3ac9ce2a691ce0afde07102e040`、`CMakeLists.txt` `d498488e652b7046780fd060daec24ba9b1cf223418d043984ba268afbffa665`、`scripts/validation_profiles.json` `3e0bb1444fca18f77167d436846639aae947cee94a5e87eb5313bf1f170d5d69`；aggregate receipt SHA256 `e4eb2f580f0d584833bbe19e4a81ac1ac7884ba345f6e0721d50c8fff486b7b0`。

### 2.2 六个原生配置的执行计数

| profile | C++（发现/通过/失败/跳过） | Lua 主 / 隔离 | CTest | Python 验证套件（runner/process/evidence/adversarial） | profile 结论 |
|---|---|---|---|---|---|
| windows-debug | 1,626 / 1,626 / 0 / 0 | 153/153 + 60/60 | 74 发现、73 通过、0 失败、1 跳过 | 22 / 15 / 78 / 57 | PASS |
| windows-release | 1,626 / 1,626 / 0 / 0 | 153/153 + 60/60 | 74 发现、73 通过、0 失败、1 跳过 | 22 / 15 / 78 / 57 | PASS |
| linux-debug | 1,601 / 1,601 / 0 / 0 | 153/153 + 60/60 | 74 发现、73 通过、0 失败、1 跳过 | 22 / 10 / 78 / 57 | PASS |
| linux-release | 1,601 / 1,601 / 0 / 0 | 153/153 + 60/60 | 74 发现、73 通过、0 失败、1 跳过 | 22 / 10 / 78 / 57 | PASS |
| macos-debug | 1,601 / 1,601 / 0 / 0 | 153/153 + 60/60 | 74 发现、73 通过、0 失败、1 跳过 | 22 / 10 / 78 / 57 | PASS |
| macos-release | 1,601 / 1,601 / 0 / 0 | 153/153 + 60/60 | 74 发现、73 通过、0 失败、1 跳过 | 22 / 10 / 78 / 57 | PASS |

- CTest 的 1 个跳过在所有 6 个配置中均为同一预声明可选测试 `CaesuraHeadlessAiSmoke`；不是失败或漏测被改记。
- 计数是各 profile 的发现结果，不能跨配置相加成“唯一用例数”，也不是覆盖率；Windows 的 C++ 与 validation-process 计数含平台特有注册，与 Linux/macOS 的差异仅如实记录，不推断原因。
- 每份 manifest 的绑定身份：

| profile | 执行 run UUID（manifest context） | manifest SHA256 | receipt SHA256 |
|---|---|---|---|
| windows-debug | 4326abb7-df38-40eb-b7aa-827655582ed1 | 4cb8cf8a78cc2527c9e44bbc50f2f2f417649e7b8c9928970653dced072c27e9 | eaa6151d5fdade1570c97e60c52ffc98b85b9e0da059bc4158b7d60aaef373c1 |
| windows-release | 9c809cd7-343f-4d0e-b599-b59dabe1e680 | f65939bf8fbe92b275c5b676475e5142b1c9245dea61cf0694b4cfd6b50412e3 | 3f737f4434e4f1a4e30dd8b26379430d447361ab73ac9e3edd8795b065aa48db |
| linux-debug | c2915b55-109d-4aae-ba8e-0d1bd1b82200 | 4399b7d66b95ed8bc8422ced36c9407f51ae75ccfad0d51d0d1ebfecf005c4bc | 09d6fad2e39b91a54ea895f3d5db865cc9e45e0685deac08ea31c89250d0df8a |
| linux-release | c3f214d6-7a68-4ac7-9229-7f5cce75b336 | 77a899668e974434978ff1a5ffb1193d4b671b907f5e35f9eba8117058b69c75 | 57f132be4ba8d9f0ba82f6e5cac3e4e631609184013214a3fd7ce5a1bde13435 |
| macos-debug | 93be6221-b532-4331-aa15-6a008194a116 | 073c7c0ead4738c8f1396a547cc3560631d5876d56e812c6de07b81fc3efe472 | 1dc7f9c60d583311c40bb3f8b54fbaf2eac5fafb0107ade584629685a5126fba |
| macos-release | 8fe2dc1e-83bb-434e-ae75-2ceb4d889f53 | 83133008d585e55c83c7ecfbf82a6dfc0be0dbc0fe3e01a65bf80874711edb20 | 81305b3ef77f7cf05aa91bd605d0df252e66a704b6e7eda3b03b881202b5ddea |

manifest 路径：`release-input-gate/aggregate/execution-contents-<execution>-execution/package/manifest.json`。六份执行包在聚合阶段的状态为 `EXECUTION_BUNDLE_VERIFIED`、`hosted_provenance=CALLER_MUST_AUTHENTICATE`、`release_ready=false`；其传输准备 scope 为 `package_identity_and_extraction`、`runtime=NOT_RUN`（即聚合阶段未重跑运行时，运行时证据来自 producer job 本体的执行）。

### 2.3 最终包校验收据（同一 artifact 内）

| 包 | 平台/配置 | 包内 payload SHA256 | 收据状态 |
|---|---|---|---|
| `CaesuraAmeKAG-1.0.1-Windows-AMD64.zip` | windows / Release | d08c2574f412c25724fd1c61e1e4d12dbb5a7a79738afd548c4820bafc64f898 | PASS，runtime=RUNTIME_PASS，cleanup=COMPLETE |
| `CaesuraAmeKAG-1.0.1-Linux-x86_64.tar.gz` | linux / Release | 5984963726ac3924e38f98553aee1c6bb59dd24857ac25e59e9bd66447cf9b04 | PASS，runtime=RUNTIME_PASS，cleanup=COMPLETE |
| `CaesuraAmeKAG-1.0.1-Linux-x86_64.AppImage` | linux / Release | 199ee728084a78498b6a75a171fa5ab297b39fe385ef546dc814f1c42f0ce755 | PASS，runtime=RUNTIME_PASS，cleanup=COMPLETE |
| `CaesuraAmeKAG-1.0.1-Darwin-arm64.tar.gz` | macos / Release | f150d55bdd25a1e9e1c273bf45b1fb8900202d588dbc3ebe98dd923e838ea9f4 | PASS，runtime=RUNTIME_PASS，cleanup=COMPLETE |
| `CaesuraAmeKAG-1.0.1-Darwin-arm64.dmg` | macos / Release | 6051fb98eb393cb154b5680e021dfacbbd1064a8771448ea1e3d5ccaf1b6c620 | PASS，runtime=RUNTIME_PASS，cleanup=COMPLETE |
| `CaesuraAmeKAG-1.0.1-Web.zip` | web / Release | 1611b44d692a9ea6f359cd9803a2dd1c0479fa6e7e48fbb32acf9714e0c6e9bc | PASS，runtime=RUNTIME_PASS |
| `artifact.tar`（Pages） | web / Release | 9d0a4c116bce778c1eef9405bfa0033de98d26aea9a341125059d08eedb471c5 | PASS，runtime=RUNTIME_PASS；pages=`PAGES_ARTIFACT_VERIFIED`、deployment=`NOT_RUN` |

- 每份收据的 scope 明确为“one final package and this host runtime; no publication approval”；包 bundle 聚合为 `BUNDLE_VERIFIED`、`authentication=NOT_PERFORMED`、`release_ready=false`、raw_stage_evidence=`RAW_STAGE_LOGS_NOT_INCLUDED_NOT_REPLAYED`。
- 以上是托管 runner 上的包身份/解包/运行校验，不是发布批准、商店签名、设备体验或人工可听性证明。

## 3. Artifacts（30 项，GitHub API 只读回读）

查询方式：`gh api repos/caesura-team/Caesura-AmeKAG/actions/runs/37195162558/artifacts?per_page=100`（2026-10-26）。共 30 项，合计 1,205,187,756 字节；全部 `expired=false`，`expires_at=2027-01-02T10:22:33Z`。digest 为 API 返回的 `sha256:...` 去掉前缀后的十六进制。

| 类别 | artifact id | name | size_in_bytes | sha256 | created_at |
|---|---|---|---|---|---|
| 最终包 | 11301234706 | caesura-amekag-macos-arm64-37195162558-1 | 86,207,851 | 214d585c7abeb3602a9d7c0391a22889877502ec3a00ae0addd8bcc3101ec698 | 2026-10-04T11:10:06Z |
| 最终包 | 11301468726 | caesura-amekag-linux-x64-37195162558-1 | 83,531,292 | 73abc5d5b62f7d630003baab48179dceef6494b28455543bedc5e268aebd0ce2 | 2026-10-04T11:05:46Z |
| 最终包 | 11302144009 | caesura-amekag-windows-x64-37195162558-1 | 41,021,037 | 0ef10337ccd746cf678ca742c5213f48050c1203a3bde86a18fb81c6dfe913eb | 2026-10-04T11:49:57Z |
| 最终包 | 11301167643 | caesura-amekag-web-37195162558-1 | 40,948,380 | aef618014c238c1b4e4550393beb587a7df73aeb05d68f475be1458c97a1b383 | 2026-10-04T10:47:08Z |
| 最终包 | 11300833856 | caesura-pages-37195162558-1 | 32,010,378 | 0350c1892a2303615cc1f263e98db783fca9cfaa62b55089c09f4bfb9f879157 | 2026-10-04T10:47:11Z |
| Android 包 | 11300862395 | android-release-artifacts | 116,939,142 | 5c2daf39c9c8ed543c8fa540136565cbf5317c259a393ab1bdef0088fd442d0d | 2026-10-04T10:33:52Z |
| Android 包 | 11301111387 | android-slices | 15,969,253 | 16e7a6c31e3185f86958d9d4ce30f1ff11062d96b1a5d0d255f22f1bd7b99b0a | 2026-10-04T10:33:45Z |
| Android 证据 | 11301370698 | android-package-contract-evidence | 1,073,332 | cf9e0f5ec1fed4cd52989fc1d2bc8dc0a81723d80a7240cb32fb6f6f014c13a4 | 2026-10-04T10:33:42Z |
| 包证据 | 11301343969 | u22-linux-package-evidence | 38,142,817 | f17ec72c9bd6669326c7c091748cea3b615613002a3ee98adb96b27fd44c8eb0 | 2026-10-04T11:05:50Z |
| 包证据 | 11301508978 | u22-macos-package-evidence | 30,454,899 | 0d89ee27aeb4ecb664af3fee6e24cebec18e699502452c745a578b623864b97b | 2026-10-04T11:10:10Z |
| 包证据 | 11300993193 | u22-web-package-evidence | 26,427,784 | d5ecb16118ef76f35154b74b0beeb029691f5c41563e0be0569fab577703b227 | 2026-10-04T10:47:14Z |
| 包证据 | 11302173968 | u22-windows-package-evidence | 14,760,674 | 69686410fb17a75e3dd414a58fab44eb19afa3a8eec71f50a1d8aa3257289159 | 2026-10-04T11:49:59Z |
| 执行证据 | 11300657335 | u23-linux-debug-37195162558-1 | 102,095,694 | 9a438266a572e57944b7b342472ac12371f6db0bbbf9f7f8aa01af8d32206339 | 2026-10-04T10:37:18Z |
| 执行证据 | 11301480899 | u23-macos-debug-37195162558-1 | 35,501,068 | 78de3042a8d476ffd574f5b607f247cafd9112d12c8df1b9f95584568a6b4134 | 2026-10-04T10:46:01Z |
| 执行证据 | 11301650121 | u23-linux-release-37195162558-1 | 32,532,686 | cceb75593e0d6e8b4935927a80ffbeec8b8c2dbaa01ba0e8f38db5b449cb187c | 2026-10-04T11:01:55Z |
| 执行证据 | 11300634635 | u23-macos-release-37195162558-1 | 23,101,122 | 2e182042559d72485e017a18c34e3f3e3e16c5133b8aa45a26fa70763e210234 | 2026-10-04T11:07:55Z |
| 执行证据 | 11300683657 | u23-windows-debug-37195162558-1 | 19,494,100 | fb506838ab87eafc5526ea1bd270d6a8cafa497070c01f50e25ddf27466b1984 | 2026-10-04T10:56:45Z |
| 执行证据 | 11301727640 | u23-windows-release-37195162558-1 | 12,131,028 | 02b27b893995912ecb881cc339c4cc5dfe5dbf651826d8b5823c59bee9f38226 | 2026-10-04T11:42:27Z |
| 聚合/门禁证据 | 11301783153 | release-input-gate-evidence | 1,425,987 | ff4494612691c3cc0903e5b15dc5bdd6c08bd9a148d6f8fafaf87212fafe843b | 2026-10-04T11:51:40Z |
| 文档/证据 | 11300413696 | linux-replay-export-evidence | 257,765 | e4830cf26224e61238b52ae4f51c7659ed2317215dd51c6e9871ef1b6014f7a2 | 2026-10-04T10:45:00Z |
| 文档/证据 | 11301199613 | windows-authoring-evidence | 163,315 | 58aabf7265f519df1112c2b1c44e6fa4e306bee63da1dd3b0c4a572d9f694c4c | 2026-10-04T11:07:12Z |
| 文档/证据 | 11300597628 | linux-authoring-evidence | 160,754 | c8e40ff03c039d44e9681b1280d039e917f03f20225e9e3c00b1a12ac44438ec | 2026-10-04T10:41:22Z |
| 文档/证据 | 11300656352 | u29-web-runtime-crossfs-37195162558-1 | 6,473 | e54e350c19e81b9944229aabd7f77c5885d3b9db137a1971dc37293d2ffcf57c | 2026-10-04T10:23:33Z |
| 文档/证据 | 11300443187 | linux-web-audio-evidence | 1,503 | 12d085bfcd14b85c77f1ca4ad9b4f69794e2b09a01aa090eabf95d64c4a68947 | 2026-10-04T10:41:34Z |
| 诊断 | 11301551088 | u23-linux-debug-diagnostic-37195162558-1 | 204,404,680 | a5697abba886d4030ca64351754c8cbdcca8ed1a96bbbac7985720c68ac9a989 | 2026-10-04T10:45:21Z |
| 诊断 | 11300538154 | u23-macos-debug-diagnostic-37195162558-1 | 71,177,489 | f4f26d4e47274570907cf0b9f5963132c1e5f9b73b7773eb70d3aa8d251d5830 | 2026-10-04T10:46:13Z |
| 诊断 | 11301149629 | u23-linux-release-diagnostic-37195162558-1 | 65,274,699 | a85c578d24b320cdf7a37a1360b7859f578ecf539d072653485d2a3ececa767c | 2026-10-04T11:05:56Z |
| 诊断 | 11302125070 | u23-macos-release-diagnostic-37195162558-1 | 46,371,179 | 087e6b63ddb9f11ad7fb2a70ba19c204e8933c731672d59cc195212ba467ea2b | 2026-10-04T11:10:14Z |
| 诊断 | 11301880180 | u23-windows-debug-diagnostic-37195162558-1 | 39,165,919 | 44acfada6d7f806cbb7de16dd2cbd58ee46df3c7e2b03bd020f1ecedada2218a | 2026-10-04T11:07:17Z |
| 诊断 | 11302762405 | u23-windows-release-diagnostic-37195162558-1 | 24,435,456 | 580b2ebc618b3cbc86ede307a9b26829d752c00d5ba499ec1765e33bcb627d96 | 2026-10-04T11:50:02Z |

## 4. 选定范围结论

范围定义沿用计划 §5.1.4 的 I8 目标：三桌面 Debug 与 Release&Package、Web Final Package、Android 编译/测试签名包、iOS 编译探针。以下结论只覆盖本记录 §1–§3 实际执行与回读的范围。

| 选定范围 | 本 run 的实际证据 | 结论 |
|---|---|---|
| Windows Debug | job 111415436811 success；执行 manifest windows-debug PASS（C++ 1,626、Lua 153+60、CTest 73+1 跳过） | PASS（托管 Debug 范围） |
| Windows Release & Package | job 111422710287 success；windows-release manifest PASS；u22-windows-package-evidence 内 ZIP 收据 PASS/RUNTIME_PASS | PASS（Release&Package 托管范围） |
| Linux Debug | job 111415436848 success；执行 manifest linux-debug PASS（C++ 1,601、Lua 153+60、CTest 73+1 跳过） | PASS（托管 Debug 范围） |
| Linux Release & Package | job 111419071334 success；linux-release manifest PASS；TGZ 与 AppImage 收据均 PASS/RUNTIME_PASS | PASS（Release&Package 托管范围） |
| macOS Debug | job 111415436779 success；执行 manifest macos-debug PASS（C++ 1,601、Lua 153+60、CTest 73+1 跳过） | PASS（托管 Debug 范围） |
| macOS Release & Package | job 111419237701 success；macos-release manifest PASS；TGZ 与 DMG 收据均 PASS/RUNTIME_PASS | PASS（Release&Package 托管范围；窗口化 Metal 运行仍 NOT_RUN，见 §6） |
| Web Final Package | job 111419071282 success；Web ZIP 与 Pages tar 收据 PASS/RUNTIME_PASS；Pages artifact `PAGES_ARTIFACT_VERIFIED`、deployment `NOT_RUN` | PASS（最终包与包内运行校验范围；Pages 部署 NOT_RUN） |
| Android 编译/测试签名包 | job 111415436854（静态契约）与 111415436884（CMake 探针）success；步骤“Build controlled unsigned packages and verify final TEST-signed bytes”“Upload exact verified APK and AAB” success；artifact android-release-artifacts（APK/AAB）与 android-package-contract-evidence 已登记 | PASS（编译/测试签名包范围；真机运行 NOT_RUN，见 §6） |
| iOS 编译探针 | job 111415436805 success；步骤 Verify Metal Shaders & Fallback paths、Configure iOS、Build module graph 均 success | PASS（编译探针范围；真机/签名 NOT_RUN，见 §6） |
| release-input dry-run 聚合 | job 111429728569 success；gate/aggregate/API 回读分别记录 `DRY_RUN_INPUTS_VERIFIED`、`INPUTS_VERIFIED`、`HOSTED_INPUTS_VERIFIED`，均 `release_ready=false` | PASS（dry-run 输入范围，不是发布） |

**总体结论**：本记录选定的执行范围为 **PASS**（run 37195162558/attempt 1 十二个 job 全部 success，六份原生配置执行 manifest PASS，7 份包收据 PASS/RUNTIME_PASS，dry-run 聚合记录 release_ready=false）；本 run 无新增 FAIL 项。该 PASS 限定在“托管构建 + 测试 + 打包 + 包内运行校验 + dry-run 输入聚合”范围，**release_ready=false**，不宣称 U29 完整完成、不授予发布/商店/设备声明，也不改写历史失败与不确定结论（§8）。

## 5. 历史本地记录计数（按日期引用，不重跑）

以下数字来自既有执行记录，不是对 `e4e40be2` 的重跑；日期口径按计划 §5.1/待办与承载文档分别保留。

| 检查 | 记录值 | 来源（日期与提交） | 与本候选的关系 |
|---|---|---|---|
| 完整 C++ | 1,630 passed / 0 failed / 0 skipped；460,880 assertions passed | [2026-10-03-001](2026-10-03-001-command-native-audit-results.md) 的“本轮实际结果”（提交 `4e095830`，2026-10-03；计划 §5.1 与待办把该计数标注为“2026-10-02 记录”） | 同一审计分支早期未提交工作树的历史计数，**不是本候选重跑**；候选自身为 Windows 1,626、Linux/macOS 1,601（§2.2） |
| Lua 主 / 隔离 | 153/153 与 60/60 | 同上；[2026-10-04-001](2026-10-04-001-github-ci-finalization.md) 最终本地验收重申（提交 `e4e40be2`，2026-10-04） | 与本候选 manifest 的 153/153 + 60/60 一致，但仍是不同执行身份 |
| CTest | 74 passed / 0 failed / 1 预声明可选 AI skip，共 75 项 | [2026-10-03-001](2026-10-03-001-command-native-audit-results.md)（计划口径 2026-10-02） | 与本候选 74 发现 / 73 通过 / 1 跳过一致；跳过测试名 `CaesuraHeadlessAiSmoke` |
| 完整 Web | 58 文件、770/770 | [2026-10-04-001](2026-10-04-001-github-ci-finalization.md) 最终本地验收（提交 `e4e40be2`，2026-10-04） | 本记录未重新执行 Web 测试；沿用该最终本地验收记录，本 run 提供 Web Final Package job |
| 其他最终本地验收 | 图层恢复 324 项、真实 Wasmoon 图层 18 项、backlog 29 项、浏览器报告 60 项、原性能套件 12/12；所有必需进程实际退出 0、源码输入稳定、进程树清理完成 | 同上（2026-10-04-001） | 属最终冻结版本的本机验收；不与托管 run 计数相加 |
| 历史 Web 失败 | 完整 Web 671/672，`perf-baseline.test.js` 主故事吞吐门槛失败 | [2026-10-03-001](2026-10-03-001-command-native-audit-results.md#尚不能通过的边界)（提交 `4e095830`） | 保留原失败；后续修复记录见 2026-10-03-002/2026-10-04-001，本 run 绿色不覆盖该历史 |
| 历史托管失败 | 37109375408、37114535328、37122411104 等首轮/次轮失败 | [2026-10-03-002](2026-10-03-002-github-ci-integration.md)、[2026-10-04-001](2026-10-04-001-github-ci-finalization.md) | 原失败与原件保留，不改记 |

日期口径说明：待办与计划把 C++/CTest 计数写作“2026-10-02 记录”，承载文档的实际提交为 `4e095830`（2026-10-03）。本记录同时保留两种口径，避免把历史数字当成候选重跑；任何新执行另行取数。

## 6. NOT_RUN / 外部条件（逐项）

| 项目 | 当前状态 | 条件 / 依据 | 既有原件或记录索引 |
|---|---|---|---|
| EAS Apple 三 lane | NOT_RUN | 用户指定的 EAS run 在 worker 分配前被拒：Free CI/CD 60 分钟额度已用尽，服务端给出重置时间 2026-11-01T00:00Z（非未来容量保证）；取源、configure、compile、tests 均 NOT_RUN；需额度/用户决策（G4） | [b533 §EAS 当前额度条件](2026-10-01-004-b533-final-package-validation.md#eas-当前额度条件三-lane-未执行)；原件 `u2-b533-eas-detail-01/diagnosis-02.json` SHA256 `522f20de28b5b796fb982efb84e62606c9fd843ef799c567897a728fb9dae507` |
| Android 真机运行 | NOT_RUN | 无设备可达：2026-09-20 独立 `adb devices -l` 实际退出 0 且列表为空；本 run 只产出编译/TEST 签名包与 android-slices（供设备日），未安装/启动 | [2026-09-20-015](2026-09-20-015-android-package-validation-execution.md)（原件 `u24-device-query-01/query.json` SHA256 `5c12d36bdf7eb1a7983daea1cfb1161bd55f339622227efdf4080a688226d29e`）；K40 历史证据见 [2026-08-24-028](2026-08-24-028-android-full-closure.md)，不得外推为本候选真机通过 |
| iOS 真机与签名 | NOT_RUN（hardware/credential-gated） | 本 run iOS job 只做 CMake 配置、模块图构建与 Metal shader 校验（未签名编译探针）；真机/模拟器运行与 Apple Developer ID/证书/描述文件均未就绪；不能把未签名编译升级为设备或签名声明 | 历史未签名编译范围见 [2026-10-01-003](2026-10-01-003-candidate-validation-repair-followup.md)；“iOS 其余全部 hardware-gated”见 [平台支持矩阵](../design/platform-support-matrix.md) |
| macOS 窗口化 Metal | NOT_RUN（hardware-gated） | 无 Mac 桌面窗口运行证据：本 run macOS job 是 headless 构建/测试/包校验；窗口化 Metal 的 Boot/Gameplay/First-VN 属硬件门 | [平台支持矩阵 §2](../design/platform-support-matrix.md#2-证据注记)（[M-C/D/E]）；历史 Apple/Metal 范围见 [2026-08-24-028](2026-08-24-028-android-full-closure.md)；U38 |
| Steam 账号 / depot | NOT_RUN | 需 Steam 账号/客户端/depot 凭据；离线拒绝合同（SDK-off）与 local provider 已测，但“Steam 正向账号操作始终为 NOT_RUN”，成就/统计/云同步/分发权限未验 | [2026-09-20-016](2026-09-20-016-optional-sdk-cloud-boundaries-execution.md)；[2026-10-03-001](2026-10-03-001-command-native-audit-results.md) |
| 物理音频 | NOT_MEASURED | 需扬声器声压、回环采样或硬件回调时间证据；软件混音与源总线 PCM/像素不等于物理可听性 | [2026-09-28-001](2026-09-28-001-u26-voice-lipsync-validation.md)（`physical_audibility=NOT_MEASURED`） |
| hosted tag 触发的 release dry-run | NOT_RUN | 只读回读 `gh run list --workflow release.yml --limit 10` 为空：`release.yml` 从未有运行记录；现有 `v1.0.1` tag 指向 `9aa299d4`（早于本轮全部工作），与候选 `e4e40be2` 不匹配；tag 触发入口需用户批准 G2 后创建/推送与源码匹配的新 tag，本记录不执行 | [2026-09-19-014](2026-09-19-014-release-input-provenance-execution.md)；[b533 §U23 dry-run](2026-10-01-004-b533-final-package-validation.md#u23-dry-run-与未运行发布入口)；计划 §5.1.5 G2 |
| 正式商店签名/上传、Pages 部署 | NOT_RUN（G3） | Pages deployment=`NOT_RUN`；Store 七级仍为 `?`；商店/正式分发未启动，需用户 G3 授权 | 计划 §5.1.5；[平台支持矩阵 §1](../design/platform-support-matrix.md#1-总表七级--六平台) |

## 7. 回读快照与摘要

本次只读回读保存在主工作区 gitignored 的 `tmp/i8-docs-closure/`（不随仓库分发，仅作复核线索）；其中 `release-input-gate-evidence.zip` 的 SHA256 与 GitHub API artifact digest 完全一致。

| 快照 | 字节 | SHA256 |
|---|---|---|
| run-37195162558.json | 26,868 | 9454d791818259a53601907a69f416e48b046b1528746a7d125c464a1a9c84a1 |
| jobs-37195162558-a1.json | 116,250 | 60c57b5d7cd80ee4d7ba68e5c151fbe1609a5c5a624b47dfc7ab7f4574aaf5c5 |
| artifacts-37195162558.json | 44,910 | c63ea9e9f2ba01f2dbfaaa7994990a07ae63529e32706fea334498f4df1b22b3 |
| attempt-37195162558-1.json | 13,462 | b13031167ee8e5fa2cc658709679a007787c74f75c6ba25ce812e28f25e78f60 |
| run-37200233061.json | 27,006 | d1183ef3280e9e068bc0dc3b4a84e479e30172e0a83ab62372f078b3c084e6a6 |
| jobs-37200233061.json | 57,792 | d8a29079eaee610deeada0a05ce2b5e2a275d7a62a1d6a0042e62b59ece55047 |
| pr-27.json | 996 | 5e3da2e9ad82261bd9480dcdebd0ab40b10a47531591c8ae8ecaeed58267909f |
| pr-25.json | 19,416 | ddc5b53acf684be337f2dad928c02bb9975bc283fc7b5a3ea8113489697ba487 |
| pr27-checks.txt | 1,782 | 6ec4f750df9cd4bfca7ee7095f78c9533612d4d95f5757a3c61768b0dd9f09ed |
| release-input-gate-evidence.zip | 1,425,987 | ff4494612691c3cc0903e5b15dc5bdd6c08bd9a148d6f8fafaf87212fafe843b |
| execution-manifest-summary.json | 29,806 | f33b6bb5125669c2e7c8fc8053f6348d89c5b4c9c7289f670f8be23ef368da48 |

## 8. 已知红点与历史边界（保留）

- **Linux sanitizer strict 仍 FAIL**：既有第三方 Mesa/EGL LSan 诊断与第一方定向修复、替代验证记录保留；本 run 的 Linux job 是普通 Debug/Release 门禁，不是 sanitizer strict，不能用本 run 绿色改写该结论。由 U34 处理。
- **CPU 性能基线 INCONCLUSIVE**：固定样本方法与环境噪声结论保留，不因本 run（无 CPU 基线任务）升级。由 U35 处理。
- **物理音频 NOT_MEASURED**（§6）。
- **能力闭包仅为结构扫描**：145 声明 = CLOSED 131 + PARTIAL 5 + UNWIRED 7 + EXPERIMENTAL 2；Platform/Packaged 声明 0；不是运行闭合、覆盖率或全平台可用证明。由 U36 处理。
- 历史失败原件（Web 671/672、EAS 三 lane、设备、Steam、hosted tag 等）均保留且未被本记录改写。
- 草稿 PR #25 仍为 OPEN：其相对当前 master 的唯一净差异是 `docs/status` 旧锚点同步（`cf7ff48a`，锚点 79467afe→b51e44c1，生成时间 2026-09-23T18:20:26Z），不含产品/源码增量；是否关闭由 U32 评估，本记录不执行 GitHub 操作。

## 9. 复用边界与后续出口

- 下游 U32（公开平台/能力声明）只能引用本记录的实际范围；不得把本记录的托管 PASS 升级为设备、商店、物理音频或正式签名声明。
- 下游 U33 的版本与 tag dry-run 准备、G1/G2 决策门独立记录；本记录不决定版本号、不创建 tag、不触发 release。
- U1–U29 其余未闭合项（EAS 三 lane、真实设备、Steam 账号、物理音频、hosted tag dry-run、CPU INCONCLUSIVE、sanitizer strict）继续保持原状态；I8 不替代 U29 完整验收。
- 第三方重建路径：PR #27 页面 → run 37195162558/attempt 1 → §1.1 的 job 名与 job id → §3 的 artifact id/digest → `release-input-gate-evidence` 内 `release-input-gate/aggregate/execution-contents-*/package/manifest.json` 与包收据 → 源码 `e4e40be2`/`0ae83992`/合并 `b994cfe6`。
- artifact 于 2027-01-02T10:22:33Z 过期；过期后本节登记的身份与摘要仍可引用，但原始内文需另行归档或重取，不得伪造。
- 本记录不替代本机 `artifacts/validation` 下的历史原件索引，也不改变任何既有记录的验收边界。
