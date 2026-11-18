# CARC 打包指南

## 概述

CARC (Caesura ARChive) 是 Caesura (AmeKAG) 引擎的加密压缩归档格式。它用于将游戏资源打包为单个文件，支持：

- **AES-256-GCM 加密**: 每个文件独立生成 32 字节 AES 密钥 + 12 字节 nonce
  加密，**始终启用**（归档整体即加密+压缩形态，无明文模式）
- **zstd 压缩**: 逐文件 zstd 压缩，平衡速度与压缩率（始终启用）
- **Ed25519 签名**: 头+内容+索引整体签名，公钥内嵌归档尾部（始终启用）

## 命令行工具

### 基本用法

工具位于构建输出目录 `bin/Debug/carc_pack.exe`（Release 为 `bin/Release/carc_pack.exe`），支持 `pack`（默认）、`list`、`extract` 三种操作：

```bash
CARC_PACK=bin/Debug/carc_pack.exe

# 打包（每次生成全新密钥对；密钥随归档尾部 + 可选落盘）
$CARC_PACK ./mygame ./release/game.carc

# 打包并额外把生成的公钥/私钥写到指定文件
$CARC_PACK ./mygame ./release/game.carc ./mygame/game.key.pub ./mygame/game.key
# (若省略密钥路径，公钥仅内嵌归档尾部、私钥不留盘)

# 注意：不支持复用已有密钥——每次 pack 都会生成新密钥对。

# 列出归档内文件（每行一个路径哈希，供脚本/导入器消费）
$CARC_PACK list ./release/game.carc [public.key]

# 提取单个文件到原始相对路径（需 --path 指定已知路径）
$CARC_PACK extract ./release/game.carc ./out --path audio/bgm_01.mp3 [public.key]

# 全量提取（归档只存路径哈希，全量提取按哈希名落盘）
$CARC_PACK extract ./release/game.carc ./outfull [public.key]
```

### 参数说明

```
carc_pack.exe <input_dir> <output.carc> [public.key] [private.key]
carc_pack.exe list <archive.carc> [public.key]
carc_pack.exe extract <archive.carc> <out_dir> [--path <rel>] [public.key]

  pack        — 默认操作。打包目录
  input_dir   — 要打包的目录
  output.carc — 输出的 CARC 归档文件
  public.key  — (可选) Ed25519 公钥路径（list/extract 用于验签）
  private.key — (可选) 保存 Ed25519 私钥的文件路径
  list        — 打印归档内文件的路径哈希（每行一个，machine-readable）
  extract     — 提取归档内容到 out_dir
  --path      — (extract) 仅提取指定相对路径到一个文件（保留原始文件名）
```

`--path` 接受归档内的相对文件路径。按名和按哈希解包都会检查输出根目录及其内部路径，拒绝符号链接、Windows 联接和目标硬链接。覆盖普通文件时，工具先写完新文件并保留原有 Windows DACL 或 POSIX 读写执行权限，再替换目标。工具以调用者选定目录的实际位置为解包边界，输出目录需由调用者在运行期间控制。

从 CARC 迁移 KAG3 场景时，`lua scripts/kag3_import.lua --carc <archive.carc> --path <relative.ks>` 会直接读取归档中的场景。添加 `-o <outdir>` 可写入转换结果。归档读取失败时，导入以非零状态退出。

### 密钥管理

- **公钥** (32 字节): 归档尾部包含一份；固定发布者模式由宿主另行选择可信公钥，用于验证签名和派生索引解密密钥
- **私钥** (64 字节): 仅用于打包时签名，**不应分发**
- 密钥文件为原始二进制格式

## CARC 文件格式

```
┌─────────────────────────────────────────┐
│  Header (64 bytes)                      │
│  ├─ Magic "CARC" (4) / Version u32 (1)  │
│  ├─ contentOffset u64 (= 64)            │
│  ├─ contentSize u64 / indexOffset u64   │
│  ├─ indexSize u64 / numFiles u32        │
│  └─ reserved[20]                        │
├─────────────────────────────────────────┤
│  Content block（逐文件 zstd 压缩 →        │
│  AES-256-GCM 加密，按条目串行拼接）       │
├─────────────────────────────────────────┤
│  Index（始终 AES-256-GCM 加密）           │
│  ├─ File count: uint32                  │
│  ├─ FileEntry × N（每条 116 bytes）      │
│  │    pathHash[32]  offset u64          │
│  │    compressedSize u64  originalSize  │
│  │    aesKey[32]  nonce[12]  tag[16]    │
│  └─ append 16-byte AES-GCM tag          │
│  索引密钥 = SHA-256(归档公钥 32B)；       │
│  nonce = CARC 版本号（4B 零填充）         │
├─────────────────────────────────────────┤
│  Trailer (96 bytes)                     │
│  ├─ Ed25519 Signature (64 bytes)        │
│  └─ Public Key (32 bytes)               │
└─────────────────────────────────────────┘
> 归档**始终**加密+压缩+签名（无明文/未签名模式）。每个文件由自己的
> aesKey/nonce/tag 加密；`list` 仅输出 `pathHash` 的 64 位十六进制
> （原始相对路径不落盘，`extract --path <rel>` 才能还原真实文件名）。
```

## 在 KAG 脚本中使用

Caesura 引擎**没有 `carc://` 协议**。资源请求统一走纯路径：打包时的相对路径即脚本中的资源路径，引擎按优先级依次在各资源提供者（磁盘目录、`data.carc`/`game.carc`/`patch.carc`）中查找同名路径。

```kag
; 背景图片
@bg "scenes/bg_classroom.png"

; 背景音乐
@bgm "audio/bgm_01.mp3"

; 角色立绘
@fg "characters/hero_smile.png"

; 音效
@se "audio/se_click.wav"
```

## 运行时加载

引擎在组合根 `registerDefaultAssetProviders`（`src/entry/Engine_Assets.cpp`）中自动发现并挂载资源根及其 `dlc/` 子目录下的归档。优先级越大，资源覆盖顺序越靠前：

| 优先级 | 自动识别的归档 |
| --- | --- |
| 40 | `patch.carc`、`patch_*.carc` |
| 30 | `dlc_*.carc`、`dlc/` 下的其他 `*.carc` |
| 20 | `lang_*.carc` |
| 10 | `base.carc`、`data.carc`、`game.carc` |
| 5 | 资源根与 `assets/` 中的松散文件 |

匹配区分大小写，固定名称和前缀按上表写法，扩展名必须为 `.carc`；资源根中不符合上述命名规则的归档不会挂载。同名归档去重，资源根中的文件优先于 `dlc/` 下的同名文件。`assetManager` 由 `Engine` 独占持有，资源请求通过提供者链按优先级查找。

### 选择发布者信任策略

默认 `--carc-trust compatible` 保持原有行为：使用归档内嵌公钥检查签名，跳过打不开或验证失败的包。这能检查包与其自报公钥是否一致，不能确认发布者身份；攻击者替换整个包和自报公钥后仍可能通过。

宿主需要确认包由指定发布者签名时，必须同时指定 `--carc-trust pinned` 和 `--carc-public-key <path>`：

```bash
# 在当前启动目录下选择可信公钥，再切换到游戏资源根
./CaesuraAmeKAG --resource-root ./release \
  --carc-trust pinned --carc-public-key ./trusted/publisher.pub

# 显式使用兼容行为（也可省略此选项）
./CaesuraAmeKAG --resource-root ./release --carc-trust compatible
```

`--carc-public-key` 必须是**恰好 32 字节的原始二进制公钥**，不能使用十六进制文本、PEM 或包含额外字节的文件。相对路径基于进程启动时的工作目录，在切换 `--resource-root` 之前只读取一次；读取后的字节复制进 `EngineConfig`。缺少模式、缺少公钥、不可读或长度不符的文件、未知模式/选项、相互冲突的重复参数都会在创建引擎之前报错并以非零状态退出。相同值的重复信任参数允许出现；`compatible` 与公钥参数同时出现会报错。

固定模式把同一份宿主公钥应用于**每个自动识别的挂载包**。任一已发现包打不开、损坏或签名不匹配，或者挂载目录发现失败，都会使引擎初始化失败并以非零状态退出；不会继续使用部分已验证包。没有 `dlc/` 是正常情况，没有归档时仍要求提供公钥。现有 `carc_pack` 每次打包都会生成新密钥对，因此分别调用它生成的多个包通常不能在单一固定公钥下同时挂载；本功能没有加入密钥复用、轮换或多发布者支持。

公钥的可信性来自宿主选择。宿主负责通过可信渠道取得并保护该文件；引擎不会从包内声明、发行清单、自动寻找的相邻 `.pub` 文件或 Lua 配置选择公钥。Lua `config.carc_verify_on_startup` 已不再控制验签：签名验证在初始挂载阶段完成，其 `true` / `false` 值均不能关闭固定发布者策略。

固定发布者模式只认证已识别并挂载的 CARC 包。松散资源、入口 Lua、清单以及可被替换的宿主程序仍在此认证范围之外；成功启动不代表整个发行目录已通过认证。CARC 的索引密钥可从公钥派生，能读取归档的人可以提取其内容；归档加密也不构成 DRM 或内容保密保证。

## 安全注意事项

1. **私钥绝不提交到版本控制** — 将 `*.key` 添加到 `.gitignore`
2. **宿主选择可信公钥** — 使用固定发布者模式时，从可信渠道提供公钥；仅信任包内嵌公钥不能认证发布者
3. **AES-256-GCM 检查条目完整性** — 索引包含解密信息，格式本身不保证内容保密
4. **Ed25519 签名认证 CARC 字节** — 固定宿主公钥后才可拒绝其他发布者的替换包；认证范围不包含整个发行目录
5. **nonce 复用检测（默认开启）** — CryptoEngine 按 (AES key, nonce) 键界维护一个**有界**（1024 条，约 45 KB）进程级复用注册表。encrypt() 以同一 (key, nonce) 加密两次会在第二次**拒绝产出密文**（返回空），fail-closed 地避免 GCM 重放 keystream 泄露。注册表按 (key, nonce) **成对**键界，因此同一 nonce 在不同 key 下使用（安全语义）不会被误判。generateNonce() 使用 CSPRNG（96-bit 碰撞概率 < 2^-48），正常打包路径不会触注册表；仅当调用方在固定 key 下以确定性/计数器方式生成 nonce 时，可调用 CryptoEngine::setNonceReuseDetection(false) 关闭检测（默认开启）。

## 常见问题

**Q: 打包后文件比原始文件大？**
A: 检查是否包含了不应打包的文件（如 `.git/`, `.DS_Store`）。仅打包 `assets/` 和 `scripts/` 目录。

**Q: 运行时提示 "CARC signature verification failed"？**
A: 公钥不匹配或归档文件被修改。确认使用的公钥与打包时的私钥配对。

**Q: 能否增量更新 CARC？**
A: `carc_pack delta <old.carc> <new.carc> <delta.carc>` 生成差分，`carc_pack verify-delta <delta.carc>` 校验差分，`carc_pack apply <base.carc> <delta.carc> <output.carc>` 生成更新后的归档。运行时通过归档挂载使用生成的文件。

**Q: Windows 上可以使用中文文件系统路径吗？**

A: 归档、签名密钥、差分文件以及打包和解包目录可使用 Unicode 路径。C++ 归档接口优先按 UTF-8 解码；UTF-8 解码失败时，Windows 兼容能无损往返的旧代码页路径。归档内部的成员名继续使用原有哈希规则。
