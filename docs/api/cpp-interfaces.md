# Caesura (AmeKAG) — C++ API Interface Reference

> **38 个 API 接口头文件；28 个具名运行时服务槽位通过 `BackendRegistry` 访问**
> 最后更新: 2026-09-06；逐文件统计见 [API Statistics](api-stats.md)。同一个接口头可以包含多个纯虚类，头文件数不等于类数。
>
> **更新记录**: 2026-08-23 — STEP14 (Track P5 Audio Focus Service): interface census 33 -> 34 (+`IAudioFocusService`)，Registry 服务槽位 24 -> 25
> 2026-08-23 — STEP13 (Track P4 组合根默认存档 provider): 无新增接口头（census 保持 33；`Engine::init` 缺省安装 `LocalFileSaveProvider`，修复未装 provider 时存档静默失效）
> 2026-08-23 — STEP12 (Track P3 统一指针输入路径): `IInputRouter` 扩展 `submitPointer` + `PointerAction`/`PointerEvent` 类型（无新增接口头，census 保持 33）
> 2026-08-23 — STEP11 (Track P2 Lifecycle Service): interface census 32 -> 33 (+`ILifecycleService`)，Registry 服务槽位 23 -> 24
> 2026-08-23 — STEP10 (Track P1 Display Service): interface census 31 -> 32 (+`IDisplayService`)，Registry 服务槽位 22 -> 23
> 2026-08-15 — round-75 docs sync: interface census 30 -> 31

---

## 目录

1. [archive — 加密归档](#1-archive--加密归档)
2. [audio — 音频后端](#2-audio--音频后端)
3. [debug — 日志与性能分析](#3-debug--日志与性能分析)
4. [di — 依赖注入与资源配额](#4-di--依赖注入与资源配额)
5. [entry — 引擎组合根](#5-entry--引擎组合根)
6. [input — 输入路由](#6-input--输入路由)
7. [job — 多线程任务系统](#7-job--多线程任务系统)
8. [live2d — 动画后端](#8-live2d--动画后端)
9. [minigame — 3D 小游戏](#9-minigame--3d-小游戏)
10. [platform — 平台抽象](#10-platform--平台抽象)
11. [render — 渲染 (7 个接口)](#11-render--7-个接口)
12. [resource — 资源管理](#12-resource--资源管理)
13. [rpc — HTTP/JSON-RPC 服务器](#13-rpc--httpjson-rpc-服务器)
14. [script — Lua 虚拟机](#14-script--lua-虚拟机)
15. [steam — Steamworks 集成](#15-steam--steamworks-集成)
16. [storage — 存档/读档](#16-storage--存档读档)

---

## 访问规则

引擎运行时后端通过 `BackendRegistry::instance()` 访问：

```cpp
auto* renderer = BackendRegistry::instance().getRenderDevice();
auto* audio    = BackendRegistry::instance().getAudioBackend();
auto* lua      = BackendRegistry::instance().getLuaManager()->state();
```

- BackendRegistry 存储非拥有指针（`I*`），Engine 持有对应后端的 `unique_ptr` 所有权。
- 子系统通过 `set*()` 注册，通过 `get*()` 访问。
- 禁止绕过 BackendRegistry 直接访问单例（DEBUG_* 宏除外）。
- RPC/Editor 属于宿主入站传输适配器，不注册到 BackendRegistry。宿主负责其所有权；当前 `main.cpp` 实例化 `RpcServer`（stdio），`--editor` 模式下另启动 `EditorServer`（HTTP，18 端点）。

### U11：恢复资源的准备与提交

恢复入口先读取和验证独立资源，再交给后端提交。准备阶段不替换当前 GPU 纹理、字体或音源；准备对象由 `unique_ptr` 持有，提交消费一次。提交后的错误由会话恢复层统一停止并清理，不能继续执行混合的新旧状态。

| 接口 / 头文件 | 方法与契约 |
|---|---|
| `resource/api/IAssetReader.h` | `readAsset(path, maxBytes) -> vector<uint8_t>`：按资源优先级读取，最高优先级来源失败不能回退到其它内容；空结果表示不可用、失败或超限。上限约束返回字节，各 provider 的读取峰值另有约束。 |
| `resource/api/IImageDecoder.h` | `decode(bytes, size, maxDecodedBytes) -> DecodedImage`：只做 CPU 解码；结果包含 `rgba`、`width`、`height`、`ok`，不暴露第三方图像类型。 |
| `audio/api/IAudioRestore.h` | `captureAudioState()`、`prepareAudioState(state, bytes, size)`、`applyAudioState(unique_ptr<IPreparedAudioState>)`、`stopSessionAudio()`。准备对象仅通过 `description()` 暴露描述。 |
| `render/api/IRenderDevice.h` | `captureFontState()`、`defaultFontState()`、`prepareFontState(state, bytes, size)`、`applyFontState(unique_ptr<IPreparedFontState>)`、`clearFontState()`。准备对象仅通过 `description()` 暴露描述。 |
| `render/api/ITextureManager.h` | `describeTexture(id, TextureSourceInfo&)` 描述资源路径或 RGBA 纯色来源；未知、临时纹理返回 false 且不改变输出。恢复通过 `loadTextureFromRGBA` 创建独占纹理。 |
| `render/api/IVideoPlayer.h` | `closeAll()` 逻辑停止全部视频，沿用播放器更新中的延迟释放；保留后端初始化状态。 |
| `render/api/IParticleSystem.h` | `activeEmitterCount() const` 查询活动发射器；`shutdown()` 清空发射器和粒子池，恢复层按原初始化状态决定重新初始化。 |
| `live2d/api/IAnimationBackend.h` | `loadedModelCount() const -> size_t`、`clearModels()`；清空模型但保留后端与设备关联，不复用旧模型句柄。 |

`AudioRestoreState` 保存 BGM 路径、秒位置、声源增益和循环状态；可选精确游标由 `framePosition`、20 位 `frameFraction`、`sourceRate` 与 `outputRate` 组成，`sourceRate == 0` 表示旧的秒位置格式。恢复停止 voice/SE，用户主音量与总线音量不属于存档。

`FontRestoreState` 包含 `active`、`FontId::{Small,Large,TTF}`、资源路径与像素尺寸。准备阶段拥有字体字节与 CPU 字形数据，提交和设备恢复不重新读取来源文件。

Engine 将 `IAssetReader`、`IImageDecoder`、`IAudioRestore` 注册到 Registry；Registry 只持有非拥有接口指针。音频恢复接口由现有音频后端实现，不创建第二套播放系统。Lua 调用契约见 [Restore](lua-modules.md#restore)。

---

## 1. archive — 加密归档

**命名空间**: `Caesura::carc`
**实现**: CARCReader, CARCWriter, CryptoEngine
**用途**: 将游戏资源打包为单个加密归档文件（.carc），支持 Ed25519 签名验证。

### 1.1 IArchiveReader

```cpp
using ArchivePublicKey = std::array<uint8_t, 32>; // Raw Ed25519 public key

class IArchiveReader {
public:
    virtual ~IArchiveReader() = default;

    virtual bool open(const std::string& path,
                      const std::string& pubKeyPath = "") = 0;
    virtual bool open(const std::string& path,
                      const ArchivePublicKey& expectedKey) = 0;
    virtual void close() = 0;
    virtual std::vector<uint8_t> readFile(const std::string& relativePath) = 0;
    virtual bool hasFile(const std::string& relativePath) const = 0;
    virtual size_t numFiles() const = 0;
    virtual bool isOpen() const = 0;
};
```

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `open(path, pubKeyPath)` | 归档路径；可选显式公钥文件路径 | `bool` | 始终验签；未指定公钥时使用归档内嵌公钥，仅证明自签内容一致性；显式公钥读取失败不回退 |
| `open(path, expectedKey)` | 归档路径；宿主提供的32字节公钥 | `bool` | 用宿主固定公钥验签并解密索引，归档或旁置文件不能更换验证公钥 |
| `close` | — | — | 关闭归档，释放资源 |
| `readFile` | `relativePath`: 归档内相对路径 | `vector<uint8_t>` | 读取文件内容，未找到返回空 |
| `hasFile` | `relativePath` | `bool` | 检查文件是否存在 |
| `numFiles` | — | `size_t` | 归档内文件总数 |
| `isOpen` | — | `bool` | 归档是否已打开 |

EngineConfig 的 `archiveTrustMode` 默认为 `ArchiveTrustMode::Compatible`。选择 `PinnedPublisher` 时，必须在 `Engine::init()` 前填入 `archivePublisherKey`；Engine持有按值复制的公钥。固定公钥约束覆盖全部识别到的自动挂载层，任一验证失败则初始化失败并回滚，不降级到低优先级资源。此策略只认证CARC，松散文件、入口Lua、manifest与宿主本身仍不因此获得认证。

### 1.2 IArchiveWriter

```cpp
class IArchiveWriter {
public:
    virtual ~IArchiveWriter() = default;

    virtual bool create(const std::string& outputPath,
                        const std::string& privateKeyPath = "",
                        const std::string& publicKeyPath = "") = 0;
    virtual bool addFile(const std::string& relativePath,
                         const uint8_t* data, size_t size) = 0;
    virtual bool finalize() = 0;
};
```

| 方法 | 说明 |
|------|------|
| `create` | 创建输出归档，可选 Ed25519 签名密钥 |
| `addFile` | 添加文件到归档（`relativePath` 为内部路径） |
| `finalize` | 写入索引和签名，关闭归档 |

### 1.3 ICryptoEngine

```cpp
class ICryptoEngine {
public:
    virtual ~ICryptoEngine() = default;

    // AES-256-GCM 加解密
    virtual std::vector<uint8_t> encrypt(
        const uint8_t* plaintext, size_t plaintextLen,
        const uint8_t* key, size_t keyLen,
        uint8_t* nonce, size_t nonceLen,
        uint8_t* tag, size_t tagLen) = 0;

    virtual std::vector<uint8_t> decrypt(
        const uint8_t* ciphertext, size_t ciphertextLen,
        const uint8_t* key, size_t keyLen,
        const uint8_t* nonce, size_t nonceLen,
        const uint8_t* tag, size_t tagLen) = 0;

    // SHA-256
    virtual void sha256(const uint8_t* data, size_t len,
                        uint8_t* hash, size_t hashLen) = 0;

    // Ed25519 签名/验证
    virtual bool sign(const uint8_t* data, size_t len,
                      const uint8_t* privateKey, size_t privateKeyLen,
                      uint8_t* signature, size_t signatureLen) = 0;
    virtual bool verify(const uint8_t* data, size_t len,
                        const uint8_t* publicKey, size_t publicKeyLen,
                        const uint8_t* signature, size_t signatureLen) = 0;

    // 随机数生成
    virtual void generateKey(uint8_t* key, size_t keyLen) = 0;
    virtual void generateNonce(uint8_t* nonce, size_t nonceLen) = 0;
    virtual void generateKeyPair(uint8_t* publicKey, size_t publicKeyLen,
                                 uint8_t* privateKey, size_t privateKeyLen) = 0;

    // 密钥文件 I/O
    virtual bool readPublicKey(const std::string& path,
                               uint8_t* key, size_t keyLen) = 0;
    virtual bool readPrivateKey(const std::string& path,
                                uint8_t* key, size_t keyLen) = 0;
    virtual bool writePublicKey(const std::string& path,
                                const uint8_t* key, size_t keyLen) = 0;
    virtual bool writePrivateKey(const std::string& path,
                                 const uint8_t* key, size_t keyLen) = 0;
};
```

---

## 2. audio — 音频后端

**实现**: SoLoudAudioEngine
**用途**: BGM / Voice / SE 三总线音频播放，支持 3D 空间音效。
**注册**: `BackendRegistry::instance().setAudioBackend()`；音频焦点中枢另经 `setAudioFocusService()` 注册（见下）。

### IAudioBackend

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `init` | — | `bool` | 初始化音频引擎 |
| `isPlaybackAvailable` | — | `bool` | owner 线程查询实际实施会话；Null 始终为 false，ManualMix 可用不等于物理输出 |
| `shutdown` | — | — | 关闭并释放所有资源 |
| `update` | `deltaTime` | — | 每帧更新音频系统 |
| `suspend` | — | — | 暂停全部播放（混音器暂停，保留已加载资源） |
| `resume` | — | — | 从暂停位置继续播放 |
| `playBGM` | `file`, `fadeTime=1.0f` | `uint` | 播放背景音乐（支持淡入） |
| `stopBGM` | `fadeTime=1.0f` | — | 停止 BGM（支持淡出） |
| `playVoice` | `file` | `uint` | 播放语音（绝对中断前一条） |
| `stopVoice` | — | — | 停止语音 |
| `playSE` | `file` | `uint` | 播放 2D 音效 |
| `playRawPCM` | `samples`, `numFrames`, `sampleRate`, `channels` | `uint` | 播放交错的 float PCM（视频音频等），返回句柄 |
| `playSE3D` | `file`, `x, y, z` | `uint` | 播放 3D 空间音效 |
| `stopSE` | — | — | 停止所有音效 |
| `setSEVolume` | `handle`, `volume` | — | 设置单个音效句柄的音量 |
| `getSEVolume` | `handle` | `float` | 获取单个音效句柄的音量 |
| `stopSEHandle` | `handle` | — | 停止指定句柄的音效 |
| `update3dListener` | `posX/Y/Z, atX/Y/Z, upX, upY, upZ` | — | 更新 3D 听者位置 |
| `setGlobalVolume` | `volume` | — | 设置全局音量 (0.0–1.0) |
| `getGlobalVolume` | — | `float` | 获取全局音量 |
| `setBusVolume` | `bus`, `volume` | — | 按总线设置音量（"bgm"/"voice"/"se"） |
| `getBusVolume` | `bus` | `float` | 按总线获取音量 |
| `flushWaveCache` | — | — | 清空波形缓存 |
| `isVoicePlaying` | — | `bool` | 语音是否播放中 |
| `isBGMPlaying` | — | `bool` | BGM 是否播放中 |
| `isSEPlaying` | — | `bool` | 是否有音效播放中 |
| `activeVoiceCount` | — | `int` | 当前活跃语音数 |
| `consumeVoiceCompletions` | — | `uint` | 返回并清零自上次调用以来自然结束的语音行数 |
| `getPosition` | `bus` | `float` | 总线当前播放位置（秒） |
| `getLength` | `bus` | `float` | 总线当前曲目总长度（秒） |
| `fadeVolume` | `bus`, `targetVolume`, `fadeTime` | — | 平滑过渡到目标音量 |
| `getBackendName` | — | `const char*` | 后端名称 |

### IAudioFocusService

**接口文件**: `src/audio/api/IAudioFocusService.h`（`AudioFocusEvent` / `AudioFocusState` 枚举与监听器接口被方法按值传递，依 AGENTS.md §2 定义在接口头文件内）

OS 音频焦点/瞬态中断仲裁的平台无关最小馈送端（Track P5）。桌面/Web 没有 OS 仲裁——服务保持空闲等待；Android/iOS 的原生音频中断回调把事件 post 进来（焦点丢失/中断开始 → 避让或暂停；焦点恢复/中断结束 → 继续）。

#### 事件与状态

```cpp
enum class AudioFocusEvent : uint8_t {
    FocusGained = 0,        // 应用重新取得独占焦点（正常播放）
    FocusLost = 1,          // OS 收回/移除音频焦点（持久）
    InterruptionBegin = 2,  // 瞬态中断开始（来电、Siri、闹钟…）
    InterruptionEnd = 3,    // 瞬态中断结束
};

enum class AudioFocusState : uint8_t { Normal, Lost, Interrupted };

class IAudioFocusListener {
public:
    virtual void onAudioFocusEvent(AudioFocusEvent event) = 0;
};
```

| 状态 | 进入条件 |
|------|----------|
| `Normal` | 初始状态；`FocusGained`；或处于 `Interrupted` 时收到 `InterruptionEnd` |
| `Lost` | `FocusLost` |
| `Interrupted` | `InterruptionBegin` |

#### 服务方法

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `addListener` | `IAudioFocusListener*` | — | 注册监听器（重复注册被忽略） |
| `removeListener` | `IAudioFocusListener*` | — | 注销监听器（null 安全） |
| `post` | `AudioFocusEvent` | — | 投递事件：先状态机迁移，再在投递线程按注册顺序同步派发监听器快照副本（与 `LifecycleService` 同契约） |
| `currentState` | — | `AudioFocusState` | 查询当前焦点状态 |

#### AudioFocusService 状态机语义

默认实现是 audio 模块的 header-only 中枢（`src/audio/AudioFocusService.h`），自身从不触碰音频设备——暂停什么由消费方决定：

- `FocusLost → Lost`、`FocusGained → Normal`、`InterruptionBegin → Interrupted`
- `InterruptionEnd` 仅当当前处于 `Interrupted` 时才回到 `Normal`（从未开始的中断结束是 no-op，防止误恢复）
- 所有方法线程安全（mutex 保护）；`post` 在锁外派发快照，监听器可在回调中安全 `removeListener`

#### Engine 接线（组合根）

`Engine::initPlatformPhase()` 创建并持有 `unique_ptr<AudioFocusService>`，先 `addListener(this)` 再 `setAudioFocusService()` 注册到 BackendRegistry。Engine 同时实现 `IAudioFocusListener`：`FocusLost` / `InterruptionBegin` → `IAudioBackend::suspend()`，`FocusGained` / `InterruptionEnd` → `resume()`（SoLoud suspend 幂等，可与生命周期后台挂起叠加而不冲突）；游戏/图层也可自行 `addListener`（例如中断时暂停玩法逻辑）。`shutdown()` 先 `removeListener(this)` 再 teardown，防止销毁期事件打到将析构的后端。

#### 移动端入口

桌面/Web 无 OS 音频仲裁，该 hub 就是 Android JNI（Android 音频焦点）与 iOS audio-session 中断通知的文档化接入点：原生层收到系统中断回调后调用 `post(...)` 即可，Engine 的挂起/恢复响应自动生效。原生回调接线尚未完成，属 Track M/I 范围。

---

## 3. debug — 日志与性能分析

**实现**: DebugManager
**访问方式**: `BackendRegistry::instance().getDebugManager()` 或直接通过 `DEBUG_*` 宏（零开销路径）

### 3.1 枚举类型

| 枚举 | 值 | 说明 |
|------|-----|------|
| `DbgLevel` | Trace=0, Debug=1, Info=2, Warn=3, Err=4, Fatal=5 | 日志级别 |
| `SubSys` | Render=0, Audio=1, Scripting=2, Input=3, Platform=4, Engine=5, Dbg=6 | 子系统标识 |
| `ErrCode` | 见头文件 `IDebugManager.h` | 错误码（按子系统分段：1xxx=Platform, 2xxx=Render, 3xxx=Audio, 4xxx=Script, 6xxx=Engine, 9xxx=Internal） |

### 3.2 IDebugManager

| 方法 | 说明 |
|------|------|
| `init(logDir)` | 初始化日志系统，指定日志目录 |
| `shutdown` | 关闭，刷新缓冲区 |
| `log(level, subsystem, code, fmt, ...)` | 记录带错误码的日志 |
| `log(level, subsystem, fmt, ...)` | 记录无错误码的日志 |
| `lastError` | 返回最近一条错误日志 |
| `errorCount` | 错误总数 |
| `entryCount` | 日志条目总数 |
| `subsystemErrorCount(s)` | 按子系统统计错误数 |
| `ringBuffer` | 返回日志环形缓冲区 |
| `getSubsystemStats(s)` | 返回子系统的 `SubsystemStats` |
| `dumpFullReport` | 生成完整报告字符串 |
| `beginProfile(label)` | 开始性能采样 |
| `endProfile(label)` | 结束性能采样 |
| `recordGpuSubmit(count)` | 记录 GPU 提交数 |
| `recordTransientAlloc(count, bytes)` | 记录瞬态分配 |
| `recordLuaGc(ms)` | 记录 Lua GC 耗时 |
| `getFrameProfile` | 返回 `FrameProfile`（含 totalMs, gpuSubmitMs, luaGcMs） |
| `beginFrameProfile` | 开始当前帧的性能采样 |
| `endFrameProfile` | 结束当前帧的性能采样 |
| `getRenderInfo` / `setRenderInfo` | 渲染子系统信息 |
| `getAudioInfo` / `setAudioInfo` | 音频子系统信息 |
| `getInputInfo` / `setInputInfo` | 输入子系统信息 |
| `logFilePath` | 当前日志文件路径（`const std::string&`） |

---

## 4. di — 依赖注入、资源配额与设备恢复

### 4.1 ISandboxQuota

Lua 沙箱资源配额管理，防止脚本资源泄漏。

| 方法 | 说明 |
|------|------|
| `setLuaState(L)` | 注入 `lua_State*`（配额服务依赖） |
| `tryAlloc(kind)` | 尝试分配资源（如 "textures", "particles_emitters"） |
| `release(kind)` | 释放资源计数 |
| `count(kind)` | 当前使用计数 |
| `maxLimit(kind)` | 最大限制 |

### 4.2 ITextureBudget

纹理内存预算自动检测和分级。

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `detect` | — | 自动检测系统内存，确定预算等级 |
| `setTier(tier)` | — | 手动设置 0–5 级 |
| `getTier` | `int` | 当前预算等级 (0=Low/128MB … 5=DevOverride/4GB) |
| `getBudgetMB` | `uint32_t` | 预算（MB） |
| `getBudgetBytes` | `uint64_t` | 预算（字节） |
| `getTierName` | `const char*` | 预算等级名称 |
| `isAutoDetected` | `bool` | 是否自动检测（false=手动设置） |

### 4.3 IDeviceLostListener

GPU 设备丢失恢复的监听契约。持有 GPU 资源的模块通过
`BackendRegistry::registerDeviceLostListener()` 注册；它不是 23 个服务槽位之一。

| 方法 | 调用时机 | 说明 |
|------|----------|------|
| `onDeviceLost()` | 渲染后端关闭前 | 销毁 GPU 句柄，保留文件路径、像素缓冲等 CPU 侧恢复数据 |
| `onDeviceRestored()` | 渲染后端重新初始化成功后 | 从 CPU 侧数据重建 GPU 资源 |

两个回调都在主线程、Lua 暂停期间执行；回调中不得提交绘制或推进渲染帧。

---

## 5. entry — 引擎组合根

**无独立接口**（`main.cpp` 与 `entry/` 共同构成组合根，是生产代码中允许
include 具体实现头文件并创建后端对象的位置）。

核心类型：
- `EngineConfig` — move-only 依赖包；构造 `Engine(std::move(config))` 时转移所有注入后端的所有权
- `Engine` — 四阶段初始化：`initPlatformPhase()` → `initScriptingPhase()` → `initAssetPhase()` → `initOptionalPhase()`

`Engine::init()` 每个实例只允许调用一次。任一必需阶段失败会立即回滚；公开服务访问器只在初始化成功后可用。关闭顺序保证异步加载先停止，再关闭资产管理器和任务系统，最后清空 `BackendRegistry` 的非拥有指针。

参见 [Engine.cpp](../../src/entry/Engine.cpp) 和 [AGENTS.md](../../AGENTS.md) 第 4 节。

---

## 6. input — 输入路由

**实现**: InputRouter
**注册**: `BackendRegistry::instance().setInputRouter()`

### IInputRouter

```cpp
enum class InputFocus { KAG, GAME };
using GameInputCallback = std::function<void(const SDL_Event&)>;

// 统一指针输入（Track P3）：平台无关指针抽象，原生触摸源无需暴露 SDL_Event
enum class PointerAction : uint8_t {
    Down,       // 首次触点
    Move,       // 触点移动
    Up,         // 触点释放
    LongPress,  // 按压超过典型阈值（约 500ms）
    Pinch,      // 双指缩放增量（scale 为累积值）
};

struct PointerEvent {
    PointerAction action = PointerAction::Move;
    float x = 0.0f;             // 窗口逻辑像素
    float y = 0.0f;
    float scale = 1.0f;         // pinch 累积缩放（1 = 基线）
    int32_t pointerId = 0;      // 多点触控手指 id
    size_t activePointers = 1;  // 并发触点数
};
```

| 方法 | 说明 |
|------|------|
| `processEvent(event)` | 处理 SDL 事件，路由到当前焦点的回调 |
| `submitPointer(event)` | 统一指针输入路径（Track P3）：与 `processEvent` 共享同一分发（KAG/GAME 互斥与防幻点击保证由构造共享）。`Down`/`Move`/`Up` → 鼠标左键等价语义；`LongPress` → 右键按下+抬起对（与 MobileAdapter 长按对等）；`Pinch` → 由累积 `scale` 换算滚轮增量（焦点切换时基线复位） |
| `setFocus(focus)` | 切换输入焦点（KAG ↔ GAME） |
| `getFocus` | 返回当前焦点 |
| `registerGameCallback(cb)` | 注册 GAME 模式的输入回调 |
| `registerKAGCallback(cb)` | 注册 KAG 模式的输入回调 |
| `registerFocusChangeCallback(cb)` | 注册焦点切换回调 |
| `registerResizeCallback(cb)` | 注册窗口大小变化回调 |
| `notifyResize(w, h)` | 通知所有 resize 回调 |
| `hasKAGClick` | KAG 是否有点击待处理 |
| `isClickPending` | 同 `hasKAGClick` |
| `getKAGCallbackCount` | 已注册的 KAG 回调数 |
| `getGameCallbackCount` | 已注册的 GAME 回调数 |
| `consumeKAGClick` | 消耗 KAG 点击事件 |

---

## 7. job — 多线程任务系统

**实现**: JobSystem（真实多线程）、NullJobSystem（测试用同步模拟）
**Mock**: `tests/mocks/NullJobSystem.h` — 所有任务在主线程同步执行

### IJobSystem

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `init` | — | — | 启动工作线程池 |
| `shutdown` | — | — | 停止接收任务，join工作线程，在后端仍存活时执行一次最终回调快照；回调中重入关停会取消剩余回调 |
| `submit` | `work`, `priority=Normal`, `onComplete=nullptr` | `uint64_t` | 提交异步任务，返回 job ID |
| `pollMainThreadJobs` | — | — | 主线程执行一个就绪快照；嵌套poll不递归排空，新任务回调留到后续poll |
| `waitIdle` | — | — | 仅等待worker工作及其回调入队，不执行回调；worker不得同步等待主线程回调 |
| `workerCount` | — | `int` | 工作线程数 |
| `pendingJobs` | — | `int` | 未完成worker及回调发布的任务数；0不代表主线程回调已经执行 |
| `isRunning` | — | `bool` | 任务系统是否运行中 |

生命周期与轮询由主线程控制。最终关停快照执行期间拒绝新任务和重新初始化；普通poll中的关停/重启会使旧批次失效。NullJobSystem保留同步执行work和callback的特性，但与真实实现一致拒绝空work，并在work关闭或重启实例后丢弃旧callback。

**线程安全约束**：
- `submit` 可将 CPU 工作（解码、物理）分发到 Worker 线程
- `onComplete` 回调在主线程 `pollMainThreadJobs` 中执行
- Worker 线程**绝不执行 Lua 代码**
- 内存分配器必须线程安全

---

## 8. live2d — 动画后端

**实现**: Live2DBackend（需 Cubism SDK）、NullAnimationBackend（PNG 降级）

### IAnimationBackend

| 方法 | 说明 |
|------|------|
| `init` | 初始化动画系统 |
| `isCubismAvailable() const` | owner 线程查询实际 Cubism 会话；Null/PNG 回退为 false |
| `shutdown` | 释放所有资源 |
| `loadModel(path, name)` | 加载模型，返回 handle（0=失败） |
| `unloadModel(handle)` | 卸载模型 |
| `isLoaded(handle)` | 模型是否已加载 |
| `showModel(handle, x, y, scale)` | 显示模型在指定位置 |
| `hideModel(handle)` | 隐藏模型 |
| `setOpacity(handle, opacity)` | 设置透明度 (0.0–1.0) |
| `render(dt)` | 渲染所有可见模型 |
| `playMotion(handle, name)` | 播放指定动作 |
| `setExpression(handle, name)` | 设置表情 |
| `setParameter(handle, param, value)` | 设置模型参数 |
| `name` | 返回后端名称 |

**降级行为**：无 Cubism SDK 时，NullAnimationBackend 支持 PNG/JPG/BMP 静态图片作为立绘，通过 TextureManager 加载。

当前没有注册 `live2d` 全局 Lua 表；后续脚本绑定必须位于 Script 模块，并在调用时经
`BackendRegistry::getAnimationBackend()` 解析接口。

---

## 9. minigame — 3D 小游戏

**实现**: BgfxMiniGameBackend（预留）
**状态**: 接口完整，3D 渲染实现待后续完成

### IMiniGameBackend

| 方法 | 说明 |
|------|------|
| `init` | 初始化，分配 GPU 资源 |
| `shutdown` | 释放所有 GPU/CPU 资源 |
| `loadScene(path)` | 加载 3D 场景（glTF/OBJ），返回 scene handle |
| `unloadScene(handle)` | 卸载场景 |
| `enter(handle)` | 进入小游戏模式（隐藏 KAG 层，激活 3D 相机） |
| `leave` | 退出小游戏模式（恢复 KAG 渲染） |
| `isActive` | 是否处于小游戏模式 |
| `update(dt)` | CPU 更新（物理、碰撞，可回调 Lua）——仅主线程，不得派发给 JobSystem |
| `render` | GPU 提交，仅主线程 |
| `processEvent(sdlEvent)` | 输入事件路由，返回 true=已消费 |
| `luaCall(L, method)` | Lua 桥接调用 |
| `setRenderDevice(dev)` | 注入渲染设备 |
| `getBackendName` | 后端名称 |

**预期生命周期**：KAG 场景 → `enter` → active loop (update+render) → `leave` → KAG 场景。
Engine 已驱动 active loop，但当前 `LuaManager` 不注册 `mini_game` 全局表；脚本适配器仍需在
Script 模块经 `IMiniGameBackend` 接口实现，不能在具体 Bgfx 后端中保存全局指针。

---

## 10. platform — 平台抽象

**实现**: SDL3PlatformBackend

### IPlatformBackend

| 方法 | 说明 |
|------|------|
| `init(title, width, height)` | 创建窗口，初始化 SDL3 |
| `shutdown` | 销毁窗口，关闭 SDL3 |
| `pollEvent` | 轮询事件，派发到回调。返回 true=有事件 |
| `getMouseState` | 返回 `MouseState{x, y, leftDown}` |
| `getTicksMs` | 返回启动后毫秒数 |
| `getNativeWindowHandle` | 返回原生窗口句柄（给 bgfx） |
| `getWindowWidth` / `getWindowHeight` | 窗口尺寸 |
| `setFullscreen` | 切换全屏 |
| `resizeWindow(w, h)` | 调整窗口大小 |
| `getBackendName` | 后端名称 |

### IMobileAdapter

移动平台生命周期与触控适配，将触摸/手势事件映射为 SDL 输入并回调 Lua。

**实现**: `platform/MobileAdapter`

| 方法 | 说明 |
|------|------|
| `onPause(L)` | 应用退到后台：标记暂停，有 Lua 状态时回调 `_G.onPause()` |
| `onResume(L, savedData)` | 回到前台：恢复并回调 `_G.onResume(savedData)` |
| `onLowMemory(L)` | OS 内存压力：回调 `_G.onLowMemory()`（STEP11 新增；无 Lua 时安全 no-op，回调异常被吞且栈平衡） |
| `onTerminate(L)` | OS 即将终止：回调 `_G.onTerminate()`（STEP11 新增；仅通知，不改拆卸顺序） |
| `onFingerDown/x(x,y,fingerId)` | 触摸按下 → 鼠标输入映射 |
| `onFingerMotion(x,y,fingerId)` | 触摸移动 → 鼠标输入映射 |
| `onFingerUp(x,y,fingerId)` | 触摸抬起 → 鼠标输入映射 |
| `onPinch(cx,cy,scale)` / `resetPinch` / `getLastPinchScale` | 捏合手势 |
| `onLongPress(x,y)` | 长按手势 |
| `getDisplayScale` / `setDisplayScale` | 显示缩放 |
| `onOrientationChanged(L, orientation)` | 朝向变化回调 `_G.onOrientationChanged(\"portrait\"/... )` |
| `isPaused` / `activeTouchCount` / `isFingerDown(id)` | 状态查询 |

### IDisplayService

统一显示度量查询（Track P1 Display Service，STEP10）：跨桌面/Web/移动的"当前屏幕状态"单一入口，业务代码无需平台 ifdef。桌面与无头环境的 `safeArea` 恒为零值；移动端实现随 Track M/I 落地。

**接口文件**: `src/platform/api/IDisplayService.h`（`Orientation` / `Insets` / `DisplayMetrics` 被接口方法按值传递，依 AGENTS.md §2 定义在接口头文件内）
**实现**: `SDL3DisplayService`（桌面 SDL3 窗口/显示器实时查询，窗口经 `IPlatformBackend` 懒获取）、`NullDisplayService`（无头/测试，固定零值）
**构造点**: 组合根工厂 `entry/createDisplayService()`（平台+GPU 可用选 SDL3，否则 Null）；经 `EngineConfig.displayService` 注入，Engine 持有 `unique_ptr` 所有权

| 方法 | 说明 |
|------|------|
| `currentMetrics()` | 返回当前 `DisplayMetrics` 快照（一次 SDL 查询，可每帧调用）：`pixelWidth/pixelHeight` 物理像素、`logicalWidth/logicalHeight` 逻辑像素、`scaleFactor` 物理/逻辑比、`dpi = 96 × scaleFactor`（SDL3 提供的是 content scale，此为文档化映射）、`orientation`、`safeArea{left, top, right, bottom}`（逻辑像素安全区 inset） |

**注册**: `Engine::init()` 中 `BackendRegistry::instance().setDisplayService(...)`（与平台后端同阶段）；消费方 `getDisplayService()`。
**Lua 绑定**: `DevCore.get_display_metrics()`（`src/script/bindings/DevCoreBinding.cpp`）→ `{pixelWidth, pixelHeight, logicalWidth, logicalHeight, scaleFactor, dpi, orientation, safeArea{...}}`；未注册服务时返回 `nil`（`DevCore.get_resolution()` 行为不变）。

### ILifecycleService

统一应用生命周期事件模型（Track P2 Lifecycle Service，STEP11）：桌面（SDL3 app-lifecycle 事件）、Android（JNI onPause/onResume/onLowMemory）与 iOS（UIApplication 通知）共用一条事件流，消费方注册一次 `ILifecycleListener` 即接入全部平台，无需平台 ifdef。

**接口文件**: `src/platform/api/ILifecycleService.h`（`LifecycleEvent` 枚举被接口方法按值传递，依 AGENTS.md §2 定义在接口头文件内）
**实现**: `LifecycleService`（platform 模块 header-only 中枢）：互斥锁保护监听器表，重复注册忽略；派发前对监听器集合取快照，派发中自移除安全，注册顺序即派发顺序
**事件源约定**: 事件必须在引擎主线程投递（或先汇入主线程），监听器可触碰 Lua；当前桌面源为 `Engine::appLifecycleWatch`（SDL3 事件 watch，主线程）

`LifecycleEvent` 六事件：

| 事件 | 说明 |
|------|------|
| `Pause` / `Resume` | 离开 / 回到焦点（预留给移动端 JNI 源；桌面 SDL3 源当前不产生） |
| `Background` / `Foreground` | 完全退到后台（无可见表面）/ 回到前台（桌面源：`SDL_EVENT_WILL_ENTER_BACKGROUND` / `SDL_EVENT_DID_ENTER_FOREGROUND`） |
| `LowMemory` | OS 内存压力——应释放缓存/纹理（桌面源：`SDL_EVENT_LOW_MEMORY`） |
| `Terminate` | OS 即将终止进程（桌面源：`SDL_EVENT_TERMINATING`） |

| 方法 | 说明 |
|------|------|
| `addListener(listener)` | 注册监听器（重复忽略），注册顺序即派发顺序 |
| `removeListener(listener)` | 移除监听器；未注册/已移除均为安全 no-op |
| `post(event)` | 向全部监听器投递事件（调用方线程同步派发） |

**构造与注册**: 组合根 `Engine::initPlatformPhase()` 创建并持有 `unique_ptr<LifecycleService>`，先 `addListener(this)` 将自身注册为首监听器，再 `BackendRegistry::instance().setLifecycleService(...)`；消费方 `getLifecycleService()`。
**Engine 内建映射**: `Background`/`Pause` → `IMobileAdapter::onPause` + 音频挂起；`Foreground`/`Resume` → `onResume` + 音频恢复；`LowMemory`/`Terminate` → `IMobileAdapter::onLowMemory`/`onTerminate`（回调 `_G.onLowMemory` / `_G.onTerminate`，无 Lua 安全）。`Engine::shutdown()` 先移除 SDL watch 再移除自身监听器（Lua 安全拆卸顺序）。

---

## 11. render — 7 个接口

渲染是接口最多的模块，覆盖设备管理、纹理、图层、粒子、GPU 监控、视频播放与骨骼网格动画。

### 11.1 IRenderDevice

**核心渲染设备抽象**，位于 `src/render/api/IRenderDevice.h`。接口使用
`RenderTextureHandle`、`RenderProgramHandle` 和 `RenderUniformHandle` 等不透明句柄，
不向调用方暴露 bgfx 类型。

**视图 ID 常量**：
| 常量 | 值 | 用途 |
|------|-----|------|
| `VIEW_RTT` | 0 | 离屏渲染到纹理 |
| `VIEW_MAIN` | 1 | 主合成管线（KAG UI） |
| `VIEW_DEBUG` | 2 | 调试覆盖层 |
| `VIEW_TRANSITION` | 99 | 过渡效果合成 |

| 分类 | 方法 | 说明 |
|------|------|------|
| **生命周期** | `init(hwnd, w, h)` | 初始化渲染设备 |
| | `isInitialized` | 渲染设备是否已初始化 |
| | `getBackbufferWidth` / `getBackbufferHeight` | backbuffer 分辨率查询 |
| | `beginShutdown` | 关闭截图接收、取消未完成票据；保留 GPU 上下文供显式销毁排空 |
| | `shutdown` | 先 `flushAllRTT` 再关闭具体渲染后端 |
| | `flushAllRTT` | 释放所有 RTT framebuffer（GPU 上下文仍存） |
| | `resize(w, h)` | 更新逻辑场景尺寸及文字缓存；未指定独立呈现尺寸时同步重建 backbuffer |
| | `setPresentSize(w, h)` | 以实际 drawable 像素尺寸调整 GPU backbuffer，保留逻辑坐标；不额外推进帧，设备恢复保留此前显式尺寸 |
| **帧管理** | `beginFrame` | 开始帧 |
| | `endFrame` | `commit_frame` + `advanceFrame` 的便捷入口 |
| | `commit_frame` | 完成场景/后处理提交与视图复位，不推进 bgfx 帧 |
| | `advanceFrame` | 为排队截图分配提交帧号并提交 readback，然后推进一次后端帧 |
| **视图** | `setViewRect(viewId, x, y, w, h)` | 设置视图矩形 |
| | `setViewClear(viewId, flags, rgba, depth, stencil)` | 设置视图清除 |
| | `setScreenOffset(dx, dy)` | 设置屏幕偏移（视图变换） |
| | `touch(viewId)` | 标记视图为活跃 |
| **离屏渲染** | `createRenderTarget(w, h)` | 创建 RTT，返回 `ViewportHandle` |
| | `destroyRenderTarget(handle)` | 销毁 RTT |
| | `blitViewport(handle, viewId, x, y, w, h)` | 将 RTT 纹理绘制到目标视图 |
| | `getViewportTexture(handle)` | 获取 RTT 的不透明 `RenderTextureHandle` |
| | `fillViewport(handle, r, g, b, a)` | 纯色填充 RTT |
| **纹理 Blit** | `blitTexture(viewId, texId, x, y, w, h, opacity)` | 将纹理（TM ID）绘制到视图 |
| | `stretchBlt(viewId, dstTexId, dx, dy, dw, dh, srcTexId, sx, sy, sw, sh, filter)` | 缩放 Blit |
| | `affineBlt(viewId, dstTexId, ..., matrix[6])` | 仿射变换 Blit |
| **批量协议** | `beginBatch` | 开始批量提交 |
| | `flushBatch` | 刷新批量提交 |
| **文字渲染** | `renderText(viewId, text, x, y, r, g, b, a)` | 渲染文字 |
| | `renderRuby(viewId, text, ruby, x, y, r, g, b, a)` | 渲染注音文字 |
| | `setFont(fontId)` | 设置字体 |
| | `loadTTF(path, fontSize)` | 从文件加载 TrueType 字体（返回 `bool`） |
| | `textLineHeight` | 行高 |
| **特效** | `submitBlend(viewId, baseTex, blendTex, mode, baseAlpha, blendAlpha, globalAlpha)` | 混合特效 |
| | `submitTransition(viewId, fromTex, toTex, ruleTex, method, progress)` | 转场特效 |
| | `submitVFX(viewId, srcTex, effect, ...)` | 视觉特效 |
| | `setColorFilter(preset)` | 设置无障碍色彩滤镜（None/Deuteranopia/Protanopia/Tritanopia/Grayscale/HighContrast），返回 `bool` |
| **调试** | `setDebugName(viewId, name)` | 设置视图调试名称 |
| | `drawDebugOverlay(title)` | 绘制调试覆盖层 |
| | `requestScreenshot(path)` | 保留的路径兼容入口；`bool` 仅表示接收，不表示文件完成；同样受渲染器就绪与容量守卫约束 |
| **截图** | `requestScreenshot(options)` | 返回 `ScreenshotResult`：有效票据 + `Pending` 表示接收，无效票据 + `Failed` 表示拒绝 |
| | `takeScreenshot(ticket)` | 查询 `Pending` 时保留请求；一次移动领取并移除 `Completed` / `Failed` / `Cancelled` 终态，后续返回 `Unknown` |
| | `cancelScreenshot(ticket)` | 仅将 `Pending` 转为 `Cancelled` 并返回 `true`；仍须 `takeScreenshot` 领取/丢弃终态 |
| **设备恢复** | `recoverDevice(hwnd, w, h)` | 重建丢失的渲染设备 |
| | `flagDeviceLost` / `consumeDeviceLost` | 在线程/帧边界间传递设备丢失状态 |
| **着色器** | `getDefaultSampler` / `getFallbackProgram` | 返回不透明采样器/未调制纹理程序句柄 |
| | `getModulatedTextureProgram` | 返回逐次绘制以 `u_color` RGBA 调制纹理的程序；未提供该程序的后端返回无效句柄，普通 Fallback 不表示支持颜色或透明度调制 |
| **后端标识** | `getBackendName` | 后端名称 |
| | `getRuntimeInfo` | 返回 `RenderRuntimeInfo`（backendName, 分辨率, viewCount, shaderReady）；`shaderReady` 表示设备可渲染且核心程序完整，Noop、IFH、丢失、退出或任一核心程序失败均为 false；可选效果失败不影响完整核心的就绪状态 |
| | `setPreferredBackend(name)` | 设置首选渲染后端（返回 `bool`） |

截图类型与方法以 [IRenderDevice.h](../../src/render/api/IRenderDevice.h) 为准：

| 类型 | 字段与合同 |
|------|------------|
| `ScreenshotTicket` | `uint64_t requestId, generation`；两者均非零才有效，请求与设备代次共同标识结果 |
| `ScreenshotOptions` | `uint32_t width, height`；均为 0 时在接收时固定原生尺寸，否则两者须为正；实际输出使用已接收的尺寸 |
| `ScreenshotStatus` | `Unknown`, `Pending`, `Completed`, `Failed`, `Cancelled` |
| `ScreenshotResult` | `ticket`, `status`, `frameId`, `width`, `height`, `std::vector<uint8_t> png`, `std::string error`；`frameId` 在 `advanceFrame` 提交时填写，提交前为 0；成功结果独立拥有 PNG 字节 |

截图调用经 `BackendRegistry::getRenderDevice()` 在 owner thread 执行，GPU 回调只交付结果，不调用 Lua 或保存槽位。未初始化、headless、关闭、设备丢失/恢复中或恢复失败时拒绝接收；成功恢复开启新代次，旧回调不能完成新请求。取消清理须先 cancel 再 take：完成可能先于取消，仍须丢弃本次已经完成的结果。关闭接收时普通票据的终态仍可领取，完整后端销毁才释放剩余记录。

当前 [ScreenshotQueue](../../src/render/ScreenshotQueue.h) 对保留票据、尺寸和字节预算设限：最多 8 个保留请求，单边至多 8192、总像素至多 8×1024×1024，并受 PNG/总保留字节预算限制；超过限制返回失败。尺寸变换在渲染模块内按像素中心最近邻拉伸，不保比例留边；格式转换与翻转不改变全局编码器状态。同帧票据可共享一次 backbuffer readback，再生成各自尺寸的 PNG。结果尚未领取时也占用保留容量，调用者必须完成清理。

正常 Engine 帧在完整场景、小游戏绘制和后处理后调用一次 `advanceFrame`，再调用平台 `postFrame`。退出阶段的资源销毁排空有独立用途，不接收新截图。上述为源码合同，真实后端/平台验证范围见 [待实际运行检查项](../solutions/deferred-gpu-tests.md)。

### 11.2 ILayerManager

三层合成管理器（BG=背景, FG=前景, MSG=消息）。

| 方法 | 说明 |
|------|------|
| `init` / `shutdown` | 生命周期 |
| `setTexture(t, texId)` | 设置图层纹理（不透明后端纹理 ID） |
| `setVisible(t, visible)` | 设置图层可见性 |
| `setOpacity(t, opacity)` | 设置图层不透明度 |
| `setPosition(t, x, y)` | 设置图层位置 |
| `setScale(t, sx, sy)` | 设置图层缩放 |
| `setBlendMode(t, blend)` | 设置混合模式 |
| `clear(t)` | 清除单个图层 |
| `clearAll` | 清除所有图层 |
| `markAllDirty` | 标记所有图层为脏 |
| `markDirty(t, x, y, w, h)` | 标记脏矩形 |
| `markDirtyWithTransparency(t, x, y, w, h)` | 标记脏矩形（递归标记下层） |
| `updateDirtyRegions(screenW, screenH)` | 合并脏矩形，设置 scissor |
| `clearDirtyRects` | 清除脏矩形 |
| `render(viewId, screenW, screenH, programId)` | 按 BG→FG→MSG 顺序提交图层 |

### 11.3 ITextureManager

纹理生命周期管理。

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `initialize()` | `bool` | 使用 GPU 默认模式初始化纹理管理器 |
| `initialize(gpuAvailable)` | `bool` | 显式选择 GPU 或 headless 模式初始化 |
| `shutdown` | — | 释放所有纹理 |
| `setDevMode(bool)` | — | Dev=true 显示棋盘格占位纹理 |
| `loadTexture(path)` | `uint32_t` | 从文件加载，返回 TM ID |
| `loadTextureFromMemory(data, size, cacheKey)` | `uint32_t` | 从内存加载 |
| `loadTextureFromRGBA(rgba, w, h, cacheKey)` | `uint32_t` | 从 RGBA 像素数据加载 |
| `createSolidTexture(r, g, b, a)` | `uint32_t` | 创建 1×1 纯色纹理，返回 TM ID |
| `getPlaceholderTexture` | `uint32_t` | 占位纹理的不透明后端 ID |
| `destroyTexture(id)` | — | 销毁纹理 |
| `getTextureHandle(id)` | `uint32_t` | TM ID → 不透明后端纹理 ID（0=无效） |
| `getTextureSizeById(id, w, h)` | — | 查询纹理尺寸 |
| `isValid(id)` | `bool` | 纹理 ID 是否有效 |
| `totalTextureBytes` | `uint64_t` | 总纹理内存占用 |
| `checkBudget(id, w, h)` | `bool` | 预算检查，超限时 LRU 驱逐；无法容纳时返回 false |
| `trackTexture(id, bytes)` | — | 以 64 位字节数跟踪纹理 |
| `untrackTexture(id)` | — | 取消跟踪 |

### 11.4 IParticleSystem

2D 粒子特效系统。

| 方法 | 说明 |
|------|------|
| `init` | 初始化粒子系统 |
| `shutdown` | 释放所有粒子 |
| `createEmitter(cfg)` | 创建发射器（`Emitter` 结构体），返回 ID |
| `destroyEmitter(id)` | 销毁发射器 |
| `emit(emitterId, count)` | 发射指定数量粒子 |
| `update(dt, screenW, screenH)` | 更新粒子物理 |
| `render(viewId)` | 渲染所有活跃粒子 |
| `aliveCount` | 活跃粒子总数 |
| `isInitialized` | 粒子系统是否已初始化 |

**参数校验**（VFXBinding 层）：`rate`, `lifeMin/Max`, `speedMin/Max`, `sizeMin/Max` 不能为负；`lifeMin > lifeMax` 时自动 clamp。

### 11.5 IGpuMonitor

GPU 性能监控和自适应降级。

```cpp
enum class GpuQuality : uint8_t { HIGH = 0, MEDIUM = 1, LOW = 2 };

struct FrameMetrics {
    double gpuTimeMs, cpuTimeMs, rollingAvgMs;
    uint32_t frameCount, overloadFrames;
    bool degraded;
    GpuQuality quality;
};
```

| 方法 | 说明 |
|------|------|
| `update(dt)` | 更新 GPU 性能数据，返回当前质量等级 |
| `metrics` | 返回 `FrameMetrics` 引用 |
| `currentQuality` | 当前质量等级 |
| `isDegraded` | 是否已降级 |
| `resolutionScale` | 当前分辨率缩放因子 |
| `vfxEnabled` | VFX 是否启用（低性能时自动关闭） |
| `reset` | 重置统计数据 |
| `setGpuAvailable(available)` | 组合根在渲染设备初始化后调用；此前 `update` 不得触碰 bgfx stats |

### 11.6 IVideoPlayer

MPEG-1/FFmpeg 视频播放。

```cpp
struct VideoHandle { uint32_t id = 0; explicit operator bool() const; };
```

| 方法 | 说明 |
|------|------|
| `open(path)` | 打开视频文件，返回 `VideoHandle` |
| `close(handle)` | 关闭视频 |
| `setLoop(handle, loop)` | 设置循环播放 |
| `setVolume(handle, volume)` | 设置视频音频音量 [0..1] |
| `update(handle, dt)` | 解码下一帧到后端纹理 |
| `updateAll(dt)` | 引擎帧循环推进所有播放中的视频 |
| `getTexture(handle)` | 返回当前帧的不透明后端纹理 ID |
| `isPlaying(handle)` | 是否播放中 |
| `hasEnded(handle)` | 是否播放完毕 |
| `width(handle)` / `height(handle)` | 视频尺寸 |
| `duration(handle)` | 总时长（秒） |
| `currentTime(handle)` | 当前播放位置（秒） |
| `pause(handle)` / `resume(handle)` | 暂停/继续 |
| `seek(handle, time)` | 跳转到指定时间 |
| `shutdown` | 停止所有视频 |
| `activeCount` | 活跃视频数 |

### 11.7 IMeshRenderer

2D 骨骼网格动画渲染器（SMA，Battle 4d S1），CPU 软蒙皮 / GPU compute 蒙皮。POD 网格类型（`MeshHandle`、`SMAMeshVertex`、`SMAMesh`、`BonePose`）定义在接口头文件中。

| 方法 | 说明 |
|------|------|
| `isInitialized` | 渲染器是否就绪 |
| `setSkinMode(mode)` / `skinMode` | 蒙皮模式：`Auto`/`Cpu`/`Gpu`（SMA S5，GPU compute 能力自动回退） |
| `createMesh(mesh)` | 上传网格，返回不透明 `MeshHandle` |
| `destroyMesh(handle)` | 释放网格 |
| `updateMesh(handle, poses)` | 每帧按骨骼 pose 重新蒙皮并更新 GPU 缓冲 |
| `drawMesh(targetView, handle, dstTexId, x, y, scale, opacity)` | 将蒙皮网格绘制到目标视图 |
| `meshCount` | 当前网格数 |

---

## 12. resource — 资源管理

### 12.1 IAssetProvider

抽象资产源（文件系统、CARC 归档等）。

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `read(path)` | `vector<uint8_t>` | 读取文件，不存在返回空 |
| `exists(path)` | `bool` | 文件是否存在 |
| `getSource` | `string` | 人类可读的来源名称 |
| `priority` | `int` | 优先级（高=先查）。CARC=10, Dir=5, Patch=8 |
| `verify` | `bool` | 完整性校验（CARC 验证 Ed25519 签名） |

### 12.2 IAsyncLoader

异步资源加载管线。

```cpp
struct CompletedLoad {
    int id; uint64_t generation; std::string path, type; bool success;
    std::vector<uint8_t> rgba; uint16_t width, height;
    std::vector<uint8_t> data;
};
```

| 方法 | 说明 |
|------|------|
| `init` / `shutdown` | 生命周期 |
| `enqueue(path, type)` | 入队加载请求，返回 load ID |
| `cancelAll` | 递增请求代次，立即结算旧请求计数并清理缓存、完成缓冲和自身旧 SDL 事件 |
| `poll` | 轮询完成结果（需每帧调用） |
| `drainCompleted` | 非 SDL 交付：返回并清空全部已完成加载（调用方负责纹理上传 + Lua 回调） |
| `isCurrent(completed)` | 消费者在上传或调用 Lua 前复核代次；已经取出的结果也可能因取消、重载或关闭失效 |
| `pendingCount` | 待处理加载数 |
| `isRunning` | 加载器是否运行中 |

控制与消费操作在主线程执行。worker 保持自身结果所有权直到完成回调；取消不提前释放 worker 正在写入的内存。Engine 在暂停时持有完成结果，恢复时逐项检查代次，并在调用 Lua 前释放旧 callback registry 引用，避免回调重入取消后误删新引用。

### 12.3 IResourceGenerationTracker

资源句柄代际服务。热重载使某类资源失效时递增对应代际，旧句柄随后无法通过
`isCurrent` 校验。具体 `GenerationTracker` 由组合根创建、`Engine` 持有，并以
非拥有接口指针注册到 `BackendRegistry`。

| 方法 | 说明 |
|------|------|
| `current(type)` | 返回指定资源类型的当前代际 |
| `invalidate(type)` | 递增指定资源类型的代际 |
| `isCurrent(handle)` | 判断句柄代际是否仍有效 |
| `makeHandle(type, id)` | 使用当前代际创建资源句柄 |

---

## 13. rpc — HTTP/JSON-RPC 服务器

### 13.1 IEditorServer

Web 编辑器 HTTP 服务器。

| 方法 | 说明 |
|------|------|
| `start(port=9876)` | 启动 HTTP 服务器 |
| `stop` | 停止服务器 |
| `isRunning` | 服务器是否运行中 |
| `port` | 当前端口号 |
| `pushLog(level, message)` | 推送日志到编辑器前端 |
| `setDispatcher(dispatcher)` | 注入线程安全的 RPC DTO dispatcher |
| `setAuthToken(token)` | 设置 HTTP 编辑器 Bearer token（非空时强制鉴权，缺失/不匹配返回 401） |
| `setWebRoot(path)` | 设置 Web 前端静态文件根目录 |
| `setArchiveWriterFactory(factory)` | 注入 `IArchiveWriter` 工厂；具体 CARC writer 由组合根创建 |

`EditorServer` 运行在后台线程，只提交自包含 DTO。Lua VM、Engine 和动画后端仅由
组合根 dispatcher 在 owner thread 访问；HTTP handler 不持有 `lua_State*`。

**已注册端点**：
| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/ping` | 健康检查 → `{"status":"ok"}` |
| GET | `/api/status` | 引擎状态（Lua 可用性、端口） |
| GET | `/api/assets` | 列出项目资源（支持 `?type=image/audio/script` 过滤） |
| POST | `/api/run` | 通过 managed coroutine（startManagedRun+pumpManagedRuns）执行剧本/代码 |
| POST | `/api/stop` | 停止执行 |
| GET | `/api/logs` | 近期日志 |
| GET | `/api/live2d/models` | 列出可用 Live2D 模型 |
| POST | `/api/live2d/load` | 通过 owner-thread dispatcher 加载 Live2D 模型 |
| POST | `/api/build` | 将 `scripts/` 与 `assets/` 打包为 CARC 归档 |

### 13.2 IRpcServer

JSON-RPC 服务器接口。

| 方法 | 说明 |
|------|------|
| `run` | 启动 RPC 服务器 |
| `stop` | 停止 |
| `isRunning` | 是否运行中 |
| `setDispatcher(dispatcher)` | 注入线程安全的 RPC DTO dispatcher |
| `pushLog(level, message)` | 推送日志 |

### 13.3 IRpcDispatcher

Owner-thread RPC 命令边界。传输适配器（`RpcServer`/`EditorServer`）从后台线程提交
自包含 DTO，dispatch 在引擎主线程执行，从而保证 Lua VM、Engine、动画后端仅由
owner thread 访问且不持有 `lua_State*`。它不是 BackendRegistry 服务（见第 2 节访问规则与附录 A）。

`RpcRequest` / `RpcReply` 及其 `RpcRequestPayload` / `RpcReplyPayload` variant（status、run_script、
eval、get_state、sma_validate、pick、sma_save、stats、capture_frame、reload_scripts、
load_animation、breakpoints、debug_resume、inspect、kag_debug 等）均定义在 `IRpcDispatcher.h`。

| 方法 | 说明 |
|------|------|
| `dispatch(request)` | 在主线程服务一条 RPC 请求，返回 `RpcReply`（含状态码、消息与 payload） |

---

## 14. script — Lua 虚拟机

**实现**: LuaManager
**注册**: `BackendRegistry::instance().setLuaManager()`；原生状态指针另通过 `setLuaState()` 提供给绑定层

### ILuaManager

| 方法 | 说明 |
|------|------|
| `init` | 初始化 Lua VM，注册绑定，执行 `scripts/init.lua` |
| `shutdown` | 关闭 Lua VM |
| `update(dt)` | 每帧驱动协程 |
| `loadScript(path)` | 加载并执行 Lua 脚本 |
| `resumeKAGCoroutine` | 恢复 KAG 协程（点击后调用） |
| `lockdownScriptEnv` | 锁定脚本环境（沙箱） |
| `registerModules` | 注册 C++ 绑定模块（KAG, Render, VFX, Debug, DevCore） |
| `state` | 返回 `lua_State*` |
| `setInstructionBudget(n)` | 设置每帧最大指令数（防死循环） |
| `getInstructionBudget` | 当前指令预算 |
| `isInstructionBudgetExceeded` | 是否超出指令预算 |
| `resetInstructionBudget` | 重置指令预算计数器 |

**线程安全约束**：Lua VM 仅在主线程操作。Worker 线程绝不执行 Lua。多 VM 隔离（如 LuaLanes）可用于高级场景。

---

## 15. steam — Steamworks 集成

**实现**: SteamBackend（条件编译 `#ifdef CAESURA_HAS_STEAM`）

### ISteamBackend

| 分类 | 方法 | 说明 |
|------|------|------|
| **生命周期** | `init` | 初始化 Steam API |
| | `isAvailable() const` | owner 线程查询实际 Steam 会话；Null 为 false，不代表远端操作成功 |
| | `shutdown` | 关闭 |
| | `runCallbacks` | 每帧调用 `SteamAPI_RunCallbacks` |
| | `isOverlayActive` | Steam 覆盖层是否激活（暂停输入） |
| **成就** | `unlockAchievement(id)` | 解锁成就 |
| | `isAchievementUnlocked(id)` | 查询成就状态 |
| | `resetAchievement(id)` | 重置单个成就 |
| | `resetAllAchievements` | 重置所有成就 |
| **统计** | `setStatInt(name, value)` / `getStatInt(name)` | 整数统计 |
| | `setStatFloat(name, value)` / `getStatFloat(name)` | 浮点统计 |
| | `storeStats` | 刷新到 Steam 服务器 |
| **云存档** | `cloudWrite(fileName, data, size)` | 写入云存档 |
| | `cloudRead(fileName, buffer, maxSize)` | 读取云存档 |
| | `cloudFileSize(fileName)` | 云文件大小 |
| | `cloudFileExists(fileName)` | 云文件是否存在 |
| | `cloudDelete(fileName)` | 删除云文件 |
| | `cloudQuotaTotal` / `cloudQuotaUsed` | 云存储配额 |
| | `cloudFileCount` | 云文件总数（启用 `Lua cloud_list`） |
| | `cloudFileNameAt(index)` | 按索引返回云文件名 |
| | `name` | 后端名称 |

---

## 16. storage — 存档/读档

### 16.1 ISaveManager

JSON 存档管理，支持加密策略、显式旧明文导入和 schema 迁移。本节的截图职责于 2026-09-08 按 [ISaveManager.h](../../src/storage/api/ISaveManager.h) 同步；API 合同不代表完整门禁已通过。

`SaveEncryptionPolicy` 定义在接口头中：`Compatible`（默认，保留旧明文读取；有 key 时仍加密每次写入）和 `RequireEncrypted`（普通读取拒绝明文，缺 key 拒绝写入）。`EngineConfig::saveEncryptionPolicy` 在组合根接入该策略；策略不能从存档文件内容推断。

| 方法 | 说明 |
|------|------|
| `init(saveDir)` | 初始化，指定存档目录 |
| `save(slot, data, sceneName, tokenIndex, thumbnailPng)` | 原子保存提供的数据和缩略图文本；Lua 调用方提供原始 Base64 PNG，缺图使用空串 |
| `load(slot, outMeta=nullptr)` | 加载槽位；失败返回 null JSON 且不改变 `outMeta`；空对象/数组是合法数据 |
| `loadLegacyPlaintext(slot, outMeta=nullptr)` | 显式只读导入旧明文；拒绝 CAES，不改变策略、不写回源文件；失败不改变 `outMeta` |
| `listSaves` | 列出所有存档槽位及元数据 |
| `slotExists(slot)` | 槽位是否存在 |
| `deleteSlot(slot)` | 删除槽位 |
| `setEncryptionKey(key[32])` | 设置 AES-256 加密密钥 |
| `clearEncryptionKey` | 清除 key，不改变策略；严格模式下后续保存失败，兼容模式下后续保存为明文 |
| `isEncryptionEnabled` | 当前是否设置了 key；不等同于策略查询 |
| `setEncryptionPolicy(policy)` | 设置 `SaveEncryptionPolicy`，不生成 key 或迁移现有文件 |
| `getEncryptionPolicy()` | 获取当前加密策略 |
| `setSaveProvider(provider)` | 注入自定义存储后端 |
| `getSaveProvider` | 获取当前存储后端 |
| `configureCloudSync(endpoint)` | `""`=本地；HTTP(S)=本地存储加显式同步；`steam`/`steam://`/`steamcloud`=Steam 云存储；配置本身不搬运字节 |
| `pushSlotToCloud(slot)` | 暂存本地字节，按当前策略/key和存档结构验证，再上传同一份字节 |
| `pullSlotFromCloud(slot)` | 暂存远端字节，按当前策略/key和存档结构验证，再将同一份字节提交到本地 |
| `currentSchemaVersion` | 当前存档格式版本号 |
| `registerMigration(fromVer, toVer, fn)` | 注册 schema 迁移函数 |

SaveManager 加密整个 JSON 信封，包括 scene、timestamp、schema_version、token_index、thumbnail、engine_version 和 data。CAES 认证失败没有明文回退。内存迁移不自动写盘；迁移返回 null 或抛异常时加载失败且元数据不变。兼容模式允许完整明文替换，两种策略均没有槽位文件名绑定和防重放保证。

`SaveManager` 不持有 GPU 就绪标志，不捕获截图、不推进帧，也不读取或删除固定 `save_thumb.png`。自动缩略图由 `SaveBinding` 经 `IRenderDevice` 请求并等待自己的结果，再调用同一个 `save()` 路径一次提交；显式缩略图和无图保存可直接使用存档管理器。Lua 等待与取消合同见 [Save 模块](lua-modules.md#save-registered-on-the-kag-module)。

### 16.2 ISaveProvider

抽象存储后端（本地文件、云同步等）。读写保留包括 NUL 在内的原始字节，不解析 JSON，也不负责加解密。SaveManager 在 provider 边界统一处理格式与策略。

| 方法 | 说明 |
|------|------|
| `readFile(path)` | 读取原始字节 |
| `writeFile(path, content)` | 写入原始字节 |
| `deleteFile(path)` | 删除文件 |
| `listFiles(pattern)` | 列出匹配文件 |
| `pushToCloud(slotPath)` | provider 直接原始字节上传；本地实现返回 false，不做 SaveManager 策略校验 |
| `pullFromCloud(slotPath)` | provider 直接原始字节下载；本地实现返回 false，不做 SaveManager 策略校验 |
| `supportsCloudSync` | 当前 provider 是否提供云同步；SaveManager 的安全同步还要求实现下述 transport |

**默认实现**：`LocalFileSaveProvider` — 使用 `std::ifstream/std::ofstream`

### 16.3 ICloudSaveTransport

可选的云传输能力，定义于 [ICloudSaveTransport.h](../../src/storage/api/ICloudSaveTransport.h)，由 `HttpCloudSaveProvider` 与 `CloudSaveProvider` 实现。接口无数据成员，四个操作均为纯虚方法：

| 方法 | 说明 |
|------|------|
| `readLocalFile(slotPath)` | 只读取本地上传源，返回暂存字节，不改变任一端点 |
| `writeLocalFile(slotPath, bytes)` | 将调用方给定的同一份字节提交到本地 |
| `readCloudFile(slotPath)` | 只取得远端下载源，返回暂存字节，不先覆盖本地 |
| `writeCloudFile(slotPath, bytes)` | 上传调用方给定的同一份字节，不重新读取本地文件 |

SaveManager 持有读取结果，验证其 CAES/明文策略、key 和可加载结构后才调用对应 write。校验失败不会调用目标写入；校验中的内存迁移不改变传输字节。必须区分本地和云端：Steam provider 的普通 `readFile/writeFile` 指向云存储，但 push/pull 分别是本地→云端和云端→本地。仅声明 `supportsCloudSync()` 而未实现此 transport 的 provider，不能通过 SaveManager 发起安全同步。

---

## 附录 A: BackendRegistry 完整 getter 列表

```cpp
class BackendRegistry {
    IRenderDevice*       getRenderDevice();
    IAudioBackend*       getAudioBackend();
    IPlatformBackend*    getPlatformBackend();
    IInputRouter*        getInputRouter();
    IVideoPlayer*        getVideoPlayer();
    ITextureManager*     getTextureManager();
    ILayerManager*       getLayerManager();
    IParticleSystem*     getParticleSystem();
    IDebugManager*       getDebugManager();
    IAsyncLoader*        getAsyncLoader();
    IMiniGameBackend*    getMiniGameBackend();
    IAnimationBackend*   getAnimationBackend();
    ICryptoEngine*       getCryptoEngine();
    ILuaManager*         getLuaManager();
    IJobSystem*          getJobSystem();
    ISandboxQuota*       getSandboxQuota();
    ITextureBudget*      getTextureBudget();
    ISaveManager*        getSaveManager();
    IResourceGenerationTracker* getResourceGenerationTracker();
    ISteamBackend*       getSteamBackend();
    IMobileAdapter*      getMobileAdapter();
    IDisplayService*     getDisplayService();
    ILifecycleService*   getLifecycleService();
    IAudioFocusService*  getAudioFocusService();
    IMeshRenderer*       getMeshRenderer();
};
```

## 附录 B: 接口命名约定

- 接口文件：`src/<module>/api/I<ModuleName>.h`
- 接口类名：`I` + PascalCase（`IRenderDevice`, `IAudioBackend`）
- 纯虚类：所有方法 `= 0`，不包含数据成员
- 类型定义（枚举、结构体）如果被接口方法使用，放在接口头文件中
- 命名空间：所有公共类型在 `Caesura::` 下（archive 在 `Caesura::carc::`）
