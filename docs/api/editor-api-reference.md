# Caesura (AmeKAG) — Editor Developer API Reference

> **面向编辑器前端开发者的完整接口文档**（React/Monaco IDE 源码在 `editor/`；发布包内随引擎分发的 `web-editor/dist/index.html` 是单文件调试面板）
> 最后更新: 2026-08-15
> 截图与异步存档合同于 2026-09-08 按当前源码同步；其余章节保留各自维护范围。

---

## 文档地图

编辑器前端通过三层接口与引擎通信：

| 层 | 协议 | 用途 | 详细文档 |
|---|------|------|---------|
| **RPC** | HTTP / stdio DTO dispatcher | 编辑器控制、调试状态、资源列表与打包 | [§1 HTTP RPC 端点](#1-http-rpc-api) |
| **Lua 绑定** | Lua C API | KAG 脚本调用的引擎能力（音频、渲染、存档等） | [§2 Lua 绑定模块](#2-lua-binding-modules) |
| **KAG 脚本** | `.ks` 文本 | galgame 场景描述语言 | [§3 KAG 命令参考](#3-kag-command-reference) |
| **C++ 接口** | C++ 虚函数 | 引擎内部架构（供引擎开发者参考） | [cpp-interfaces.md](cpp-interfaces.md) |

---

## 1. HTTP RPC API

**Base URL**: `http://127.0.0.1:9876`
**Content-Type**: `application/json`
**CORS**: 仅放行本机来源——origin 主机为 `127.0.0.1` 或 `localhost` 均放行（逐 origin 校验并回显具体值，不设 `*`）；访问与示例一律用 `127.0.0.1`（服务只绑 IPv4，Windows 下 localhost 优先解析 ::1）

> 当前状态：默认 `--editor` 启动 HTTP `EditorServer` 并监听 9876；
> `--editor-stdio` 启动 GPU + stdin/stdout 换行分隔 JSON-RPC，`--headless` 启动
> 无 GPU 的 stdio RPC。两种传输都只提交 DTO，由 Engine owner thread dispatcher
> 执行；transport worker 不持有或访问 `lua_State`。
>
> stdio 已接入 breakpoint、Continue、Step、变量检查和调试状态命令。
> `run` / `eval` 已迁移到 managed coroutine（`startManagedRun` + `pumpManagedRuns`），
> 可正常执行含 `coroutine.yield()` 的脚本；不再返回 `unsupported_yieldable_execution`。
> 暂停时只处理实际 yield 的值，保留 native continuation 的关闭守卫；正常结束、错误、
> abort 与 reload 在释放 registry 引用前关闭协程，以退休挂起截图等资源。
>
> **静态根覆盖**：默认 `--editor` 以探测推导的 webRoot 服务 `/` 与 `/index.html`（发布包内为
> `web-editor/dist` 单文件调试面板：ifstream 直读，不设 mount）。设置环境变量
> `CAESURA_EDITOR_WEBROOT=<dir>` 且 `<dir>/index.html` 存在时，`<dir>` 经
> `set_mount_point("/")` 成为静态根——整棵多文件 SPA（index.html + assets/*.js/*.css）以
> 正确 Content-Type 服务（httplib 内建扩展表），显式 `GET /` 与 `GET /index.html` handler
> 仍按解析后的根注册。激活契约：仅 mount 成功时打印
> `[EDITOR] webroot override active: <dir>`（grep 该行可作激活门，防静默 set_mount_point
> 失败）；未设置或指向不含 index.html 的目录时，逐字节回退默认 webRoot（
> `[EditorServer] Serving web editor from: <dir>` 行仍在）。**路由计数 36（34 `/api/*` +
> `GET /` / `GET /index.html`）两种模式均不变**——mount 与 route handler 并存时 httplib
> 先派发 mount 点文件、后派发 route，无新路由注册。

### 1.1 健康检查

**`GET /api/ping`**

```
→ (no body)
← {"status":"ok","engine":"CaesuraAmeKAG"}
```

---

### 1.2 引擎状态

**`GET /api/status`**

```
→ (no body)
← {"status":"ok","engine":"CaesuraAmeKAG","lua":true,"port":9876}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `status` | string | `"ok"` |
| `engine` | string | `"CaesuraAmeKAG"` |
| `lua` | bool | Lua VM 是否已初始化 |
| `port` | int | 服务器端口 |

**`GET /api/state`**（round 18：IDE 预览面板引擎状态回显）

```
→ (no body)
← {"status":"ok","scene":"demo/start.ks","token_index":42,
   "nvl_mode":false,"language":"zh","backlog_count":3,"layer_count":5}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `status` | string | `"ok"` |
| `scene` | string | 当前场景路径（无游戏运行时为空串） |
| `token_index` | int | 当前脚本 token 位置 |
| `nvl_mode` | bool | NVL 全屏文本模式是否激活 |
| `language` | string | 当前 UI 语言（i18n.current） |
| `backlog_count` | int | backlog 条目数 |
| `layer_count` | int | 图层树节点数 |
| `current_cmd` | string | 当前脚本正在执行的命令（场景未运行时为空串） |

> 旧路径 `GET /api/debug/getState` 保留（返回相同字段）；stdio 传输的
> `getState` 方法同样返回完整状态。

---

### 1.2b SMA 资产校验（round 19）

**`GET /api/sma/validate?path=...`**（IDE SMA 资产面板）

校验 SMA 资产 JSON——引擎内跑共享校验器 `kag.sma_check`（与运行时
加载器同一份代码，保证口径一致）。路径受限：仅允许 `assets/` 与
`demo/assets/` 前缀的相对路径（禁 `..`、禁绝对路径）。

```
→ GET /api/sma/validate?path=demo/assets/sma/hero.json
← {"status":"ok","ok":true,"errors":[],
   "meta":"{\"bones\":8,\"anims\":[\"idle:2\",\"wave:2\"],\"parts\":5,\"verts\":8,\"tris\":4}"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `status` | string | `"ok"` |
| `ok` | bool | 资产是否通过校验 |
| `errors` | string[] | 违规列表（带字段路径，如 `mesh.indices[3]: vertex index 99 out of range`） |
| `meta` | string | JSON 文本：结构摘要 `{bones, anims, parts, verts, tris}`（前端解析） |

错误示例：`path=../../outside.json` → HTTP 400（不安全路径）；
不存在的文件 → `ok:false` + "cannot open file"。
stdio 传输的 `smaValidate` 方法同构（请求字段 `path`）。


### 1.2c SMA 资产编辑保存（round 26）

**`POST /api/sma/save`**（编辑器 SMA 资产面板 Save 按钮）

保存 SMA 资产——引擎先用共享校验器 `kag.sma_check.validate_text`
校验 JSON（与 §1.2b 的 `/api/sma/validate` 同一份代码、同一口径），
校验通过后才写回磁盘。**校验失败不会写盘**。

请求体（`Content-Type: application/json`）：

| 请求字段 | 类型 | 必填 | 说明 |
|---------|------|------|------|
| `path` | string | 是 | 目标相对路径，仅允许 `assets/` 与 `demo/assets/` 前缀（禁 `..`、禁绝对路径） |
| `content` | string | 是 | SMA 资产的 JSON 文本（原样写入） |

```
→ POST /api/sma/save
→ {"path":"demo/assets/sma/hero.json","content":"{...}"}
← {"status":"ok","ok":true,"errors":[]}
```

| 响应字段 | 类型 | 说明 |
|---------|------|------|
| `status` | string | `"ok"` |
| `ok` | bool | 是否校验通过并已写盘 |
| `errors` | string[] | 违规列表（带字段路径，与 §1.2b 格式一致；成功时为空） |

错误示例：

- 坏 JSON（`content` 非法）→ `ok:false` + `errors`（含具体违规，与校验端点一致，**不写盘**）
- 不安全路径（如 `path=../../outside.json`）→ HTTP 400
- 目标目录不存在 / 写入失败 → `ok:false` + 错误信息

stdio 传输的 `smaSave` 方法同构（请求字段 `path` / `content`）。
与 §1.2b 呼应：编辑器 Save 按钮的保存路径复用同一共享校验器，保证
“保存前校验”与“手动校验”口径完全一致，杜绝前端自校验与引擎后端不一致。

---

### 1.3 资源列表

**`GET /api/assets?type={image|audio|script|bg|fg|bgm|se|voice|...}`**

`type` 参数可选，省略则返回全部。过滤值既可以是粗粒度类别（`image`/`audio`/`script`），
也可以是 Scene Builder 需要的按目录槽位（`bg`/`fg`/`bgm`/`se`/`voice`/`char`/`ui`/`scripts`）。
每项同时携带 `type`（粗类别）与 `kind`（目录槽位），供场景搭建面板按槽位取资源。

```
→ (no body)
← [
    {"path":"assets/bg/scene01.png","name":"scene01.png","type":"image","kind":"bg"},
    {"path":"assets/fg/girl_uniform.png","name":"girl_uniform.png","type":"image","kind":"fg"},
    {"path":"assets/bgm/theme.ogg","name":"theme.ogg","type":"audio","kind":"bgm"},
    {"path":"assets/voice/line01.ogg","name":"line01.ogg","type":"audio","kind":"voice"},
    {"path":"assets/scripts/chapter1.ks","name":"chapter1.ks","type":"script","kind":"scripts"}
  ]
```

扫描的目录映射（`type` 过滤匹配粗类别或目录槽位）：

| type | 扫描目录 | kind |
|------|---------|------|
| `image` | `assets/bg/`, `assets/fg/`, `assets/char/`, `assets/ui/` | `bg`/`fg`/`char`/`ui` |
| `audio` | `assets/bgm/`, `assets/voice/`, `assets/se/` | `bgm`/`voice`/`se` |
| `script` | `assets/scripts/` | `scripts` |

示例：`/api/assets?type=fg` 只返回 `assets/fg/` 下的前景精灵；`/api/assets?type=bgm` 只返回 BGM。
前端 `EngineClient.assets(type)` 已同步支持该契约（`AssetEntry.kind` 为新增字段）。

---

### 1.4 执行 Lua 脚本

**`POST /api/run`**

提交脚本到 owner-thread dispatcher，由 managed coroutine 执行
（`startManagedRun` + 每帧 `pumpManagedRuns`），可正常执行含 `coroutine.yield()`
的脚本。请求体为原始脚本文本。

```
→ playbgm('theme.ogg')\nbg('scene01.png')\nch('Hero', 'Hello world!')\np()
← {"status":"ok"}
```

错误响应 (4xx/5xx)：
```
← {"error":"Empty script"}    // 空请求体 → HTTP 400
← {"status":"invalid_request", ...}   // dispatcher 拒绝/失败
```

---

### 1.5 停止执行

**`POST /api/stop`**

向 owner-thread dispatcher 提交停止命令，Engine 在当前帧结束后退出。

```
→ (no body)
← {"status":"ok"}
```

---

### 1.5b 重载脚本（HTTP）

**`POST /api/reload`**

请求 owner-thread 重新加载 Lua 脚本（热重载；与 stdio `reload` 方法同构）。

```
→ (no body)
← {"status":"ok"}
```

---

### 1.6 查看日志

**`GET /api/logs`**

返回最近 200 条日志。

```
→ (no body)
← [
    {"level":"info","message":"Running scene script...","time":"14:32:05"},
    {"level":"error","message":"playbgm: file not found","time":"14:32:06"}
  ]
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `level` | string | `"info"` / `"warn"` / `"error"` |
| `message` | string | 日志内容 |
| `time` | string | `HH:MM:SS` 格式时间戳 |

---

### 1.7 Live2D 模型与静态立绘列表

**`GET /api/live2d/models`**

扫描 `models/`, `assets/models/`, `assets/live2d/` 目录中的动画模型与
静态立绘文件。返回数组按 `path` 排序，文件名和路径会进行标准 JSON 转义。

```
→ (no body)
← [
    {"name":"haru.moc","path":"models/haru.moc"},
    {"name":"shizuku.model.json","path":"assets/models/shizuku.model.json"},
    {"name":"hero.PNG","path":"assets/live2d/hero.PNG"}
  ]
```

文件过滤不区分扩展名大小写：

- Cubism/模型：`.moc`, `.moc3`, `.json`，或文件名含 `.model`。
- 静态立绘：`.png`, `.jpg`, `.jpeg`, `.bmp`。

---

### 1.8 加载并展示动画模型或静态立绘

**`POST /api/live2d/load`**

```
→ {"modelPath":"assets/live2d/hero.png","x":420,"y":80,"scale":1.25}
← {"status":"ok","modelId":1,"name":"hero"}
```

| 请求字段 | 类型 | 必填 | 默认值 | 说明 |
|---------|------|------|--------|------|
| `modelPath` | string | 是 | — | 模型或静态图片路径。**Live2D 模型必须指向 `.model3.json`**（引擎把它当 setting 解析；指向 `.moc3` 会返回 500 `animation_load_failed`）。纹理/动作/物理文件按 setting 内的相对路径在模型同目录解析 |
| `x` | number | 否 | `0` | 展示位置 X；必须是有限数值 |
| `y` | number | 否 | `0` | 展示位置 Y；必须是有限数值 |
| `scale` | number | 否 | `1` | 展示缩放；必须是有限正数 |
| `show` | bool | 否 | `true` | 加载成功后是否立即展示 |

省略 `show` 时，owner thread dispatcher 会在 `loadModel()` 成功后调用
`showModel(modelId, x, y, scale)`。传入 `"show": false` 时只加载资源，
保持隐藏，供后续编辑器操作使用。HTTP worker 只解析并提交 DTO，不直接访问
Engine、Lua 或动画后端。

非法 JSON、错误字段类型、非有限坐标或 `scale <= 0` 返回 HTTP 400：

```
← {
    "status":"invalid_request",
    "code":"invalid_animation_request",
    "error":"scale must be greater than zero",
    "message":"scale must be greater than zero"
  }
```

---

### 1.9 一键打包 (CARC)

**`POST /api/build`**

将 `scripts/` 和 `assets/` 下所有文件打包为加密 `.carc` 归档。

```
→ (可选) {"outputPath":"build/game.carc","keyPath":"build/game.key"}
← {"status":"ok","path":"build/game.carc","size":1048576,"files":42}
```

| 请求参数 | 必填 | 默认值 | 说明 |
|---------|------|--------|------|
| `outputPath` | 否 | `build/game.carc` | 输出归档路径 |
| `keyPath` | 否 | `build/game.key` | Ed25519 密钥基础路径 |

| 响应字段 | 说明 |
|---------|------|
| `status` | `"ok"` |
| `path` | 生成的 .carc 文件路径 |
| `size` | 文件大小（字节） |
| `files` | 打包的文件数量 |

### 1.10 执行 Lua 片段（/api/eval 桥 — LSP 语言服务 + 引擎跳转，round 80）

**POST /api/eval** — 提交一段原始 Lua（返回它的求值结果）。
HTTP worker 与 `/api/run` 走同一 owner-thread Lua 派发（Lua 脚本错误 → HTTP 500，错误信息透传）。
这是编辑器 LSP 语言服务（`kag.lsp.json`）与**大纲驱动实机跳转**（engineJump）的通路。

| 参数 | 类型 | 说明 |
|------|------|------|
| body | string | 原始 Lua 脚本文本（`return` 的表达式结果即为响应 `result`） |

| 响应字段 | 类型 | 说明 |
|---------|------|------|
| status | string | `"ok"` |
| result | string | Lua 片段返回值的字符串化结果 |

示例：

```
→ POST /api/eval
→ return 6 * 7
← {"status":"ok","result":"42"}
```

> **LSP 语言服务**：编辑器 `lspCall()` 把请求桥接为 `return kag.lsp.json(...)` 经本端点求值，
> 返回 JSON 文本字符串供渲染器解析。支持的方法见下方 [附录 C：LSP 语言服务方法](#附录-c-lsp-语言服务方法经-apieval-桥接)。

### 1.10b 运行时统计（HTTP）

**`GET /api/stats`**

IDE 面板用引擎运行时统计（纹理预算/网格/任务队列/Lua 堆）。

```
→ GET /api/stats
← {"status":"ok","texture_tier":2,"texture_budget_mb":156.25,"mesh_count":12,
   "job_workers":4,"job_pending":0,"lua_kb":512}
```

### 1.10c 预览帧拾取（HTTP）

**`GET /api/pick?x=&y=`**

IDE 预览帧命中测试：返回包含该窗口像素的 Lua 图层树节点（自下而上）。
坐标越界（<0 或 >8192）→ HTTP 400。

```
→ GET /api/pick?x=640&y=360
← {"status":"ok","hits":[...]}
```

### 1.11 调试端点（HTTP /api/debug/*）

Lua 调试器既可通过 stdio RPC 调用（见 §1.12），也可经以下 HTTP 路由访问，
共 10 条（`GET /api/debug/getState` 同时是 §1.2 `/api/state` 的保留旧路径），
均由 HTTP worker 提交 DTO、由 owner thread dispatcher 执行：

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/debug/getState` | 引擎状态（与 `/api/state` 同字段） |
| `GET` | `/api/debug/getFrame?w=&h=` | 为本次请求绘制并截取完整帧，返回原始 Base64 PNG（默认 1280×720；单边上限 8192，另受渲染器像素/内存预算约束） |
| `POST` | `/api/debug/setBreakpoint` | 设置源码行断点（请求体 `source` + `line`） |
| `POST` | `/api/debug/removeBreakpoint` | 移除断点（请求体 `source` + `line`） |
| `POST` | `/api/debug/clearBreakpoints` | 清除全部断点 |
| `POST` | `/api/debug/continue` | 恢复暂停的调试运行（可选 `pauseId`） |
| `POST` | `/api/debug/stepInto` | 单步进入（进入被调用的 Lua 函数） |
| `POST` | `/api/debug/stepOver` | 单步跳过（不进入被调用的 Lua 函数） |
| `POST` | `/api/debug/stepOut` | 单步跳出（运行至当前函数返回） |
| `GET` | `/api/debug/inspect?name=&frame=&global=` | 检查 Lua 局部或全局变量 |

`getFrame` 在 owner thread 经 Engine 请求自己的截图票据，在完整绘制和后处理后统一呈现一次，并明确等待该票据完成；等待不额外推进空帧，不读取循环命名的旧截图文件。输出按请求的 `w`/`h` 最近邻拉伸，成功响应为 `{"status":"ok","base64":"..."}`，其中图像文本没有 data URI 前缀。HTTP 要求两维均为 1..8192；渲染器还限制总像素与保留字节量，所以维度初检通过不保证接收。headless、不就绪、截图失败或完成等待超时会通过 dispatcher 返回 `capture_failed`，不会伪造旧图。当前 Engine 的完成轮询设有 2 秒期限，超时取消后仍领取/丢弃自己的终态；此期限不能抢占底层驱动、PNG 编码或单次结果领取的互斥等待，不是整个 HTTP 响应的硬实时上限。

`setBreakpoint` / `removeBreakpoint` 请求体（`application/json`）：

```
→ {"source":"assets/scripts/main.ks","line":120}
← {"status":"ok"}
```

`inspect` 缺省检查局部变量，传 `global=1` 时检查全局变量。

---

### 1.12 stdio JSON-RPC 方法总览

`--editor-stdio` / `--headless` 通过 stdin/stdout 换行分隔 JSON-RPC（每行一个
完整 JSON 对象）。方法名与 §1 的 HTTP 端点同名、共用同一个 dispatcher DTO，
共 29 个方法：

**传输 / 生命周期**：`ping` `run` `stop` `reload` `logs` `assets` `eval`

**资源与状态**：`getState` `getFrame` `stats` `pick` `smaValidate` `smaSave`

**Lua 调试器**：`setBreakpoint` `removeBreakpoint` `clearBreakpoints` `continue`
`stepInto` `stepOver` `stepOut` `inspectLocal` `inspectGlobal` `getDebugState`

**KAG 场景调试**：`kagSetBreakpoint` `kagClearBreakpoints` `kagDebugContinue`
`kagDebugStep` `kagReloadScene` `kagInspectScopes`（详见附录 B：KAG 场景调试）

---
## 2. Lua Binding Modules

编辑器通过注入 Lua 脚本来驱动引擎。以下是所有可用的 Lua 绑定模块。

### 2.1 Render 模块

```lua
-- 全局变量: Render
-- 访问: BackendRegistry::instance().getRenderDevice()
```

| 函数 | 签名 | 说明 |
|------|------|------|
| `load_texture` | `(path: string) → id: int` | 从文件加载纹理，返回纹理 ID（0=失败） |
| `destroy_texture` | `(id: int)` | 释放纹理 |
| `create_solid_texture` | `(r, g, b, a: int) → id: int` | 创建 1×1 纯色纹理 |
| `text_set_font` | `(face, size, color?)` | 设置字体；`"default"` 重置为内置位图字体 |
| `text_reset_state` | `()` | 重置文字渲染器内部行/字符状态 |
| `get_resolution` | `() → width, height` | 获取 backbuffer 分辨率 |
| `set_view_name` | `(viewId, name)` | 设置 bgfx 视图调试名称 |
| `set_screen_offset` | `(dx, dy)` | 平移 VIEW_MAIN（camera/quake）；小数被四舍五入 |
| `create_viewport` | `(w, h) → handle` | 创建 RTT 视口（非法尺寸返回 0） |
| `destroy_viewport` | `(handle)` | 销毁 RTT 视口 |
| `draw_viewport` | `(handle, x, y, w?, h?)` | 把 RTT 视口画到 VIEW_MAIN |
| `submit_batch` | `(table)` | 批量提交绘制四边形（见下方） |
| `submit_blend` | `(baseTexId, blendTexId, mode, baseAlpha, blendAlpha, globalAlpha)` | 提交混合特效 |
| `submit_transition` | `(fromTexId, toTexId, ruleTexId, method, progress)` | 提交转场特效 |
| `submit_vfx` | `(srcTexId, effect, fadeAlpha, r, g, b, blur, quakeX, quakeY)` | 提交 VFX |
| `stretch_blt` | `(dstTexId, dx, dy, dw, dh, srcTexId, sx, sy, sw, sh, filter)` | 缩放 Blit |
| `affine_blt` | `(dstTexId, dx, dy, dw, dh, srcTexId, sx, sy, sw, sh, m0..m5)` | 仿射变换 Blit |
| `fill_viewport` | `(vpId, r, g, b, a)` | 纯色填充 RTT |
| `set_color_filter` | `(preset)` | 无障碍颜色滤镜：none/deuteranopia/protanopia/tritanopia/grayscale/high_contrast |
| `resize` | `(width, height)` | 通知引擎窗口大小变化 |
| `is_valid_handle` | `(type: int, id: int) → bool` | 验证资源句柄（type: 0=Texture,1=Shader,2=RTT,...） |
| `load_texture_async` | `(path, callback?) → int` | 异步加载纹理（`<0` 出错）；可选 `(ok, path, texId)` 回调 |
| `cancel_async_loads` | `()` | 取消所有异步加载 |
| `invalidate_handles` | `(type)` | 按类型使缓存资源代际句柄失效 |
| **视频** | | |
| `video_play` | `(path) → handle` | 播放视频 |
| `video_stop` | `(handle)` | 停止视频 |
| `video_update` | `(handle) → bool` | 解码下一帧 |
| `video_get_texture` | `(handle) → texId` | 获取当前帧纹理 ID |
| `video_is_playing` | `(handle) → bool` | 是否播放中 |
| `video_has_ended` | `(handle) → bool` | 是否播放完毕 |
| `video_get_size` | `(handle) → width, height` | 视频尺寸 |
| `video_pause` | `(handle)` | 暂停 |
| `video_resume` | `(handle)` | 继续 |

**submit_batch 格式**：
```lua
Render.submit_batch({
    { tex = textureId, x = 0, y = 0, w = 1280, h = 720, opacity = 255, view = 1 },
    { tex = charTexId,  x = 400, y = 100, w = 480, h = 620, opacity = 255, view = 1 },
})
```

### 2.2 VFX 模块

```lua
-- 全局变量: VFX
-- 访问: BackendRegistry::instance().getParticleSystem()
```

| 函数 | 签名 | 说明 |
|------|------|------|
| `particles_init` | `() → bool` | 初始化粒子系统 |
| `particles_shutdown` | `()` | 关闭粒子系统 |
| `particles_create_emitter` | `(cfg: table) → id: int` | 创建发射器，返回 -1=参数无效 |
| `particles_destroy_emitter` | `(id)` | 销毁发射器 |
| `particles_emit` | `(id, count)` | 发射 N 个粒子 |
| `particles_update` | `(dt)` | 更新粒子物理 |
| `particles_render` | `()` | 渲染所有粒子 |
| `particles_alive_count` | `() → int` | 活跃粒子数 |
| `particles_is_initialized` | `() → bool` | 系统是否已初始化 |
| `particles_clear` | `()` | 清除所有发射器和粒子 |

**Emitter 配置表**：
```lua
{
    x = 640, y = 360,          -- 发射位置
    rate = 10.0,               -- 发射速率（粒/秒，0=手动 emit）
    lifeMin = 0.5, lifeMax = 2.0,   -- 生命期范围（秒）
    speedMin = 10, speedMax = 50,    -- 速度范围
    angleMin = 0, angleMax = 6.283,  -- 角度范围（弧度）
    sizeMin = 2, sizeMax = 8,        -- 尺寸范围
    r = 1, g = 1, b = 1, a = 1,    -- 颜色
    gravityX = 0, gravityY = 0,     -- 重力
}
```

### 2.3 KAG 模块 (C++ 绑定)

```lua
-- 全局变量: KAG
-- 访问: BackendRegistry::instance() + C++ binding layer
```

| 函数 | 签名 | 说明 |
|------|------|------|
| `play_bgm` | `(file, volume?, loop?) → bool` | 播放 BGM |
| `stop_bgm` | `(fadeTime?) → bool` | 停止 BGM（可淡出） |
| `play_se` | `(file, volume?) → bool` | 播放音效 |
| `play_se_3d` | `(file, x, y, z?) → bool` | 播放定位音效 |
| `stop_se` | `() → bool` | 停止所有音效 |
| `is_se_playing` | `() → bool` | SE 总线是否活跃 |
| `play_voice` | `(file, volume?) → bool` | 播放语音 |
| `stop_voice` | `() → bool` | 停止语音 |
| `replay_voice` | `()` | 重播当前语音 |
| `set_global_volume` | `(vol: 0.0–1.0)` | 设置主音量 |
| `get_global_volume` | `() → number` | 获取主音量 |
| `set_bus_volume` | `(bus, vol)` | 设置总线音量（"bgm"/"voice"/"se"） |
| `get_bus_volume` | `(bus) → number` | 获取总线音量 |
| `set_bgm_volume` | `(vol)` | 设置 BGM 总线音量 |
| `set_se_volume` | `(vol)` | 设置 SE 总线音量 |
| `set_voice_volume` | `(vol)` | 设置 Voice 总线音量 |
| `audio_fade_volume` | `(bus, target, seconds)` | 平滑音量过渡 |
| `audio_get_length` | `(bus) → number` | 当前音轨长度（秒） |
| `audio_get_position` | `(bus) → number` | 当前播放位置（秒） |
| `is_bgm_playing` | `() → bool` | BGM 是否播放中 |
| `is_voice_playing` | `() → bool` | 语音是否播放中 |
| `get_active_voices` | `() → int` | 活跃语音数 |
| `flush_wave_cache` | `()` | 清空解码波形缓存 |
| `render_text` | `(text, x, y, scale, r, g, b, a)` | 渲染文字 |
| `render_ruby` | `(text, ruby, x, y)` | 渲染注音 |
| `clear_text` | `()` | 清除文字 |
| `clear_text_layer` | `()` | 清除文字层（同 clear_text） |
| `clear_screen` | `()` | 清除画面图层 |
| `line_height` | `() → number` | 行高 |
| `quake` | `(duration_ms, amplitude?)` | 屏幕震动 |
| `show_text` | `(text)` | 显示文本行 |
| `show_image` | `(path, ...)` | 将图片显示到图层 |
| `wait_click` | `()` | 等待点击 |
| `set_listener` | `(...)` | 设置音频监听器 |
| `log` | `(message)` | 日志 |

### 2.4 Debug 模块

```lua
-- 全局变量: Debug
```

| 函数 | 签名 | 说明 |
|------|------|------|
| `log` | `(level, message)` | 写入引擎日志 |
| `get_last_error` | `() → string` | 最近一次引擎错误 |
| `get_error_count` | `() → int` | 累计错误数 |
| `get_subsystem_stats` | `(subsystem)` | 子系统错误/状态计数 |
| `dump_report` | `()` | 输出结构化错误/状态报告 |
| `get_render_info` | `()` | 渲染后端诊断 |
| `get_audio_info` | `()` | 音频后端诊断 |
| `get_input_info` | `()` | 输入后端诊断 |
| `get_log_path` | `() → string` | 引擎日志文件路径 |
| `get_stats` | `()` | 聚合运行时统计 |

这里的 `Debug` Lua 表提供日志、错误与诊断绑定。`DebugProtocol` 由 `Engine`
按 Lua VM 生命周期持有，支持可 yield KAG 协程的非阻塞断点、继续、step
into/over/out 和变量检查。KAG scheduler 的所有普通推进均经过同一 resume 仲裁入口，
暂停期间的 frame update、点击批处理及其他 Lua 回调不会越过断点。完整调试命令当前
通过 stdio RPC 暴露；HTTP 调试路由已开放（7 条 `/api/debug/*`）。

### 2.5 DevCore 模块

```lua
-- 全局变量: DevCore
```

| 函数 | 签名 | 说明 |
|------|------|------|
| `set_input_focus` | `(bool)` | 输入焦点切到 KAG / 游戏 |
| `get_input_focus` | `() → bool` | 输入焦点是否在游戏 |
| `log` | `(message)` | 写一条引擎日志 |
| `quit` | `()` | 请求退出引擎 |
| `set_resolution` | `(w, h)` | 设置渲染分辨率 |
| `get_resolution` | `() → w, h` | 获取当前分辨率 |
| `set_fullscreen` | `(bool)` | 切换全屏 |
| `get_window_size` | `() → w, h` | 获取 OS 窗口尺寸 |

### 2.6 Save 模块（注册在 KAG 模块上）

```lua
-- 存档/读档绑定注册到 KAG 模块（KAG.save_game, KAG.load_game, ...），
-- 没有独立的 Save 全局变量。
-- Backend: BackendRegistry::instance().getSaveManager()
-- 缩略图: BackendRegistry::instance().getRenderDevice()
```

| 函数 | 签名 | 说明 |
|------|------|------|
| `save_game` | `(slot, data, sceneName, tokenIndex, thumbnail?) → bool, error?` | 同步原子保存提供的状态及缩略图文本，不自行截图 |
| `load_game` | `(slot: int) → table` | 从槽位加载 |
| `list_saves` | `() → table` | 列出所有存档 |
| `delete_save` | `(slot: int)` | 删除存档 |
| `save_exists` | `(slot: int) → bool` | 槽位是否有存档 |
| `get_save_dir` | `() → string` | 存档写入目录 |
| `set_encryption_key` | `(key)` | 设置/派生存档加密密钥 |
| `clear_encryption_key` | `()` | 清除存档加密密钥 |
| `capture_thumbnail` | `(cancelToken?) → base64OrNil, status, errorOrNil` | 请求本次 320×180 缩略图，等待自己的票据完成；第一返回值保持原始 Base64 字符串/nil |
| `configure_cloud` | `(endpoint: string) → bool` | 配置 HTTP 云存档端（`""` 恢复本地）；离线降级不抛错 |
| `cloud_push` | `(slot: int) → bool` | 把槽位文件推送到云端 |
| `cloud_pull` | `(slot: int) → bool` | 从云端拉取槽位文件到本地 |

`capture_thumbnail` 的可选参数为现有 `kag.cancel_token` 实例。待完成期间通过 Lua 5.4 continuation 零值 yield，同一次调用在恢复时继续领取原票据；不会以 `nil, 'pending'` 提前返回。结果状态为 `completed`、`failed`、`cancelled`、`unavailable`，失败原因在第三返回值；成功字符串不含 `data:image/png;base64,` 前缀。无 GPU/未就绪时明确无图。不可 yield 的调用若尚未完成，会取消并领取终态后返回 `nil, 'unavailable', 'not-yieldable'`，不泵额外帧。

`[save]` 在等待前固定槽位、状态副本、场景、位置和描述，沿现有 Operation/会话关闭机制取消挂起操作；停止、重载、取消或 owner 替换后的旧操作不再写盘。同 owner/slot 已有挂起保存时返回 `save-busy`。显式缩略图及 `ctx.captureThumbnail(token)` hook 保持原有优先级；普通可选截图失败允许无图存档，并分别记录 `tf.thumbnail_result` / `tf.thumbnail_error` 与 `tf.save_result` / `tf.save_error`。完整签名和状态表见 [Lua Save 模块](lua-modules.md#save-registered-on-the-kag-module)。

正常剧本结束后，宿主可对 runner 保留的最终上下文发起一次新的保存；runner 仅在清理成功后授予此资格，并确认仍是当前 owner、没有调度协程/继续执行状态且不在会话切换中。开始转场或清理失败会清除资格。这不会重新激活旧会话，也不会让已失效的旧挂起保存恢复写盘。新最终状态保存等待期间若被替换，仍须取消；旧上下文及清理/重载中的重入保存继续拒绝。

### 2.7 Steam 模块（无条件注册，无 SDK 时安全降级）

```lua
-- 全局变量: steam（始终可用；无 Steam SDK 时由 Null 后端返回安全默认值）
```

| 函数 | 签名 | 说明 |
|------|------|------|
| `unlock_achievement` | `(id)` | 解锁成就 |
| `is_achievement_unlocked` | `(id) → bool` | 成就是否已解锁 |
| `reset_achievement` | `(id)` | 重置单个成就 |
| `reset_all_achievements` | `()` | 重置全部成就 |
| `set_stat_int` | `(name, value)` | 设置整数统计 |
| `get_stat_int` | `(name) → int` | 获取整数统计 |
| `set_stat_float` | `(name, value)` | 设置浮点统计 |
| `get_stat_float` | `(name) → float` | 获取浮点统计 |
| `store_stats` | `()` | 提交统计变更到 Steam |
| `is_overlay_active` | `() → bool` | Steam 覆盖层是否激活 |
| `cloud_write` | `(name, data) → bool` | 写入云存档文件 |
| `cloud_read` | `(name) → string/nil` | 读取云存档文件（不存在返回 nil） |
| `cloud_file_size` | `(name) → int` | 云文件字节数 |
| `cloud_file_exists` | `(name) → bool` | 云文件是否存在 |
| `cloud_delete` | `(name) → bool` | 删除云文件 |
| `cloud_quota_total` | `() → int` | 云配额总字节 |
| `cloud_quota_used` | `() → int` | 云已用字节 |
| `cloud_list` | `() → table` | 云文件列表（最多 256 项） |

---

> **Note:** the authoritative command reference is the auto-generated
> [command-contracts.md](command-contracts.md) (102 contract commands). The
> hand-maintained list below is legacy context.

## 3. KAG Command Reference

KAG 脚本语法：`[command param="value"]`，写在 `.ks` 文件中。

### 3.1 音频命令

| 命令 | 参数 | 说明 |
|------|------|------|
| `playbgm` | `storage` (路径), `volume` (0–1, 默认1), `loop` (bool) | 播放 BGM |
| `stopbgm` | `time` (毫秒, 淡出) | 停止 BGM |
| `fadebgm` | `volume` (目标音量), `time` (毫秒) | 淡入/淡出 BGM |
| `xfadebgm` | `storage`, `time` (毫秒) | 交叉淡入淡出新 BGM |
| `playse` | `storage`, `volume` | 播放音效 |
| `stopse` | — | 停止所有音效 |
| `playvoice` | `storage`, `volume` | 播放语音 |
| `stopvoice` | — | 停止语音 |
| `waitsound` | — | 等待当前 SE 播放完毕 |
| `waitbgm` | — | 等待 BGM 淡入淡出完毕 |
| `setbgmvolume` | `volume` | 设置 BGM 总线音量 |
| `setsevolume` | `volume` | 设置 SE 总线音量 |
| `setvoicevolume` | `volume` | 设置 Voice 总线音量 |

### 3.2 图层命令

| 命令 | 参数 | 说明 |
|------|------|------|
| `bg` | `storage` (路径), `time` (毫秒) | 设置背景图 |
| `fg` | `storage` (路径), `layer` (层号), `clear` (bool) | 设置前景立绘 |
| `cl` | `layer` ("bg"/"fg"/"msg") | 清除指定图层 |
| `image` | `storage`, `layer`, `x`, `y`, `w`, `h` | 在指定位置放置图片 |
| `position` | `layer`, `x`, `y`, `scale` | 定位图层 |
| `layopt` | `layer`, `opacity` (0–1) | 设置图层渲染选项 |

### 3.3 文本命令

| 命令 | 参数 | 说明 |
|------|------|------|
| `ch` | `name` (说话人), `text` | 角色对话 |
| `text` | `text` | 旁白/叙述 |
| `l` | — | 换行 |
| `r` | — | 回车 |
| `er` | — | 清除消息图层所有文字 |
| `p` | — | 分页/等待点击 |
| `ruby` | `text`, `ruby` | 带注音的文字 |
| `font` | `face`, `size`, `color` | 设置字体 |
| `skip` | — | 切换 Skip 模式 |
| `reset` | — | 重置文字状态 |
| `pt` | `speed` (毫秒/字) | 打字机速度 |
| `button` | `text`, `target` (`*label`) | 选项按钮 |
| `endbutton` | — | 确认选项集，等待选择 |

### 3.4 系统命令

| 命令 | 参数 | 说明 |
|------|------|------|
| `wait` | `time` (毫秒) | 等待 N 毫秒 |
| `eval` | `exp` (Lua 表达式) | 执行 Lua，结果存入 ctx |
| `emb` | `exp` (Lua 代码) | 沙箱内执行 Lua |
| `history` | — | 打开 Backlog 界面 |

### 3.5 流程控制

| 命令 | 参数 | 说明 |
|------|------|------|
| `if` | `exp` (Lua 表达式) | 条件分支 |
| `else` | — | 否则分支 |
| `endif` | — | 结束条件块 |
| `jump` | `target` (`*label`) | 跳转到当前场景标签 |
| `call` | `target` (场景路径) | 调用子场景（可 return） |
| `return` | — | 从子场景返回 |
| `link` | `target` (场景路径) | 跨场景跳转 |
| `label` | (内联 `*name`) | 定义跳转目标 |
| `macro` | `name` | 定义宏 |
| `endmacro` | — | 结束宏定义 |
| `end` | — | 结束脚本 |

### 3.6 过渡效果

| 命令 | 参数 | 说明 |
|------|------|------|
| `trans` | `type` (效果名), `time` (毫秒) | 应用命名过渡效果 |
| `move` | `layer`, `x`, `y`, `time` | 动画移动图层 |
| `quake` | `time`, `amplitude` | 屏幕震动 |
| `fade` | `type` ("in"/"out"), `time` | 淡入/淡出 |

### 3.7 特效 / 视频 / 资源 / 存档

| 命令 | 参数 | 说明 |
|------|------|------|
| `vfx` | `type` (shake/flash/blur/sepia), `time`, ... | 视觉特效 |
| `video` | `storage`, `loop`, `volume` | 播放视频 |
| `stopvideo` | — | 停止视频 |
| `preload` | `storage`, `type` | 预载资源 |
| `get_texture` | `storage` | 按路径获取纹理句柄 |
| `is_loaded` | `storage` | 资源是否已载入 |
| `is_pending` | `storage` | 资源是否载入中 |
| `flush_cache` | — | 清空资源缓存 |
| `save` | `slot` (整数) | 存档 |
| `load` | `slot` (整数) | 读档 |
| `listsaves` | — | 列出所有存档 |

---

## 4. C++ 接口

引擎内部架构文档，供需要修改引擎核心的合作开发者参考。

→ [cpp-interfaces.md](cpp-interfaces.md) — 31 个 `I*` 纯虚接口，15 模块，BackendRegistry 完整 getter 列表。

---

## 附录 A: 快速开始 — 编辑器工作流

```
1. 启动 HTTP 编辑器宿主
   → `CaesuraAmeKAG --editor`，监听 http://127.0.0.1:9876

2. 编辑器客户端连接 HTTP API
   → GET /api/ping 确认连接；仓库附带静态 web-editor 前端（`web-editor/dist/
   index.html`，单文件调试面板，随发布包分发进 `web-editor/dist`）：`/` 与
   `/index.html` 静态壳免令牌，页面读 `?token=` 存入 localStorage 后所有
   `/api/*` 戴 Bearer 令牌（default-deny）；启动日志直接给出可点开的地址
   （`[EditorServer] Open the editor: http://127.0.0.1:9876/?token=...`）。

3. 浏览资源
   → GET /api/assets?type=image  列出所有图片
   → GET /api/assets?type=audio  列出所有音频
   → GET /api/assets?type=script 列出所有脚本

4. 编写 KAG 场景
   → POST /api/run 通过 managed coroutine 执行脚本，支持 coroutine.yield()

5. 调试
   → GET /api/logs 查看执行日志
   → POST /api/stop 停止预览

6. 发布
   → POST /api/build 一键打包为 .carc
```

## 附录 B: 通用约定

- **命名空间**: 所有 C++ 公共类型在 `Caesura::` 下（archive 在 `Caesura::carc::`）
- **Lua 全局变量**: 绑定模块注册为 Lua 全局变量（`KAG`, `Render`, `VFX`, `Debug`, `DevCore`, `Save`, `Steam`）
- **BackendRegistry**: 运行时引擎后端访问的唯一入口点；`DebugProtocol` 与 RPC/HTTP 传输由组合根持有，不进入 Registry（参见 [cpp-interfaces.md §A](cpp-interfaces.md#附录-a-backendregistry-完整-getter-列表)）
- **接口文件**: `src/<module>/api/I<Name>.h`，纯虚类，不包含实现
- **构建**: `cmake --build build --config Debug`
- **测试**: `build/tests/Debug/CaesuraTests.exe --no-skip`（以当前 fresh build 输出为准）

## 附录 B：KAG 场景调试 JSON-RPC 方法

`--headless` / `--editor-stdio` 的 stdin/stdout JSON-RPC 除 Lua 调试器（`setBreakpoint`/`stepInto`/`inspectLocal`…）外，还提供 **KAG 场景层调试**（Lua 调试器看不到 KAG token）：

| 方法 | 参数 | 说明 |
|---|---|---|
| `kagSetBreakpoint` | `scene`（必填）+ `cmd`（命令名）或 `line`（token 序号） | 设置场景断点；命中后调度器 yield `__kag_pause`，runner 停止推进 |
| `kagClearBreakpoints` | `scene`（可选；缺省清全部） | 清除断点 |
| `kagDebugContinue` | — | 恢复执行 |
| `kagDebugStep` | — | 单步：下一个 token 前暂停 |
| `kagInspectScopes` | `scope`（`f`/`sf`/`tf`/`mp`/`lf`/`all`，缺省 all） | 返回变量作用域 JSON |
| `kagReloadScene` | `scene`（可选；缺省当前场景） | 场景热重载：重解析 .ks（缓存失效），按内容/最近 label 重映射执行位置，保留游戏状态；非当前场景仅失效缓存 |

示例：

```json
{"jsonrpc":"2.0","id":1,"method":"kagSetBreakpoint","params":{"scene":"assets/script/main.ks","cmd":"ch"}}
{"jsonrpc":"2.0","id":2,"method":"kagDebugContinue"}
{"jsonrpc":"2.0","id":3,"method":"kagInspectScopes","params":{"scope":"f"}}
{"jsonrpc":"2.0","id":4,"method":"kagReloadScene"}
```

Lua 侧 API：`require("kag_debug")`（`set_breakpoint`/`step`/`continue_run`/`inspect`/`serialize_json`）。运行中通过编辑器 `eval` 也可直接调用 `kag_runner.debug_resume()` / `kag_runner.debug_step()`。

---

## 附录 C：LSP 语言服务方法（经 /api/eval 桥接，round 80）

语言服务（`scripts/kag/lsp.lua`，纯 Lua，由声明式命令契约驱动）通过
**POST /api/eval** 暴露给编辑器：编辑器 `lspCall()` 把每次请求包装成一条
`return kag.lsp.json(...)` 的 Lua 片段，引擎求值后返回 JSON 文本字符串，
渲染器解析一次。所有方法都是纯函数（无 ctx / 无 I/O）。

### C.1 方法总览

| 方法（lsp.json 第一个参数） | Lua 签名 | 返回形状 |
|---------------------------|----------|---------|
| `completion` | `lsp.completion(line_text)` | `[{label, kind, detail, insertText}]` |
| `hover` | `lsp.hover(cmd, param?)` | `[{title, text}]`（契约 + 描述） |
| `diagnostics` | `lsp.diagnostics(text)` | `[{line, col, message, severity}]` |
| `definition` | `lsp.definition(text, line, char)` | `[{name, line, col}]`（跨场景目标 name-only） |
| `references` | `lsp.references(text, labelName)` | `[{kind, line, col}]`（definition/reference） |
| `rename` (round 80) | `lsp.rename(text, line, char, newName)` | `{renamed, edits}`（见下） |

### C.2 rename —— 标签重命名（round 80 新增）

光标位于一个 `*label` 上（或在 `[jump]/[call]/[link]/[goto]/[sel]/[select]`
的 `target="*name"` / 裸 `*name` 引用上）时，返回 `{renamed:true, edits:[…]}`，
覆盖该标签的定义点 + 全部导航引用；每个编辑项：

| 编辑字段 | 类型 | 说明 |
|---------|------|------|
| `kind` | string | `"definition"`（定义点）或 `"reference"`（导航引用） |
| `line` | int | 名称字节位置所在行（1-based） |
| `col` | int | 首字符 **紧跟 `*` 之后**的字节偏移（editor 原地套用） |
| `length` | int | 旧名称的字节长度 |
| `newText` | string | 新名称 |

响应三种形态：

| 形态 | 条件 |
|------|------|
| `{"renamed":true,"edits":[ … ]}` | 正常：定义 + N 个引用编辑 |
| `{"renamed":false,"edits":[]}` | 新名称非法被拒（`lsp.rename` 返回 `nil`） |
| `{"renamed":true,"edits":[]}` | 光标处无已知标签 / 跨场景目标无编辑（空编辑） |

请求示例（`/api/eval` 桥，等待返回 JSON 字符串）：

```
→ POST /api/eval
→ return kag.lsp.json("rename", renTxt, 1, 1, "scene")
← {"status":"ok","result":"[{\"renamed\":true,\"edits\":[...]}]"}
```

新名校验：可选前置 `*` 会被剥离；合法名需匹配 `[A-Za-z_][A-Za-z0-9_]*`
（与 tokenizer 标识符同字符集），否则被拒（`renamed:false`）。同名校忽略
（返回空编辑集）。

### C.3 大纲驱动实机跳转（engineJump，round 80）

编辑器面板的 **▶ 执行到标签 / 右键跳转** 通过同一 `/api/eval` 桥实现：
先经 `flow.find_label` 校验标签存在，再调用 `kag.jump` 并设置
调度器接管锚点（`_next_index`），让运行中的调度器在下一 token 处接管
执行——**零引擎改动**。路由片段受沙箱白名单约束（`_G._CAESURA_CTX`
锚点）。SceneOutlinePanel（`editor/`）提供该跳转的 UI 入口。


## F. Project Management (Sprint 2/5c)

### GET /api/project/templates

Returns available project templates.

### GET /api/project/list

Lists managed projects under `./projects/`.

### POST /api/project/create

Create a new project from a template. Body: `{"template":"basic","name":"my_vn"}`

-> `{"ok":true,"path":"projects/my_vn"}`

400 invalid name or unknown template; 409 already exists.

### POST /api/project/duplicate

Duplicate an existing project.

### POST /api/project/import

Register an existing directory as a managed project.

### GET /api/project/meta?path=projects/name

Reads project metadata (infers defaults when caesura.project.json absent).

### POST /api/project/meta

Saves project metadata.

## G. Web Packaging (Sprint 5c)

### POST /api/package/web

One-click web packaging via `scripts/package_game.sh`.

Body: `{"storyPath":"demo/example_game/story.ks","outName":"example_game"}`

-> `{"ok":true,"outputDir":"dist/example_game","logTail":"..."}`

400 = path outside whitelist or invalid outName.
