---
name: ast-grep
description: 当需要对 Caesura (AmeKAG) 引擎进行 C++ 语法树级别的结构化代码搜索、宏查找、纯虚接口合规性分析或批量 AST 重构时使用。
---

# AST-Grep Structural Code Search & Refactor Guide

`ast-grep` (sg) 是一种基于抽象语法树（AST）的代码分析与重构工具。相比基于文本正则的 grep，它能精准识别 C++20 语言的语法结构（忽略注释、空格、格式差异），并支持语法模式匹配。

先确认当前环境已有 `ast-grep` CLI。模式中的 `$` 是 AST 元变量，PowerShell 和 Bash 均使用单引号保留字面值。工具不可用时继续用 `rg` 与源码审查，不自动安装或宣称执行过 AST 验证。批量重写先只读检查命中范围，遵守 AGENTS.md 并保留未提交修改。

## 常用命令与模式

### 1. 结构化搜索 (Pattern Search)
在项目根目录运行 `ast-grep`：
```bash
# 搜索所有调用 BackendRegistry 单例的地方
ast-grep run -p 'BackendRegistry::instance().$METHOD($$$ARGS)' -l cpp src/

# 搜索所有直接 new 模块对象的违规代码 (检查是否违背 AGENTS.md 模块边界)
ast-grep run -p 'new $TYPE($$$ARGS)' -l cpp src/
```

### 2. 检查纯虚类接口定义
```bash
# 查找所有包含纯虚方法的类
ast-grep run -p 'virtual $RET $FUNC($$$ARGS) = 0;' -l cpp src/
```

### 3. 结构化重构与替换 (Rewrite)
```bash
# 批量替换过时的后端获取方法
ast-grep run -p 'BackendRegistry::instance().getOldBackend()' -r 'BackendRegistry::instance().getRenderDevice()' -l cpp --inline
```
