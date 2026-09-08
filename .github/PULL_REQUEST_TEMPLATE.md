## 变更摘要
<!-- 用一句话描述这个 PR 做了什么 -->

## 关联 Issue
<!-- Closes #NNN -->

## 变更内容
- 

## 验证（合并前必须全部通过）
<!-- 按 AGENTS.md 与当前 profile 填写实际证据；未执行不得勾选。测试数量由本次发现结果产生。 -->

- [ ] 全量构建零错误：`cmake --build build --config Debug --parallel`
- [ ] `CaesuraTests.exe` 全绿（0 failed, 0 skipped，从 `build/tests/Debug/` 运行）
- [ ] Lua 主套件：`build/lua/Debug/lua.exe tests/scripts/run_lua_tests.lua`
- [ ] Lua 隔离套件：`build/lua/Debug/lua.exe tests/scripts/run_orphan_tests.lua`
- [ ] `ctest -C Debug --test-dir build --output-on-failure` 的全部必需检查通过；可选跳过按既定 profile 单列
- [ ] 受影响的 Web 集成及真实后端验证通过，实际计数和未测范围已说明
- [ ] 耦合门禁：`python scripts/count_coupling.py --ci` PASS
- [ ] `git diff --check` 无空白错误
- [ ] 新增功能带测试（doctest / Lua 套件）
- [ ] 证据对应当前源码、配置和实际产物，没有用历史全绿替代本次验证

<!-- Windows 命令示例；其他配置使用对应构建产物。Git 操作前先确认 .git 存在。操作细节见 docs/team/development-guide.md。 -->

## 模块边界合规
<!-- 勾选适用的项 -->
- [ ] 只通过 `api/` 接口跨模块（未 include 具体实现头）
- [ ] 后端访问走 `BackendRegistry`（未绕过）
- [ ] 未在非组合根位置 new 具体后端
- [ ] 模块目录全小写，命名遵循 `Caesura::` / `I` 前缀规范

## 附加上下文
<!-- 性能数据、设计决策、已知限制 -->
