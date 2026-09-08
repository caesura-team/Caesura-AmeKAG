# Caesura 开发操作指南

本指南供维护者和 Codex 在仓库内开发时查阅，迁自原有操作手册。[AGENTS.md](../../AGENTS.md) 规定模块边界、接口、命名、耦合预算和合并门槛；[Codex 工作流](codex-workflow.md) 规定任务接续、技能选择和证据复用。本指南提供构建、测试与源码导航，不替代这两份规范。

当前计划从 [plans/README.md](../plans/README.md) 进入。2026-09-05 重新规划后，旧排期、冻结规则和执行授权均不再生效，产品方向保持**底层优先、Studio 暂停**。历史完成声明需与当前源码和对应验证产物核对。

## 开始工作

在当前任务中先确认工作目录和适用规则，保留已有未提交内容。执行 Git 命令前确认仓库或父目录存在 `.git`；它可以是目录，也可以是 worktree 使用的文件。确认后查看 `git status --short --branch`、`git diff` 和近期提交；没有 Git 元数据时继续非 Git 工作流。

按任务读取相关源码、当前计划、[项目记忆](project-memory.md) 和 [solutions/](../solutions/) 中的故障经验。会话接续时优先检查已经生成的证据，不要求每轮重复读取无变化文件或重跑同一检查。

Codex 的项目技能按需要选用：

| 工作 | 技能入口 |
|---|---|
| 需求分解和实现顺序 | [caesura-plan](../../.agents/skills/caesura-plan/SKILL.md) |
| 行为修复与测试先行 | [caesura-tdd](../../.agents/skills/caesura-tdd/SKILL.md) |
| 变更审查 | [caesura-review](../../.agents/skills/caesura-review/SKILL.md) |
| 构建故障定位 | [caesura-build-fix](../../.agents/skills/caesura-build-fix/SKILL.md) |
| 验证范围与证据 | [caesura-validate](../../.agents/skills/caesura-validate/SKILL.md) |
| 文档同步 | [caesura-docs](../../.agents/skills/caesura-docs/SKILL.md) |
| 输入、认证和安全边界 | [caesura-security](../../.agents/skills/caesura-security/SKILL.md) |
| 有边界的重构 | [caesura-refactor](../../.agents/skills/caesura-refactor/SKILL.md) |

使用当前会话实际提供的工具和模型。独立子任务适合并行时再分配明确的文件写入范围；不规定固定代理数量。

## Shell 与文件保护

Windows 示例使用 PowerShell。脚本需要 Bash 时显式调用该解释器，不切换整个开发流程。带空格的可执行文件路径用调用运算符 `&`；文件操作使用 `-LiteralPath`。PowerShell 版本会影响管道和文本编码行为，写源码与文档时显式使用 UTF-8 或补丁工具，不依赖重定向的默认编码。

大文件优先局部编辑。终止后台进程前检查 PID、命令行和归属，避免按 `node.exe` 等进程名批量结束宿主或 IDE。提交正文使用实际换行和明确的文件编码，避免 Shell 把 `${...}`、反引号等正文当代码执行。

## 构建与运行

以下命令从仓库根目录执行。已有构建目录应先核对其 CMake cache、生成器、架构与工具链，避免把不同配置混到同一目录。依赖安装和跨平台细节见[入门指南](../guides/getting-started.md)。

```powershell
# Windows / MSVC；SDL3 等依赖按本机工具链配置
cmake -S . -B build -DCAESURA_LIVE2D=OFF
cmake --build build --config Debug --parallel

# 普通引擎入口
& './build/Debug/CaesuraAmeKAG.exe'
```

单配置生成器应在配置阶段指定 `-DCMAKE_BUILD_TYPE=Debug`。Linux/macOS 可使用 `cmake --build build --parallel`；FFmpeg 不可用时可在配置阶段指定 `-DCAESURA_ENABLE_FFMPEG=OFF`，并明确该构建的能力范围。

| CMake 选项 | 当前默认与用途 |
|---|---|
| `CAESURA_LIVE2D` | `OFF`；启用需提供 `CUBISM_SDK_ROOT`，见 [Live2D 指南](../guides/live2d-setup.md) |
| `CAESURA_ENABLE_FFMPEG` | `ON`；控制 FFmpeg 视频解码集成 |
| `CAESURA_DEBUG` | 随构建配置处理调试日志和断言，具体见根 CMake 配置 |
| `CAESURA_HAS_STEAM` | `OFF`；启用 Steamworks 集成还需相应 SDK |
| `CAESURA_REQUIRE_TEST_PREREQUISITES` | `OFF`；正式验证 profile 要求 `ON` 时，缺失验证前置条件会使配置失败 |

选项权威定义在 [CMakeLists.txt](../../CMakeLists.txt)。引擎命令行定义在 [src/main.cpp](../../src/main.cpp)：`--resource-root <dir>` 选择资源根，`--frames <N>` 限制帧数，`--backend <name>` 指定已编译的渲染后端。退出码为零不等于真实渲染成功，还应检查实际后端、降级日志和所需的画面行为。

现有 RPC 维护入口：`--editor` 启动本机 HTTP 服务（默认 `127.0.0.1:9876`，隐藏 GPU 窗口）；`--editor-stdio` 启动 stdin/stdout JSON-RPC。HTTP 认证使用 `CAESURA_EDITOR_TOKEN`，未设置时由程序生成；端口可通过 `CAESURA_EDITOR_PORT` 配置。两者是现有工具链维护入口，Studio 功能开发仍暂停。端点说明见 [RPC API](../api/editor-api-reference.md)。

## 测试与检查

先跑能覆盖本次行为的定向检查，再按 [AGENTS.md](../../AGENTS.md)、变更范围和当前验证 profile 完成所需门槛。全量 Debug 构建与 `CaesuraTests` 全绿是合并要求；不能用某一 Lua 文件或 Web 测试通过代替原生验收。

```powershell
# C++：资源路径要求测试程序在此目录运行
Push-Location 'build/tests/Debug'
try {
    & './CaesuraTests.exe'
} finally {
    Pop-Location
}

# doctest 定向过滤也要在相同 CWD 执行
# & './CaesuraTests.exe' '-tc=*SaveManager*'
# & './CaesuraTests.exe' '-ts=*Render*'

ctest --test-dir build -C Debug --output-on-failure

# Lua：从仓库根目录运行，使用本次构建的 lua_cli 产物
& './build/lua/Debug/lua.exe' tests/scripts/run_lua_tests.lua
& './build/lua/Debug/lua.exe' tests/scripts/run_orphan_tests.lua

# Web 与编辑器的现有测试；按变更影响选择
npm --prefix web test
npm --prefix editor test

python scripts/count_coupling.py --ci
python tests/scripts/check_test_coverage.py
```

以上测试命令相互独立；自动执行时逐项保留退出码和原始输出，不以最后一条命令的成功掩盖前面的失败。测试数量由本次运行器实际发现结果决定；缺失、失败和跳过都要报告，不能手写固定总数当作通过证据。Lua 主套件有沙箱与全局状态顺序约束；孤儿套件的隔离运行方式由 [run_orphan_tests.lua](../../tests/scripts/run_orphan_tests.lua) 管理，不应为凑总数直接并入主套件。

C++ 测试列表在 [tests/CMakeLists.txt](../../tests/CMakeLists.txt)，测试链接应用使用的模块库。无 GPU 的测试应使用接口替身或构造／访问器检查；真实 GPU 结果需要相应环境的证据。源码检出中的 Lua 位置由构建树决定，发布包可能使用 `external/lua/lua[.exe]`；不要把旧的本地副本当作本次构建产物。

接口变化后检查全部实现、注册表与组合根，并运行 `python scripts/api_stats.py` 同步 [API 普查](../api/api-stats.md)。新增运行时 Lua 模块需核对 [kag/init.lua](../../scripts/kag/init.lua) 沙箱预加载，以及 Web 的 `node web/gen-index.mjs` 索引更新。C++ 格式以 [.clang-format](../../.clang-format) 为准。

## 创作者 CLI

命令定义见 [scripts/caesura.py](../../scripts/caesura.py)。`build` 组装已有引擎二进制和游戏资源，不调用 CMake 编译引擎；相关实现见 [caesura_build.py](../../scripts/caesura_build.py)。

```powershell
python scripts/caesura.py doctor
python scripts/caesura.py create my_vn --template basic
python scripts/caesura.py check my_vn/story.ks
python scripts/caesura.py flow my_vn --lint --format mermaid
python scripts/caesura.py build my_vn --engine build/Debug/CaesuraAmeKAG.exe --config Debug
python scripts/caesura.py package my_vn --target windows --engine build/Debug/CaesuraAmeKAG.exe
python scripts/caesura.py package my_vn --target web
```

`create` 会拒绝已存在的目标目录；打包请选择本次输出路径并保留现有产物。桌面组装后从输出目录运行引擎，保证资源根一致。Web 打包由 Node 实现；构建、静态校验、浏览器运行和发布验证分别说明，不能合并声称已验证。

## 受控验证与 RC 证据

真实执行由 [run_validation.py](../../scripts/run_validation.py) 记录，检查集合由 [validation_profiles.json](../../scripts/validation_profiles.json) 指定。[verify_release_candidate.py](../../scripts/verify_release_candidate.py) 重新解析原始报告，并将结果绑定到 profile、源码身份和外部可信执行回执；它不会自动批准发布。

以下是需要正式执行验证时的调用示例，本指南没有执行这些命令或生成证据。将日期占位符换成本次唯一目录，保留已有记录；运行前应按选定 profile 配置 `CAESURA_REQUIRE_TEST_PREREQUISITES=ON`。

```powershell
$runDir = 'artifacts/validation/windows-debug-YYYYMMDD-HHMMSS'
$bundleDir = 'artifacts/validation/windows-debug-bundle-YYYYMMDD-HHMMSS'
python scripts/run_validation.py --profile scripts/validation_profiles.json --profile-name windows-debug --configuration Debug --run-dir $runDir

# 只有上一步真实完成并产生 run.json 后，才组装并校验证据
python scripts/verify_release_candidate.py --generate-bundle --run "$runDir/run.json" --profile scripts/validation_profiles.json --profile-name windows-debug --expected-run "$runDir/run.json" --artifacts-dir $bundleDir

# 后续可只读复核同一份证据，不重复执行测试
python scripts/verify_release_candidate.py --check --profile scripts/validation_profiles.json --profile-name windows-debug --expected-run "$runDir/run.json" --artifacts-dir $bundleDir
```

`--expected-run` 必须来自证据包外的可信执行边界，托管 CI 还需核对真实 workflow/run/attempt/artifact 身份。需要固定源码时，`--commit` 接受完整小写提交 SHA。脏工作树只能作为诊断证据，并需显式使用 `--diagnostic`；不能升级成干净源码的发布证明。缺失可选证据使用 `--skip-if-missing` 时退出 77，含义是跳过，不是通过。旧的合成 PASS 清单和手工 RC-GO 声明不具备执行证明效力。

## 架构导航与持续记录

组合根为 `src/main.cpp` 和 `src/entry/`：创建具体后端，组装 `EngineConfig`，由 `Engine::init()` 注册到 `BackendRegistry`；其他模块经接口访问后端。图形句柄使用引擎接口定义，不向外泄漏第三方实现类型。详细模块关系见[架构拓扑](../design/engine-architecture-topology.md)与[后端依赖指南](../design/backend-registry-dependency-guide.md)。

Lua 运行时位于 `scripts/`，KAG 调度入口为 `scheduler.lua`，命令实现位于 `scripts/kag/commands/`，原生绑定位于 `src/script/bindings/`。命令契约、表达式、Lua 和 C++ 接口分别见 [command-contracts.md](../api/command-contracts.md)、[kag-expression-language.md](../api/kag-expression-language.md)、[lua-modules.md](../api/lua-modules.md) 和 [cpp-interfaces.md](../api/cpp-interfaces.md)。

既有 UI 架构把游戏 UI 放在 Lua 层，ErrorUI 使用直接渲染以减少故障显示对 UI 框架状态的依赖；不要因维护工具面板而顺手引入新的即时模式 GUI 框架。相关调整需有独立的架构理由和当前计划依据。

提交使用 `type(scope): description`，常用类型遵循 AGENTS.md；新分支默认 `codex/<description>`。是否提交、推送或发布取决于当前任务授权。长期经验放 [team/](./) 或 [solutions/](../solutions/)，执行记录放 [plans/](../plans/)，API／设计／使用指南按各自目录归类。记录实际改动、验证范围、未验证边界和下一步，不以文档声明替代结果。
