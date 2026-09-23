# U29 底层整合候选执行记录

本记录对应唯一[运行时底层计划](2026-09-05-001-refactor-runtime-foundation-plan.md)的整合准备；目标仍为完整U1–U29，底层优先、Studio暂停。工作树为 `artifacts/validation/u29-worktree`，分支 `codex/u29-foundation-integration`。下列本地merge提交仅保存可审查的源码组合，不表示通过合并门禁、进入master或发布。

## 2026-09-24 来源与冲突处理

从U27干净 `c25d81ebfb324b85b54252ce937039c6dc790f28` 创建隔离工作树，依次整合：

| 增量 | 输入提交 | 本地整合提交 | 实际处理 |
|---|---|---|---|
| U23（包含U22生产修复） | `ea0281bca942bcc218047fe6021517c388145697` | `8a574e454db19dadc05d9c6ff0df0e18008b3f2f` | 保留U27独有74路径及U23独有23路径原blob；加3项Mac维护CTest，下限62→65 |
| U24 | `4bb5426a75e2f9f63dcd533ad348fac3c58372a6` | `ddc9b77a36cfc4adae8f9609b0196e479404ae2b` | 保留Mac安装与cache排除，加入原Android输出复制guard和3项维护CTest，下限65→68 |
| U26 | `cbf5904bd6b10b69027dcf4f1b963a1554849eac` | `cddec53e20a0fc20542e74d090a20ef02a28f0b7` | 云恢复生产源码与incoming原blob相同；追加2项native冷进程CTest，下限68→70；6组native C++下限各+40 |

U2 Expo源码 `f646b820` 已是U27起点的祖先，不重复整合；这不补齐U2当前源码的跨平台或sanitizer执行。U26整合后的C++发现下限为Windows1517、Linux1473、macOS1360，均尚待实际发现和执行。官方API扫描得到16模块、41接口头、457纯虚方法，统计文档由 `scripts/api_stats.py` 生成；API参考总数同步为41。平台文档冲突暂保留历史执行行，后续由维护中的生成器同步，未把旧证据改为整合通过。

三次独立静态审查均未发现可行动的整合问题；报告位于本工作树 `artifacts/validation/integration-01/`。U23审查SHA256为 `a6890599f596e077f723b4e88cfa6ce2692dbac714d44834c136e08b1b29931d`；U24为 `3077f59c9c6bcac1004cd6c6ba0349b77d36bb609929b57a16bbdbcd7d12f4b6`；U26为 `12ce622ea81c596f7b9dccbbfa4ad8b15a823a01ca945f8f09964f24a45ae001`。这些报告证明来源保留、CMake注册合成、JSON政策和静态API计数，不证明运行行为。当前尚未在整合工作树执行完整配置、构建、C++/Lua/CTest、包或真实后端验证。

## 待完成的整合出口

先完成U28声明和证据边界检查，再冻结整合候选与运行前required范围。完整Debug/Release、平台配置、最终包、冷恢复、真实后端、性能/长跑及AE1–AE8/回退演练均按原计划承接。原c25分支的一小时运行及后续CPU比较保持原源码/二进制锁，不能因本地整合将它们改标为U29通过；整合候选需要自己的适用完整门禁。设备、真实SDK账号、签名与发布分别保留边界，不减少required集合取得绿色结论。
