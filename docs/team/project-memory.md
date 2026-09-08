# Caesura (AmeKAG) 长期项目记忆

> **用途与范围（2026-09-08）**：本文件保存可追溯的工程经验和历史线索，按当前任务相关性读取。[AGENTS.md](../../AGENTS.md) 是规则入口，[开发指南](development-guide.md) 提供命令，[Codex 工作流](codex-workflow.md) 规定接续与证据复用；无变化内容无需每轮重读。
>
> **历史边界**：2026-09-05 前的排期、冻结规则、执行授权和“当前全绿”声明不再提供现行依据。当前方向为底层优先、Studio 暂停，计划从 [plans/README.md](../plans/README.md) 进入。第 4–6 节保留历史环境、提交和故障出处；使用时核对源码与实际证据。
>
> **更新方式**：只有形成新的可复用经验、决策或需要修正旧记录时更新对应小节，附日期与来源；一次性执行状态记入 `docs/plans/`，不强制每轮改写长期记忆。

---

## 1. 当前工程约束速查

1. **环境与 Shell 规范**：
   - Windows 开发支持当前会话的 PowerShell；仅在具体脚本要求时调用 Bash，不规定全局 Shell。
   - 路径和正文按当前 Shell 正确引用。带空格的可执行路径在 PowerShell 中用 `&` 调用，文件操作优先 `-LiteralPath`。
   - PowerShell 各版本的编码和命令连接行为不同；写源码、Markdown、Lua 时显式使用 UTF-8 或补丁工具，自动检查逐项保存退出码。
   - 提交正文避免 `${...}` 等内容被 Shell 展开，复杂正文使用明确编码的正文文件。
2. **工具操作与文件保护**：
   - **大文件（60KB+）禁止全量覆写**（如 `src/entry/Engine.cpp` 1500+ 行），必须使用增量替换/局部编辑工具，严防工具阶段截断导致 C1075 语法错误。
   - **绝对禁止盲目终止 node 进程**：严禁执行 `taskkill /F /IM node.exe`（会杀掉 DSH 宿主与 IDE 后台）。清理残留 node 进程前必须检查 PID 及命令行参数。
3. **真实性与零伪造红线（Iron Rule）**：
   - **禁止捏造或修饰任何验证证据**：凡无法提供真实可复核证据（命令输出、日志、哈希、时间戳）的平台状态，一律诚实标注为 `claimed / device-unverified (hardware-gated)`，严禁用文档声明替代实测。
   - **真实设备硬件档案严禁编造**：第 4 节记录的是历史使用设备，不能代替本次设备探测，也不能扩大为其他型号的验证结果。
   - **日志字符串必须与源码对齐**：文档中引用的日志必须与引擎实际输出格式（如 `[BgfxRenderDevice]`、`[BackendRegistry]`、`[FirstVN]`）完全一致。
4. **仓库卫生与产物出清**：
   - 构建中间件、发布包和本地验证产物按 `.gitignore` 与发布流程管理；清理前确认归属和当前任务范围，保留已有工作与可复核证据。不要把忽略规则误当作删除授权。
5. **门禁闭环与 CI 规则**：
   - **合并门槛以 AGENTS.md 与当前验证 profile 为准**：全量构建和原生测试要求不得缩减，其他检查按影响范围执行。测试总数以运行器本次实际发现为准；失败、缺失与跳过分别报告。
   - 既有证据只有在源码、依赖、环境和验证范围仍匹配时才能复用；变更或证据缺口触发必要重验，不每轮机械重跑全部套件。
   - **CI 红灯必修**：提交后若 CI 变红，必须第一时间定位修复，严禁在 CI 红灯状态下继续堆砌新功能。
   - **新增 KAG 模块预加载**：新增的 `scripts/kag/*.lua` 模块必须在 `scripts/kag/init.lua` 中注册预加载。
   - **接口变更普查同步**：修改 `src/*/api/I*.h` 接口后，必须重新运行 `python scripts/api_stats.py` 并同步更新 `docs/api/api-stats.md`。

---

## 2. 工程规范（代码 / 测试 / 提交）

### 2.1 架构与模块边界（16 模块铁律）
- **模块目录全小写**：`archive/`, `audio/`, `debug/`, `di/`, `entry/`, `input/`, `job/`, `live2d/`, `minigame/`, `platform/`, `render/`, `resource/`, `rpc/`, `script/`, `steam/`, `storage/`。
- **符号隔离**：每个模块只能通过 `api/` 子目录对外暴露符号（`src/<module>/api/I<ModuleName>.h`），接口必须为纯虚类（`= 0`），禁止包含数据成员。
- **唯一访问点**：所有后端访问必须通过 `BackendRegistry::instance().get*()`；禁止直接访问底层单例。
- **组合根唯一性**：`src/main.cpp` 与 `src/entry/` 是唯一创建具体后端对象的地方。
- **架构耦合预算**：
  - `entry` ≤ 14, `di` ≤ 14, `script` ≤ 14；
  - 其他 13 个业务模块必须 ≤ 4。执行 `python scripts/count_coupling.py --ci` 检验。

### 2.2 测试入口与证据范围

构建、定向测试、C++／Lua／Web／CTest 命令统一见[开发指南](development-guide.md)。正式执行集合与最低发现数由 [validation_profiles.json](../../scripts/validation_profiles.json) 管理；它是验证要求，不是预先写好的执行结果。

原始输出应记录命令、工作目录、源码身份、环境、发现数、失败与跳过。旧数字不能充当当前基线；测试列表增长后核对注册和 profile，但不能通过改报告数字获得绿灯。

### 2.3 提交规范与代码格式化
- **提交格式**：约定式提交 `type(scope): description`（如 `feat(kag)`、`fix(render)`、`docs(status)`、`test(input)`）。
- **提交内容**：纯净提交，不包含签名表情或外部注入元信息。
- **代码格式化**：C++ 代码统一遵守 `.clang-format`（WebKit 风格，C++20，120 列宽，4 空格缩进，指针左对齐 `PointerAlignment: Left`）。

---

## 3. 当前开发流程

1. 核对本次任务、当前计划、源码与未提交状态；先确认 `.git` 存在，再执行 Git 检查。保留已有工作，明确本次修改范围和验收条件。
2. 按 [Codex 工作流](codex-workflow.md) 选用项目技能，先复现行为缺口，再以定向测试支撑增量修改。只有存在独立且有用的子任务时才并行，并声明文件写入边界。
3. 修改后审查实际 diff；依影响范围运行必要检查。代码、测试与原始证据共同支持行为完成声明；纯文档任务可完成文档本身，不借此宣称实现已通过。
4. 按当前授权和门槛提交、推送或交付；继续跟踪相关 CI。记录结果覆盖的源码和环境，以及仍未验证的边界。

**受控证据流程**：先由 `scripts/run_validation.py` 真实执行，得到外部可信的 `run.json`；再用 `scripts/verify_release_candidate.py --generate-bundle --run <真实回执>`，同时传入 `--profile`、`--profile-name`、`--expected-run` 和新的 `--artifacts-dir`。完整示例见[开发指南](development-guide.md)。`--check` 同样要求可信 profile 与外部回执，不再只是核对手工清单内部自洽。脏工作树证据只能诊断，缺失或跳过不等于通过。

---

## 4. 历史平台与环境线索

本节保留此前开发机、设备与实现的定位信息，未在本次文档迁移中重新运行验证。版本、路径、UID、端口和状态可能变化，使用前实时确认。当前平台表述以[平台支持矩阵](../design/platform-support-matrix.md)及其引用证据为准。

### 4.1 Windows 开发机
- **工具链**：Visual Studio 2022 (MSVC v143), CMake 3.25+, vcpkg (`C:/vcpkg`), Python 3.12+, Node 18+。
- **构建输出**：桌面引擎 `build/Debug/CaesuraAmeKAG.exe`，测试程序 `build/tests/Debug/CaesuraTests.exe`。
- **编辑器端口漂移（WinNAT 动态排除段）**：Windows 动态保留端口段随 WSL2/Docker/Hyper-V 生命周期漂移，可能覆盖 9876——症状『昨天还能跑今天不行』（editor bind WSAEACCES 两实证 `bcf23b0c`/`6286146a`）。自查：`netsh int ipv4 show excludedportrange protocol=tcp`（看 9876 是否落在排除段）+ `netstat -ano | grep 9876`；解锁：管理员 `net stop winnat && net start winnat`（重排动态段，也可能不复现），或设 `CAESURA_EDITOR_PORT=<空闲端口>` 再跑 `--editor`（覆盖生效 stderr 打 `[EditorServer] CAESURA_EDITOR_PORT override: <port>`；默认仍 9876）。FAQ 见 `docs/guides/getting-started.md`。

### 4.2 Android 移动端实机
- **硬件档案**：`Redmi K40 (M2012K11AC, haydn, Snapdragon 870 / Adreno 650 / Android 13)`，Magisk Root。
- **本地工具链**：NDK r27.3 (`/d/green/ndk/27.3.13750724`)，SDL3 3.2.4 安卓源码，OpenSSL 3.3.2，JDK 17，Gradle 8.9，Ninja，Android SDK 35。
- **真机交互铁律**：
  - `adb push` 必须使用 Windows 绝对路径；
   - 历史 `su` 写入 `/data/user/0/com.caesura.app/files/caesura_root` 后需恢复应用所有权；当时 UID 为 `u0_a242`，重新安装后必须查询实际 UID，不能直接复用旧值；
  - MIUI 拒绝无 `INJECT_EVENTS` 权限的普通 adb 触控，模拟点击必须使用 `su -c input tap <x> <y>`；
  - 日志读取：`adb logcat -d | grep engine-stderr`。
- **渲染与生命周期**：
  - 分辨率适配：物理屏幕（如 2320×956）通过 `SDL_GetWindowSizeInPixels` 注入 `IRenderDevice::setPresentSize`，逻辑保持 1920×1080；
  - 横屏强制锁定：窗口创建前必须调用 `SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft,LandscapeRight")`；
  - 外部 EGL 上下文：SDL3 创建 GLES 3.0 上下文并绑定到 bgfx 平台数据。
- **存储系统**：
  - 快速存档：`slot=-1` $\rightarrow$ `save_quick.json`；
  - 自动存档：`slot=-2` $\rightarrow$ `save_auto.json`；
  - 截图两阶段：`requestScreenShot` 挂钩下一帧，避免当前帧内重复调用 `bgfx::frame()` 产生双 present。

### 4.3 渲染引擎（Render Architecture）
- **设计分辨率**：全平台统一 1920×1080，UI 布局基于 `scripts/viewport.lua` 计算相对坐标。
- **每帧提交**：`scripts/layers.lua` 对可见节点每帧提交 quad（`dirty` 标记仅用于 RTT 懒分配，不可用于跳过顶点提交）。
- **字形图集**：`TextRenderer` 采用 **2048×2048 RGBA8** 动态图集（`r=g=b=255, a=coverage`），启动时预载 `NotoSansCJKsc-Regular.otf` 约 8074 个常用字符，完美兼容移动端 GLES `fs_texture` 单一贴图直通采样。
- **bgfx 渲染器编译面由 vendored CMake 平台分支决定**：`external/bgfx/bgfx/CMakeLists.txt` 按 WIN32→D3D11+GL43、APPLE→Metal、ANDROID→GLES30、UNIX→GL43（**置于 ANDROID 之后**——CMake 在 ANDROID 下同时设 UNIX=1）、公共尾行 VULKAN=0；平台分支未命中=零渲染器编译，auto-select 全 0 分→Noop 胜出（黑屏但 exit=0 的『假绿』，Android/Linux 双实证）。引擎侧 `BgfxDeviceCore::platformDefaultBackend()` 按平台返回默认（_WIN32→Direct3D11、__APPLE__→Metal、else→OpenGL，`fcf9824b`），与 `--backend` 运行时显式覆盖互不冲突（编译面 vs 运行时首选）。init 后失配哨兵：`[RENDER] [ERROR] requested=%s actual=%s`（仅当 `caps->rendererType != s_preferredBackend`，Release 也打；requested 侧经引擎映射表 `requestedBackendName()`——bgfx 会把未编译渲染器槽位 stub 成 Noop 占位，直接 `getRendererName` 会失真）。出处 `docs/solutions/runtime-crashes/bgfx-noop-renderer-platform-gap.md` + `fcf9824b`/`80815bb7`。

### 4.4 Web 端（WebAssembly / Wasmoon）
- **路径归一化**：引擎资源路径统一归一化为 `/assets/<path>`，防止出现 `/assets/assets/` 404。
- **索引与胶水文件**：
  - `web/scripts-index.json` 是提交工件，新增 `scripts/*.lua` 后必须运行 `node web/gen-index.mjs`；
  - `glue.wasm` 必须本地 Pin（`__CAESURA_WASM_FILE__`），严禁依赖 unpkg 在线 CDN。
- **打包与冒烟**：`bash scripts/package_game.sh` + `bash scripts/verify_web_package.sh dist/<game>`（25 断言，CI Linux job 硬门）+ `node scripts/web_browser_smoke.mjs`（`--print-browser` 只解析浏览器；`CHROME_BIN` 仅对 chrome 生效；`CI=1` 加 `--no-sandbox`）。`ks_bake.lua --dir` 自 2026-09-03 起在 POSIX 可用（此前为 cmd.exe `dir /s /b`，Linux 上静默产出 0 场景）。

### 4.5 CI 与自动化门禁
- **工作流配置**：`.github/workflows/ci.yml`（Windows MSVC、Ubuntu GCC/Clang、macOS AppleClang）。
- **探针策略**：无物理设备的 macOS/iOS 编译步骤配置为 continue-on-error / `HARDWARE-GATED`。

---

## 5. 状态与决策索引

### 5.1 历史版本记录（非当前验收）

旧记录以提交 `9f5a022f` 和候选版本 `1.0.0-rc.1` 为背景，曾写有全绿与 RC-GO 结论。它们不证明当前 HEAD、测试状态或发布就绪；缺少受控执行来源的旧 RC 清单不能作为验收证据。当前工作状态从 [plans/README.md](../plans/README.md) 对照源码和对应原始验证产物核实。

### 5.2 历史验证记录与缺口

以下是旧记录的声明及证据定位线索，保留其追溯价值；不继承为本轮通过结论。

- **旧记录声称已实测的范围**：
   - Windows x64 引擎运行、编辑器 RPC 与 C++ 测试；
  - Linux WSL/Ubuntu 无头与窗口化验证（注：t92 之前的『窗口化』存在 requested/actual 失配假绿风险，真渲染绿证见下方 ⏳ 项）；
   - Web 播放器 PWA 离线缓存、DOM 渲染和 Wasmoon 测试；
  - Android 真机（Redmi K40）全链路闭环：启动、渲染、字形、分支、存档、生命周期。
- **旧记录中的声明、硬件缺口与后续追溯**：
  - **IME 输入法候选词真机输入**：C++ 底层接口与 Lua 绑定已闭环，真机实体输入法弹出与实际打字处于 `claimed / device-unverified (pending)`；
  - **iOS 真机实测**：12/12 Metal Shaders 与 Xcode 流程完备，物理真机处于 `hardware-gated`；
  - **低内存压力中断（onLowMemory）**：真机物理触发未覆盖；
  - **多指手势注入**：单测覆盖，真机物理多指注入未实测；
  - **Linux 真渲染绿证（GL 实际渲染、非 Noop）**：**已获证（round-7，run 33245271845 @ 0fce3311）**——Linux · Package verify 30/30 含 renderdisabled=0，为 Linux 发布包真渲染首证（M1）；round-4 教训参考：Linux 包内游戏曾 requested=D3D11→actual=Noop、exit 0 假绿；
  - **macOS §3 红（editor 检查）**：**已修复并获绿证（round-8 run 33248475888 verify 30/30；自 round-9 起 mac verify 硬门）**——round-7 红定性为 verify 脚本 ps 探测误报（mac runner 对活进程 ps -args 返回空→活判死+首查 000；证据：无 .ips、demo 探针 rc=0、GPU degrade WARN 证明帧循环长期运行、rc 未落盘=进程未退出、同节 browser-nav/api-ping 通过），修复=launcher 直记 pid/rc 判死（`0422c17c` §3/§4 对称；mac 实测 editor ready 3s/token 1s，45s 上限余量充足）。取证工具链保留：`bca67d42` 抓 `CaesuraAmeKAG*.ips` 头 + 纯诊断 demo 探针。

### 5.3 关键历史决策与评估
- **KAG3 生态兼容决策**：
  - 决策：通过 `scripts/kag3_import.lua`、`.xp3` 归档解析器与 TLG5/6 解码器提供 KAG3 导入管道；
  - 《LimeLight Lemonade Jam》商业作品移植评估：引擎核心能力 100% 就绪，移植阻塞点为官方加密 XP3（需通过提取后的 `unencrypted.xp3` 载入）。
- **统一语义层（KAG Semantic Layer）**：
  - 决策：在 `scripts/kag/semantic.lua` 建立统一 AST / CFG 模型，作为 Story Flow、i18n、LSP、CLI 统一真相源，根除正则表达式解析分歧。
- **『响亮降级』配套铁律**：把崩溃改成 exit 0 + 降级标记（渲染禁用/静音继续跑）的修复，**必须同批**给验证层加『降级标记缺席』断言——退出码从此不再是该子系统健康的充分证据（实例：音频 NullAudio 降级靠 packaged-lua 正属性兜底；渲染 IFH 守卫靠 `verify_release_package.sh` §5 grep `rendering disabled (BGFX_DEBUG_IFH)`=0，配套 `c1f43885` 让包验证在守卫被触发时失败）。
- **IFH 禁渲染门只看核心程序**：`coreProgramsBroken()` = `{fallback, blend}` 两程序（vsSprite+fsTexture / vsFullscreen+fsBlend）任一无效才禁用渲染（`BGFX_DEBUG_IFH` + `[RENDER][ERROR]` 标排行，帧循环继续）；非核心（vfx/transition/stretch/affine/postfx）失败→响亮计数 + 各自 draw 点守卫跳过，**渲染继续**（单坏 VFX 不得黑屏；`e771c853`）。
- **blend 合法枚举 uniform 驱动同程序别名**：28 模式共用单 `fs_blend` 程序（模式经 `u_blendParams.w` uniform 传递），未注册合法键（10/11/16）别名到确定性靶 `NormalKey()` 并计数（`m_aliasedVariants` 为 doctest 判别面）；越界键保持响亮 ERROR + Normal 降级；`precompileCommon` 改注册表快照同源迭代根除预编/注册双清单漂移（`7fa1c40f`）。
- **EditorServer bind 失败响亮化 + lastError 契约**：bind 失败 stderr 打 `[EditorServer] [ERROR] failed to bind 127.0.0.1:<port>: socket error <code>`（10013=排除段/权限，10048=被占）+ 排查提示（不再静默退出），并暴露 `lastError()` 供测试断言（`bcf23b0c`）。

---

## 6. 踩坑库（按模块持续沉淀）

| 模块 | 问题现象 | 根本原因 | 解决方案 | 验证与来源 |
|---|---|---|---|---|
| **Render** | Android GLES 下文字全黑或不可见 | TTF 原使用 R8 单通道贴图，GLES `fs_texture` 采样丢失 Alpha | 重构为 RGBA8（`r=g=b=255, a=cov`）2048×2048 图集，预载 Noto 8074 字符 | Commit `c6170e39` / Round 028 |
| **Render** | QuadBatch 多纹理立绘丢失 | 单批次仅分配单个 TransientBuffer，首个 `bgfx::submit` 后被 GPU 丢弃 | 改为每个 `MergeGroup` 独立分配 TransientBuffer 并重设 `bgfx::setState` | Round 028 / `BgfxQuadBatch.cpp` |
| **Render** | 对话框 RTT 被立绘拉伸覆盖 | Lua 层 RTT ID 与普通纹理 ID 小整数重叠，`resolveTexture` 误命中立绘 | 解耦 `tex` 与 `rt` 查询分支，`rt` 严格走 `getViewportTexture` | Round 028 / `RenderBinding.cpp` |
| **Input** | 手机屏幕点击分支按钮无响应 | Android 物理像素（2320×956）与 1920×1080 逻辑坐标未换算 | 在 `Engine.cpp` 中增加物理到逻辑视口比例缩放映射 | Round 028 / `Engine.cpp` |
| **Platform** | Android 手机旋转时画面错位 | Manifest 横屏配置被 SDL3 默认系统旋转行为覆盖 | 窗口创建前调用 `SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft,LandscapeRight")` | Commit `c6170e39` |
| **Android** | `adb push` 脚本后应用报 Module not found | su 写入的文件所有者为 `root:root (600)`，应用无读取权限 | 写入后恢复应用实际 UID 的所有权；当时为 `u0_a242`，不得照抄到重新安装的应用 | Plan 027 / Android 指南 |
| **Android** | `adb shell input` 模拟点击被 MIUI 拦截 | MIUI 限制 uid2000 调用 `INJECT_EVENTS` 接口 | 必须通过 root 通道执行 `su -c input tap <x> <y>` | Plan 027 / Android 指南 |
| **Audio** | 存档截图时音频微顿与双重 Present | `captureThumbnailPNG` 帧内同步调用 `bgfx::frame()` | 改为挂钩下一帧两阶段异步捕获，消除帧内重复 present | Round 028 / `SaveManager.cpp` |
| **Script** | 帧回调中 `require` 报 not preloaded | 沙箱清除了 searchers，仅允许 `package.loaded` | 所有运行时子模块必须在 `scripts/kag/init.lua` 中预载 | Plan 027 / `sandbox.lua` |
| **Web** | Web 玩家静态资源 404 报错 | 引擎资源默认带 `assets/` 前缀，Web 侧拼接导致 `/assets/assets/` | 在 `web/main.mjs` 中增加路径归一化剥离多余前缀 | Plan 027 / Track W |
| **Toolchain** | Windows Python 子进程乱码与管道卡死 | Windows 默认使用 GBK 编码导致 UTF-8 字符解码崩溃 | 子进程显式声明 `encoding="utf-8", errors="replace"` | Commit `31e2fb32` / CLI |
| **QA / RC（历史实现）** | 后续日常提交导致 RC `--check` 误报红 | 旧 Verifier 将 HEAD Commit 与 Manifest 写死比对 | `9f5a022f` 当时改为清单自洽校验；该策略现已被受控执行回执、可信 profile 和源码身份绑定替代，不再作为当前流程 | Commit `9f5a022f` / 当前 `scripts/verify_release_candidate.py` |
| **Matrix** | 平台矩阵 `test: null` 绕过校验 | `str(None)` 转换为字符串 `"None"` 导致空判定失效 | 显式拦截 None 值并增强 probe 状态下引用文档存在性校验 | Commit `5a87ca86` / Matrix |
| **Render（日志）** | 引擎日志分段两种形态并存——多数 `[RENDER] [ERROR]` 带空格，BgfxRenderDevice IFH 行 `[RENDER][ERROR]` 无空格 | debug 宏经 `Caesura::debug::log`（`[%s] [%s]` 带空格分段），IFH 行是手写 printf 紧连 | 断言/复核 grep 用单 token（`grep -F '[RENDER]'`）或带空格/无空格双形态各测；连写 `[RENDER][ERROR]` 漏掉多数日志、带空格漏掉 IFH 行（曾致误报『0 错误』） | Round34 教训 / `BgfxRenderDevice.cpp:115,395` |
| **Render（shader repack）** | GL 内嵌数组手工 repack 后 `createProgram` 返回 INVALID / 编译失败 | 三坑：①uniform 记录缺 texInfo/texFormat 字段（ver>=8/>=10 应有 2+2 字节，每条记录固定 10 字节尾部）；②`fsh.hashIn` 必须等于配对 `vsh.hashOut`（bgfx_p.h:5140 配对检查，失败即 INVALID，BX_TRACE 不可见）；③bgfx 内部 debugfont 覆盖层用 `gl_FragColor`（GLSL 3.30 后移除，C7616 WARN——观察项非业务 bug，别与业务数组症状混淆） | 按布局重建（shaderSize **替换而非 append**）+ `fs[4:8]=vs[8:12]` + 字节 walk 回放校验 + `--backend opengl --frames 60` 断言 program READY 无 build failed；工具 `scripts/repack_gl_embed.py`（--hashin-from-vs，幂等） | `docs/solutions/build-errors/bgfx-shader-binary-repack.md` / t79 实测 |
| **QA/RC（历史实现）** | 本机 RC 对抗突变测试随 Lua 套件增长出现旧基线不匹配 | 旧脚本动态读取套件总数，历史清单落后于测试注册 | 不再手工重写清单数值：按当前 profile 真实执行后，用 `--generate-bundle --run <真实回执>` 收集；可信来源参数见第 3 节。当前可选证据缺失的 exit 77 仅表示跳过 | 旧脚本 `get_lua_suite_counts` 设计 / 当前 `scripts/run_validation.py` 与 `scripts/verify_release_candidate.py` |
