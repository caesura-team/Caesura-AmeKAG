# 平台声明与原始证据重验

`generate_platform_status.py` 默认只检查历史矩阵的结构、文档引用与生成输出同步。`evidence_head_commit` 是文档同步锚点；逐行的commit与日期描述原记录，不能通过改锚点把旧运行改为当前通过。默认输出 `NOT_REVERIFIED` 和当前证据 `NOT_RUN`。

显式重验不运行引擎。调用者先从受控执行边界选择完整源码SHA、必需profile、run/attempt和固定产物，再锁定选择JSON的SHA256。托管来源需先由U23验证真实GitHub身份；本入口本身不联网或认证producer。不要从待验manifest自选预期身份。

```powershell
python scripts/generate_platform_status.py --evidence-selection '<selection.json>' --evidence-selection-sha256 '<sha256>' --candidate-source '<full-source-sha>' --evidence-root '<artifact-root>' --evidence-profile '<trusted-validation-profiles.json>' --evidence-report '<new-report.json>' --output '<new-platform-evidence.md>'
```

输出必须相互分离且此前不存在；显式证据模式不覆盖仓库的默认历史矩阵。`--dry-run`可以省去Markdown输出，`--evidence-report`仍会保存本次结果。任何失败退出非零；已有报告不能被后一次运行覆盖。以下是格式示例，不是执行证据，占位符不能通过校验。

```json
{
  "schema": "caesura.platform-evidence-selection.v1",
  "source_sha": "<40 lowercase hex>",
  "claims": [{
    "id": "windows-debug",
    "kind": "execution",
    "platform": "windows",
    "configuration": "Debug",
    "bundle": "collected/windows-debug",
    "manifest_sha256": "<64 lowercase hex>",
    "expected_run": "raw/run.json",
    "receipt_sha256": "<64 lowercase hex>",
    "profile_name": "windows-debug",
    "profile_sha256": "<64 lowercase hex>",
    "context": {
      "run_id": "<controlled executor UUID>",
      "run_attempt": 1,
      "repository": "caesura-team/Caesura-AmeKAG",
      "workflow": "local/run_validation.py"
    },
    "expected_cache": {"CAESURA_LIVE2D": "OFF", "CAESURA_HAS_STEAM": "OFF"}
  }]
}
```

所有声明均必需，集合不能为空，ID唯一。路径使用证据根内的相对POSIX分隔，不接受绝对路径、`..`或链接穿越。外部执行收据必须位于bundle外；可信profile独立通过CLI传入，不能取bundle内的副本作为权威。U1会重读原始报告和required集合；错误源码、平台、配置、run/context、摘要、缺日志、失败、未声明跳过或运行时源码变化都会拒绝。`expected_cache`用于核对已经记录的配置；SDK ON只证明该profile中的配置/执行范围，不推导账号、模型或物理设备通过。

`package_receipt`使用共同字段 `id/kind/platform/configuration/bundle/manifest_sha256`，加上 `version`、`producer` 和 `expected_files`。后者是完整文件名到 `{ "kind": "file 或 directory", "sha256": "<外部摘要>" }` 的映射。`producer`遵循U22的固定来源合同；GitHub Actions需要provider、repository/repository_id、run_id/run_attempt、workflow_ref/workflow_sha及job_key。所有值来自此前独立认证的选择，不从包内自报值授予认证。

| 层 | 本入口实际检查 | 不由此推出 |
|---|---|---|
| execution | U1 profile全部必需检查及原始报告重解析，外部身份/摘要/配置匹配 | 未选profile、任意feature、物理设备或发布批准 |
| package_receipt | 最终包实际字节、U22原收据、版本、平台及producer上下文匹配 | 原始包运行日志重放、新引擎运行、GitHub实时认证 |
| package_runtime或设备类型 | 当前版本不支持并拒绝 | 不回退为较弱的receipt-only通过 |

报告的 `CURRENT_EVIDENCE_VERIFIED` 仅表示显式选定集合通过对应层校验，始终 `release_ready=false`、`hosted_authentication=NOT_PERFORMED`。包声明保留 `RAW_STAGE_LOGS_NOT_INCLUDED_NOT_REPLAYED`。JSON里的每项范围、未测项和错误必须随结果保留，不能只摘取PASS。Python接口的diagnostic模式仅供受控fixture/诊断使用，其结果不能成为current verified；生产CLI没有开启该模式的参数。

能力闭环矩阵另属源码扫描：Structural是结构关系，Test references是测试源码引用，Platform/Package/Observable是人工声明。它们不替代本指南的原始证据，也不表示行/分支覆盖率。
