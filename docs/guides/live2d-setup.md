# Live2D Cubism SDK 集成指南

## 前置条件

- **Live2D Cubism SDK for Native** 许可
  - 免费获取: [Live2D 官网下载页](https://www.live2d.com/download/cubism-sdk/)
  - 需要同意 Live2D 专有许可协议
- 支持的平台:
  - Windows (D3D11 渲染路径，需 **MSVC (VS2022 v143 工具集)**；MinGW/Clang-cl 不支持)
  - macOS (OpenGL 路径；Metal 路径尚未完成生产验证)
  - Linux (OpenGL 渲染路径)

## 无 SDK 环境

**Caesura 默认不包含 Live2D Cubism SDK。** 没有 SDK 时：

- 引擎使用 `NullAnimationBackend`，提供降级的静态 PNG 立绘支持
- `loadModel("character.png")` 会将 PNG 作为静态纹理加载
- 动态功能（形变、眨眼、口型同步）不可用

## 安装步骤

### 1. 下载 SDK

从 Live2D 官网下载 Cubism SDK for Native R5 或更高版本。解压后你会得到类似结构的目录：

```
CubismSdkForNative/
├── Core/
│   ├── include/
│   └── lib/
├── Framework/
│   └── src/
└── Samples/
```

### 2. 放置 SDK 文件

**SDK 是专有许可，不可随仓库分发（`.gitignore` 已排除 `CubismSdkForNative-*/`），需你自行下载并解压到本地。** 推荐放在 `thirdparty/CubismSdkForNative-5-r.5/`（CMake 会自动探测该位置，无需传参）：

```
Caesura(AmeKAG)/
└── thirdparty/            # 本地依赖目录（不入库）
    └── CubismSdkForNative-5-r.5/
    ├── Core/
    │   ├── include/       # Cubism 核心头文件
    │   │   ├── Live2DCubismCore.h
    │   │   └── ...
    │   └── lib/           # 平台相关库文件
    │       ├── windows/x86_64/
    │       ├── macos/
    │       └── linux/x86_64/
    └── Framework/
        └── src/           # Live2D 框架源码
```

也可放在项目根目录（`CubismSdkForNative-5-r.5/`，同样自动探测），或通过 `CUBISM_SDK_ROOT` 指定任意位置。

### 3. CMake 配置

```bash
# 启用 Live2D 支持
cmake -B build -DCAESURA_LIVE2D=ON \
  -DCUBISM_SDK_ROOT="path/to/CubismSdkForNative-5-r.5"

# 构建
cmake --build build --config Debug --parallel
```

CMake 探测顺序：显式 `CUBISM_SDK_ROOT` → `thirdparty/CubismSdkForNative-5-r.5` → 项目根目录 `CubismSdkForNative-5-r.5`。找到后：
- 为 `CaesuraLive2D` 和组合根 `CaesuraEntry` 定义 `CAESURA_LIVE2D` 预处理器宏
- 链接对应平台的 Cubism Core 库
- 编译 `Live2DBackend` 替代 `NullAnimationBackend`

### 4. 放置模型文件

将 Live2D 模型文件放入 `assets/live2d/` 目录：

```
assets/live2d/
└── character_name/
    ├── character_name.moc3    # 模型文件
    ├── character_name.model3.json  # 模型描述
    ├── textures/              # 纹理
    │   └── texture_00.png
    ├── motions/               # 动作
    │   ├── idle.motion3.json
    │   └── tap_body.motion3.json
    └── expressions/           # 表情
        └── happy.exp3.json
```

### 5. 当前可调用入口与验证范围

2026-09-24 源码核对：模型加载、显示，以及动作、表情和参数控制的 C++ 接口位于 `src/live2d/api/IAnimationBackend.h`。引擎初始化后，通过 `BackendRegistry::instance().getAnimationBackend()` 取得接口；先确认 `isCubismAvailable()`，避免把静态 PNG 降级当作 Cubism 已初始化。Windows 真实模型验证需要 D3D11、对应模型纹理和实际渲染结果。

现有编辑器 HTTP `POST /api/live2d/load` 支持模型加载及显示。在编辑器服务器已经启动、请求满足其令牌认证要求时，可使用以下请求体；`modelPath` 必须指向实际资源根内的文件，SDK 目录存在本身不满足资源路径约束。

```json
{
  "modelPath": "assets/live2d/character_name/character_name.model3.json",
  "x": 0,
  "y": 0,
  "scale": 1,
  "show": true
}
```

取得模型句柄只证明加载调用成功。纹理、实际图像、动作和释放行为仍需分别验证；当前 SDK ON 证据与待验条件见 [U26 执行记录](../plans/2026-09-20-016-optional-sdk-cloud-boundaries-execution.md)。C++ `playMotion`、`setExpression` 与 `setParameter` 是已有入口，但动作分组参数及动作/表情对象所有权存在待真实 SDK 复现的静态问题，不能由编译成功推断播放和释放通过。

KAG 动态路径目前尚未接线。`fg` 使用纹理加载；`motion`、`expression` 并不是当前注册的对应命令。已声明的 `live2d_motion`、`live2d_expression`、`live2d_lip_sync` 只记录脚本上下文，能力目录明确标为 `command_not_wired`，现有 Lua/RPC 没有这些动态控制绑定。不要把上下文测试当作模型执行结果。

口型也需区分控制来源：C++ 可通过 `setParameter` 指定模型参数；自动读取语音包络并驱动 LipSync 参数的应用接线尚未实现。模型动作本身可能包含嘴部曲线，因此观察到嘴部运动不能单独证明语音驱动口型同步。

## 编译宏参考

| 宏 | 说明 |
|-----|------|
| `CAESURA_LIVE2D` | CMake 选项默认 OFF；启用并找到 SDK 后，也作为目标的编译宏启用真实后端 |

## 渲染路径

引擎根据平台自动选择渲染后端：

| 平台 | 首选 | 回退 |
|------|------|------|
| Windows | D3D11（bgfx 渲染器必须为 D3D11，否则初始化失败） | — |
| macOS | 取决于 bgfx 渲染器：OpenGL/GLES → OpenGLShared；Metal → MetalNative（已实现·待 macOS 实机验证） | OpenGL/GLES 分支：OpenGLReadback；Metal 分支：引擎层回退 NullAnimation |
| Linux | OpenGL | OpenGLReadback |

渲染路径实现在 `src/live2d/Live2D/` 下：
- `D3D11NativeRenderPath.cpp`
- `MetalNativeRenderPath.cpp`
- `OpenGLSharedRenderPath.cpp` / `OpenGLReadbackRenderPath.cpp`

## 历史验证记录（2026-07/08）

以下记录保留各自日期和当时的验证范围，不证明当前候选的 SDK ON、模型、动作或各平台完整验收。当前进度以 [U26 执行记录](../plans/2026-09-20-016-optional-sdk-cloud-boundaries-execution.md) 为准。

> 状态更新（2026-08-07）：**D3D11 路径再次全量编译+运行验证**（`CAESURA_LIVE2D=ON` 全量构建零错误；editor HTTP RPC `POST /api/live2d/load {modelPath:"models/Haru.model3.json"}` 返回 modelId 1，模型加载成功）。**Metal 路径由 stub 完整实现**（ObjC++ 离屏读回）；**OpenGL shader 部署缺失已修复**（FrameworkShaders 随激活渲染器复制）。OpenGL/Metal 运行验证仍需 Linux/macOS 硬件。`CAESURA_LIVE2D` 默认仍 OFF。

| 渲染路径 | 平台 | 状态 | 说明 |
|----------|------|------|------|
| `D3D11NativeRenderPath` | Windows | ✓ 已验证（2026-08-01） | 首次真实编译+运行：Haru.moc3 加载渲染成功、无设备丢失。要点：共享 bgfx D3D11 设备，`SetConstantSettings(1, device)`，Cubism 渲染进共享纹理（RTV），`bgfx::overrideInternal()` 交给 bgfx；模型纹理经 D3D11 SRV + `BindTexture`；shader 依赖 `FrameworkShaders/*.fx`（构建时复制到输出目录） |
| `OpenGLSharedRenderPath` | Windows/Linux/macOS | 代码就绪·未验证 | 随 Apple/Linux 分支进入编译（CMake 将 OpenGLShared/OpenGLReadback 一起加入源文件），但从未实机运行验证；Windows 仅编译 D3D11 路径 |
| `OpenGLReadbackRenderPath` | Windows/Linux/macOS | 代码就绪·未验证 | FBO + `glReadPixels` + `bgfx::updateTexture2D` 读回方案；OpenGL 分支 init 失败时的回退路径 |
| `MetalNativeRenderPath` | macOS/iOS | 实现完成·待 macOS 验证 | 2026-08-07 由 stub 完整实现：共享 bgfx MTLDevice，每模型 Cubism 离屏目标 + 命令队列渲染，同步读回上传 bgfx 纹理（与 GL 读回同契约）。CMake 以 OBJCXX 编译（`set_source_files_properties`），仅 `__APPLE__` 生效。需 macOS 实机验证 |
| OpenGL shader 部署 | Linux/macOS | ✓ 修复（2026-08-07） | FrameworkShaders 复制改为随激活渲染器（D3D11 .fx / OpenGL .vert+.frag）——此前 GL 复制缺失，Linux/macOS 模型运行时无 shader（C1 闭环） |
| `NullAnimationBackend`（PNG 静态降级） | 全部平台 | ✅ 已测试的默认降级 | 无 SDK 时的默认路径，由 `tests/cpp/test_live2d.cpp` 覆盖；仅支持静态 PNG 立绘 |

要点：

- **SDK 不在仓库内**：`CubismSdkForNative-5-r.5` 未随仓库提供（`.gitignore` 已忽略），需按上文「安装步骤」手动下载。
- **CI 不编译任何 Cubism 路径**：没有 SDK 就没有 `CAESURA_LIVE2D` 宏，`Live2DBackend` 及其全部渲染路径都不会进入构建。
- **D3D11 方案要点（已实现并验证）**：共享 bgfx 的 D3D11 设备与纹理（RTV+SRV）→ Cubism 渲染进共享纹理 → `bgfx::overrideInternal()` 挂给 bgfx。2026-08-01 首次真实编译并加载 SDK Haru 模型渲染成功。
- **Metal 已实现但未实机验证**：2026-08-07 已由 stub 完整实现（ObjC++ 离屏读回，CMake 以 OBJCXX 编译，仅 `__APPLE__` 生效），但尚未在真实 macOS 硬件上运行验证。注意旧 stub 日志可能声称「Falling back to OpenGL readback」，但实际代码路径是 `Live2DBackend::init()` 失败后由引擎层（`Engine::init()`，Engine.cpp 第 443-450 行）整体回退到 `NullAnimationBackend`。

### 验证路线图

有 SDK 访问权限的开发者应按以下顺序验证，并在完成后更新状态：

1. 下载 Cubism SDK for Native（R5），放到项目根目录（默认查找 `CubismSdkForNative-5-r.5`）或通过 `CUBISM_SDK_ROOT` 指定位置。
2. 执行 `cmake -B build -DCAESURA_LIVE2D=ON`，确认实际编译包含 `CAESURA_LIVE2D`、`Live2DBackend` 编译通过（2026-08-01 的编译结果仅是历史记录；当前候选需对应的新证据）。
3. Windows 上以 D3D11 渲染器运行，加载 `.moc3` 模型并渲染，验证 D3D11Native 路径（✓ 2026-08-01 完成：HTTP RPC 加载 `Samples/Resources/Haru/Haru.model3.json`，渲染多帧无崩溃、无设备丢失）。
4. 逐路径验证其余平台（Linux/macOS 的 OpenGLShared → OpenGLReadback → MetalNative），修复发现的问题后，同步更新能力矩阵（`docs/design/engine-capability-matrix.md` 的 C1 行）与本文档的状态表。

### 2026-08-01 验证记录（Windows D3D11）

首次真实编译+运行验证（SDK for Native 5-r.5，VS2022 Debug）：

- **编译修复**：3 个硬错误（`Live2DModel::textures` 成员缺失、`ILive2DRenderPath::createTexture` 静态不存在、`blitTexture` 传 `bgfx::TextureHandle` 与 `uint32_t` 形参不匹配）+ 缺失 `IRenderDevice` include。
- **运行时修复**：`CubismRenderer_D3D11::SetConstantSettings(1, device)`（模型加载前必须）、`beginFrame` 绑定共享纹理 RTV + `StartFrame/DrawModel/EndFrame` + 恢复 bgfx 渲染目标、`endFrame` 直接 `bgfx::overrideInternal`（去掉 `CopyResource`）、`cubismModel->Update()` 顶点更新、模型纹理 D3D11 SRV + `BindTexture`、renderer double-free 修复、`CubismFramework::Option` 生命周期修复。
- **shader**：`FrameworkShaders/*.fx` 由 CMake 构建时复制到输出目录；`LoadFileFunction/ReleaseBytesFunction` 回调接入。
- **验证方式**：`--editor` 模式 HTTP `POST /api/live2d/load` 加载 `Samples/Resources/Haru/Haru.model3.json`（ASCII 路径副本），多帧渲染无崩溃、无设备丢失、无 shader 编译错误、无 `ContextNum` 警告。
- **套件**：Live2D 构建与无 Live2D 构建均 976/976 通过（round 108 基线），ctest 10+AI 跳过，耦合度 PASS。
- **遗留**：OpenGLShared/OpenGLReadback 路径未实机运行验证（无对应平台环境）；Metal 已实现但待 macOS 实机验证；画面像素正确性未做视觉确认。


## 常见问题

**Q: 构建提示找不到 `Live2DCubismCore.h`**
A: 确认 `CUBISM_SDK_ROOT/Core/include/Live2DCubismCore.h` 存在。

**Q: 运行时崩溃 "Cubism Core not initialized"**
A: 确认 `.moc3` 和 `.model3.json` 文件路径正确。检查模型文件版本是否与 SDK 版本兼容。

**Q: macOS 上 Metal 渲染不工作**
A: Metal 路径已实现（2026-08-07 由 stub 完成），但尚未在真实 macOS 硬件上验证，因此暂不建议作为发布能力启用；请使用 OpenGL renderer，或保持默认 NullAnimation 降级。

**Q: 能否在 Release 构建中去掉 Live2D？**
A: 可以。不设置 `-DCAESURA_LIVE2D=ON`，引擎将使用 `NullAnimationBackend`（仅支持静态 PNG 立绘）。
