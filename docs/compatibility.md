# Caesura (AmeKAG) — Compatibility Policy

> **本文档是 1.x series 的权威兼容承诺。** 它定义了什么可以依赖、什么不可以、
> 跨版本升级时哪些行为会被保留、哪些会被安全地移除，以及出现破坏性变更时的
> 处理流程。

- 适用范围：**1.x series**（当前 `v1.0.1`）。
- 状态：**生效中**（Phase 0「1.x 稳定化」交付物，见 `docs/plans/audit/Caesura-AmeKAG_产品化推进总任务书.md` §5）。
- 变更纪律：任何承诺修改必须走 Breaking-Change 流程（§8），并同步更新 `command-contracts.md` / `api-stats.md`。

---

## 1. 前言与适用范围

Caesura (AmeKAG) 是跨平台视觉小说引擎（C++20 / bgfx / SDL3 / SoLoud / Lua 5.4）。
从 v1.0.x 起，产品目标从「功能丰富」转向「**可预测、可依赖、升级安全**」。

本文件回答四个问题，对应 Phase 0 的四个目标：

1. **脚本兼容**：1.x 里的 KAG/Neo-Genesis 脚本，1.y 还能不能原样跑？
2. **存档兼容**：玩家在 1.x 存的档，升级后能不能继续读？
3. **API 可预测**：KAG 命令、Lua API、C++ 接口、RPC 哪些有稳定性承诺？
4. **升级安全**：1.0 → 1.1 → 1.2 → 2.0 怎么迁移，作品怎么不被破坏？

### 1.1 兼容性承诺的边界

- 本文件约束 **1.x series 内部**（1.0 升级到 1.1/1.2）。
- **2.0** 迁移策略见 §7：允许有限制的 breaking（仅加工艺定义的迁移路径）。
- 未列入「稳定承诺」的部分视为**不保证稳定**（见 §8 exception 列表）。

---

## 2. KAG3 compatibility 范围

Caesura 不是 KAG3 逐字节克隆，而是「KAG 语法继承之上的新一代标准（KAG Neo-Genesis）」。
KAG3 兼容是**有边界的**——清单内兼容，清单外不假装支持。

### 2.1 明确兼容（可依赖）

| 能力 | 说明 | 依据 |
|------|------|------|
| 裸位置参数 13 families | KAG 3.0 兼容别名族（`r`/`s`/`delay`/`wait`/`se`/`voice`/`play`/`jump`·`call`·`link`/`goto`/`unlock`/`macro`/`shake`/`quake` 等 13 族）支持裸位置参数 | `docs/api/kag-commands.md`「KAG 3.0 Compatibility Aliases (13)」 |
| TJS 表达式运算符 | `&&` `||` `!` `!=` `?:`（三元）`??`（空合并）在 `[if]`/`[eval]`/`${}`/`[switch exp=]` 自动翻译为 Lua；字符串字面量感知 | `docs/api/kag-expression-language.md` |
| 旧变量 `%f.x%` | KAG3 风格 `%f.name%` / `$f.name` 旧表访问在文本插值中保留 | `docs/api/kag-expression-language.md` |
| `[elsif]` | `[elsif]` 作为 `[elseif]` 的 KAG3 拼写兼容别名 | `engine-capability-matrix.md` (S2f) |
| `[call *label]` | 场景内 `*label` 子例程调用（栈式） | `engine-capability-matrix.md` (S2f) |
| `[end] → ending` | `[end]` 结束剧本执行并进入结局结算 | `engine-capability-matrix.md` (S2f) |
| `[goto]` → `[jump]` | `[goto *label]` 是 `[jump]` 的严格别名（`kag3_import` 转换，运行时同样接受） | `docs/api/kag-commands.md` |

### 2.2 明确不兼容（不假装支持）

- 标记 **experimental / unverified** 的能力（§8）**不构成兼容承诺**。
- KAG3 老旧不合理逻辑（如裸值参数不规范化、无界流程）**不**保证逐字节复现；以 Neo-Genesis 语义为准。
- 清单外 KAG3 标签结**不保证**原样运行——迁移走 `docs/guides/kag3-migration.md`（6 步流水线：xp3 解包 → tlg → png → 音频 → `kag3_import --strict` → 资产路径重写 → 验证）；`docs/api/kag-commands.md` 已被 `command-contracts.md` 取代。

---

## 3. KAG Neo-Genesis syntax 稳定性

`docs/api/command-contracts.md` 是 **134 个 KAG Neo-Genesis 命令**的声明式契约参考（自动生成，权威；计数随生成器演进，以该文件头部自述为准）。

### 3.1 稳定性承诺

1. **命令集合稳定**：已发布命令（含分类、阻塞性）在 1.x 内**不删除、不重命名**。
2. **参数 schema 稳定**：name / type / default / required 不做破坏性修改。
3. **别名稳定**：KAG3 兼容别名（§2.1）保持生效。
4. **宏覆盖语义稳定**：宏名冲突按确定性规则裁决——**非专用分支命令**（如 `[text]`）的宏**覆盖**内置处理器；**专用分支命令**（`[jump]`/`[call]`/`[if]` 等流控/分支命令）仅**静态安全宏**（顶层定义、无 erase 依赖）在编译期内联覆盖，动态宏在运行期被分支命令**胜出并静默忽略**（依据 `scripts/scheduler.lua:348` 静态安全宏判定；`kag/compiler.lua`/`scheduler.lua` 派发链）。

### 3.2 参数语义规则

| 语义 | 承诺 |
|------|------|
| default | 已声明默认值在 1.x 内不变（否则可能改变脚本结果） |
| typing | 值按 schema 声明类型解析；非法类型走「清晰报错」而非静默换语义 |
| clamping | 超 Range 按声明钳制（如 `[save]` 槽位 0..99、`[camera]` 0..2000）；边界不随 minor 变化 |
| 插值 | `interpolate = true` 字段展开 `$tbl.key` / `%tbl.key%` / `${expr}` |

### 3.3 高亮禁止的 breaking 范围

1.x 内**禁止**（除非经 §8 exception 流程 + Golden-Project/迁移测试）：命令名/分类/阻塞性翻转；
default/range/type 破坏性改变；已发布参数移除；i18n 语言键与存档 schema 字段删除。

任何新增**扩展**（新命令/新参数/新可选字段）必须**只增不删**、缺省行为与旧版一致。

---
## 4. Save compatibility

### 4.1 存档格式

以下存档说明于 **2026-09-05 按当前源码同步**，不代表最新构建或回归已经通过。

- **内容**：SaveManager 将 `schema_version`、`timestamp`、`scene`、`token_index`、`thumbnail`、`engine_version` 和 `data` 一起组成 JSON 信封，再对整个 `envelope.dump()` 编码。
- **加密布局**：`CAES`（4B）+ nonce（12B）+ GCM tag（16B）+ ciphertext。设置 key 后，**整个 JSON 信封都在密文内**；上述元数据不是明文文件头，并随密文接受 AES-256-GCM 认证。CAES 外层没有独立版本字段，JSON `schema_version` 属于加密内容。
- **存储职责**：`ISaveProvider` 只传输原始字节；SaveManager 在调用 provider 前编码、读取后解码。本地默认 provider、自定义 provider、HTTP 和 Steam 路径遵循相同的 SaveManager 加密策略。
- **策略来源**：`SaveEncryptionPolicy` 由调用方选择，不能由待验存档自报。`EngineConfig::saveEncryptionPolicy` 默认 `Compatible`；C++ 可通过 `ISaveManager::setEncryptionPolicy()` 配置。策略不生成或持久化 key，key 仍由调用方通过现有接口提供。

| 策略 | 普通读取 | 保存 |
|---|---|---|
| `Compatible`（默认） | 可读合法旧明文；CAES 必须有正确 key 并通过认证 | 有 key 一律写 CAES，无 key 写明文 |
| `RequireEncrypted` | 拒绝非 CAES；CAES 必须有正确 key 并通过认证 | 缺 key 时拒绝写入；清除 key 不会清除策略，也不能覆盖旧档 |

识别为 CAES 后，缺 key、截断或认证失败直接终止，不能回退 JSON。`Compatible` 为保留旧明文兼容而允许整个文件被替换为合法明文 JSON；它不提供防此类替换的保证。两种策略都不提供槽位文件名绑定或旧密文防重放。

**显式旧明文导入**：`loadLegacyPlaintext(slot, outMeta)` 是 C++ 只读入口，可在严格策略下由调用方明确选择使用。它拒绝 CAES，不临时降低策略，不重写源文件；调用方取得数据后，提供 key 并显式 `save()` 才会写出密文。原档保留和目标槽位由调用方选择，没有自动批量迁移。

**云同步**：SaveManager 使用 `ICloudSaveTransport` 暂存一次读取的字节，按当前策略/key、JSON 信封和迁移结果验证，成功后才将**同一份原始字节**上传或提交到本地。Steam 的本地/云端端点明确区分；不会把 push 错接成 cloud→cloud。验证失败不写入目标，验证成功也不重新读取或重新加密。provider 的直接字节传输 API 不替调用方执行此策略，游戏应使用 SaveManager 的槽位同步 API。

实现依据：[SaveManager.cpp](../src/storage/SaveManager.cpp)、[ISaveManager.h](../src/storage/api/ISaveManager.h)、[ICloudSaveTransport.h](../src/storage/api/ICloudSaveTransport.h)。安全边界见 [存档安全说明](design/save-security-audit.md)。

### 4.2 schema 迁移链

- 存档 schema 版本 **v1 → v2 → v3 → v4 → v5** 自动升级链（`SaveManager` 的 `migrate()`，链式查找已注册迁移，步数上限 64 防环）。
- 加载时对 `data` 执行已注册的内存迁移，不自动覆盖存档，也不自动创建 `.bak`。只有调用方随后显式保存才写出当前格式。
- `load()` / `loadLegacyPlaintext()` 失败返回 null JSON，且保持调用方传入的 `SaveMeta` 不变；缺失/null `data`、迁移返回 null 或抛异常均属失败。合法空对象 `{}` 与空数组 `[]` 可以成功返回，不能仅用 `json.empty()` 判断加载失败。
- 当前实现保留未来 `schema_version` 不迁移透传的行为；这不等于已经验证新版本存档能被旧版本引擎正确解释。
- 权威声明：`docs/design/engine-architecture-topology.md`（storage 行「schema v1→v5 迁移」）、`docs/design/engine-capability-matrix.md` (C4)。

### 4.3 Golden Save 跨版本迁移承诺

- **Golden Save 语料**用于保留版本参考存档；升级回归需要检查「旧版本存档 → 新版本引擎 → 读档成功，且场景/变量/进度语义不丢失」。夹具存在本身不证明所有发布版之间已完成验证，结论以实际样本来源和运行记录为准。
- 1.x 内任何 schema 变更都必须通过 Golden Save 从最旧支持版本开始的完整链迁移。

### 4.4 字段扩展规则（只增不删）

- 允许：**只增**新字段（缺省值向后兼容）、扩充范围（如新增合法枚举值）。
- 禁止：删除字段、改写既有字段语义、改变 `schema_version` 以外字段的默认值导致旧存档读入后行为变化。
- `ctx.*`（Save/Load 场景状态快照）遵循同一规则——新增 `ctx` 字段不得破坏依赖旧结构的存档。

---

## 5. Project compatibility

### 5.1 project layout

项目根（相对 working directory / CWD）结构稳定：

```
<project>/
├── entry.lua          # KAG runner 启动入口（Lua）
├── story.ks           # 场景脚本（.ks，可多场景）
├── assets/            # 游戏资源根（相对游戏根解析）
│   ├── bg/ fg/ chara/ ui/ lut/     # 图像
│   ├── bgm/ se/ voice/             # 音频（wav|flac|mp3|ogg，voice 首选 wav）
│   ├── fonts/                      # 字体（otf|ttf）
│   ├── script/                     # 场景 *.ks（跨场景跳转以此为根）
│   ├── lang/                       # i18n 语言表（zh/en/ja .lua）
│   ├── video/ sma/ live2d/         # 视频 / 骨骼动画 / Live2D
└── mods/                           # 可选 mod 目录（见 5.4）
```

### 5.2 资产路径语义

- 资源默认从 **项目根的 `assets/`** 解析（引擎按 CWD 解析）。
- 场景文件固定位于 `assets/script/*.ks`（scheduler `is_safe_scene_path` 要求 `^assets/script/` 前缀 + `.ks` 后缀，禁止 `..` 穿越）；跨场景 `[jump]/[call]/[link]` 只解析 `assets/script/<target>.ks`。
- 子目录命名约定见 `docs/guides/asset-pipeline.md`。

### 5.3 entry.lua

- 项目入口是 **`entry.lua`**（KAG runner 启动入口），启动形如 `lua <project>/entry.lua`（checkout 内解释器为构建产物 `build/lua/<cfg>/lua.exe`，发布包内为 `external/lua/lua.exe`；Lua 5.4）。
- 入口与启动方式在 1.x 内保持稳定；模板见 `docs/guides/template-quickstart.md`。

### 5.4 mod 目录

- 可选 `mods/<名称>/` **镜像项目布局**提供覆盖资产/脚本，配合 `mods.enable` 启用、`mods.resolve` 运行时优先命中（`scripts/mods.lua`）。
- 适用于「不加改原项目即可注入内容」（如 KAG3 迁移资产注入）。

---

## 6. Lua compatibility

### 6.1 运行时

- **Lua 5.4**（checkout 内解释器为构建产物 `build/lua/<cfg>/lua.exe`，发布包内为 `external/lua/lua.exe`；Web 端 wasmoon，Lua 5.4）。
- 引擎内嵌**指令预算沙箱** + 白名单环境（`kag/init.lua` 预加载清单，sandbox `require` 只认 `package.loaded`）。

### 6.2 脚本 API 稳定性

- 绑定层（`src/script/bindings/*.cpp`，160 个 `luaL_Reg` 条目（`docs/api/api-stats.md`，自动生成源），注册 `KAG`/`Render`/`VFX`/`Debug`/`DevCore`/`mini_game`/`sma`/`steam`/`AI`/`Engine` 等全局）在 1.x 内**不删除、不重命名**已公开绑定。
- Lua 模块 API 参考：`docs/api/lua-modules.md`。新绑定只增不删；已公开签名参数不可破坏性修改。

### 6.3 沙箱边界（安全与可预测）

- Lua 代码运行在受限环境：`io`/`os` 等敏感能力被剥离或白名单化（内置 `io.open` 白名单：`scripts/assets/tests/demo` 前缀）。
- 指令预算防死循环，超预算给可诊断错误而非挂死。因此脚本**不能**依赖主机文件系统读写、网络、任意 `require`——1.x 内保持受限。

---

## 7. Version migration strategy

### 7.1 迁移路径

```
1.0 ──▶ 1.1 ──▶ 1.2 ──▶ 2.0 ──▶ …
```

目标：**这条链上每一步都不破坏既有作品**。

1. **1.x 内（1.0 → 1.1 → 1.2）零强制 breaking**：除 §8 exception 外，KAG/Lua/Save/Project 承诺保持稳定；`CHANGELOG.md` 明确标注每个 minor 的变更。
2. **2.0**：允许「加工艺定义的」breaking，但**分两步**——a) 前一版本标记 deprecation（仍可用但告警）；b) 2.0 正式移除并给出迁移工具（`kag3_import`、`scripts/ks_i18n.lua`、`tools/xp3_tool.py`、`tools/tlg2png.py`、`mods` 注入）与指南。

### 7.2 每级允许的 breaking

| 版本步进 | 是否允许 breaking | 限制 |
|----------|-------------------|------|
| 1.0 → 1.1 | 禁止（仅扩展 + bugfix） | 必须通过 Golden Save / Golden Project 回归 |
| 1.1 → 1.2 | 禁止（同上） | 同上 |
| 1.2 → 2.0 | 有限允许 | 仅经 deprecation 期 + 迁移工具/指南覆盖的项；其余保持兼容 |

### 7.3 迁移工具

- `scripts/ks_i18n.lua` — 脚本 i18n 处理；`tools/xp3_tool.py` — XP3 解析（27 unittest）；`tools/tlg2png.py` — TLG5/6→PNG（38 断言）；`kag3_import` — KAG3 脚本/资产导入。
- 迁移指南：`docs/guides/xp3-compat.md`、`tlg-compat.md`、`kag3-migration.md`。

### 7.4 CHANGELOG 纪律

- `CHANGELOG.md` 由 Conventional Commits 经 `python scripts/gen_changelog.py` 生成后手工打磨为分组条目（当前 `v1.0.1`）。
- 每个 minor/patch**必须**记录：新增、修复、**可能的兼容性影响**、迁移提示；涉及 §8 的 breaking 必须有显式 `[BREAKING]` 标注。

---

## 8. Breaking change policy

### 8.1 什么叫 breaking

以下任一项在版本升级后语义变化即为 breaking：

- KAG 命令/参数：删除、重命名、改默认值、改范围、翻转阻塞性、破坏性改 type；
- Lua API：删除/重命名已公开 binding、改已公开签名参数；
- Save：删除字段、改写既有字段语义、导致旧存档资源格式不再可读；
- Project：破坏 `entry.lua`/`assets/` 布局/资产路径语义/配置文件格式。

### 8.2 通知周期与流程

所有 breaking 走四步：

1. **计划**：在版本计划（`docs/plans/`）声明将发生的 breaking 与理由；
2. **deprecation**：提前一个 minor 标记 deprecated（文档 + 运行期告警）；
3. **迁移**：提供迁移工具/指南（§7.3）；
4. **移除**：在符合条件的版本（1.x 内不做；2.0 起按 §7.2）执行，并在 `CHANGELOG.md` 标注 `[BREAKING]`。

### 8.3 exception 列表（可暂不稳定）

下列领域**明确不纳入** 1.x 稳定性承诺，可随版本演进（调用方需按符号自适配）：

- **Plugin ABI**（插件二进制接口——Plugin SDK 待核心稳定后另行定义）；
- **Editor internal RPC**（编辑器内部私有 RPC，非公开端点契约）；
- **Experimental AI APIs**（`AI` bin、`[ai_dialog]`、LLM 相关）；
- **Experimental 3D APIs**（minigame 3D、`IMiniGameBackend` 等实验能力）；
- **Developer-only interfaces**（`DevCore`、`_CAESURA_*` 调试内部等）。

标记 **experimental / unverified** 的能力（总任务书工作纪律）一律不构成兼容承诺——不假装支持、不保证稳定。

---

## 9. 附录：支持矩阵速查表

| 维度 | 值 | 权威来源 |
|------|----|---------|
| C++ API 接口 | **34** 个接口头 / **412** 纯虚方法 | `docs/api/api-stats.md`（自动生成源；数字随 `python scripts/api_stats.py` 同步） |
| KAG Neo-Genesis 命令 | **134** 个契约命令 | `docs/api/command-contracts.md` |
| 能力闭环矩阵 | **134 = CLOSED 129 + PARTIAL 2 + UNWIRED 0 + EXPERIMENTAL 3**（EXTRA 31 = 注册但无合约，属设计行为；四层口径见矩阵统计行：Structural Closed=129 · Runtime 测试证据=139 · Platform=0 · Packaged=0） | `docs/design/capability-closure-matrix.md` |
| KAG3 兼容 | 裸位置参数 **13** families / TJS 表达式 / `%f.x%` / `[elsif]` / `[call *label]` / `[end]` / `[goto]`→`[jump]` | `docs/api/kag-commands.md`、`kag-expression-language.md` |
| 存档格式 | JSON + AES-256-GCM + `CAES` 信封 | `docs/design/save-security-audit.md`、`engine-architecture-topology.md` |
| 存档 schema | 迁移链 **v1 → v5**（自动升级，步数上限 64） | `engine-capability-matrix.md` (C4) |
| 脚本运行时 | **Lua 5.4**（指令预算沙箱） | `docs/guides/getting-started.md`、`docs/team/development-guide.md` |
| 教程 | **16** 个递进式教程（tutorial_01–16） | `docs/guides/community.md`、`engine-capability-matrix.md` (C10) |
| 项目布局 | `entry.lua` + `assets/` 子目录 + 可选 `mods/` | `docs/guides/asset-pipeline.md`、`template-quickstart.md` |
| 当前版本 | **v1.0.1** | `CHANGELOG.md` |

> 数字更新须随 `python scripts/api_stats.py` 与 schema 文档重新生成保持同步；本文档数字在 Phase 0 稳定化中被视为权威承诺基线。

## 附录 A：已审计缺口（2026-08-28 全量核验，t51）

> 本附录记录审计发现的缺口，不改变上文承诺；G1 已随本次提交修正。

- **G1（已修）**：命令契约计数 123→134（本文两处 + AGENTS.md/CLAUDE.md 同步）。
- **G2**：KAG3 宏命名实参在命令参数位不转换（test_kag3_import.lua:382-388 测试锁定可见）。
- **G3**：导入器 KNOWN/UNSUPPORTED 分类依赖 ambient 缓存（require 顺序敏感）。
- **G4**：§4.3「每主要版本保留参考存档」部分满足——现有夹具仅 schema 期 v1..v5，无真实发布版全量存档、无 1.x→2.0 样例。
- **G5**：project.json 无版本化/迁移器（§5 亦无对应承诺，待定级）。
- **G6**：存档降级/前向兼容（新档旧引擎）语义未定义。
- **G7**：KAG3 importer 无批量模式（--dir）。
- **G8**：无 KAG3 引擎↔本引擎 A/B 双跑对拍设施。
- **G9**：doctor 工具组不含存档迁移链自检。
- **G10**：docs/api/lua-modules.md 为手写参考，存在漂移风险（无生成器锁定）。
- **G11（已修，2026-08-30 t140）**：§9 速查表 C++ API 计数过时（31/390）——按 `docs/api/api-stats.md` 权威值改为 **34 接口头 / 412 纯虚方法**（自动生成源，数字随 `python scripts/api_stats.py` 再生成同步，勿手改）。
- **G12（已修，2026-08-30 t140）**：宏名冲突语义未入承诺清单——已增补 §3.1 第 4 条（非专用分支命令宏覆盖内置；专用分支命令仅静态安全宏编译期内联覆盖、动态宏被分支胜出静默忽略；依据 `scripts/scheduler.lua:348`）。判据：该规则是作者可观察的确定性派发语义（迁移脚本依赖宏覆盖 `[text]` 等行为），属兼容承诺面而非实现细节，故入 §3.1 而非仅附录。
