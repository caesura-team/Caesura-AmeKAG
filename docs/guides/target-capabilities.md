# 目标能力与作品声明

引擎在运行和打包前检查作品要求的能力。目录位于 [runtime-capabilities.json](../../config/runtime-capabilities.json)，覆盖本轮已确认的目标差异；它不是发布认证或所有 API 的实现清单。平台交付状态仍以[平台支持矩阵](../design/platform-support-matrix.md)为准。

## 在作品中声明

将 `capabilities` 放入作品根目录的 `caesura.project.json`。旧项目可以没有此字段。

```json
{
  "capabilities": {
    "required": ["audio.play"],
    "optional": ["video.play", "render.particles"],
    "accept_approximate": ["render.postfx.lut3d"]
  }
}
```

- `required`：即使静态场景未调用，也必须具备。构建配置不满足时拒绝打包；实际后端不满足时拒绝启动项目策略。
- `optional`：不支持时可以跳过调用，并记录目标、场景、命令和原因。它不会把不支持改成支持。
- `accept_approximate`：作者明确接受对应的近似实现。近似结果仍标记为 `approximate`。仅声明 `optional` 不会自动接受近似效果。

字段值必须是已知能力键组成的数组。拼错、重复、类型错误、重复 JSON 属性以及 `required`/`optional` 冲突都会拒绝。可静态确认的调用默认是必需的；希望降级的调用必须列为可选。示例作品在自己的元数据中声明了可选音频和装饰效果，不能把这些设置当作其他作品的默认策略。

## 检查与打包

从仓库根目录执行；作品路径可以包含中文和空格。

```powershell
python scripts/caesura.py check 'D:\作品\我的游戏' --target web --json-output 'web-capabilities.json'

$engine = '.\build\presets\windows-foundation\Debug\CaesuraAmeKAG.exe'
python scripts/caesura.py check 'D:\作品\我的游戏' --target native --engine $engine --json-output 'native-capabilities.json'
python scripts/caesura.py build 'D:\作品\我的游戏' --engine $engine

node scripts/package_game.mjs demo/example_game --out dist/example-game
```

不带 `--target` 的 `check` 保留普通场景检查。`build`、`package` 和 Node Web 打包入口始终检查能力；`--skip-check` 只跳过普通场景 lint。目录输入会递归收集场景，显式追加的 `--entry` 也进入检查。多场景输入必须属于同一项目声明。

原生检查查询选中的可执行文件 `--capabilities-json`，绑定该文件的 SHA256 与实际编译选项，不借用另一个构建目录的 CMake 配置。查询本身不初始化 GPU、音频或 Steam。

Web 构建记录构建开始时的源码集合，核对实际打包模块，完成后再次检查源码。`--no-web-build` 只接受来源和产物字节均匹配的构建；仅修改运行时 Lua、播放器源码或已构建的 Service Worker 都会让旧 profile 失效。最终包继承已核验的构建来源，并核对实际复制的播放器文件及明确注入的项目声明。

Web 输出包含 `CAPABILITIES.json` 和 `capabilities-build.json`；原生构建信息包含检查结果及选中引擎的身份。报告中的 `checked_inputs`/输入清单和 profile 用于复核被检查的具体输入，不认证游戏素材、设备或账号。

## 如何解释状态

| 字段或状态 | 含义 |
|---|---|
| `scope: build` | 对选中二进制或 Web 构建的实现资格判断；没有执行资源加载 |
| `scope: runtime` | 本次宿主实际后端的可用状态，每次重新查询 |
| `supported` | 该目标有对应实现，且当前 scope 的条件满足 |
| `approximate` | 有明确标注的近似实现，需要作者接受 |
| `unsupported` | 没有实现、命令未接线、SDK 未编译或后端不可用 |
| `proven: false` | 宿主缺失或查询无效，没有已证明的支持状态 |
| 操作结果 `applied` / `approximate` | 本次操作的成功返回值已确认；不等于物理声卡、显示器或在线服务的验收 |
| 操作结果 `failed` | 实现资格满足，但本次资源或操作失败 |

Lua `backend.get_capability(feature)` 返回新表。受保护的后端操作保留原有句柄/布尔第一返回值，第二返回值给出操作结果；调用者不能把 Lua 中为真的 `0` 当作有效后处理句柄。粒子 ID 的合法起点仍是 `0`，失败为 `-1`。

```lua
local backend = require('backend')
local capability = backend.get_capability('render.postfx.lut3d')
local handle, result = backend.set_postfx('lut3d', {
    lutId = texture, intensity = 0.6, strength = 0.6,
})
if result then print(result.status, result.feature, result.reason) end
```

标签和公开 `kag` 命令调用共用运行时检查；动态 Lua 不能通过直接调用 `kag.live2d_motion` 等未接线命令来制造已执行状态。清理已有资源仍可执行。请求不支持的 BGM 淡出时会停止现有播放，并单独报告淡出未应用；必需淡出的错误仍保留。

## 当前差异

| 能力 | 原生 | Web |
|---|---|---|
| `render.postfx.bloom/vignette/lut/softblur` | 运行时渲染后端必须就绪并支持对应阶段 | 未实现 |
| `render.postfx.lut3d` | 实际 LUT 后处理，受后端及资源条件限制 | 固定 CSS 色调近似，不采样作者 LUT 像素 |
| `render.particles` | 粒子与渲染会话均须就绪 | 未实现，不分配伪句柄 |
| `render.blur` | 当前命令未接线 | 未实现 |
| `video.play` | 基础 MPEG 路径不依赖 FFmpeg | 未实现 |
| `video.ffmpeg` | 需要实际编译 FFmpeg 且视频后端可用 | 未实现 |
| `audio.play` | 需要实际音频会话 | 需要 AudioContext；本次加载/解码仍可能失败 |
| `audio.fade` | 需要音频会话 | 支持 clip 淡入/淡出和 bus 音量渐变；需要 AudioContext，本次调度仍可能失败 |
| `audio.crossfade` | 当前 Lua backend 路径未接线 | 未实现 |
| `live2d.cubism` | 需要实际编译 Cubism 且模型后端可用 | 未实现 |
| `kag.live2d_motion/expression/lip_sync` | 三个 KAG 命令尚未接入实际模型操作 | 同样不支持 |
| `steam.achievements/stats/cloud` | 需要实际编译 Steam 且会话可用 | 未实现 |

Null、PNG 回退、SDK 文件夹存在、配置开关请求开启都不能证明相应后端可用。SoLoud ManualMix 的测试结果也不代表物理声卡输出。

Web 的 `playbgm` 淡入和 `stopbgm`/`playbgmstop` 淡出只作用于当前声音的 clip 增益；`fadebgm`/`fadevol` 改变 bus 增益。停止不会将 bus 留在零音量。脚本渐变不写入用户持久音量设置，用户直接调节会覆盖已有 bus 渐变。

v1 音频存档记录保存瞬间的 BGM clip 增益和位置。恢复时将该增益作为静态值，清除旧声音及其淡出尾音，保留当前 bus 音量与用户设置；不恢复剩余渐变曲线。实际 Web Audio PCM、手势解锁与暂停恢复的复核入口为 `node scripts/web_audio_smoke.mjs --out artifacts/validation/web-audio-check`，输出目录必须未存在。该检查使用静音输出端分析 PCM，不证明扬声器或耳机的实际听感。

## 未证明的动态范围

静态检查不执行作者的表达式或 Lua。动态资源格式、动态效果选择、`iscript` 等记录到 `not_proven`，不能据此授予全路径能力完整性，也不会仅因为使用 Lua 就拒绝整个包。运行到实际能力调用时仍要重新检查。未执行分支、未加载素材、真实设备、SDK 模型与账号行为均需各自的运行证据。

当前 Node `--out` 仍要求仓库内的目录；完整仓外作者旅程、输出事务与搬迁验收属于 U21/U22，不能从能力预检通过推断它们已完成。
