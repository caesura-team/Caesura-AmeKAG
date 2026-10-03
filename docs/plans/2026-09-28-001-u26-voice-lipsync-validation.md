# U26 原生语音口型：当前 Windows 诊断记录（2026-09-28）

本记录是 [当前 U1–U29 计划](2026-09-05-001-refactor-runtime-foundation-plan.md) 的一次执行证据，不表示 U26、候选包或完整计划已完成。受误删恢复范围约束，本轮只在 `E:/CaesuraRecovery/20260924-1446` 的工作树与证据目录写入；原始 `D:` 检出未修改。下述原始日志、进程回执、模型素材和图像保存在该本地恢复目录，**没有随 Git 提交**，其他机器不能只凭本文件复现 PASS。

## 实现与能力边界

候选以 `e51e6e5add7dfcbe0c8946531326e793550d4bf6` 为基底，在未提交工作树实现 VOICE PCM 只读电平与代次、Cubism `ParamMouthOpenY` 包络、Lua `Live2D` 原生绑定、KAG 模型生命周期和 `[live2d_lip_sync]` 的 `manual`/`voice`/`off`。上下文临时句柄不进入存档；活动模型的保存被明确拒绝。Windows D3D11 动态目标首次挂接和模型重新加载中的失效纹理由真实图像失败定位并修正。`live2d_motion` 与 `live2d_expression` 的 KAG 执行仍未接线。

能力目录对原生口型仅在 Live2D 已编译且 Cubism 会话实际可用时声明 `supported`；`source=voice` 另外要求音频播放能力。无 SDK/静态 PNG 和 Web 不冒充 Cubism 口型。当前目录 SHA-256 为 `8de0aaa5c262c8a776b8a0336ccac8fc8f1a7339017bcabb51d59ff448c7dc34`。

## 当前原生与门禁证据

运行时代码与目录同步后的工作树指纹为 `ca1fd6ead64e27b09cd1c2629fb53748c9bbdc3d9318d53536cc300efc372e05`。该指纹在下列原生、Debug 和 Web 检查中保持稳定；后续仅文档与文档生成器更新会改变工作树指纹，不能把旧回执称作最终提交 SHA 的完整门禁。

| 范围 | 实际结果 | 本地原始证据 |
|---|---|---|
| SDK-ON KAG/Live2D L1/M5 | `voice`、`stopvoice`、重复名、缺失模型、未知模型、冲突参数、存读档事务共 7 条独立进程通过；476 个 owner 帧、35 张 PNG、25 组图像比较。失败命令保留旧上下文与模型；M5 验证保存拒绝、准备失败回滚和成功恢复后新模型默认关闭。 | `U26-L1-M5-RESULT-20260928.json`，SHA-256 `96c7db490383afd792b87606d5d654182e6ce834418c93c2ddf41fb811ef32f3` |
| 真正的设备输出路径 D1 | WinMM 后端、44.1 kHz、942 个 owner 帧、10 张 PNG、8 组比较通过；自然结束和显式停止均关闭嘴型。215 个不同的滤波观察序号是 VOICE 过滤器数据，**不是**硬件回调次数。未调用宿主混音。 | `u26-device-real-probe-01/device-attempt-01/acceptance-01.json`，SHA-256 `019113429303b41a6d2cbab162bfa2008f6d5d57cb6e4093499ff1b843b4ff06` |
| Windows Debug profile | 验证器接受本次诊断：完整 Debug 构建；C++ 1549/1549、453527 条断言；Lua 主 147/147、孤儿 57/57；CTest 71 通过、0 失败、`CaesuraHeadlessAiSmoke` 1 个按 profile 允许跳过。 | `u26-current-windows-debug-gate-01-bundle/e51e6e5add7dfcbe0c8946531326e793550d4bf6/9fcadeb4-d5bd-4968-b4bd-c4c2f84f52a9/windows-debug/manifest.json`，SHA-256 `b6251a4ba1151eac6208889594c93b6500dbaee4790b230cc3631a370ee4ac17` |
| Web 现有 suite | 先用本次 Lua 烘焙故事并完成 Vite 构建，再绑定该 Lua 执行：52 文件、638 用例通过，0 失败、0 跳过。它是 Node/jsdom/Vite 证据，不是实际浏览器或 Cubism 后端证据。 | `u26-current-web-check-01/web-acceptance-02.json`，SHA-256 `427eaf612d173e734408ca8c02e6730a99057160ad3adf303d705b4b0ac77616` |

先前 Haru M2/M3/M4 的真实 D3D11 渲染证据另见恢复目录 `U26-HARU-M3-M4-RESULT-20260928.md`；它们使用较早的源码指纹，不能替代本表当前指纹下的门禁。原始失败没有覆盖：首次 `stopvoice` 外部探针误要求数字句柄，而实际 `KAG.play_voice` 成功返回布尔值；Device 子进程实际退出 0 后，首次外层归档误把探针的说明性源码字段当成 Git HEAD。两处均在外部探针/审查器纠正，原进程结果和失败回执保留，没有为求绿色重播相同场景。

## 未完成与下一步

- `physical_audibility=NOT_MEASURED`：WinMM 后端和源总线 PCM/像素已核对，没有扬声器声压、回环采样或精确硬件回调时间证据。
- 当前候选是脏工作树诊断。文档同步后需审查最终补丁、确定提交/候选 SHA，并在 U29 的选定范围上重新绑定正式门禁与包；本记录不批准合并、标签、签名或发布。
- Steam 实际账号与云冲突、非 Windows 的真实 SDK/设备、U27 长跑性能及 U28/U29 全范围仍按当前计划和各自外部条件独立跟踪。
