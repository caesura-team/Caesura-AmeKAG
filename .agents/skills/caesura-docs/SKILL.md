---
name: caesura-docs
description: 同步 Caesura 的工程约束、Codex 工作流、开发指南、生成文档和执行记录，检查链接与来源，不把旧计划当当前授权。
---

# Caesura 文档维护
先确定文档用途和权威来源：AGENTS.md是工程规范；docs/plans/README.md是唯一计划入口；源码/CLI/真实receipt是事实。
- 按用户范围扫描根文档、docs及项目技能，区分维护中的规范、历史执行记录、自动生成参考和一次性运行痕迹。
- API参考放 docs/api，架构放 docs/design，指南放 docs/guides，执行记录放 docs/plans，经验放 docs/solutions，团队流程放 docs/team。
- 使用现有 api_stats、平台/能力矩阵与计划事实生成器；不为文档迁移重测或改写历史平台结论，不凭空新增CODEMAPS/CONTRIB/RUNBOOK路径。
- 构建和测试命令从当前CMake/脚本 --help / package scripts核对，删除固定旧计数和未安装插件的强制命令。
- Codex行为参考官方文档和当前可用工具；技能、插件、MCP、子代理与应用任务分别说明。
- 清理用户授权的一次性提示词/旧宿主约束前，核对所有引用与必要源码/夹具用途；本地删改与GitHub发布状态分别验证。
- 内部Markdown链接/技能入口必须存在；链接到历史删除文件时改为Git历史引用或明确标记历史，不能留下失效的当前执行入口。
总结扫描范围、实际修改、保留历史的理由及未验证内容；普通文档编辑不强制运行引擎全套。

详细执行约定见 [Codex 工作流](../../../docs/team/codex-workflow.md)。
