# Lua Module API — Complete Reference

> C++ binding modules exposed to Lua scripts. All modules are global variables after `Engine::init()`.

---

## Render

```lua
-- Global: Render
-- Backend: BackendRegistry::instance().getRenderDevice()
```

> 阶段 G（round 102）新增 **PostFx 后处理家族 5 API**（set_postfx / destroy_postfx /
> clear_postfx / is_postfx_supported / is_postfx_active，见下方「Blend / Transition / VFX」表）；
> api-stats 记账 Render 共 **38 个绑定 API**（自动生成，勿手改）。

### Texture Management

| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `load_texture` | `(path)` | `int` | Load texture from file. Returns 0 on failure. |
| `destroy_texture` | `(id)` | `bool` | Release texture by ID. No-op for invalid IDs. |
| `create_solid_texture` | `(r, g, b, a)` | `int` | Create 1×1 solid colour texture. Returns TextureManager ID. |

### Text Rendering

| Function | Signature | Description |
|----------|-----------|-------------|
| `text_set_font` | `(face, size, color?)` | Set font face/size; `"default"` resets to the built-in bitmap font |
| `text_reset_state` | `()` | Reset the text renderer's internal line/char state |

> The text entry-points `render_text`, `render_ruby`, `clear_text`, `set_font`,
> and `line_height` live on the **KAG** module (see [KAG section](#kag-c-audio-bindings)),
> not on Render.

### View & Resolution

| Function | Signature | Description |
|----------|-----------|-------------|
| `get_resolution` | `() → w, h` | Get backbuffer width and height |
| `set_view_name` | `(viewId, name)` | Set bgfx view debug marker |
| `set_screen_offset` | `(dx, dy)` | Pan VIEW_MAIN (camera/quakes); fractional values are rounded |
| `create_viewport` | `(w, h) → handle` | Create an RTT viewport (0 on invalid dimensions) |
| `destroy_viewport` | `(handle)` | Destroy an RTT viewport |
| `draw_viewport` | `(handle, x, y, w?, h?)` | Blit an RTT viewport onto VIEW_MAIN |
| `resize` | `(w, h)` | Notify engine of window resize |

### Batch Submission

```lua
Render.submit_batch({
    { tex = id, x = 0, y = 0, w = 1280, h = 720, opacity = 255, view = 1 },
    -- "rt" key supported for RTT viewport handles
    { tex = 0, rt = viewportHandle, x = 0, y = 0, w = 640, h = 360, opacity = 255, view = 1 },
})
```

Each quad entry:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `tex` | int | required | TextureManager ID |
| `rt` | int | 0 | Alternative: RTT ViewportHandle ID |
| `x` | float | 0 | Left position |
| `y` | float | 0 | Top position |
| `w` | float | 128 | Quad width |
| `h` | float | 128 | Quad height |
| `opacity` | int (0–255) | 255 | Opacity |
| `view` | int | 1 (VIEW_MAIN) | Target bgfx view |

### Blend / Transition / VFX

| Function | Signature | Description |
|----------|-----------|-------------|
| `submit_blend` | `(baseTexId, blendTexId, mode, baseAlpha, blendAlpha, globalAlpha)` | Submit blend effect |
| `submit_transition` | `(fromTexId, toTexId, ruleTexId, method, progress)` | Submit transition |
| `submit_vfx` | `(srcTexId, effect, fadeAlpha, r, g, b, blur, quakeX, quakeY)` | Submit VFX |
| `stretch_blt` | `(dstTexId, dx,dy,dw,dh, srcTexId, sx,sy,sw,sh, filter)` | Stretch blit (0=Nearest,1=Linear,2=Aniso) |
| `affine_blt` | `(dstTexId, dx,dy,dw,dh, srcTexId, sx,sy,sw,sh, m0..m5)` | Affine 2×3 matrix blit |
| `fill_viewport` | `(vpId, r, g, b, a)` | Fill RTT with solid colour |
| `set_color_filter` | `(preset)` | Accessibility colour filter: none/deuteranopia/protanopia/tritanopia/grayscale/high_contrast |
| `set_postfx` | `(kind, params)` | Enable/update a PostFx effect. kind: bloom/vignette/lut/softblur; params: {strength, radius, amount, rgb="r,g,b", lutMix} → handle (0 = unsupported/no-op) |
| `destroy_postfx` | `(kind)` | Disable one PostFx effect |
| `clear_postfx` | `()` | Disable the whole PostFx chain (postfx=none) |
| `is_postfx_supported` | `(kind)` | `bool` — headless/Null devices return false |
| `is_postfx_active` | `()` | `bool` — is any PostFx effect active |

### Video Playback

| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `video_play` | `(path)` | `handle` | Open and start playing video |
| `video_stop` | `(handle)` | `bool` | Close video and release resources |
| `video_update` | `(handle)` | `bool` | Decode next frame (manual drive; the engine frame loop already advances all playing videos automatically with frame-rate pacing). |
| `video_get_texture` | `(handle)` | `texId` | Get current frame as texture ID (0=no frame) |
| `video_is_playing` | `(handle)` | `bool` | Is video currently playing |
| `video_has_ended` | `(handle)` | `bool` | Has video reached the end |
| `video_get_size` | `(handle)` | `w, h` | Video dimensions |
| `video_pause` | `(handle)` | `bool` | Pause playback |
| `video_resume` | `(handle)` | `bool` | Resume playback |

### Resource Validation

| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `is_valid_handle` | `(type, id)` | `bool` | Validate resource handle. type: 0=Texture,1=Shader,2=RTT,3=Audio,4=Video,5=Font,6=Model,7=Steam |
| `load_texture_async` | `(path, callback?)` | `int` | Enqueue an async texture load (`<0` on error); optional `(ok, path, texId)` callback |
| `cancel_async_loads` | `()` | `bool` | Cancel all pending async loads |
| `invalidate_handles` | `(type)` | `bool` | Invalidate cached resource generation handles by type |

---

## VFX (Particle System)

```lua
-- Global: VFX
-- Backend: BackendRegistry::instance().getParticleSystem()
```

### Lifecycle

| Function | Signature | Description |
|----------|-----------|-------------|
| `particles_init` | `() → bool` | Initialize particle system |
| `particles_shutdown` | `()` | Shutdown and release all particles |
| `particles_is_initialized` | `() → bool` | Check if system is initialized |
| `particles_clear` | `()` | Destroy all emitters and particles |

### Emitters

| Function | Signature | Description |
|----------|-----------|-------------|
| `particles_create_emitter` | `(cfg) → id` | Create emitter. Returns -1 if parameters invalid. |
| `particles_destroy_emitter` | `(id)` | Destroy emitter by ID |
| `particles_emit` | `(id, count)` | Emit N particles from emitter |

Emitter config table (`cfg`):

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `x` | float | 0 | Spawn X position |
| `y` | float | 0 | Spawn Y position |
| `rate` | float | 10 | Particles per second (0 = manual emit only) |
| `lifeMin` | float | 0.5 | Minimum lifetime in seconds |
| `lifeMax` | float | 2.0 | Maximum lifetime in seconds |
| `speedMin` | float | 10 | Minimum initial speed |
| `speedMax` | float | 50 | Maximum initial speed |
| `angleMin` | float | 0 | Minimum emission angle (radians) |
| `angleMax` | float | 6.283 | Maximum emission angle (radians) |
| `sizeMin` | float | 2 | Minimum particle size |
| `sizeMax` | float | 8 | Maximum particle size |
| `r`, `g`, `b`, `a` | float | 1.0 | Colour channels |
| `gravityX` | float | 0 | Horizontal gravity |
| `gravityY` | float | 0 | Vertical gravity |

### Update & Render

| Function | Signature | Description |
|----------|-----------|-------------|
| `particles_update` | `(dt)` | Advance particle simulation by dt seconds |
| `particles_render` | `()` | Submit particle draw calls to GPU |
| `particles_alive_count` | `() → int` | Total active particle count |

---

## MiniGame (3D)

```lua
-- Global: mini_game
-- Backend: BackendRegistry::instance().getMiniGameBackend()
```

Programmatic 3D scenes: spawn objects/materials/lights from Lua, then
`enter(0)` to activate (renders the spawned object set without a JSON scene).
JSON scenes load via `load_scene(path)` + `enter(handle)`.

### Lifecycle

| Function | Signature | Description |
|----------|-----------|-------------|
| `load_scene` | `(path) → handle` | Load a JSON scene descriptor; 0 on failure |
| `unload_scene` | `(handle)` | Unload a loaded scene |
| `enter` | `(handle)` | Activate a scene; `0` activates spawned objects (programmatic mode) |
| `leave` | `()` | Deactivate the active scene |
| `is_active` | `() → bool` | True while a scene is active |

### Objects

| Function | Signature | Description |
|----------|-----------|-------------|
| `spawn_cube` | `(x, y, z, scale?, r?, g?, b?, matId?) → id` | Spawn a cube |
| `spawn_sphere` | `(x, y, z, scale?, r?, g?, b?, matId?) → id` | Spawn a sphere |
| `spawn_plane` | `(x, y, z, w?, h?, r?, g?, b?, matId?) → id` | Spawn a plane |
| `remove_object` | `(id)` | Remove an object |
| `set_material` | `(objId, matId)` | Assign a material to an object |
| `set_velocity` | `(id, vx, vy, vz)` | Set linear velocity |
| `set_gravity` | `(id, enabled)` | Toggle gravity for an object |
| `set_camera` | `(eyeX, eyeY, eyeZ, atX, atY, atZ)` | Position the camera |

### Materials & Lighting

| Function | Signature | Description |
|----------|-----------|-------------|
| `create_material` | `(r, g, b, rough?, metal?, spec?, name?) → id` | Create a PBR material |
| `set_ambient` | `(r, g, b)` | Ambient light color |
| `set_directional` | `(x, y, z, r?, g?, b?, intensity?)` | Directional light |
| `add_point_light` | `(x, y, z, r?, g?, b?, intensity?, range?, name?) → id` | Add a point light |
| `remove_light` | `(id)` | Remove a point light |

### Physics

| Function | Signature | Description |
|----------|-----------|-------------|
| `check_collision` | `(idA, idB) → bool` | Test object pair collision |
| `set_collision` | `(enabled)` | Toggle the collision system |

## Debug

```lua
-- Global: Debug
-- Backend: BackendRegistry::instance().getDebugManager()
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `log` | `(level, message)` | Write to engine log (log_level + message) |
| `get_last_error` | `() → string` | Most recent engine error |
| `get_error_count` | `() → int` | Total error count |
| `get_subsystem_stats` | `(subsystem)` | Subsystem error/stat counters |
| `dump_report` | `()` | Dump a structured error/state report |
| `get_render_info` | `()` | Render backend diagnostics |
| `get_audio_info` | `()` | Audio backend diagnostics |
| `get_input_info` | `()` | Input backend diagnostics |
| `get_log_path` | `() → string` | Path of the engine log file |
| `get_stats` | `()` | Aggregate runtime stats |

---

## DevCore

```lua
-- Global: DevCore
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `set_input_focus` | `(bool)` | Route input to KAG vs. game focus |
| `get_input_focus` | `() → bool` | Whether input focus is on the game |
| `log` | `(message)` | Write a message to the engine log |
| `quit` | `()` | Request the engine to quit |
| `set_resolution` | `(w, h)` | Set the rendering resolution |
| `get_resolution` | `() → w, h` | Get the current resolution |
| `set_fullscreen` | `(bool)` | Toggle fullscreen |
| `get_window_size` | `() → w, h` | Get the OS window size |

---

## KAG (C++ Audio Bindings)

> ⚠️ 阶段 G 的 `[tween]` / `[layout]` / `[vfx postfx=` 是**纯 Lua 命令handler**
> （scripts/kag/commands/tween.lua / layout.lua / vfx.lua + scripts/kag/layout_math.lua），
> 不新增 C++ 绑定——它们经 KAG 调度器执行并写 `layers.move_layer` / `ctx.tweens`。
> 其契约（参数/钳制/枚举）见 `docs/api/command-contracts.md`（123 命令，自动生成）；
> 本文件只记录 C++ 侧绑定（`Render.set_postfx` 家族属于此列）。


```lua
-- Global: KAG
-- Direct C++ bindings for audio operations called by KAG command handlers
```

| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `play_bgm` | `(file, volume?, loop?)` | `bool` | Play background music |
| `stop_bgm` | `(fadeTime?)` | `bool` | Stop BGM with optional fade (ms) |
| `play_se` | `(file, volume?)` | `bool` | Play sound effect |
| `stop_se` | `()` | `bool` | Stop all sound effects |
| `play_se_3d` | `(file, x, y, z?)` | `bool` | Play a positional sound effect |
| `is_se_playing` | `()` | `bool` | Whether the SE bus is active |
| `audio_fade_volume` | `(bus, target, seconds)` | `bool` | Smooth volume change (bus: bgm/se/voice) |
| `audio_get_length` | `(bus)` | `number` | Current track length (seconds) |
| `audio_get_position` | `(bus)` | `number` | Current playback position (seconds) |
| `clear_screen` | `()` | `bool` | Clear the screen layers |
| `clear_text_layer` | `()` | — | Clear the text layer (delegates to clear_text) |
| `flush_wave_cache` | `()` | `bool` | Flush the decoded-wave cache |
| `quake` | `(duration_ms, amplitude?)` | `bool` | Screen shake (KAG3 classic) |
| `render_ruby` | `(text, ruby, x, y)` | `bool` | Furigana annotation for the current text line |
| `play_voice` | `(file, volume?)` | `bool` | Play voice line |
| `stop_voice` | `()` | `bool` | Stop current voice |
| `set_global_volume` | `(vol)` | — | Master volume 0.0–1.0 |
| `get_global_volume` | `() → number` | — | Current master volume |
| `set_bus_volume` | `(bus, vol)` | — | Set bus volume: "bgm"/"voice"/"se" |
| `get_bus_volume` | `(bus) → number` | — | Get bus volume |
| `render_text` | `(text, x, y, scale, r, g, b, a)` | — | Render text |
| `clear_text` | `()` | — | Clear text layer |
| `set_font` | `(face, size, color?)` | — | Set font face/size; `"default"` resets to the built-in bitmap font |
| `line_height` | `() → number` | — | Current line height |
| `is_bgm_playing` | `() → bool` | — | Is BGM playing |
| `is_voice_playing` | `() → bool` | — | Is voice playing |
| `get_active_voices` | `() → int` | — | Active voice count |
| `replay_voice` | `()` | `bool` | Replay the current voice line |
| `set_bgm_volume` | `(vol)` | — | Set BGM bus volume |
| `set_se_volume` | `(vol)` | — | Set SE bus volume |
| `set_voice_volume` | `(vol)` | — | Set Voice bus volume |
| `show_text` | `(text)` | — | Show text line |
| `show_image` | `(path, ...)` | — | Show an image onto a layer |
| `wait_click` | `()` | — | Wait for a click |
| `set_listener` | `(...)` | — | Set an audio listener |
| `log` | `(message)` | — | Write to engine log |

---


## Gesture hooks & overlay pump (native SwipeDown/SwipeUp consumers)

Native touch gestures map to synthetic keyboard events on the engine side:
`MobileAdapter` converts SwipeDown to `SDLK_SPACE` and SwipeUp to
`SDLK_PAGEUP`, and the Engine.cpp key-down handler routes them to the Lua
hooks below (same guard style as `_KAG_onCtrlDown`). The web player layer
serves the same gestures to every game (web/main.mjs); the runtime default
gives native games the same parity.

### Global hooks: `_KAG_onKeySpace` / `_KAG_onKeyPageUp`

- `_G._KAG_onKeySpace` — SwipeDown / `SDLK_SPACE`: toggle the `message`
  layer visibility (mirrors the web gesture hiding/toggling the dialogue
  box). Headless-safe: no ctx / no message layer / layers module absent →
  no-op.
- `_G._KAG_onKeyPageUp` — SwipeUp / `SDLK_PAGEUP`: open the
  backlog/history overlay (`input_focus ~= "history"` guard + single-flight
  coroutine + `kag.commands.system.history`). Headless-safe: no ctx →
  no-op.
- **First-definition-wins override rule**: the runtime installs the defaults
  with `_G._KAG_onKeySpace = _G._KAG_onKeySpace or <default>` (and the same
  for `_KAG_onKeyPageUp`) at kag.lua setup time; an entry that defines the
  same globals AFTER `require("kag")` overrides them (no init-registration
  needed). Install site: `scripts/kag.lua` (setup tail, before `return KAG`).

### `KAG.gesture_defaults` (public export)

`KAG.gesture_defaults = { space = ..., page_up = ... }` (`scripts/kag.lua`)
are the canonical runtime defaults — exported so tests and future entries
reference the same functions instead of replicating them. Entries override
the GLOBALS after `require("kag")` (first-definition-wins); both defaults
are headless-safe; `page_up` state lives on `ctx._gesture_history_co`
(single-flight, per-context) and the overlay coroutine needs a frame driver
(see `kag_runner.pump_gesture_overlay` below).

### `ctx._gesture_history_co` slot

The default `page_up` hook parks its single-flight overlay coroutine on
`ctx._gesture_history_co` (per-context). The runtime pump (below) drives it
every frame; entries that override the hook never set the slot → zero
overhead.

### `kag_runner.pump_gesture_overlay(ctx)`

Runtime overlay pump (`scripts/kag_runner.lua`, exported for the lua suite
so the dead/error/no-op paths are directly drivable):

- no coroutine in `ctx._gesture_history_co` → no-op;
- coroutine `dead` → clear the slot;
- resume error → print `[History] overlay error: ...`, clear the slot,
  restore `ctx.input_focus = "kag"` and call `history_ui._hideAll(ctx)`
  (only the ERROR path forces focus back; normal close restores it inside
  the history command itself).

### Override example: demo entry

Entries with their own pumps override the hooks and never set the gesture
slot (zero overhead). The reference override is `scripts/kag_demo_entry.lua`
(`_KAG_onKeySpace` / `_KAG_onKeyPageUp` + its `engine_update` drains its own
`history_co`). The demo keeps the override deliberately: its per-frame
driver owns its own coroutine — dropping it would route PAGEUP to the
runtime coroutine, which nothing drives in that entry (overlay freeze with
`input_focus` stuck on "history").


## Restore

`Restore` is an internal runtime bridge used by `kag.presentation` and the save/load transaction. It is separate from the low-level `KAG.save_game` storage API. Scripts should normally use KAG `[save]` / `[load]` rather than applying individual presentation resources.

| Function | Signature | Contract |
|---|---|---|
| `prepare_image` | `(assetPath) → ticket` | Read and decode an independent image without changing the active scene. |
| `prepare_color` | `(r, g, b, a) → ticket` | Prepare a 1×1 RGBA colour; components are integers 0–255. |
| `image_info` | `(ticket) → width, height` | Query an unconsumed image preparation. |
| `materialize_image` | `(ticket) → textureId` | Consume once and create an owned texture; failure also consumes the ticket. |
| `discard_image` | `(ticket)` | Release preparation; repeated discard is harmless. |
| `describe_texture` | `(textureId) → description` | Return a portable asset or colour description, excluding GPU handles. |
| `capture_audio` | `() → snapshot` | Capture story-owned BGM state; `bgm=false` represents silence. |
| `prepare_audio` | `(snapshot) → ticket` | Read/decode independently; do not start a voice. |
| `apply_audio` | `(ticket) → boolean` | Consume preparation, stop voice/SE, resume BGM; failure leaves session audio stopped. |
| `discard_audio` | `(ticket)` | Release unconsumed preparation. |
| `stop_audio` | `() → boolean` | Stop story audio without changing user volume preferences. |
| `capture_font` / `default_font` | `() → snapshot` | Describe current / startup font state. |
| `prepare_font` | `(snapshot) → ticket` | Own font bytes and prepare glyph data without changing the current font. |
| `apply_font` | `(ticket) → boolean` | Consume once and install the prepared font. |
| `discard_font` / `clear_font` | `(ticket)` / `()` | Release preparation / clear the active font. |
| `capture_transients` | `() → counts` | Return nonnegative integer `videos`, `particles`, `emitters`, `models`, `postfx` counts. |
| `stop_transients` | `() → boolean` | Attempt cleanup of every available transient backend; report the first failure. |

Native preparation/query failures return `nil, error`; boolean operations return `false, error`. Prepared values are opaque and cannot be saved or reused after consumption. Web preparations may await fetch/decode through the Lua bridge; the runner verifies that their originating session is still current before committing.

Audio snapshots use `{version=1, bgm=false}` or a BGM description containing `path`, `position`, `gain`, `looping` and an optional native `cursor`. Font snapshots use `{version=1, active=false}` or `active=true` with `font`, `path`, `size`; bitmap fonts use IDs 0/1 and sizes 16/32, TTF uses ID 2 with a portable resource path.

### KAG save state contract

The current KAG payload version is **3**, inside the native storage envelope version **5**. These versions describe different layers. Old release samples and their producer hashes are recorded in `tests/golden_saves/README.md`; an artificial legacy fixture is not evidence of an old executable's output.

- Slot-owned values include exact `f`, `sf`, `lf`, `mp`, variables, unlock/seen state, call/loop/conditional frames, continuation cursor, language, layers, text, font and BGM. Loading removes future keys absent from the slot. `tf` is rebuilt; `sf.save_list` is derived and excluded.
- User preferences such as master/bus volumes and favourites remain outside the slot. Voice/SE and unsupported transient effects stop during commit.
- Scenes and caller frames are reconstructed from trusted current source; saved tokens are never executed. Ordinary call/for/if frames and stable text/wait boundaries are supported. Saved wait/delay state retains the remaining time; old slots without this field restart the wait duration.
- Saving during a choice, input dialog, menu, history view, active video/particle/model/postfx, unfinished tween or other active scene operation is rejected with a reason. Pending screenshot leases from other saves do not represent scene effects; different slots may wait independently, while a second pending save to the same owner/slot returns `save-busy`. Macro definitions and streams rewritten by runtime macro expansion are also rejected. Live coroutines and native handles are never serialized.
- Parse, migration, validation and resource preparation precede the commit point. Failure before commit preserves the old session. A serious failure after commit stops the candidate, cancels its operations and releases partial resources; a new session may then be started.
- Values must be finite and serializable: dense arrays or string-keyed maps, valid UTF-8 strings (including embedded NUL), bounded depth/node count. Sparse/mixed-key tables, unsupported values and invalid numbers are rejected without overwriting an existing slot.

## Save (registered on the KAG module)

```lua
-- No separate `Save` global; the save/load bindings are registered onto
-- the KAG module (KAG.save_game, KAG.load_game, ...).
-- Backend: BackendRegistry::instance().getSaveManager()
-- Thumbnail capture: BackendRegistry::instance().getRenderDevice()
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `save_game` | `(slot, data, sceneName, tokenIndex, thumbnail?) → bool, error?` | Synchronously commit state plus the supplied thumbnail text; this function does not capture an image |
| `load_game` | `(slot) → table` | Load game state from slot |
| `list_saves` | `() → table` | List all save slots with metadata |
| `delete_save` | `(slot)` | Delete save slot |
| `save_exists` | `(slot) → bool` | Whether a slot has a save |
| `get_save_dir` | `() → string` | Directory where saves are written |
| `set_encryption_key` | `(key)` | Set/derive the save encryption key |
| `clear_encryption_key` | `()` | Clear the save encryption key |
| `capture_thumbnail` | `(cancelToken?) → base64OrNil, status, errorOrNil` | Request a 320×180 PNG and yield until this call's renderer ticket reaches a terminal state |
| `configure_cloud` | `(endpoint) → bool` | Configure HTTP cloud-save endpoint ("" = local only); offline-safe |
| `cloud_push` | `(slot) → bool` | Push slot file to the cloud |
| `cloud_pull` | `(slot) → bool` | Pull slot file from the cloud |

`KAG.capture_thumbnail()` returns **raw Base64 text**, without a `data:image/png;base64,` prefix. The native renderer resizes to 320×180 using nearest-neighbor stretch. An optional token is an existing `kag.cancel_token` instance; no separate Lua request/poll API is required. Status values are:

| Status | First return value | Meaning |
|--------|--------------------|---------|
| `completed` | Base64 string | This request's PNG was consumed successfully; `errorOrNil` is nil |
| `failed` | nil | An admitted capture or native conversion failed; the third value gives the reason |
| `cancelled` | nil | The operation, device generation or renderer owner expired; callers must not turn this into a late save |
| `unavailable` | nil | No renderer, unsupported/not-ready capture, rejected admission, or `not-yieldable` calling context |

Pending capture stays inside the same C binding call using a Lua 5.4 continuation and yields **zero values**; it never returns `nil, 'pending'`. Each resume checks the same ticket and ignores extra resume arguments. A to-be-closed userdata owns the request and native buffers. Token cancellation, coroutine close and GC fallback share idempotent cleanup; cleanup cancels then takes/discards that ticket's terminal even if completion won the race. Owners must close abandoned or errored coroutines; the KAG runner and native managed-run host do so before releasing their references. The binding does not advance frames. A nonyieldable caller consumes an already available result or retires a still-pending request and returns `nil, 'unavailable', 'not-yieldable'`.

The `[save]` handler freezes slot, copied state, scene, token index and description before awaiting a thumbnail. It uses `kag.operation`, checks cancellation/session ownership again before the single `save_game` commit, and releases its owner/slot reservation on completion or close. An explicit nonempty `thumbnail` skips capture. Otherwise `ctx.captureThumbnail(token)` remains preferred over the native binding. An ordinary optional-image failure can still produce a state-only save; `tf.thumbnail_result` / `tf.thumbnail_error` describe that outcome separately from `tf.save_result` / `tf.save_error`. Cancellation, stop, reload or owner replacement prevents the pending save from writing. SaveManager itself owns no GPU state and never reads a previous request's screenshot file.

After normal scene completion, the host may start a **new** save from the runner's retained final context. The runner grants this eligibility only after successful retirement, and must confirm that it still owns that context, has no scheduler coroutine or continuation, and is not changing sessions. Eligibility is cleared before a transition or failed cleanup. This does not reactivate the old session or rescue a capture that was already pending when its owner expired. Replacement cancels even a newly started asynchronous final-state save; stale contexts and cleanup/reload reentry remain rejected.

Sources: [SaveBinding.cpp](../../src/script/bindings/SaveBinding.cpp), [save.lua](../../scripts/kag/commands/save.lua), and [IRenderDevice screenshot contract](cpp-interfaces.md#111-irenderdevice).

---

## Steam (unconditionally registered; safe Null defaults without the SDK)

```lua
-- Global: steam
-- Always present. Without the Steam SDK every call returns a safe
-- default (false / 0 / "" / {}) instead of a nil-function error.
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `unlock_achievement` | `(id)` | Unlock achievement by ID |
| `is_achievement_unlocked` | `(id) → bool` | Whether an achievement is unlocked |
| `reset_achievement` | `(id)` | Reset a single achievement |
| `reset_all_achievements` | `()` | Reset all achievements |
| `set_stat_int` | `(name, value)` | Set an integer statistic |
| `get_stat_int` | `(name) → int` | Get an integer statistic |
| `set_stat_float` | `(name, value)` | Set a float statistic |
| `get_stat_float` | `(name) → float` | Get a float statistic |
| `store_stats` | `()` | Commit statistic changes to Steam |
| `is_overlay_active` | `() → bool` | Whether the Steam overlay is active |
| `cloud_write` | `(name, data) → bool` | Write a cloud-save file (Remote Storage) |
| `cloud_read` | `(name) → string/nil` | Read a cloud file; nil when missing |
| `cloud_file_size` | `(name) → int` | Cloud file size in bytes |
| `cloud_file_exists` | `(name) → bool` | Cloud file existence |
| `cloud_delete` | `(name) → bool` | Delete a cloud file |
| `cloud_quota_total` | `() → int` | Total cloud quota bytes |
| `cloud_quota_used` | `() → int` | Used cloud bytes |
| `cloud_list` | `() → table` | Cloud file name list (up to 256) |
---

## AI (LLM query binding)

```lua
-- Global: AI
-- Backend: BackendRegistry::instance().getJobSystem() (async replies on owner thread)
-- Config: config.ai.{ endpoint, model, api_key, system, timeout_ms }
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `available` | `() → bool` | True when an AI endpoint is configured (`config.ai.endpoint` non-empty) |
| `query` | `(prompt, opts?) → string` | Blocking LLM request. `opts` (optional table) may override `endpoint`/`model`/`system`/`timeout_ms`. Synchronous variant. |
| `query_async` | `(prompt, opts?, callback) → bool` | Async LLM request; fires `callback(text, err)` on the main thread. Callback goes at arg 3, or arg 2 when `opts` is omitted. |
| `cancel` | `()` | Cancel all pending AI requests (epoch watermark); dropped callbacks never fire |

---

## sma (Skeletal Mesh Animation)

```lua
-- Global: sma
-- Backend: BackendRegistry::instance().getMeshRenderer()
-- Thin mesh upload/draw surface; skeleton/hierarchy/animation logic lives in scripts/kag/sma.lua
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `create_mesh` | `(verts, indices) → handle` | Upload a mesh. `verts` = array of `{x, y, u, v, bone0, w0, bone1?, w1?}`; `indices` = array of numbers (count multiple of 3). Returns 0 on failure. |
| `destroy_mesh` | `(handle)` | Release a mesh |
| `update_mesh` | `(handle, poses)` | Re-skin by bone poses: `poses` = array of `{rot, scale, ox, oy}` |
| `draw_mesh` | `(handle, view?, texId?, x?, y?, scale?, opacity?)` | Draw a skinned mesh to a view |
| `count` | `() → int` | Number of live meshes |
| `initialized` | `() → bool` | Renderer is ready |
| `set_skin_mode` | `(mode)` | Skinning: `"auto"` (default) / `"cpu"` / `"gpu"` |
| `get_skin_mode` | `() → string` | Current skinning mode (`auto`/`cpu`/`gpu`) |

---

## Engine (Backend selection)

```lua
-- Global: Engine
-- Runtime backend creation/selection (composition-root surface for scripts/tests)
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `select_render_backend` | `(name, subBackend?) → bool` | Create and select a render backend; optional preferred sub-backend. `nil + err` on unknown backend. |
| `select_audio_backend` | `(name) → bool` | Create and select an audio backend. `nil + err` on unknown backend. |
| `select_platform_backend` | `(name) → bool` | Create and select a platform backend. `nil + err` on unknown backend. |
| `get_backend_info` | `() → table` | Current backend names: `{ render = ..., audio = ..., platform = ... }` |

---

## i18n (Lua runtime localization module)

> Pure-Lua runtime module (`scripts/i18n.lua`), not a C++ binding. Exposed
> as the global `i18n` table like `scheduler`. Handles multi-language
> string tables (`assets/lang/<code>.lua`) and runtime template
> interpolation. Fallback chain: current language → default language → raw key.
> Plural support (round 80): a dictionary value may be a CLDR-style plural
> variant table (`{ one = "...", other = "..." }`); `translate(text, { n = … })`
> selects the variant for the count's `plural_category` and interpolates `{n}`
> with the literal number (see `plural_category` / `_plural_form` below).

| Function | Signature | Description |
|----------|-----------|-------------|
| `current_language` | `() → string` | Return the currently selected language code (`i18n.current`). Pairs with `set_language()`. |
| `set_language` | `(code, opts) → strings` | Select a per-language dictionary with fallback chain current → default → raw key. Loads `assets/lang/<code>.lua`, sets `i18n.current`, returns the active strings table. `opts.default` overrides the fallback default (updates `i18n.default_language`); `opts.reload=true` forces a re-read even when `code` equals the current language. |
| `translate` | `(text, params) → string` | Runtime template interpolation: resolve the template through the normal localization path, then fill `{name}` placeholders from `params`. Unknown placeholders and inline-markup tags are left intact. With no `params` behaves like `localize()`. Plural + numeric format (round 80): a string-table value may be a plural variant table (`items = { one = "{n} item", other = "{n} items" }`); with `params.n` the variant for that count's `plural_category` is picked and its `{n}` is interpolated to the literal number, without `params.n` the generic (`other`) form is resolved. Example: `i18n.translate("Hello, {name}!", { name = "Caesura" })` → `"Hello, Caesura!"`; `i18n.translate("items", { n = 1 })` → `"1 item"` (en one). |
| `plural_category` | `(count) → string` | Return the CLDR plural category for a count in the current language: `en` → `"one"` when count == 1 else `"other"`; `zh`/`ja` (and any unknown language) always → `"other"` (no singular/plural distinction). |
| `_plural_form` | `(entry, count?) → string` | Resolve a dictionary value that is a plain string (returned unchanged) or a plural variant table: picks the variant for `count`'s category, falling back to `other` then `one`; with `count` nil uses the generic `other` form (safe for `t()`/`expand()` with no `{n}`). |
| `t` | `(key) → string` | Plain lookup of `key` in the current strings table, falling back to the default language then returning the raw key. A plural-variant value resolves to its generic (`other`) form so a plain lookup never leaks a raw table. |
| `expand` | `(text) → string` | Replace `{key}` tokens in `text` with translations (`i18n.t`). Unknown keys keep their braced form; inline markup tag names (`{b}/{i}/{s}/{color}/{size}`) are whitelisted and never treated as keys. |
| `localize` | `(text, scene?) → string` | Localize dialogue text: per-line translation first (`lines["<scene>:<fnv1a(text)>"]`, Ren'Py-style content-addressed key), else `{key}` token expansion (`expand`), else the original text. Applied by the KAG text pipeline before markup parsing. |
| `available` | `() → table` | List available language codes by scanning `assets/lang/` for `.lua` files. |
| `load` | `(langCode) → strings` | Load a language dictionary from `assets/lang/<code>.lua` into `i18n.strings`; falls back to a built-in table when the file is missing. Sets `i18n.current`. |
| `reload` | `(langCode) → strings` | Hot-reload a language dictionary from disk (re-reads `assets/lang/<code>.lua` even if already current); preserves `i18n.current`, `i18n.default_language` and the cached fallback. |
| `default_language` | field (`string`) | Fallback dictionary language (default `"en"`); configurable via `set_language`'s `opts.default`. |
| `fnv1a` | `(text) → number` | FNV-1a 32-bit hash, used to build content-addressed per-line translation keys. |
| `current` | field (`string`) | Currently selected language code (set by `load`/`set_language`). |
| `strings` | field (`table`) | Active language strings table (current dictionary). |
| `fallback` | field (`table`) | Default-language dictionary used as the second fallback rung. |
