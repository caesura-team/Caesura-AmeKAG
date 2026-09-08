# 发布验证与执行证据

本指南说明如何为一个明确的源码、平台和构建配置收集验证证据。工程约束以 [AGENTS.md](../../AGENTS.md) 为准，开发流程见 [开发指南](../team/development-guide.md)，当前工作范围由 [计划入口](../plans/README.md) 确定。验证通过只证明执行过的 profile，不授予发布、签名、商店提交或合并授权。

## 历史资料与当前边界

旧 [Release Candidate Gate](https://github.com/ailiasdesu/Caesura-AmeKAG/blob/41c9e8a7addc8e4daa2e4d79d61a257250913c07/docs/Caesura_AmeKAG_Agent_Pack/05_RELEASE_CANDIDATE.md) 中的九类发布阻断问题保留为验收概念；其 `RC-GO`、Web `RC-ready`、API Freeze 和阶段排期是历史规则。2026-09-05 的重新规划已废止旧排期、冻结和执行授权，不能据此限制当前计划或宣称当前版本通过。

[历史 RC 评估报告](../status/release-candidate-report.md) 保留当时的数字和结论供追溯，未经本轮重验的记录不是当前 HEAD 的证据。平台矩阵中迁移至本页的文档引用仅更新路径：原 `status`、证据 `commit`、`verified_at` 和测量内容保持原样，不表示平台重新验证或升级状态。

## 九类发布阻断问题

每一类都必须检查目标平台、目标产物及实际用户流程；没有证据时保留“待验证”，不能按没有发现错误推定通过。

| 类别 | 应验证的行为 |
| --- | --- |
| 崩溃、挂起 | 启动、正常运行、错误处理和退出均可完成，故障可诊断。 |
| 存读档损坏 | 保存、读取、迁移、损坏输入与恢复失败不会破坏已有有效状态。 |
| 分支结果错误 | 选择、跳转、调用、返回及恢复后的变量、奖励和结局符合脚本语义。 |
| CJK 缺字 | 目标字体和实际渲染后端正确显示所需中文、日文、韩文及标点。 |
| 输入失效 | 鼠标、键盘、触摸、焦点与声明支持的 IME 流程可操作。 |
| 包装或资源缺失 | 干净构建、打包、安装和从产物目录启动均成功，资源无需依赖开发机路径。 |
| 平台生命周期故障 | 暂停、切后台、恢复、窗口或设备生命周期不导致状态丢失或停滞。 |
| 音频恢复故障 | 音频解锁、中断、暂停和恢复保持约定语义，旧回调不影响新会话。 |
| 平台间玩法语义不同 | 同一 First-VN 路线的变量、选择、存读档与结局一致，平台服务通过接口隔离。 |

视觉打磨、文档笔误和可选实验功能等剩余问题也要记录；是否阻断取决于当前声明的能力与真实影响。Studio 仍按当前计划暂停，不能把旧编辑器排期重新带入发布范围。

## 选择验证范围

- 核心门禁覆盖全量构建、`CaesuraTests`、Lua 主套与补充套、CTest 和模块耦合。`CaesuraTests` 从 `build/tests/Debug/` 对应配置目录执行，发现的用例必须全部通过、零失败、零跳过；实际命令、最低发现数量及其他检查允许的例外以 [validation_profiles.json](../../scripts/validation_profiles.json) 和工程约束为准，不复制历史固定计数。
- 声明支持的平台分别执行 First-VN 和产物验证。编译探针、无头测试、浏览器集成、真实 GPU、真实音频和设备运行证据必须分别标明，不能互相替代。Android 必须绑定实际测试源码；iOS 的真实设备能力需要真机证据，不能由着色器或编译通过推定。
- Web 除单元与集成回归，还应按目标交付验证 Chrome／Edge、离线、子路径部署、CJK、音频、输入、存读档、压力与标签页暂停／恢复。只运行本机 Lua 或 jsdom 不能证明这些真实浏览器行为。
- 旧证据只有在源码、依赖、夹具、运行配置和需求范围仍匹配时才可复用。变化影响相应行为或缺少身份记录时补做必要验证；文档路径迁移本身无需重跑引擎全套。

## 执行、收集、核验

当前证据链为 [run_validation.py](../../scripts/run_validation.py) → [collect_validation_evidence.py](../../scripts/collect_validation_evidence.py) → [verify_release_candidate.py](../../scripts/verify_release_candidate.py)。执行器产生原始日志和 `run.json`；收集器只复制真实记录；核验器用包外的可信 profile 和 `--expected-run` 重建并比对结果。

下例在仓库根目录执行 `windows-debug` 的本地诊断验证。构建目录须已按该 profile 配置；执行器会核对 CMake 源目录、平台、配置及必要的缓存选项。每次使用新的目录，保留失败记录。

```powershell
if (-not (Test-Path -LiteralPath '.git')) { throw '缺少 Git 源码身份，无法执行受控验证。' }
$validationProfile = 'windows-debug'
$validationRunDir = 'artifacts/validation/raw/windows-debug-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
python scripts/run_validation.py --profile scripts/validation_profiles.json --profile-name $validationProfile --build-dir build --configuration Debug --run-dir $validationRunDir
if ($LASTEXITCODE -ne 0) { throw '执行未通过；保留原始日志并排查失败。' }

$validationReceipt = Get-Content -LiteralPath "$validationRunDir/run.json" -Raw | ConvertFrom-Json
$validationBundle = "artifacts/validation/evidence/$($validationReceipt.source_sha)/$($validationReceipt.run_id)/$validationProfile"
python scripts/collect_validation_evidence.py --profile scripts/validation_profiles.json --profile-name $validationProfile --run "$validationRunDir/run.json" --output $validationBundle
if ($LASTEXITCODE -ne 0) { throw '收集未通过；不能把该记录解释为通过。' }

python scripts/verify_release_candidate.py --check --diagnostic --artifacts-dir $validationBundle --profile scripts/validation_profiles.json --profile-name $validationProfile --expected-run "$validationRunDir/run.json" --commit $validationReceipt.source_sha
if ($LASTEXITCODE -ne 0) { throw '证据核验未通过。' }
```

诊断模式允许 dirty 或测试夹具用途的执行记录，不能作为干净版本的发布证据。审核干净发布候选时必须使用对应干净源码实际产生的记录，并去掉 `--diagnostic`；去掉参数本身不会把旧诊断记录变成发布证明。`--purpose test-fixture` 的记录不能用于发布核验。

输出目录末尾必须为 `<source_sha>/<run_id>/<profile_name>`。`--expected-run` 和可信 profile 必须位于证据包外；不能让包内自带的记录证明自身可信。哈希证明字节身份，不证明不可信执行者运行过命令；GitHub Actions 的 workflow、run、attempt 和下载产物身份仍需由调用方独立核实。源码或夹具在执行中变化、必要检查未通过、发现数量不足及报告不匹配都必须保留为失败。`--skip-if-missing` 返回 77 表示未验证，不能解释为通过。

## 平台矩阵与报告同步

[platform-matrix.yaml](../status/platform-matrix.yaml) 保存平台状态、证据提交和时间；[platform-status.md](../status/platform-status.md) 由现有脚本生成。更新引用后运行生成器及 `python scripts/generate_platform_status.py --check`。如果 `evidence_head_commit` 与当前代码不符，应报告证据新鲜度缺口，不能为让检查变绿而只改提交标记或验证时间。

显式 `--head <历史提交>` 只能检查那个历史矩阵的渲染是否同步；通过不表示当前 HEAD 已验证。当前执行报告应列出实际命令、退出码、原始记录路径、源码／配置身份、失败与未执行项。现有核验器不会签发 `RC-GO`，发布决策须另有当前证据和明确授权。
