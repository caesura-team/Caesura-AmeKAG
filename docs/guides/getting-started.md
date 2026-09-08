# Getting Started with Caesura (AmeKAG)

> 本指南带你**从克隆仓库到跑通示例游戏**：逐平台环境准备 → 构建 → 运行 →
> 测试 → 常见问题。全程约 30–60 分钟（构建占大头）。
>
> 配套文档：[KAG 语言教程](kag-language-tour.md)（语法）· [命令契约](../../docs/api/command-contracts.md)
> （134 命令权威参考）· [资源管线](asset-pipeline.md)（资产格式与目录）·
> [示例库](sample-library.md)（16 教程 + 完整游戏）。

---

## ⭐ 30 分钟速通：从零到第一个可运行 VN（KPI-1 路径）

> 这是新手最短路径（产品化 KPI-1：陌生开发者 ≤30 分钟完成首个 Tiny VN）。
> 两条路径：**路径 A 用发布包（推荐，KPI-1 口径）**，路径 B 克隆源码构建（另计
> 10–30+ 分钟）。完整细节见后文 §1–§10；两条路径都覆盖「下载 → 创建 → 编辑 →
> 运行 → 打包」。

### ⚡ 路径 A（推荐）：Release ZIP 解压即建即跑

> **判定你的 ZIP 是"新包"还是"旧包"**：解压后看一眼目录——新包（Sprint 4 起，
> 判据：解压目录含 `tools/project_templates/`、`scripts/`、`external/lua/lua.exe`）
> 才能解压即建；**[v1.0.1 ZIP](https://github.com/caesura-team/Caesura-AmeKAG/releases)
> 及更早的发布包为旧包**（无这些目录，`--editor` 会打印
> `web-editor/dist not found; serving API only`）——旧包请走路径 B。
> 包内容验证用 `bash scripts/verify_release_package.sh`（30 项断言；
> 发布流程见 [release-process.md](release-process.md) §5，含 SDL3/lua_cli/模板/
> web-editor 自包含安装）。

**路径 A 五步（发布包；KPI-1 口径 ≤30 分钟）**

1. **下载并解压**：GitHub Releases 取最新 `CaesuraAmeKAG-<ver>-Windows-AMD64.zip`，
   `cd <解压目录>`。包内自包含：`scripts/`（运行时 Lua）、`tools/project_templates/`
   （5 模板 + manifest.json）、`external/lua/lua.exe`（Lua 解释器）、`demo/`（示例）、
   引擎 exe 与 DLL——无需克隆/构建。
2. **创建项目**：`python scripts/caesura.py create my_vn --template basic`
   （从包内 `tools/project_templates/basic` 复制，产出 `story.ks` 两场景双分支 +
   `caesura.project.json` + `entry.lua`；模板按 CLI 脚本自身位置解析，任意 CWD 成立）。
3. **编辑**：任意文本编辑器改 `my_vn/story.ks`（`[bg]`/`[ch]`/`[sel]` 语法见
   [KAG 语言教程](kag-language-tour.md)）。**Monaco 编辑器 + Project Manager +
   Build Manager 面板属于 `editor/` 这个 React IDE（源码树内，发布包不含）**；
   发布包内可用引擎自带 `--editor` 单文件调试面板（见下）。
4. **运行**：`python scripts/caesura.py build my_vn -o dist/my_vn-game` →
   `cd dist/my_vn-game` 运行 `CaesuraAmeKAG.exe`（冒烟：`./CaesuraAmeKAG.exe --frames 60`
   退出码 0）。
5. **打包**：`python scripts/caesura.py package my_vn --target windows|web|both`
   压成可分发归档（game-only 目录 = 引擎 exe + SDL3/FFmpeg DLL + 游戏 `assets/` +
   预编译 `cache/ksc` + `HOW-TO-PLAY.txt` + `BUILD-INFO.json`）。

**路径 B 五步（克隆源码 + 构建；10–30+ 分钟，不占 30 分钟 KPI 预算）**

1. **构建引擎**（一次性，§1–§2）：先备好 vcpkg SDL3（§1.1 第 4 步，Windows **必备**——
   新鲜克隆的 `external/SDL3` 只含头与 CMake 配置，预编译 `.lib/.dll` 被 `.gitignore`
   的 `SDL3-*/` 排除、不入库；**不传 toolchain 直接 configure 会报
   `SDL3::SDL3 is not an executable or library`**——CI 的 build-windows 自己就用
   vcpkg，见 `.github/workflows/ci.yml` `Install SDL3 (vcpkg)` 步），然后：
   `cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"`
   → `cmake --build build --config Debug --parallel`（示例命令与 §2.1 相同；
   三平台差异速查表里 `SDL3 来源` 行的 Windows 单元格已同步为 vcpkg 口径）。
   例外：本地工作树若带**完整** vendored 拷贝（`external/SDL3/SDL3-3.2.0` 含
   `lib/` `bin/` 的历史手工产物）才能不传 toolchain 自动使用——**新鲜克隆没有这个例外**。
   **注意**：git bash 下不带 `-G` 的 `cmake -B build` 会选错生成器
   （首跑报 `CMAKE_C_COMPILER not set`）——git bash 与 PowerShell 都建议带
   `-G "Visual Studio 17 2022" -A x64`。
2. **创建项目**：`python scripts/caesura.py create my_vn --template basic`（同路径 A，
   模板按 CLI 自身位置解析，仓库根或解压的发布包内均可）。
3. **编辑**：同路径 A。
4. **运行**：从仓库根 `./build/Debug/CaesuraAmeKAG.exe`；静态校验
   `build/lua/Debug/lua.exe scripts/ks_check.lua my_vn/story.ks`；无 GPU 逻辑验证：
   `SAMPLE_STORY=my_vn/story.ks build/lua/Debug/lua.exe tests/scripts/sample_game_headless.lua`
   （输出 `RESULT DONE:<帧数>`，退出码 0）。**`kag_runner.lua` 是模块不是入口脚本，
   不能 `lua scripts/kag_runner.lua <scene>` 直跑**（裸 lua 缺 `scripts/` 的
   package.path，且无人驱动帧循环）；`my_vn/entry.lua` 已自带入口自定位
   （P0-1 修复），可 `lua my_vn/entry.lua` 做逻辑冒烟（加载 `my_vn/story.ks`、
   启动 KAG Runner 后退出）。
5. **打包**：`python scripts/caesura.py build my_vn -o dist/my_vn-game`（同路径 A 的
   game-only 目录；实测 `--frames 60` 退出码 0、Runner 正常启动）；Web 站
   `bash scripts/package_game.sh --out dist/my_vn my_vn`（itch.io / GitHub Pages /
   Netlify 直接托管）。
   > **Web 站前置**：首次跑 `package_game.sh` 前先 `cd web && npm ci`（依赖安装见
   > §6，装好后无需重复）。**仓库没有 `web/dist/` 时 `package_game.sh` 按设计 FAIL**
   > （提示 `FAIL: web player not built (missing web/dist/index.html)`，不是脚本 bug）——
   > 先 `cd web && npm ci && npm run build` 生成 `web/dist/` 再打包。

**引擎自带的 `--editor` 是什么**：`./build/Debug/CaesuraAmeKAG.exe --editor` 在
:9876 起 HTTP 服务并提供 `web-editor/dist/index.html`——一个**单文件调试面板**
（脚本 / 调试 / 帧捕获 / 日志四个页签），**没有** Project Manager、没有 Monaco。
它默认要求 bearer 令牌：启动时令牌打印在 stderr 并写入启动目录的
`.caesura-editor-token`。

> **令牌与浏览器（两种版本行为不同，先认版本）**：
> - **Sprint 4 之后**（判据：启动日志出现
>   `[EditorServer] Open the editor: http://127.0.0.1:9876/?token=...`）——
>   静态壳（`/` 与 `/index.html`）免令牌，页面读取 `?token=` 存入 localStorage，
>   之后所有 `/api/*` 带 bearer；直接点开日志里那条地址即可。安装树实测：
>   `GET /` 无令牌 200、带令牌 200（`<title>Caesura Web Editor</title>`），
>   `/api/project/templates` 带令牌 200。
> - **Sprint 4 之前的二进制**——门禁覆盖包括静态页在内的所有非 OPTIONS 请求：
>   `curl -H "Authorization: Bearer <令牌>" http://127.0.0.1:9876/` 返回 200，
>   但浏览器地址栏访问 `/` 或 `/?token=<令牌>` 都 **401**（地址栏无法附带
>   Authorization 头，服务端也不解析 `?token=`），唯一浏览器入口是
>   `--editor-insecure`。

### ✅ 完成判定（Tiny VN 必须包含）

background ✓ · character ✓ · dialogue ✓ · choice ✓ · BGM ✓ · save/load ✓

> 现成验收范例：`tests/projects/first_vn/`（包含上述全部要素，13 项验收门禁
> `bash scripts/verify_first_vn.sh`）。新手可复制它的写法快速上手。

---

## 0. 你需要什么

Caesura (AmeKAG) 是 **C++20 + bgfx 渲染 + SDL3 窗口 + Lua 5.4 脚本** 的跨平台视觉小说引擎。
克隆源码后你不需要任何游戏引擎 IDE——一个文本编辑器 + 命令行即可起步。

| 需求 | 最低版本 | 说明 |
|------|---------|------|
| Git | 任意 | 克隆仓库、语义提交 |
| CMake | **3.25+** | 顶层 CMakeLists 要求 min 3.25 |
| C++ 编译器 | VS2022 (MSVC v143) / GCC 13+ / Xcode 15+ (Apple Clang) | 见 §1 各平台 |
| Python | 3.8+ | 仅标准库：api_stats / gen_changelog / 耦合计数脚本 |
| Lua | 不需安装 | CMake 构建自带 `lua_cli` 目标（产物 `build/lua/<配置>/lua.exe`，Lua 5.4）；发布包内置 `external/lua/lua.exe`。CLI 的 `find_lua` 依次探测两处再回退 PATH |
| Node.js | 18+ | 仅 Web 播放器 / editor 需要（§6） |

> **磁盘与内存**：全量 Debug 构建约需 3–5 GB 磁盘（thirdparty + build 目录）；
> 并行构建建议 16 GB 内存。Release 构建额外 ~2 GB。

---

## 1. 逐平台环境准备

### 1.1 Windows（主开发平台）

```powershell
# 1) 安装 Visual Studio 2022（含 "Desktop development with C++" 工作负载，
#    即 MSVC v143 工具集 + Windows SDK）
winget install Microsoft.VisualStudio.2022.BuildTools

# 2) 安装 CMake（或从 https://cmake.org/download 装二进制）
winget install Kitware.CMake

# 3) 安装 Git
winget install Git.Git

# 4) 安装 vcpkg 并提供 SDL3（Windows **必备**——与 CI 同源：仓库新鲜克隆的
#    external/SDL3 只含头+CMake 配置（3.6M/92 文件），预编译 .lib/.dll 被
#    .gitignore 的 SDL3-*/ 排除不入库；CI build-windows 的 "Install SDL3 (vcpkg)"
#    步就是这么装的，见 .github/workflows/ci.yml）：
git clone https://github.com/microsoft/vcpkg C:/vcpkg
cd C:/vcpkg && .\bootstrap-vcpkg.bat
# vcpkg 新版 bootstrap 需要 cmake >= 4.3.3（runner 可能滞后——CI 先 pip 升级：
#   python -m pip install --quiet --upgrade cmake）
python -m pip install --quiet --upgrade cmake
.\vcpkg install sdl3 --triplet x64-windows
# 记住 vcpkg 根目录，稍后配置时用（CI 用 $env:VCPKG_INSTALLATION_ROOT 指它）
# 例外：本地工作树带完整 vendored 拷贝（external/SDL3/SDL3-3.2.0 含 .lib/.dll）
# 才可省 toolchain；git-internal 只有头+CMake 配置，新鲜克隆必须走本步。

# 5) （可选，不阻塞）git bash：仓库内脚本约定用 git bash 运行
```

> **vcpkg 位置**：以下假设 `C:/vcpkg`，配置命令里的 `$env:VCPKG_INSTALLATION_ROOT`
> 会自动指向。若装在别处，改路径即可。

### 1.2 Linux（Ubuntu 24.04 参考）

```bash
# 1) 基础 + 编译工具
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config

# 2) 引擎依赖（FreeType / zstd / OpenSSL / X11 / 音频）
sudo apt-get install -y libfreetype-dev libzstd-dev libssl-dev \
  libx11-dev libpulse-dev libasound2-dev

# 3) SDL3 —— 推荐源码装 3.2.0：
wget https://github.com/libsdl-org/SDL/releases/download/release-3.2.0/SDL3-3.2.0.tar.gz
tar xzf SDL3-3.2.0.tar.gz && cd SDL3-3.2.0
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
sudo cmake --install build
cd ..

# 4) （可选）FFmpeg 全格式视频解码：
sudo apt-get install -y libavcodec-dev libavformat-dev libswscale-dev \
  libavutil-dev libswresample-dev
# 不装也没关系：引擎自动回退 pl_mpeg（仅 MPEG-1 视频），构建不失败
```

> **Linux 运行游戏额外依赖（libEGL.so.1）**：bgfx 的 GL 上下文走 `glcontext_egl` 路径，运行时 `dlopen('libEGL.so.1')`（非 GLX）。运行/发行 Linux 机器需装 `libegl1`；缺失时启动日志出现连串 `[bgfx WARN] Failed get egl*`（每个符号解析失败），随后经空函数指针 SEGV——不是干净的 init 失败。安装一行：
>
> ```bash
> sudo apt-get install -y libegl1
> ```

Linux 真渲染已验证（CI round-7：run 33245271845 @ 0fce3311，Linux Package 30/30 含 `renderdisabled=0`）。

### 1.3 macOS（Xcode + Homebrew）

```bash
# 1) 安装 Command Line Tools（Xcode 打开时会提示，或：）
xcode-select --install

# 2) Homebrew（若未安装）：https://brew.sh

# 3) 依赖
brew install cmake sdl3 freetype zstd openssl@3   # ffmpeg 可选，见上

# 4) 构建时用 CMAKE_PREFIX_PATH 让 CMake 找到 openssl@3（Homebrew 把
#    openssl 装成 keg-only，不加入默认搜索路径）
```

### 1.4 三平台差异速查

| 维度 | Windows | Linux | macOS |
|------|---------|-------|-------|
| 生成器 | Visual Studio 17 2022（多配置） | Unix Makefiles / Ninja（单配置） | Xcode / Unix Makefiles |
| 构建命令 | `cmake --build build --config Debug` | `cmake --build build`（配置在 configure 时定） | 同 Linux |
| 可执行产物 | `build/Debug/CaesuraAmeKAG.exe` | `build/CaesuraAmeKAG` | `build/CaesuraAmeKAG` |
| 测试二进制 | `build/tests/Debug/CaesuraTests.exe` | `build/tests/CaesuraTests` | `build/tests/CaesuraTests` |
| SDL3 来源 | vcpkg 必备（同 CI build-windows；仅带完整 vendored 拷贝的本地工作树可免 toolchain） | 源码装（见上） | Homebrew |
| 默认渲染后端 | D3D11（bgfx auto） | OpenGL 4.3 | Metal（D3D11 失败后 auto-select） |
| CPU 要求 | x86_64 + SSE4.1（MSVC 从不设 ISA 门槛，Windows 二进制始终如此；2008+ 常规 CPU 均满足） | x86_64 + SSE4.1（142d0dbe 起显式 `-msse4.1` 对齐；Steam Deck Zen2 无影响） | Apple Silicon（Arm64；Intel 支持为 hardware-gated 待验证） |
| 运行时依赖（游戏） | 无需额外系统包（D3D11 随系统） | **libegl1**（bgfx GL 上下文运行时 dlopen `libEGL.so.1`；缺失=`Failed get egl*` 后 SEGV） | 系统框架内置（Metal） |
| FFmpeg | 内置（可选 vcpkg） | apt dev 包（可选） | brew ffmpeg（可选） |
| 注意 | 必须从**项目根**启动（资源相对 CWD） | 同左 | 同左 |

> 平台×能力的七级支持分级（Support→Build→Boot→Gameplay→First-VN→Package→Store，每格附证据注记）见 [平台支持矩阵](../design/platform-support-matrix.md)。

---

## 2. 克隆与首次配置

```bash
# 推荐 1：部分克隆（仅按需下载当前提交的 blob，保留完整分支历史，下载量仅 ~25MB，速度提升 15 倍）
git clone --filter=blob:none https://github.com/caesura-team/Caesura-AmeKAG.git CaesuraAmeKAG
cd CaesuraAmeKAG

# 推荐 2：浅克隆（仅拉取最新 HEAD，适合 CI 或快速体验）
# git clone --depth=1 https://github.com/caesura-team/Caesura-AmeKAG.git CaesuraAmeKAG

# 确认在 master 分支
git branch --show-current          # -> master
```

**目录速览**（完整结构见 README）：

```
Caesura(AmeKAG)/
├── src/                  # C++ 引擎（16 个静态模块库 + 15 个 API-only CMake targets）
│   ├── entry/            # 组合根（唯一 new 具体后端的地方）
│   ├── render/           # bgfx 渲染（IRenderDevice）
│   ├── script/           # Lua VM + KAG 绑定（ILuaManager）
│   ├── resource/         # 异步资源加载（IAssetProvider）
│   ├── archive/          # CARC 加密归档
│   └── ...（audio/storage/platform/input/job/live2d/minigame/steam/debug/rpc/di）
├── scripts/              # Lua 运行时（kag/ 命令处理器 + scheduler + 工具脚本）
├── demo/                 # 教程 tutorial_01–16、示例游戏 example_game/、模板 template/
├── assets/               # 共享资产池（bg/fg/bgm/se/voice/fonts/lang）
├── tests/                # C++ (doctest) + Lua 套件
├── external/             # vendored 第三方库（SDL3/bgfx/soloud/lua/…）
├── thirdparty/           # 本地可选依赖（如 Cubism SDK，不入库）
├── web/                  # Web 播放器（wasmoon + vite）
├── editor/               # 网页编辑器（React + TS）
├── docs/                 # 文档（api/design/guides/plans/solutions）
└── CMakeLists.txt        # 顶层构建（min CMake 3.25）
```

### 2.1 构建（Windows 为例）

```powershell
# 1) 配置（一次性；生成 Visual Studio 解决方案）。
#    默认 = vcpkg SDL3（Windows 必备，与 CI build-windows 的 Configure 步同款：
#    含 -DCMAKE_TOOLCHAIN_FILE 与 -DVCPKG_APPLOCAL_DEPS=OFF；CI 路径用
#    $env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake，本机自装为
#    C:/vcpkg/...，见 .github/workflows/ci.yml）：
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_APPLOCAL_DEPS=OFF
#    例外：本地工作树带完整 vendored 拷贝（external/SDL3/SDL3-3.2.0 含
#    .lib/.dll 的历史手工产物）可省 toolchain 行——git-internal 只含头+CMake
#    配置（预编译件被 .gitignore 的 SDL3-*/ 排除），**新鲜克隆没有这个例外**。

# 2) 构建 Debug（主开发配置）
cmake --build build --config Debug --parallel
# 预期：零错误；产物 build/Debug/CaesuraAmeKAG.exe
```

**常用 CMake 选项**（完整表见[开发指南](../team/development-guide.md)）：

| 选项 | 默认 | 作用 |
|------|------|------|
| `CAESURA_LIVE2D` | `OFF` | Live2D Cubism SDK（需手动下载 SDK，见 live2d-setup.md） |
| `CAESURA_ENABLE_FFMPEG` | `ON` | FFmpeg 视频解码（找不到时自动回退 pl_mpeg） |
| `CAESURA_DEBUG` | ON (Debug) | Debug 日志与断言宏 |

> **首次构建耗时**：Debug 全量约 5–15 分钟（高核数）到 30+ 分钟（低核数）。
> 之后是增量构建，几秒到几十秒。

### 2.2 构建（Linux / macOS）

```bash
# 配置（单配置生成器：Debug 是默认 CMAKE_BUILD_TYPE）
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  # macOS 追加： -DCMAKE_PREFIX_PATH="$(brew --prefix openssl@3)"
  # 无 FFmpeg 时追加： -DCAESURA_ENABLE_FFMPEG=OFF

# 构建
cmake --build build --parallel
# 产物：build/CaesuraAmeKAG
```

---

## 3. 运行

> **最重要的一条规则：从项目根目录启动。** 引擎把所有资源路径
> （`assets/`、`demo/`、`scripts/`）相对**当前工作目录 (CWD)** 解析。
> 从 build 目录启动会找不到资源，导致背景/音频缺失、画面异常。

```bash
# Windows（在仓库根目录执行）
./build/Debug/CaesuraAmeKAG.exe

# Linux / macOS
./build/CaesuraAmeKAG
```

你会看到一个窗口渲染 KAG 默认 demo（`demo/galgame_demo.ks`），日志打印
`[Engine]` 各子系统注册行。按 **Esc** 或关闭窗口退出。

**命令行参数**（引擎**没有实现 `--help`**——传了 `--help` 会被静默忽略并
直接启动窗口；参数以本表为准，源码见 `src/main.cpp` 参数解析段）：

| 参数 | 作用 |
|------|------|
| `--resource-root <dir>` | 指定资源根（须含 `assets/`；优先级：该参数 > 环境变量 `CAESURA_RESOURCE_ROOT` > CWD 上探） |
| `--editor` | HTTP 编辑器（:9876）。未设 `CAESURA_EDITOR_TOKEN` 时引擎自动生成令牌，打到 stderr 并写入启动目录的 `.caesura-editor-token` |
| `--editor-insecure` | 关闭编辑器鉴权（仅本机调试；会大声告警） |
| `--editor-stdio` | stdio JSON-RPC 编辑器 |
| `--backend <type>` | 强制渲染后端（metal/opengl/vulkan/dx11/dx12/webgpu） |
| `--frames N` | 渲染 N 帧后确定性退出（CI/冒烟用） |
| `--export-replay r.json --export-dir out` | 逐帧导出 PNG 序列（§8） |
| `--headless` | 无窗口模式（无 GPU 时给 test/determinism 用） |
| `--resolution WxH` | 渲染画布分辨率（默认 1920x1080，窗口自适应） |

---

## 4. 测试（跑通 Demo 前的自检）

> **测试基线（本机实测，2026-08-29 master `1af807c0` + 本轮 N1 用例）**：C++ 用例 **1120**（385790 断言）·
> Lua 主套件 **143** + 孤儿套件 **25**（2026-08-29 起含 select→跨场景 [jump] 回归） · Web vitest **368**（工件齐备全跑） · Editor vitest **625**（36 文件，2026-08-29 实测于 HEAD b7e1e7ce；t54 在途改动可能再增，以提交时点 CI 为准） ·
> CTest **17** 个 target（含 CaesuraGoldenVn 金项目门禁、CaesuraBuildCli / CaesuraVerifyWebPackage / CaesuraSmokeBrowserResolve SKIP77 契约） · 16 教程 · **134** 命令契约 · **34** 个 C++ 接口头（`docs/api/api-stats.md`，由 `python scripts/api_stats.py` 生成）。
> 任何 PR 合入前这些必须全绿。基线数字随开发增长，**以本地实跑输出为准**，不要把本行当门禁。

### 4.1 C++ 测试（doctest / CTest）

```bash
# Windows：从 build/tests/Debug 运行（CWD 影响资源路径，别从仓库根跑）
cd build/tests/Debug
./CaesuraTests.exe
# 预期输出尾部（2026-08-28 本机实测；数字随开发增长，关键是 0 failed / 0 skipped）：
#   [doctest] test cases:   1120 |   1120 passed | 0 failed | 0 skipped
#   [doctest] assertions: 385790 | 385790 passed | 0 failed

# Linux / macOS
cd build/tests && ./CaesuraTests
```

**doctest 过滤器**（调试单个模块时很有用）：

```bash
./CaesuraTests.exe -tc="*SaveManager*"   # 按测试用例名（支持通配符）
./CaesuraTests.exe -ts="*Render*"        # 按测试套件名
./CaesuraTests.exe -tce="*Slow*"         # 排除匹配用例
./CaesuraTests.exe -s                    # 显示通过用例的输出
./CaesuraTests.exe -d                    # 显示每个用例耗时
```

或经 CTest：

```bash
ctest -C Debug --test-dir build --output-on-failure
# 17 个 target（`ctest --test-dir build -N` 列出）：资产同步 + 6 组分模块 doctest 分片
# + headless CLI/RPC/HTTP 冒烟 + AI smoke（无 Ollama 自动跳过）+ RC/平台矩阵对抗测试
# + 构建 CLI 契约（无引擎时 SKIP 77）+ CaesuraGoldenVn（金项目门禁）
```

### 4.2 Lua 套件（两种：主套件 + 孤儿套件）

```bash
# 从仓库根运行；解释器用构建产物 build/lua/<配置>/lua.exe——全新克隆没有
# external/lua/lua.exe（那是发布包内置路径，find_lua 会依次探测两处）
build/lua/Debug/lua.exe tests/scripts/run_lua_tests.lua      # 主套件（实测 143 passed / 143 total）
build/lua/Debug/lua.exe tests/scripts/run_orphan_tests.lua   # 孤儿套件（实测 26 passed / 26 total）

# 主套件 = 大部分测试；孤儿套件 = 会创建全局 mock 的测试（沙箱中途锁全局），
# 每个测试子进程隔离运行。两套都必须全绿，孤儿永远不能并入主套件。
```

> **为什么分两个套件**：主套件的 sandbox 会在运行中途锁定全局表；
> 创建全局 mock 的测试（如 test_tween / test_layout_cmds）必须在孤儿套件里
> 以独立子进程运行，否则会互相污染。

### 4.3 静态契约校验（.ks 场景）

```bash
# 校验单个场景（引擎+Web 双端在跑的脚本契约）
build/lua/Debug/lua.exe scripts/ks_check.lua demo/galgame_demo.ks
# 校验全部 16 个教程
build/lua/Debug/lua.exe scripts/ks_check.lua demo/tutorial/*.ks
# 预期：OK — all scenes pass contract checks（0 violations）
```

### 4.4 Web / 脚本索引保鲜（CI 守卫）

```bash
node web/gen-index.mjs --check    # 三平台 CI 都跑；改过 scripts/*.lua 必须先
                                  # node web/gen-index.mjs 重生成并提交
```

---

## 5. 你的第一个场景

### 5.1 写剧本

创建 `my_first_scene.ks`：

```kag
; my_first_scene.ks — Your first KAG scene

*start
[bg storage="bg/school.png"]
[wait time=500]

[ch name="Hero" text="Hello, this is my first visual novel scene!"]
[p]

[playbgm storage="bgm/title.ogg" volume=0.8]
[ch name="Hero" text="Background music starts playing..."]
[p]

[fg storage="chara/hero_smile.png" layer=1]
[position layer="fg0" x=0.5 y=0.3 scale=1.0]
[ch name="Hero" text="Here is a character sprite!"]
[p]

[stopbgm time=500]
[end]
```

> `storage=` 路径相对**游戏根**（即 `assets/` 池）。引擎找不到资产时
> **不会崩溃**：缺图显示占位纹理（开发紫/发布灰），缺音频静音。
> ——所以你可以先写剧本、后补资产。

### 5.2 校验与运行

```bash
# 1) 静态契约校验（推荐每次都跑）
build/lua/Debug/lua.exe scripts/ks_check.lua my_first_scene.ks

# 2) 用 headless 驱动把剧本跑到 [end]（无 GPU 也能验证逻辑）
#    ——注意不能 `lua scripts/kag_runner.lua <scene>` 直跑：kag_runner.lua 是模块，
#       裸 lua 的默认 package.path 不含 scripts/，且没有驱动帧循环；
#       本驱动自带 package.path 前缀与 mock 绑定，跑到 [end] 输出 RESULT DONE。
SAMPLE_STORY=my_first_scene.ks build/lua/Debug/lua.exe tests/scripts/sample_game_headless.lua
#    预期输出尾部：RESULT DONE:<帧数>（退出码 0；FRAME_LIMIT = 剧本卡死）

# 3) 在引擎里跑：把 .ks 放进 scripts/ 或 demo/，从引擎入口引用；
#    或直接改 demo/example_game/story.ks 的剧情（§7 模板法）
```

### 5.3 从模板起步（推荐路线）

不想从零组织脚本/资产？两条现成路径：

- **项目模板** `demo/template/`：两场景 + 一次选择的最小骨架。
  ```bash
  cp -r demo/template my_game
  # 改 my_game/story.ks → lua my_game/entry.lua（逻辑冒烟：入口自带 package.path
  #   自定位，加载 my_game/story.ks（优先同目录）并启动 KAG Runner 后退出；
  #   要开窗口用引擎：./build/Debug/CaesuraAmeKAG.exe）
  # 全链路校验：bash scripts/verify_template.sh
  # 打包：bash scripts/package_game.sh --out dist/my_game --assets my_assets my_game
  ```
  详见 [template-quickstart.md](template-quickstart.md)。
- **完整示例游戏** `demo/example_game/`（《单程回信 The One-Way Reply》）：
  三结局 + 选择分支 + i18n 双语 + SMA 骨骼动画 + 双存档点。
  ```bash
  build/lua/Debug/lua.exe demo/example_game/entry.lua
  # 或从 build 输出目录：cd build/Debug && build/lua/Debug/lua.exe ../../demo/example_game/entry.lua
  ```

教程按 **tutorial_01_hello → tutorial_16_tween** 顺序学习全部能力（16 个教程，
每个独立可跑、逐行注释、引擎 + Web 双验证）。运行方式：

```bash
for f in demo/tutorial/tutorial_*.ks; do
  build/lua/Debug/lua.exe scripts/ks_check.lua "$f"
done
```

---

## 6. Web 播放器（可选）

`web/` 是纯前端的 KAG 播放器（wasmoon Lua VM + vite），直接在浏览器跑 `.ks`，
**无需构建 C++ 引擎**：

```bash
cd web
npm ci                 # 首次装依赖
npm run dev:web        # 开发服务器 http://127.0.0.1:5174
# 打开页面 → 场景下拉框选你的 .ks → Run
```

生产构建与测试：

```bash
npx vite build         # 输出 web/dist/
npm test               # vitest 单元/集成测试
node gen-index.mjs     # 刷新 scripts-index.json（改了 scripts/*.lua 就要重跑）
```

---

## 7. 视频导出（差异化能力）

录制的回放可以驱动游戏逐帧导出 PNG 序列（见 [replay] 系统）：

```bash
# 1) 录制输入（游戏内 [replay mode="record"]）→ demo_replay.json
# 2) 导出帧序列（需真实 GPU 窗口；--headless 会被自动忽略）
./build/Debug/CaesuraAmeKAG.exe --export-replay demo_replay.json \
  --export-dir export_out --frames 300
# 3) ffmpeg 合成视频
ffmpeg -framerate 60 -i export_out/frame_%05d.png -c:v libx264 -pix_fmt yuv420p trailer.mp4
```

---

## 8. 常见问题（FAQ）

### 构建失败

**Q: `CMake Error: Could not find a package configuration file provided by "SDL3"`**
A: Windows 上未装 vcpkg SDL3 或没传 toolchain 文件。`vcpkg install sdl3 --triplet x64-windows`
   后，configure 时带 `-DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"`。
   Linux/macOS 上确认已装 SDL3（§1.2/§1.3），或改用 Homebrew `brew install sdl3`。

**Q: CMake（或 vcpkg）报 `Could not find any instance of Visual Studio`**
A: VS 实例注册缺失（vswhere 查为空 / `HKLM\SOFTWARE\Microsoft\VisualStudio` 的 Instances
   键不存在——MSVC 工具链可能在而注册不在）。两招：① 钉实例——configure 时加
   `-DCMAKE_GENERATOR_INSTANCE="<VS 安装路径>,version=<版本>"`（路径用
   `vswhere -latest` 查——如 `C:/Program Files/Microsoft Visual Studio/2022/Community`；
   值需带 `,version=17.x` 后缀）；② VS Installer →“修改”→“修复”重建实例注册。
   vcpkg 报同错（`VCPKG_VISUAL_STUDIO_PATH` 也无效）——先修实例注册再装 SDL3。

**Q: Windows 下 `--editor` 启动即退出、退出码 1（且无任何 `[EditorServer]` 输出）**
A: 端口绑定失败的典型症状（进程干净关机、退出码 1、stdout 无 `[EditorServer] Started`/
   `Listening on port` 行）。根因多为端口被占或**落在 Windows 动态排除端口段**——
   2026-08-29 本机实测 `netstat` 无任何 9876 监听，但 bind 报 `WSAEACCES(10013)`：
   排除段 [9813-9912] 恰好覆盖 9876（WinNAT 家族，随 WSL2/Docker/Hyper-V 生命周期漂移，
   这也是“昨天还能跑今天不行”的来源）。自查：`netsh int ipv4 show excludedportrange 
   protocol=tcp`（看 9876 是否落在某区间；`netstat -ano | grep 9876` 见被占）；
   解锁：管理员 PowerShell `net stop winnat && net start winnat`（重排动态段，也可能复现）
   或改监听端口。修复后版本（本轮 P1）会在 stderr 打印响亮错误：
   `[EditorServer] [ERROR] failed to bind 127.0.0.1:<port>: socket error <code>`
   （10013=排除段/权限，10048=被占）+ 排查提示，不再静默退出。
   无 admin 的替代解锁：设环境变量 `CAESURA_EDITOR_PORT=<空闲端口>` 再跑 `--editor`
   （覆盖生效时 stderr 打印 `[EditorServer] CAESURA_EDITOR_PORT override: <port>`；默认仍为 9876）。

**Q: CMake 报 `CMake 3.25 or higher is required`**
A: 版本太旧。`cmake --version` 查看；Windows 用 winget 装最新，Linux `sudo apt install cmake`
   后 Ubuntu 24.04 自带 3.28，macOS `brew upgrade cmake`。

**Q: 编译报 `C4715: not all control paths return a value`（MSVC 警告）**
A: 见仓库记忆：修改 `main.cpp` 的 if constexpr RPC 分支链时**务必保留每个分支的
   return 收尾**——这条警告往往是静默丢 return 的前兆，运行期会 0xC0000005。

**Q: 构建卡住/内存不足**
A: 降低并行度：`cmake --build build --config Debug -j 4`。或先清理 `build/` 重来
   （`rm -rf build` 后重新 configure）。

**Q: 中文路径/带括号路径构建失败**
A: 本项目路径含 `( )`（`Caesura(AmeKAG)`），git bash 下子进程 argv 会被 cmd 分词截断
   ——用 `call "..."` 包裹，或把仓库放到无括号路径。

### 运行问题

**Q: 窗口打开但背景/立绘全缺 / 报一大片资源加载错误**
A: 从**构建目录**启动了。回到仓库根目录再运行（§3 的关键规则）。

**Q: `cannot open file: assets/...`**
A: 资源路径相对游戏根。确认 `storage=` 的路径在 `assets/` 下真实存在
   （`test -f assets/xxx` 验证），缺失时引擎会占位但不报错。

**Q: 视频不播 / 只播 MPEG-1**
A: 构建时没找到 FFmpeg（`CAESURA_ENABLE_FFMPEG=ON` 但库缺失会静默回退 pl_mpeg）。
   装 dev 包后**重新 configure + 构建**；验证：`CMakeCache.txt` 里 grep
   `CAESURA_VIDEO_FFMPEG`（只有找到 FFmpeg 才定义）。

**Q: `lua` 命令找不到**（或报 `module 'kag_runner' not found`）
A: 无需安装：构建后解释器在 `build/lua/<配置>/lua.exe`（`lua_cli` 目标产物），
   发布包内置 `external/lua/lua.exe`（注意：该文件不入库，全新克隆里没有，
   须先构建）。文档命令里的 `lua` 指其中任意一个；PATH 里没有时用完整路径。
   `module 'kag_runner' not found` 的根因是**裸 lua 的默认 package.path 不含
   `scripts/`**：本仓库入口（`entry.lua` / `kag_demo_entry.lua`）已自带自定位
   前插（P0-1 修复），headless 驱动自带前缀；你自己写的脚本若还报此错，在文件
   顶部加一行：
   `package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;scripts/kag/commands/?.lua;" .. package.path`

**Q: 引擎窗口闪退**
A: 在终端里直接运行看输出（不要双击 exe）。常见：缺 SDL3.dll（Windows Release
   运行时需把 DLL 放同目录）、从错误 CWD 启动、`--editor` 缺 token。

### 测试问题

**Q: `CaesuraTests` 报一堆资源路径错误**
A: 必须在 `build/tests/Debug` 目录下运行（CWD 需匹配资源路径），不是仓库根。

**Q: Lua 套件在 test_tween 处失败**
A: test_tween 创建全局 mock，必须在**孤儿套件**（run_orphan_tests.lua）运行。
   确认它登记在孤儿清单里；切套件由 run_orphan_tests.lua 管理。

**Q: 改了 scripts/*.lua 后 CI 报 gen-index 红**
A: `node web/gen-index.mjs` 重生成 `web/scripts-index.json` 并提交。

---

## 9. 快速验证清单（Smoke Checklist）

从克隆源码到 Demo 可跑，新开发者按序自检（每步应无错误）：

### 1. 构建

- [ ] `cmake -B build -DCAESURA_LIVE2D=OFF`（Windows MSVC 缺省单配置 Debug）
- [ ] `cmake --build build --config Debug --parallel` 零错误
- [ ] 产物存在：`build/Debug/CaesuraAmeKAG.exe`（Windows）/ `build/CaesuraAmeKAG`（单配置）
- [ ] 从**项目根目录**启动（资源路径相对 CWD 解析），不是从 build 目录：
      `./build/Debug/CaesuraAmeKAG.exe` → 应看到引擎窗口/日志，无资源加载错误

### 2. 测试

- [ ] C++ 测试：`cd build/tests/Debug && ./CaesuraTests.exe` → **0 failed, 0 skipped**（2026-08-29 实测 1120 用例 / 385790 断言）
- [ ] Lua 主套件：`build/lua/Debug/lua.exe tests/scripts/run_lua_tests.lua` → 0 failed（实测 143）
- [ ] Lua 孤儿套件：`build/lua/Debug/lua.exe tests/scripts/run_orphan_tests.lua` → 0 failed（实测 25）
- [ ] CTest：`ctest -C Debug --test-dir build --output-on-failure`（17 target，含 CaesuraGoldenVn）
- [ ] 耦合门禁：`python scripts/count_coupling.py --ci` → `PASS: All modules within thresholds`
- [ ] Web 脚本索引守卫：`node web/gen-index.mjs --check` → `CHECK OK: N modules up to date`
- [ ] 平台矩阵新鲜度：`python scripts/generate_platform_status.py --check` → `[OK] ... up-to-date`
- [ ] 首个 VN 用户旅程：`bash scripts/verify_first_vn.sh` → `RESULT: PASS (13/13 checks)`
      （⚠️ 顺序注意：其 Step12 调 `package_game.sh` 会**默认重建 web/dist**——web
      vitest 套件与该项共享该工件，需在 web 检查之后跑或使用
      `--no-web-build` 语义；工件缺失时 web 套件按设计报红，属哨兵行为）

### 3. Demo 运行

- [ ] KAG 示例游戏：`build/lua/Debug/lua.exe demo/example_game/entry.lua`（发布包内用
      `external/lua/lua.exe`；入口已自带 package.path 自定位）
      → 打印 `[ExampleGame] Loading: demo/example_game/story.ks` + `[KAG Runner] Started: ...`
- [ ] 打开 KAG 语言向导剧本：`build/lua/Debug/lua.exe scripts/kag_demo_entry.lua`
      （加载 `scripts/demo_story.ks` 并启动 Runner；无引擎时无窗口/无帧循环——逻辑冒烟级）
- [ ] 脚本契约校验：`build/lua/Debug/lua.exe scripts/ks_check.lua demo/example_game/story.ks` → 0 violations
- [ ] Web 播放器：`cd web && npm run dev:web` → 打开 http://127.0.0.1:5174 能跑剧本/看到画面

### 4. KAG3 导入器烟测（可选）

- [ ] 转换+检查：`lua scripts/kag3_import.lua --strict <scene.ks>` → 退出码 0（干净）
- [ ] 转换输出：`lua scripts/kag3_import.lua -o out/ <scene.ks>` 生成 `<name>.imported.ks`
- [ ] CARC 提取导入：`lua scripts/kag3_import.lua --carc game.carc --path assets/script/main.ks`
      （依赖 `bin/Debug/carc_pack.exe`，见 [carc-packaging](../guides/carc-packaging.md)）
- [ ] 导入器回归：`build/lua/Debug/lua.exe tests/scripts/test_kag3_import.lua` → 全绿（99 check）

### 5. 编辑器 / 视频导出（可选）

- [ ] 编辑器（单文件调试面板）：`./build/Debug/CaesuraAmeKAG.exe --editor` → 日志出现
      `[EditorServer] Serving web editor from: <repo>/web-editor/dist` 与令牌行；
      带令牌请求 `curl -H "Authorization: Bearer $(cat .caesura-editor-token)" http://127.0.0.1:9876/api/ping`
      → `{"status":"ok",...}`，不带令牌 → `401`
- [ ] React IDE（Project Manager / Monaco / Build Manager）：`cd editor && npm ci && npm run dev`
      → http://localhost:5173 出现 `Caesura Editor`，在页头填入令牌后 Connect
- [ ] 视频导出：`./build/Debug/CaesuraAmeKAG.exe --export-replay r.json --export-dir out --frames 300`
      （需真实 GPU 窗口；`--headless` 在有 `--export-replay` 时被自动忽略）

> `lua` 指 `lua_cli` 构建产物 `build/lua/<配置>/lua.exe`（发布包内则是内置的 `external/lua/lua.exe`）；把常用命令固化进脚本可省去每次重复。

---

## 10. Next Steps

- [KAG 语言教程](kag-language-tour.md) — 完整语法 + 命令分类 + 五段常用模板
- [命令契约](../../docs/api/command-contracts.md) — 134 命令权威参考（`lua scripts/schema_doc.lua` 生成）
- [Lua 模块 API](../../docs/api/lua-modules.md) — Lua 绑定 API
- [资源管线](asset-pipeline.md) — 资产格式支持矩阵 + 目录规范
- [一键打包](packaging-ux.md) — 从 .ks 到可分发 Web 站的单条命令
- [桌面发布](release-process.md) — Release + CPack + GitHub Release 全流程
- [首次贡献](../../CONTRIBUTING.md) — 参与开发的流程与门禁
