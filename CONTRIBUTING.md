# Contributing to Caesura (AmeKAG)

感谢你考虑为 **Caesura (AmeKAG)** 贡献代码、文档或内容！本指南是贡献入口；
完整构章规则以仓库根目录的 [AGENTS.md](AGENTS.md)（权威宪章）为准，
操作命令见 [开发指南](docs/team/development-guide.md)，Codex 的技能、插件与 MCP 使用见 [Codex 工作流](docs/team/codex-workflow.md)。

- **想聊天/提问/晒作品** → 见 [docs/guides/community.md](docs/guides/community.md)（GitHub Discussions + 学习路径）
- **想直接改代码** → 继续往下读。

---

## 参与方式概览

| 角色 | 入口 | 说明 |
|------|------|------|
| 提问 / 求助 | GitHub Discussions →「提问」 | 常见问题、引擎/工具使用、编译环境 |
| 报告 Bug / 请求功能 | GitHub Issues（[bug_report](.github/ISSUE_TEMPLATE/bug_report.md) / [feature_request](.github/ISSUE_TEMPLATE/feature_request.md) 模板） | 用模板，附复现步骤 |
| 提交代码 / 文档 / 测试 | 本文档（fork → PR） | 见下方「PR 流程」 |
| 内容创作（用引擎做游戏） | Discussions →「作品展示」+ itch.io | 见 community.md（示例游戏打包入口） |
| 引擎开发（深入源码） | 本文档 + AGENTS.md §1–12 | 接口契约 / BackendRegistry / 耦合预算 |

---

## 项目是什么

Caesura (AmeKAG) 是跨平台视觉小说引擎：C++20 + bgfx 渲染、SDL3 窗口、
SoLoud 音频、Lua 5.4 脚本、Live2D / 3D 小游戏 / 云存档。核心语言是
**KAG Neo-Genesis** —— 脱胎于 KAG3 的新一代标签语法（保留 `[标签 参数=值]`
形态，语言核心与工具链完全现代化，KAG3 工程可导入迁移）。

---

## 快速开始

```bash
# Windows (MSVC)
cmake -B build -DCAESURA_LIVE2D=OFF
cmake --build build --config Debug --parallel

# macOS / Linux（FFmpeg 常不可用，关闭它）
cmake -B build -DCAESURA_LIVE2D=OFF -DCAESURA_ENABLE_FFMPEG=OFF
cmake --build build -j$(nproc)

# 从项目根目录启动（资源路径相对 CWD 解析）
./build/Debug/CaesuraAmeKAG.exe
```

> 完整的逐平台依赖安装、FFmpeg/Live2D 说明与 Smoke 自检清单见
> [docs/guides/getting-started.md](docs/guides/getting-started.md)。

---

## 模块边界（铁律摘要，完整见 AGENTS.md §1–4）

- 16 个内部模块（`src/archive` `audio` `debug` `di` `entry` `input`
  `job` `live2d` `minigame` `platform` `render` `resource` `rpc`
  `script` `steam` `storage`）通过 `api/` 子目录的纯虚接口（`I*.h`）对外暴露。
- **禁止模块间 include 具体实现头**；只允许 `I*.h`（接口必须纯虚、无数据成员）。
- 唯一例外：`src/entry/` + `src/main.cpp`（组合根）可以 new 具体后端。
- 所有后端访问必须走 `BackendRegistry`（`src/di/`），禁止绕过单例。
- 改接口走 AGENTS.md §10 五步流程（改 api → 更新实现 → registry → init → 全量门禁）。

---

## PR 流程

### 1. Fork → Clone → Branch

1. Fork 本仓库到你的 GitHub 账号。
2. Clone 并新建功能分支：
   ```bash
   git clone <your-fork-url>
   cd Caesura(AmeKAG)
   git checkout -b codex/<描述>   # 或 feature/<描述>
   ```
3. 小步、聚焦：一个 PR 解决一件事。分支命名用 `codex/<描述>` 或 `feature/<描述>`。

### 2. 提交格式（语义化提交）

`type(scope): description`

- **types**：`feat` `fix` `test` `docs` `review` `merge` `plan` `refactor` `chore` `perf` `ci`
- **scopes**：模块名（`render` `script` `storage` `rpc` …）或层（`p1` `p2` `kag` `backend`）
- 示例：`feat(palette): add day/night mode toggle`、`fix(backend): route render_text to KAG.render_text`
- 提交信息脚本中请勿带 emoji / 工具署名；保持简洁专业。

### 3. 合并前必须全绿（四套件门禁）

> 任一不通过即不可合并。全部命令从**仓库根**或标注目录执行。

| 套件 | 命令 | 通过标准 |
|------|------|----------|
| **全量构建** | `cmake --build build --config Debug --parallel` | 零错误 |
| **C++ 测试** | `cd build/tests/Debug && ./CaesuraTests.exe` | 0 failed, 0 skipped（doctest） |
| **C++ CTest** | `ctest -C Debug --test-dir build --output-on-failure` | 全过 |
| **Lua 主套件** | `external/lua/lua.exe tests/scripts/run_lua_tests.lua` | 全过（顺序敏感，新测试放 `test_sandbox` 前） |
| **Lua 孤儿套件** | `external/lua/lua.exe tests/scripts/run_orphan_tests.lua` | 全过（与主套件分开跑） |
| **Web 前端套件** | `cd web && npm test` | vitest 全过 |
| **Editor 前端套件** | `cd editor && npm test` | vitest 全过 |
| **耦合门禁** | `python scripts/count_coupling.py --ci` | PASS（entry/di/script ≤14，其余 ≤4） |
| **Web 索引守卫** | `node web/gen-index.mjs --check` | CHECK OK（改过 `scripts/*.lua` 先重跑 gen-index） |
| **格式** | `git diff --check`、`clang-format -i src/path` | 无空白错误，对齐 .clang-format |

**测试要求（AGENTS.md §8 强制）**：新增功能必须带测试；C++ 用 doctest
（`tests/cpp/test_<module>.cpp`，每个新模块至少构造不崩溃 + 核心功能）；
Lua 用 `tests/scripts/test_*.lua` 并注册进 `run_lua_tests.lua`；
渲染测试不应在无窗口环境创建真实 GPU 资源（默认构造 + 访问器验证）。
禁止合并导致测试数量减少或新增失败的 PR。

### 4. 提交 PR

- 用 [.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md) 模板。
- 关联 Issue（`Closes #NNN`）。
- 附验证清单：四套件门禁结果 + 模块边界合规。
- 保持 git 历史干净：按语义分提交，可考虑逻辑分组后合并。

---

## 测试规范（四套件详解）

```
tests/
├── cpp/test_<module>.cpp     # C++ doctest（构造 + 核心功能 + 访问器）
├── scripts/test_*.lua        # Lua 套件（主套件 run_lua_tests.lua + 孤儿套件 run_orphan_tests.lua）
├── mocks/                    # NullJobSystem 等同步测试替身
web/src/**/*.test.*           # Web 播放器 vitest 套件
editor/src/**/*.test.*        # Editor 前端 vitest 套件
```

- **C++**：`CaesuraTests` 链接与 App 相同的内部静态库（不重复编译），runner 报告权威用例总数，每个用例必须通过。
- **Lua**：主套件顺序敏感（sandbox 锁 `require`）；孤儿套件（创建全局 mock）必须与主套件分开跑，绝不合并。
- **Web / Editor**：两个独立 vitest 套件（`web/` 与 `editor/`），分别 `npm test`。

---

## 文档规范（5 类，AGENTS.md §12）

| 类别 | 目录 | 适用内容 |
|------|------|----------|
| `api/` | docs/api/ | API 参考（命令契约、Lua 模块、C++ 接口、RPC 端点） |
| `design/` | docs/design/ | 架构 / 设计 / 标准 / 能力矩阵 |
| `guides/` | docs/guides/ | 用户与开发者指南（含本社区文档 community.md） |
| `plans/` | docs/plans/ | 执行计划与记录（`YYYY-MM-DD-NNN-描述.md`） |
| `solutions/` | docs/solutions/ | 可复用经验 / 模式（YAML frontmatter 可搜索） |

- 新文档按 5 类归位；**禁止**将一次性执行提示词留在 docs/（执行后删除，仅留总结）。
- API 参考文档由脚本自动生成者为权威（如 `docs/api/command-contracts.md`）。

---

## 代码风格

- C++：clang-format（WebKit 风格，C++20，120 列，4 空格缩进，指针左对齐）：`clang-format -i src/path/to/file.cpp`
- Lua：遵循现有 `scripts/` 风格（4 空格缩进，`--` 注释）。
- 命名：模块目录全小写；接口 `I` 前缀 + PascalCase；实现 PascalCase；命名空间 `Caesura::`；include 用 `../<module>/` 相对或 src/ 根裸路径。

---

## 示例游戏与内容创作

- **《单程回信》（The One-Way Reply）**：仓库自带完整示例游戏，位于 `demo/example_game/`（现代校园 · 温情悬疑 · 短篇多结局，约 15–18 分钟）。改剧本只需编辑 `story.ks`；跑 `lua demo/example_game/entry.lua`。设计权威见 [demo/example_game/DESIGN.md](demo/example_game/DESIGN.md)。
- **打包分发**：`bash scripts/package_game.sh <你的游戏目录>` 一键打包为静态 Web 站（itch.io / GitHub Pages / Netlify），见 [docs/guides/packaging-ux.md](docs/guides/packaging-ux.md) 与 [docs/guides/community.md](docs/guides/community.md)（示例游戏发布入口）。
- 教程路径 01–16 见 [docs/guides/sample-library.md](docs/guides/sample-library.md)。

---

## 问题反馈

- Bug / 功能请求：用 Issue 模板（`.github/ISSUE_TEMPLATE/`）。
- 闲聊 / 提问 / 晒作品：GitHub Discussions（见 community.md）。
- 安全相关：请直接联系维护者（勿公开披露）。

感谢你的贡献！
