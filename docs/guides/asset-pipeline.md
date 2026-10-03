# 资源管线 (Asset Pipeline)

## 运行时目录枚举与视频入口

目录扫描使用 [`KAG.list_assets` 和安全视频API](../api/lua-modules.md#kag-资产目录与安全视频)对应的挂载资源服务，不通过`io.popen`、shell命令或写文件探测。枚举是非递归的完整叶名列表，有4096条／1MiB名称上限；provider不支持、非法或链接路径、超限、I/O失败均拒绝，不能当空目录继续。

`fileutil.scan_dir`的过滤器是Lua pattern，不支持正则表达式的`|`。多个音频／图片扩展名应分别匹配，或枚举后显式比较扩展名。独立Lua工具使用LuaFileSystem，必须保留`lfs.dir`返回的iterator与directory userdata，并排除符号链接项。

KAG视频只接受相对资产路径，经读取器的64MiB上限和`openMemory`解码。会话token属于当前VM和后端generation；不是TextureManager ID或原始decoder handle。真实解码／绘制／关闭与硬件可用性按实际验证记录描述。

> Caesura (AmeKAG) 引擎的资源管线负责加载、解码和管理游戏所需的所有外部资源。
> 本文档给出：**格式支持矩阵**（对照 src/ 解码器）、**资产目录规范**（每目录的
> 支持格式 / 尺寸建议 / 命名约定）、**资源加载流程**（异步预加载与运行时解析）。

---

## 1. 图片（stb_image）

引擎使用 **stb_image** 解码图片（`src/render/stb_impl.cpp` 单一实现 TU，
`src/resource/ImageDecoder.cpp` / `src/render/TextureManager.cpp` 消费），支持：

| 格式 | 支持 | 透明通道 | 推荐用途 | 备注 |
|------|------|---------|---------|------|
| PNG | ✅ | ✅ (RGBA) | 立绘、UI 元素 | **首选格式**，无损压缩 + alpha；支持 8/16-bit |
| JPG/JPEG | ✅ | ❌ (RGB) | 背景图片 | 有损压缩，文件小，适合大尺寸背景 |
| TGA | ✅ | ✅ | 纹理资源 | 支持 RLE 压缩 |
| BMP | ✅ | ❌ | — | 无压缩，不推荐 |
| PSD | ✅ | ✅ | — | Photoshop 原生格式，开发期可用 |
| GIF | ✅ | ✅ | — | 静态 GIF 支持，**不支持动画** |
| HDR | ✅ | N/A | HDR 光照贴图 | 高动态范围（渲染扩展用） |
| PIC | ✅ | — | — | Softimage PIC 格式 |
| PNM | ✅ | — | — | Netpbm 格式 (PBM/PGM/PPM) |

> **不支持**：WebP（未编译 STB/webp）、AVIF、EXR、TLG（KiriKiri 专属格式——
> 需先行 `tools/tlg2png.py` 转 png，见 [tlg-compat.md](tlg-compat.md)）。
> 引擎加载失败时**不崩溃**：占位纹理兜底（开发紫色 / 发布灰色）。

### 1.1 纹理限制

| 属性 | 限制 |
|------|------|
| 最大分辨率 | TextureBudget 分级控制 (128MB–4GB) |
| 最大单纹理尺寸 | 硬件相关（通常 4096² 或 8192²） |
| 推荐立绘尺寸 | 1024×2048（全身）/ 1024×1024（半身） |
| 推荐背景尺寸 | 1920×1080（16:9） |
| 推荐 UI 元素 | 按实际显示尺寸，不超过 512×512 |

### 1.2 图片命名约定

| 目录 | 内容 | 命名建议 | 示例 |
|------|------|---------|------|
| `assets/bg/` | 场景背景 | `scene_place.png/jpg`（小写 snake_case） | `bg/classroom.png` |
| `assets/fg/` | 前景 / 可移动层 | `object_name.png` | `fg/curtain.png` |
| `assets/chara/` | 角色立绘（[csp] 默认搜索） | `<name>.png`、`<name>_<表情>.png` | `chara/hero_smile.png` |
| `assets/ui/` | UI 元素 | `dialog_box.png`、`choice_button.png` | `ui/choice_button.png` |
| `assets/lut/` | 调色 LUT（palette 用） | `night.png` 等（`[palette effect=]` 引用） | `lut/night.png` |

---

## 2. 音频（SoLoud）

引擎使用 **SoLoud** 音频引擎（`src/audio/SoLoudAudioEngine.cpp`），支持：

| 格式 | 支持 | 推荐用途 | 备注 |
|------|------|---------|------|
| WAV | ✅ | 音效 (SE)、语音 (Voice)、BGM | 无损，**推荐**用于音效和语音 |
| FLAC | ✅ | 高质量 BGM | 无损压缩，文件比 WAV 小 |
| MP3 | ✅ | 压缩 BGM | 有损，文件最小；建议 ≥192kbps |
| OGG (Vorbis) | ✅ | 压缩 BGM | 开源有损格式，q5+ 质量 |

> **不支持**：MIDI（`.mid`）、MOD/S3M 等 tracker 格式、自定义音频容器。
> KAG3 作品迁移时 `.mid` 需转 wav/ogg（见 [kag3-migration.md](kag3-migration.md) 步骤 ③）。

### 2.1 音频参数建议

| 用途 | 采样率 | 位深 | 声道 | 推荐格式 |
|------|--------|------|------|---------|
| BGM | 44100 Hz | 16-bit | 立体声 | MP3 192kbps / OGG q5 / FLAC |
| 语音 | 22050 Hz | 16-bit | 单声道 | WAV |
| 音效 | 44100 Hz | 16-bit | 单声道/立体声 | WAV |

### 2.2 音频总线（三条独立总线）

| 总线 | Lua API | 同时播放数 | 用途 |
|------|---------|-----------|------|
| BGM | `KAG.play_bgm` / `KAG.stop_bgm` | 1 | 背景音乐（带交叉淡化 xfadebgm） |
| Voice | `KAG.play_voice` / `KAG.stop_voice` | 1 | 角色语音（阻塞 `[voice]` 等待播完） |
| SE | `KAG.play_se` / `KAG.stop_se` | 多个 | 音效（点击/脚步/环境） |

### 2.3 音频目录与命名

| 目录 | 内容 | 命名建议 | 示例 |
|------|------|---------|------|
| `assets/bgm/` | 背景音乐 | `trackname.wav|mp3|ogg|flac`（小写 snake_case） | `bgm/title.ogg` |
| `assets/se/` | 音效 | `click.wav`、`door_open.wav` | `se/click.wav` |
| `assets/voice/` | 语音 | `<角色>_<编号>.wav` | `voice/hero_001.wav` |

---

## 3. 视频（FFmpeg / pl_mpeg）

视频解码默认启用 FFmpeg（CMake 选项 `CAESURA_ENABLE_FFMPEG`，默认 `ON`）。**当编译时
找到 FFmpeg（`external/ffmpeg` / vcpkg / pkg-config）时，引擎通过 FFmpeg 支持其可探测的
全部容器与编码格式**（MP4/H.264、HEVC、VP9、WebM、MKV、MPEG-1/2 等），并启用硬件解码与
SIMD 加速、含音频轨。`pl_mpeg` 仅作零依赖回退：只在未编译 FFmpeg、或 FFmpeg 打开失败时，
回退到 **MPEG-1 专用**解码。

| 解码器 | 支持范围 | 备注 |
|--------|---------|------|
| FFmpeg（首选，`CAESURA_ENABLE_FFMPEG=ON` 且找到 FFmpeg） | FFmpeg 支持的全格式（MP4/H.264、HEVC、VP9、WebM、MKV、MPEG-1/2 等） | 硬件解码 + SIMD；含音频轨（重采样为浮点立体声 PCM） |
| pl_mpeg（回退） | 仅 MPEG-1 | FFmpeg 未启用或打开失败时回退；帧经 `bgfx` 上传为纹理 |

> 若需固定 MPEG-1 兼容性（如目标平台不随附 FFmpeg），可 `-DCAESURA_ENABLE_FFMPEG=OFF`
> 强制走 pl_mpeg 回退，并预先用 FFmpeg 将素材转码为 MPEG-1。

### 3.1 视频目录

| 目录 | 内容 | 说明 |
|------|------|------|
| `assets/video/` | 开场/结局动画 | `.mp4`（H.264 编码兼容性最好）、`.mkv`、`.webm` |

> **Android 注意**：Android 交叉编译强制 `CAESURA_ENABLE_FFMPEG=OFF`（无 sysroot 构建），
> 播放走 pl_mpeg（仅 MPEG-1）——移动端视频素材需预转码为 MPEG-1（见 [android-build.md](android-build.md) §3.1）。

---

## 4. 字体（FreeType 2）

文本渲染使用 **FreeType 2**（`src/render/TextRenderer.cpp`）：加载 **OpenType（.otf）与
TrueType（.ttf）** 字体，字形烘焙进 R8 灰度图集（atlas）供 bgfx 采样。

| 格式 | 支持 | 说明 |
|------|------|------|
| OTF (OpenType) | ✅ | 支持 CFF 与 TrueType 轮廓；**CJK 字重推荐**（`NotoSansCJKsc-Regular.otf` 仓库自带） |
| TTF (TrueType) | ✅ | 经典格式；任意 Latin/CJK 字体 |
| TTC (字体集合) | 部分 | FreeType 可打开，引擎取首个 face——不保证多 face 行为 |
| WOFF/WOFF2 | ❌ | 网页压缩字体格式，桌面引擎不加载 |

### 4.1 字体目录与命名

| 目录 | 内容 | 命名建议 |
|------|------|---------|
| `assets/fonts/` | 全部字体文件 | `<Family>-<Weight>.<otf|ttf>`，CJK 用 NotoSansCJKsc 系列 |

```kag
[font face="assets/fonts/NotoSansCJKsc-Regular.otf" size=24 color="#ffffff"]
```

> **fallback 语义**：text 管线支持在 face 缺失时回退默认字体（engine 内置位图 fallback）；
> CJK 文本务必显式指定 CJK 字体（默认字体若不覆盖 CJK 字形会显示空白/方框）。

---

## 5. 推荐目录结构（完整规范）

```
assets/                              # 游戏根（相对游戏根解析，从项目根 CWD 启动）
├── images/                          # 图像（也可直接放 bg/ fg/ ui/ 在根层）
│   ├── bg/                          # 背景图片（1920×1080 png/jpg）
│   │   ├── classroom.png
│   │   └── hallway.jpg
│   ├── fg/                          # 前景/立绘（png，含 alpha）
│   │   ├── hero_normal.png
│   │   ├── hero_smile.png
│   │   └── heroine_default.png
│   └── ui/                          # UI 元素（≤512×512 png）
│       ├── dialog_box.png
│       └── choice_button.png
├── audio/
│   ├── bgm/                         # 背景音乐（mp3/ogg/flac/wav）
│   │   ├── title.mp3
│   │   └── peaceful_day.ogg
│   ├── se/                          # 音效（wav 首选）
│   │   ├── click.wav
│   │   └── door_open.wav
│   └── voice/                       # 语音（wav，22050 Hz 单声道）
│       ├── hero_001.wav
│       └── heroine_001.wav
├── fonts/                           # 字体（otf/ttf）
│   ├── NotoSansCJKsc-Regular.otf
│   └── LICENSE-NotoSansCJK.txt
├── lang/                            # i18n 语言文件（zh.lua / en.lua / ja.lua）
├── live2d/                          # Live2D 模型（需 CAESURA_LIVE2D=ON）
│   └── character_name/
│       ├── character_name.moc3
│       ├── character_name.model3.json
│       ├── textures/  motions/  expressions/
├── script/                          # .ks 场景（migration 专用，见 kag3-migration.md）
├── sma/                             # SMA 骨骼动画 JSON
└── video/                           # 视频（FFmpeg 支持任意容器，如 .mp4/.mkv/.webm）
    ├── opening.mp4   # H.264
    └── ending.mkv
```

> **两个资产根**：仓库 `assets/` 是引擎共享资产池；`demo/assets/` 是示例游戏专属
> （如 SMA 骨架）。打包时 `package_game.sh --assets <dir>` 决定随 Web 站分发哪个根。

**目录规范汇总表**：

| 目录 | 支持格式 | 推荐尺寸/质量 | 命名约定 |
|------|---------|--------------|---------|
| `assets/bg/` | png, jpg（tga/bmp/gif/psd 开发期） | 1920×1080 | `scene_<place>.png/jpg` |
| `assets/fg/` | png（需 alpha 时） | 全屏或片段 | `object_name.png` |
| `assets/chara/` | png | 1024×2048 / 1024×1024 | `<name>[_<emotion>].png` |
| `assets/ui/` | png | ≤512×512 | `dialog_box.png` 等 |
| `assets/bgm/` | wav, flac, mp3, ogg | 44100 Hz 16-bit 立体声 | `track_name.ext` |
| `assets/se/` | wav 首选（ogg/mp3 亦可） | 44100 Hz 16-bit | `click.wav` 等 |
| `assets/voice/` | wav 首选 | 22050 Hz 16-bit 单声道 | `<char>_<nnn>.wav` |
| `assets/fonts/` | otf, ttf | — | `<Family>-<Weight>.ext` |
| `assets/video/` | mp4/mkv/webm（FFmpeg）/ mpg（pl_mpeg） | 720p/1080p | `opening.mp4` 等 |
| `assets/lang/` | lua（i18n 语言表） | — | `<code>.lua`（zh/en/ja） |
| `assets/script/` | ks | — | `scene.ks`（跨场景跳转以 assets/script/ 为根） |
| `assets/sma/` | json | — | `skeleton.json` |
| `assets/live2d/` | moc3 + model3.json + png | — | `<name>/` 目录组织 |

---

## 6. 资源加载流程

### 6.1 运行时解析顺序（Provider Chain）

资源请求（`storage=` / `path=` / `file=` 相对游戏根）由 `ProviderChain` 按优先级
依次在下列提供者中查找同名路径，命中即停：

1. **磁盘目录**（开发/裸发布：`assets/` 直接文件系统）
2. **CARC 归档**（发布形态）：`data.carc` → `game.carc` → `patch.carc`
   （组合根 `registerDefaultAssetProviders` 自动探测并注册，见 [carc-packaging.md](carc-packaging.md)）

> 引擎**没有 `carc://` 协议**：CARC 内路径即脚本中的相对路径，纯路径语义。
> 同名路径在磁盘目录优先于归档。

### 6.2 异步预加载（[preload]）

异步预加载通过 KAG 标签 `[preload]` 声明（`scripts/kag/commands/resource.lua`），
底层由 C++ 侧 `AsyncLoader` + `JobSystem` 在工作线程执行 IO/解码，完成后回调通知主线程；
Lua 侧管理缓存与占位纹理回退。

```ks
; 预加载图片到纹理缓存（异步，先返回占位纹理）
[preload type="texture" path="images/bg/school.png" wait="false"]

; 同步等待全部加载完成后再继续
[preload type="texture" path="images/bg/a.png,images/bg/b.png" wait="true"]

; 预加载音频 / 场景
[preload type="audio" path="audio/bgm_01.mp3"]
[preload type="scene" path="act2.ks"]
```

- `type` — `texture` / `audio` / `scene`
- `path`（或 `storage`）— 逗号分隔的资源路径（相对游戏根）
- `wait` — `"true"` 同步阻塞协程直至加载完成；`"false"` 后台加载，提前使用时显示占位纹理（开发紫色 / 发布灰色）

加载状态由 `kag.commands.resource` 的 `is_loaded` / `is_pending` / `flush_cache`
函数管理（供其他 KAG 命令内部调用，并非 `KAG.*` 直接绑定）。

### 6.3 场景加载（跨场景跳转）

- 场景文件固定位于 `assets/script/*.ks`（scheduler 的 `is_safe_scene_path` 要求
  `^assets/script/` 前缀 + `.ks` 后缀，禁止 `..` 穿越）——跨场景 `[jump]`/`[call]`/`[link]`
  只解析 `assets/script/<target>.ks`。
- 场景资源（背景/立绘/音频）的 `storage=` 与场景文件位置无关，相对游戏根解析。

---

## 7. 相关工具与文档

| 工具/文档 | 位置 | 用途 |
|----------|------|------|
| stb_image | vendored `external/` | 图片解码（编译集成） |
| SoLoud | vendored `external/soloud` | 音频引擎（WAV/FLAC/MP3/OGG） |
| FreeType 2 | vendored `external/freetype` | 字体渲染（otf/ttf） |
| FFmpeg / pl_mpeg | 可选 / vendored | 视频解码 |
| `tools/tlg2png.py` | tools/ | TLG → PNG 转换（KAG3 迁移，见 tlg-compat.md） |
| `scripts/ks_bake.lua` | scripts/ | Web bundle 烘焙（--web 模式） |
| [carc-packaging.md](carc-packaging.md) | docs/guides/ | CARC 打包与运行时归档 |
| [sample-game-assets.md](sample-game-assets.md) | docs/guides/ | 示例游戏资产审计与降级策略 |
