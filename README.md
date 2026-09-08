<div align="center">

<img src="docs/assets/branding/caesura-engine.png" alt="Caesura engine logo" width="144" height="144">

# Caesura (AmeKAG)

**Next Generation of Visual Novel**

**现代化跨平台视觉小说引擎 · KAG Neo-Genesis 脚本 · C++20 内核**

*A modern, cross-platform visual novel engine — KAG Neo-Genesis scripting on a C++20 core.*

[![CI](https://github.com/caesura-team/Caesura-AmeKAG/actions/workflows/ci.yml/badge.svg)](https://github.com/caesura-team/Caesura-AmeKAG/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/caesura-team/Caesura-AmeKAG)](https://github.com/caesura-team/Caesura-AmeKAG/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20Web%20%7C%20Android%20%7C%20iOS-2ea44f)

[快速开始](#快速开始) · [教程与示例](#教程与示例) · [文档](#文档) · [平台支持](#平台支持) · [参与开发](#参与开发)

</div>

---

Caesura 是面向程序员与独立团队的开源视觉小说引擎。剧本语言是 **KAG Neo-Genesis** —— KAG
脚本语言的现代化迭代（由 KAG3 演化而来、兼容 KAG3，旧工程可导入迁移）；从写下第一行剧本，
到把游戏打包成 Windows / Linux / macOS / Web 四个平台的成品，全部工作流在一个仓库内完成。

引擎内核为 C++20：bgfx 渲染、SDL3 窗口、SoLoud 音频、Lua 5.4 脚本 VM —— 16 个静态模块库、
34 个纯虚接口、零循环依赖（[实时 API 普查](docs/api/api-stats.md)）。

**当前开发方向：底层优先，Studio 暂停。** 后续工作以运行时正确性、状态恢复、真实后端与交付验证为主，进度和验收范围见[当前计划](docs/plans/README.md)。已有编辑器入口保留供维护，创作入门以引擎和 CLI 为主。

## 核心特性

**创作体验**

- **KAG Neo-Genesis 剧本语言**：134 个声明式契约命令——对白、选择支、存档/回滚/履历、NVL、参数化宏、内联文本标记、i18n 热切换（[命令参考](docs/api/command-contracts.md)）
- **KAG + Lua 混合脚本**：`[eval]` / `[iscript]` 在剧本内嵌 Lua，`kag.*` API 反向驱动剧情；也可以纯 Lua 直驱引擎
- **Caesura Studio（开发暂停）**：仓库保留编辑器与 RPC 实现；现阶段不作为完整创作流程的就绪承诺，底层与 CLI 的开发优先。
- **caesura 命令行**：`doctor` 环境体检、`create` 五模板脚手架、`check` 剧本契约校验、`flow` 分支图与死分支诊断、`i18n` 本地化管线、`build` / `package` 一键出包

**引擎内核**

- **渲染**：bgfx 多后端（D3D11 / OpenGL 已真机验证，Metal 收尾中）——三层合成、GPU 粒子、视频播放、转场特效、后处理链（bloom / vignette / LUT）、FreeType CJK + Ruby 注音
- **音频与动画**：SoLoud 三总线（BGM / Voice / SE）、Live2D（Cubism SDK 可选）、SMA 骨骼动画、3D 小游戏场景
- **存档系统**：加密存档 + 版本迁移 + 回滚快照 + 履历回看；Steamworks 集成（成就 / 统计 / 云存档）
- **分发安全**：CARC 加密归档（AES-256-GCM + Ed25519 签名），发布包自包含并附 30 项自动验证

**质量保障**

- 三平台 CI（Windows MSVC / Linux GCC / macOS Clang，另有 iOS / Android 编译探针），每次提交经 C++ / Lua / 编辑器 / Web / Golden 项目多层测试门禁
- 崩溃诊断面向玩家友好呈现（项目/场景/命令定位 + 日志目录直达），ErrorUI 零 GUI 框架依赖、崩溃时也能显示错误

## 快速开始

> 完整入门（前置依赖、三平台差异、常见问题）：**[docs/guides/getting-started.md](docs/guides/getting-started.md)**

**路径 A —— 发布包**：从 [Releases](https://github.com/caesura-team/Caesura-AmeKAG/releases) 下载解压即用（各版本世代差异见入门指南）。

**路径 B —— 源码构建**（Windows 需 vcpkg 提供 SDL3；Linux / macOS 见指南）：

```bash
git clone --filter=blob:none https://github.com/caesura-team/Caesura-AmeKAG.git   # 部分克隆 ~25MB
cd Caesura-AmeKAG
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug --parallel
```

构建完成后，可通过 CLI 创建、校验并组装第一个游戏：

```powershell
python scripts/caesura.py create my_vn --template basic
python scripts/caesura.py check my_vn/story.ks
python scripts/caesura.py build my_vn --engine build/Debug/CaesuraAmeKAG.exe --config Debug
```

随后从生成的 `dist/my_vn-game/` 目录启动 `CaesuraAmeKAG.exe`。`caesura build` 使用已有引擎二进制组装游戏目录；需要 ZIP 时运行 `python scripts/caesura.py package my_vn --target windows --engine build/Debug/CaesuraAmeKAG.exe`。具体操作与验证边界见[开发指南](docs/team/development-guide.md)。

## 教程与示例

| 内容 | 位置 | 说明 |
|---|---|---|
| 16 步教程 | `demo/tutorial/` | 从 hello 到 tween，覆盖全部核心命令 |
| 示例游戏《单程回信》 | `demo/example_game/` | 三结局完整短篇（The One-Way Reply，约 15–18 分钟） |
| 项目模板 ×5 | `tools/project_templates/` | blank / basic / kag3 / live2d / showcase |
| Golden 项目 | `tests/projects/golden_vn/` | 端到端质量基准（对白/选择/回滚/履历/存读档） |

## 文档

| 主题 | 文档 |
|---|---|
| 入门：从克隆到第一个游戏 | [guides/getting-started.md](docs/guides/getting-started.md) |
| KAG3 兼容性与迁移 | [compatibility.md](docs/compatibility.md) |
| KAG 命令参考（134 条契约） | [api/command-contracts.md](docs/api/command-contracts.md) |
| 表达式语言（`[if]` / `[eval]` / `${}`） | [api/kag-expression-language.md](docs/api/kag-expression-language.md) |
| Lua API 参考 | [api/lua-modules.md](docs/api/lua-modules.md) |
| 编辑器 RPC API | [api/editor-api-reference.md](docs/api/editor-api-reference.md) |
| 引擎架构拓扑 | [design/engine-architecture-topology.md](docs/design/engine-architecture-topology.md) |
| 能力矩阵（82 项） | [design/engine-capability-matrix.md](docs/design/engine-capability-matrix.md) |
| 资源管线 | [guides/asset-pipeline.md](docs/guides/asset-pipeline.md) |
| 发布流程 | [guides/release-process.md](docs/guides/release-process.md) |
| 构建、测试、CLI 与受控验证 | [team/development-guide.md](docs/team/development-guide.md) |
| Codex 项目工作流与技能 | [team/codex-workflow.md](docs/team/codex-workflow.md) |

## 平台支持

| 平台 | 状态 | 产物 |
|---|---|---|
| Windows | 稳定 | ZIP（CPack，30 项自动验证） |
| Linux | 稳定 | TGZ |
| Web | 稳定 | 静态站点（现代浏览器） |
| macOS | 收尾中 | TGZ（打包链路已验证，Metal 渲染修复中） |
| Android / iOS | 推进中 | CI 编译探针绿；真机链路详见平台矩阵 |

平台矩阵与验证证据：[docs/status/platform-status.md](docs/status/platform-status.md)

## 参与开发

- **[AGENTS.md](AGENTS.md)** 是本仓库的工程宪章：模块边界、接口规范、BackendRegistry 依赖注入、耦合预算——动手前必读。
- 合并门槛：全量构建零错误 + C++ / Lua / CTest 满足当前验证要求；发现数、失败和跳过以实际运行结果报告（命令见[开发指南](docs/team/development-guide.md)，协作方式见[Codex 工作流](docs/team/codex-workflow.md)）。
- 提交信息遵循 `type(scope): description`（`feat` / `fix` / `test` / `docs` / …）。

## 赞助项目

欢迎通过 **微信支付** 或 **支付宝** 支持 Caesura 的开发与维护：[查看收款码与赞助方式](docs/guides/sponsorship.md)。感谢每一份支持。

| 微信支付 | 支付宝 |
| :---: | :---: |
| <img src="docs/assets/sponsorship/wechat.jpg" alt="微信支付收款码" width="320"> | <img src="docs/assets/sponsorship/alipay.jpg" alt="支付宝收款码" width="320"> |

## 许可证

[MIT](LICENSE)
