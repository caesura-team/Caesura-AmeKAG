# KAG Neo-Genesis Command Contracts (auto-generated)

> Generated from the declarative schema registry (`kag/schema.lua`) — do not edit.
> Regenerate: `lua scripts/schema_doc.lua > docs/api/command-contracts.md`

> Explicit canonical parameters take precedence over aliases; aliases resolve before defaults and use the same type/range validation.

## Commands (145)

### `[add]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible add: var += value_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `name` | string | - | - | yes | - |
| `value` | number | - | - | yes | - |

### `[ai_dialog]`

_Category: system · Blocking: yes (waits for completion) · AI-driven dialogue line (LLM, async)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `fallback` | string |  | - | - | - |
| `max_wait_ms` | number | 15000 | 100..120000 | - | - |
| `model` | string |  | - | - | - |
| `name` | string |  | - | - | - |
| `prompt` | string | - | - | yes | - |
| `system` | string |  | - | - | - |

### `[assert]`

_Category: system · Blocking: no (fire-and-forget) · development-time assertion on an expression_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `exp` | string | - | - | yes | - |
| `msg` | string | - | - | - | - |

### `[auto]`

_Category: text · Blocking: no (fire-and-forget) · KAG3-compatible auto command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `mode` | string | - | off,on,toggle | - | - |

### `[bg]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible bg command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `file` | file | - | - | - | - |
| `layer` | string | bg | - | - | - |
| `path` | string | - | - | - | - |
| `storage` | file | - | - | - | - |

### `[bgm]`

_Category: audio · Blocking: no (fire-and-forget) · play BGM (KAG3 alternate for [play bus=bgm])_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `file` | string | - | - | - | - |
| `storage` | string | - | - | - | - |
| `volume` | number | - | 0..1.5 | - | - |

### `[blur]`

_Category: transition · Blocking: yes (waits for completion) · gaussian blur the whole frame_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `amount` | number | 0 | 0..100 | - | - |
| `duration` | number | 300 | 0..30000 | - | - |
| `time` | number | 300 | 0..30000 | - | - |

### `[br]`

_Category: text · Blocking: no (fire-and-forget) · KAG3 line-break alias_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[button]`

_Category: text · Blocking: no (fire-and-forget) · register a choice button label ([endbutton] draws it)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `caption` | string | - | - | - | - |
| `cond` | string | - | - | - | - |
| `target` | string | - | - | - | - |
| `text` | string |  | - | - | caption |
| `x` | string | - | - | - | - |

### `[camera]`

_Category: transition · Blocking: no (fire-and-forget) · KAG3-compatible camera command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `restore` | boolean | true | - | - | - |
| `time` | number | 500 | 0..30000 | - | - |
| `x` | number | 0 | 0..2000 | - | - |
| `y` | number | 0 | 0..2000 | - | - |

### `[cancel]`

_Category: system · Blocking: no (fire-and-forget) · cancel current voice/transition (KAG3 compat)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `all` | boolean | false | - | - | - |
| `layer` | string |  | - | - | - |

### `[ch]`

_Category: text · Blocking: yes (waits for completion) · KAG3-compatible ch command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `chars_per_line` | number | 0 | 0..512 | - | - |
| `max_width` | number | 0 | 0..4096 | - | - |
| `name` | string |  | - | - | character |
| `sprite` | string | - | - | - | - |
| `text` | string |  | - | - | message |
| `voice` | string |  | - | - | voicefile |

### `[chapter]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible chapter command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `id` | string | - | - | - | - |
| `label` | string | - | - | - | - |

### `[cl]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible cl command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `layer` | string | all | - | - | - |

### `[close]`

_Category: system · Blocking: no (fire-and-forget) · close active scene, return to menu (KAG3 compat)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[cps]`

_Category: text · Blocking: no (fire-and-forget) · KAG3 [textspeed] alias: chars per second ([cps 50])_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `cps` | number | 50 | 1..120 | - | - |

### `[csd]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible csd command: hide/remove a character on a layer_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `layer` | string | 0 | - | - | - |
| `name` | string | - | - | yes | - |

### `[csl]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible csl command: move a character layer (no visibility change)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `layer` | string | 0 | - | - | - |
| `name` | string | - | - | yes | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[csp]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible csp command: show a character image on a layer (default assets/char/<name>.png at 0,0)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `file` | file | - | - | - | - |
| `layer` | string | 0 | - | - | - |
| `name` | string | - | - | yes | - |
| `path` | string | - | - | - | - |
| `storage` | file | - | - | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[dec]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible dec (inc twin): var -= amount (default 1)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `amount` | number | 1 | - | - | - |
| `name` | string | - | - | yes | - |

### `[delay]`

_Category: system · Blocking: yes (waits for completion) · KAG3-compatible delay command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `duration` | number | - | 0..60000 | - | - |
| `ms` | number | - | 0..60000 | - | - |
| `time` | number | 1000 | 0..60000 | - | - |

### `[div]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible div: var /= value_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `name` | string | - | - | yes | - |
| `value` | number | - | - | yes | - |

### `[edit]`

_Category: text · Blocking: yes (waits for completion) · KAG3 alias of [input]_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `bg_color` | string | #202020 | - | - | - |
| `btn_cancel` | string |  | - | - | - |
| `btn_ok` | string | OK | - | - | - |
| `color` | string | #ffffff | - | - | - |
| `cond` | string | - | - | - | - |
| `default` | string |  | - | - | - |
| `font_size` | number | 28 | 12..72 | - | - |
| `height` | number | 180 | 60..1080 | - | - |
| `max_length` | number | 32 | 1..512 | - | - |
| `maxlen` | number | - | 1..512 | - | - |
| `name` | string | - | - | yes | - |
| `password` | boolean | false | - | - | - |
| `prompt` | string |  | - | - | - |
| `width` | number | 640 | 120..1920 | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[emb]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible emb command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `code` | string | - | - | - | - |
| `exp` | string | - | - | - | - |

### `[endbutton]`

_Category: text · Blocking: yes (waits for completion) · draw the registered choice buttons and wait for a pick_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `target` | string | - | - | - | - |

### `[ending]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible ending command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `id` | string | - | - | - | - |
| `name` | string | - | - | - | - |

### `[endselect]`

_Category: text · Blocking: yes (waits for completion) · KAG3 alias of [endbutton]_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[er]`

_Category: text · Blocking: no (fire-and-forget) · erase line_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[eval]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible eval command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `code` | string | - | - | - | - |
| `exp` | string | - | - | - | - |

### `[fade]`

_Category: transition · Blocking: yes (waits for completion) · fade a layer between from/to opacity_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `duration` | number | 500 | 0..30000 | - | - |
| `from` | number | 0 | 0..255 | - | - |
| `layer` | string | fg | - | - | - |
| `time` | number | 500 | 0..30000 | - | duration |
| `to` | number | 255 | 0..255 | - | - |

### `[fadebgm]`

_Category: audio · Blocking: yes (waits for completion) · KAG3-compatible fadebgm command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `fadein` | number | 0 | 0..30000 | - | - |
| `time` | number | 1000 | 0..30000 | - | - |
| `volume` | number | 0 | 0..1.5 | - | - |

### `[fadeout]`

_Category: layer · Blocking: yes (waits for completion) · KAG3-compatible fadeout command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `alpha` | number | 0 | 0..1.0 | - | - |
| `duration` | number | 500 | 0..30000 | - | - |
| `layer` | string | bg | - | - | name |
| `opacity` | number | 0 | 0..1.0 | - | alpha |
| `time` | number | 500 | 0..30000 | - | duration |

### `[fadevol]`

_Category: audio · Blocking: yes (waits for completion) · KAG3-compatible fadevol command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `time` | number | 1000 | 0..30000 | - | - |
| `volume` | number | 1.0 | 0..1.5 | - | - |

### `[fg]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible fg command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `clear` | boolean | false | - | - | - |
| `file` | file | - | - | - | - |
| `layer` | string | fg | - | - | - |
| `path` | string | - | - | - | - |
| `storage` | file | - | - | - | - |

### `[flash]`

_Category: vfx · Blocking: yes (waits for completion) · KAG3-compatible flash command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `b` | number | 255 | 0..255 | - | - |
| `g` | number | 255 | 0..255 | - | - |
| `r` | number | 255 | 0..255 | - | - |
| `time` | number | 200 | 0..10000 | - | - |

### `[font]`

_Category: text · Blocking: no (fire-and-forget) · KAG3-compatible font command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `color` | string | white | - | - | - |
| `face` | string | default | - | - | - |
| `size` | number | 22 | 4..256 | - | - |

### `[gallery]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible gallery command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `id` | string | - | - | - | - |

### `[history]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible history command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[hr]`

_Category: text · Blocking: no (fire-and-forget) · horizontal rule_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[i18n]`

_Category: system · Blocking: no (fire-and-forget) · hot-switch the UI language mid-scene (language=xx)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `language` | string | - | - | yes | - |

### `[image]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible image command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `file` | file | - | - | - | - |
| `h` | number | - | 0..8192 | - | - |
| `layer` | string | fg | - | - | - |
| `storage` | file | - | - | - | - |
| `w` | number | - | 0..8192 | - | - |
| `x` | number | - | - | - | - |
| `y` | number | - | - | - | - |

### `[inc]`

_Category: system · Blocking: no (fire-and-forget) · increment a numeric variable (by default 1)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `by` | number | 1 | - | - | - |
| `var` | string | - | - | yes | - |

### `[input]`

_Category: text · Blocking: yes (waits for completion) · prompt user for text input via virtual keyboard / IME and store to variable_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `bg_color` | string | #202020 | - | - | - |
| `btn_cancel` | string |  | - | - | - |
| `btn_ok` | string | OK | - | - | - |
| `color` | string | #ffffff | - | - | - |
| `cond` | string | - | - | - | - |
| `default` | string |  | - | - | - |
| `font_size` | number | 28 | 12..72 | - | - |
| `height` | number | 180 | 60..1080 | - | - |
| `max_length` | number | 32 | 1..512 | - | - |
| `maxlen` | number | - | 1..512 | - | - |
| `name` | string | - | - | yes | - |
| `password` | boolean | false | - | - | - |
| `prompt` | string |  | - | - | - |
| `width` | number | 640 | 120..1920 | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[l]`

_Category: text · Blocking: no (fire-and-forget) · line break_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[layfade]`

_Category: layer · Blocking: yes (waits for completion) · fade one layer to an opacity_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `alpha` | number | - | 0..255 | - | - |
| `duration` | number | 300 | 0..30000 | - | - |
| `layer` | string | bg | - | - | name |
| `name` | string | - | - | - | - |
| `opacity` | number | - | 0..255 | - | - |
| `time` | number | 300 | 0..30000 | - | duration |
| `to` | number | 255 | 0..255 | - | - |

### `[layopt]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible layopt command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `layer` | string |  | - | - | - |
| `opacity` | number | 1.0 | 0..1.0 | - | - |
| `visible` | boolean | true | - | - | - |

### `[layout]`

_Category: layer · Blocking: no (fire-and-forget) · declare an hbox/vbox/grid container that computes child x/y (calculator, not a render layer)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `align` | string | start | - | - | - |
| `cols` | number | - | 1..128 | - | - |
| `gap` | number | 0 | 0..8192 | - | - |
| `h` | number | - | 0..8192 | - | - |
| `kind` | enum | - | hbox, vbox, grid | yes | - |
| `layer` | string | - | - | - | - |
| `name` | string | - | - | yes | - |
| `padding` | number | 0 | 0..8192 | - | - |
| `paddingX` | number | - | 0..8192 | - | - |
| `paddingY` | number | - | 0..8192 | - | - |
| `w` | number | - | 0..8192 | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[layout_place]`

_Category: layer · Blocking: no (fire-and-forget) · place an element layer at an absolute offset inside a [layout] container frame_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `h` | number | - | 0..8192 | - | - |
| `layer` | string | - | - | yes | - |
| `parent` | string | - | - | yes | - |
| `w` | number | - | 0..8192 | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[layout_slot]`

_Category: layer · Blocking: no (fire-and-forget) · register an element layer into a slot of a declared [layout] container_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `index` | number | - | 1..1024 | - | - |
| `layer` | string | - | - | yes | - |
| `parent` | string | - | - | yes | - |
| `size` | string | - | - | - | - |

### `[ld]`

_Category: layer · Blocking: no (fire-and-forget) · delete a layer (KAG3 compat)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `layer` | string | - | - | - | - |
| `name` | string | - | - | - | - |

### `[listsaves]`

_Category: save · Blocking: no (fire-and-forget) · KAG3-compatible listsaves command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[live2d_expression]`

_Category: character · Blocking: no (fire-and-forget) · Set a Live2D facial expression on a model_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `expression` | string | - | - | yes | - |
| `model` | string | - | - | yes | - |
| `weight` | number | 1.0 | 0.0..1.0 | - | - |

### `[live2d_hide]`

_Category: character · Blocking: no (fire-and-forget) · Hide or unload a context-owned Live2D model_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `model` | string | - | - | yes | - |

### `[live2d_lip_sync]`

_Category: character · Blocking: no (fire-and-forget) · Control a loaded Live2D mouth manually, from VOICE PCM, or turn automatic control off_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `model` | string | - | - | yes | - |
| `source` | enum | manual | manual, voice, off | - | - |
| `value` | number | - | 0.0..1.0 | - | - |

### `[live2d_load]`

_Category: character · Blocking: no (fire-and-forget) · Load a Live2D model owned by the current scene context_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `model` | string | - | - | yes | - |
| `storage` | file | - | - | yes | - |

### `[live2d_motion]`

_Category: character · Blocking: no (fire-and-forget) · Play a Live2D motion animation on a model with optional fade times_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `fadein` | number | 500 | 0..10000 | - | - |
| `fadeout` | number | 500 | 0..10000 | - | - |
| `model` | string | - | - | yes | - |
| `motion` | string | - | - | yes | - |

### `[live2d_show]`

_Category: character · Blocking: no (fire-and-forget) · Show a loaded Live2D model_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `model` | string | - | - | yes | - |
| `scale` | number | 1 | 0.001..- | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[live2d_unload]`

_Category: character · Blocking: no (fire-and-forget) · Hide or unload a context-owned Live2D model_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `model` | string | - | - | yes | - |

### `[load]`

_Category: save · Blocking: yes (waits for completion) · KAG3-compatible load command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `slot` | number | - | -2..99 | - | - |

### `[loadplace]`

_Category: save · Blocking: no (fire-and-forget) · KAG3-compatible loadplace command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[mod]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible mod: var %= value (zero operand -> visible error, no-op)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `name` | string | - | - | yes | - |
| `value` | number | - | - | yes | - |

### `[move]`

_Category: transition · Blocking: yes (waits for completion) · KAG3-compatible move command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `duration` | number | 300 | 0..30000 | - | - |
| `layer` | string | fg | - | - | name |
| `name` | string |  | - | - | - |
| `time` | number | 300 | 0..30000 | - | duration |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[moveto]`

_Category: layer · Blocking: yes (waits for completion) · KAG3-compatible moveto command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `layer` | string | - | - | - | - |
| `left` | number | - | - | - | - |
| `scale` | number | 1.0 | 0.01..16 | - | - |
| `top` | number | - | - | - | - |
| `unit` | string | ndc | - | - | - |
| `x` | number | - | - | - | - |
| `y` | number | - | - | - | - |

### `[mul]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible mul: var *= value_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `name` | string | - | - | yes | - |
| `value` | number | - | - | yes | - |

### `[music]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible music command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[nameplate]`

_Category: text · Blocking: no (fire-and-forget) · KAG3-compatible nameplate command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `color` | string | 0,0,0 | - | - | - |
| `h` | number | 36 | 16..256 | - | - |
| `opacity` | number | 220 | 0..255 | - | - |
| `text_color` | string | 255,255,255 | - | - | - |
| `w` | number | 220 | 32..1024 | - | - |
| `x` | number | 32 | - | - | - |
| `y` | number | 480 | - | - | - |

### `[notify]`

_Category: system · Blocking: no (fire-and-forget) · show a brief corner toast notification (author feedback)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `kind` | string | - | - | - | - |
| `msg` | string | - | - | yes | - |
| `time` | number | 2500 | 0..30000 | - | - |

### `[nvl]`

_Category: text · Blocking: no (fire-and-forget) · NVL mode: full-screen accumulated text (Ren'Py parity); [nvl clear] page break, [nvl off] exit; [nvl prefix="「%s」："] customizes the speaker prefix format (%s = name)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `mode` | string | - | - | - | - |
| `prefix` | string | - | - | - | - |

### `[p]`

_Category: text · Blocking: yes (waits for completion) · click-to-advance_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[palette]`

_Category: vfx · Blocking: no (fire-and-forget) · KAG3-compatible palette/LUT color-grading command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `effect` | enum | apply | - | - | - |
| `id` | string |  | - | - | - |
| `intensity` | number | 1.0 | 0.0..1.0 | - | - |
| `path` | string |  | - | - | - |

### `[particle_weather]`

_Category: vfx · Blocking: no (fire-and-forget) · Declarative weather atmosphere particle system_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `action` | enum | start | - | - | - |
| `count` | number | 10 | 0..500 | - | - |
| `intensity` | number | 1.0 | 0.0..5.0 | - | - |
| `speed` | number | 1.0 | 0.1..10.0 | - | - |
| `type` | enum | - | - | - | - |
| `wind` | number | 0 | -10..10 | - | - |

### `[particles]`

_Category: vfx · Blocking: no (fire-and-forget) · KAG3-compatible particles command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `a` | number | 1 | 0..255 | - | alpha |
| `alpha` | number | 1 | 0..255 | - | - |
| `angleMax` | number | 6.283 | - | - | angle_max |
| `angleMin` | number | 0 | - | - | - |
| `angle_max` | number | 0 | - | - | - |
| `b` | number | 1 | 0..255 | - | blue |
| `blue` | number | 1 | 0..255 | - | - |
| `g` | number | 1 | 0..255 | - | green |
| `green` | number | 1 | 0..255 | - | - |
| `lifeMax` | number | 2.0 | 0..60 | - | life_max |
| `lifeMin` | number | 0.5 | 0..60 | - | - |
| `life_max` | number | 0.5 | 0..60 | - | - |
| `r` | number | 1 | 0..255 | - | red |
| `rate` | number | 10 | 0..1000 | - | - |
| `red` | number | 1 | 0..255 | - | - |
| `sizeMax` | number | 8 | 0..512 | - | size_max |
| `sizeMin` | number | 2 | 0..512 | - | - |
| `size_max` | number | 2 | 0..512 | - | - |
| `speedMax` | number | 50 | 0..10000 | - | speed_max |
| `speedMin` | number | 10 | 0..10000 | - | - |
| `speed_max` | number | 10 | 0..10000 | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[play]`

_Category: audio · Blocking: no (fire-and-forget) · play audio on bus=bgm|se|voice (Neo-Genesis unified)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `bus` | string | - | bgm,se,voice | - | - |
| `file` | string | - | - | - | - |
| `storage` | string | - | - | - | - |
| `volume` | number | - | 0..1.5 | - | - |

### `[playbgm]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible playbgm command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| **requires one of** | — | — | file, storage | yes | — |
| `fadein` | number | 0 | 0..30000 | - | - |
| `file` | file | - | - | - | - |
| `loop` | boolean | true | - | - | - |
| `storage` | file | - | - | - | - |
| `volume` | number | 1.0 | 0..1.5 | - | - |

### `[playbgmstop]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible playbgmstop command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `fadein` | number | 0 | 0..30000 | - | - |
| `fadeout` | number | 0 | 0..30000 | - | - |
| `file` | file | - | - | - | - |
| `volume` | number | 1.0 | 0..1.5 | - | - |

### `[playse]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible playse command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| **requires one of** | — | — | file, storage | yes | — |
| `fadein` | number | 0 | 0..30000 | - | - |
| `file` | file | - | - | - | - |
| `storage` | file | - | - | - | - |
| `volume` | number | 1.0 | 0..1.5 | - | - |

### `[playstop]`

_Category: audio · Blocking: no (fire-and-forget) · stop BGM playback (KAG3 compat)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `fadeout` | number | 0 | 0..30000 | - | - |

### `[playvoice]`

_Category: audio · Blocking: yes (waits for completion) · play a voiced line (blocks until finished)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `file` | file | - | - | - | - |
| `path` | file | - | - | - | - |
| `storage` | file | - | - | - | - |
| `volume` | number | 1.0 | 0..1.5 | - | - |

### `[position]`

_Category: layer · Blocking: no (fire-and-forget) · KAG3-compatible position command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `layer` | string | fg | - | - | name |
| `name` | string |  | - | - | - |
| `pos` | string |  | - | - | - |
| `scale` | number | 1.0 | 0.01..16 | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[postprocess]`

_Category: vfx · Blocking: no (fire-and-forget) · Apply post-processing full-screen shader effect_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `amount` | number | 0.0 | 0.0..1.0 | - | - |
| `b` | number | 255 | 0..255 | - | - |
| `effect` | enum | - | - | yes | - |
| `g` | number | 255 | 0..255 | - | - |
| `intensity` | number | - | 0.0..1.0 | - | - |
| `lutMix` | number | 0.0 | 0.0..1.0 | - | - |
| `r` | number | 255 | 0..255 | - | - |
| `radius` | number | 0.0 | 0.0..64.0 | - | - |
| `rgb` | string | - | - | - | - |
| `strength` | number | - | 0.0..1.0 | - | - |

### `[postprocess_off]`

_Category: vfx · Blocking: no (fire-and-forget) · Disable post-processing: the whole chain, or one stage with effect=_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `effect` | enum | - | - | - | - |

### `[preload]`

_Category: resource · Blocking: no (fire-and-forget) · Preload assets (texture/audio/scene) ahead of use; async unless wait=true_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `path` | string | - | - | - | - |
| `storage` | string |  | - | - | - |
| `type` | enum | texture | - | - | - |
| `wait` | enum | true | - | - | - |

### `[pt]`

_Category: text · Blocking: no (fire-and-forget) · point text at position_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `speed` | number | 50 | 8..5000 | - | - |

### `[quake]`

_Category: transition · Blocking: yes (waits for completion) · KAG3-compatible quake command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `amplitude` | number | 5 | 0..100 | - | - |
| `duration` | number | 300 | 0..30000 | - | - |
| `intensity` | number | 5 | 0..100 | - | amplitude |
| `time` | number | 300 | 0..30000 | - | duration |

### `[quickmenu_auto]`

_Category: ui · Blocking: no (fire-and-forget) · Toggle auto-advance mode (QuickMenu)._

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `mode` | string | toggle | off,on,toggle | - | - |

### `[quickmenu_config]`

_Category: ui · Blocking: no (fire-and-forget) · Open settings panel (QuickMenu)._

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[quickmenu_log]`

_Category: ui · Blocking: no (fire-and-forget) · Show dialogue backlog (QuickMenu)._

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[quickmenu_qload]`

_Category: ui · Blocking: no (fire-and-forget) · Quick-load from slot (QuickMenu)._

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `slot` | number | 0 | 0..99 | - | - |

### `[quickmenu_qsave]`

_Category: ui · Blocking: no (fire-and-forget) · Quick-save to slot (QuickMenu)._

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `slot` | number | 0 | 0..99 | - | - |

### `[quickmenu_skip]`

_Category: ui · Blocking: no (fire-and-forget) · Toggle skip mode (QuickMenu)._

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `mode` | string | toggle | off,on,seen,toggle | - | - |

### `[quickmenu_title]`

_Category: ui · Blocking: no (fire-and-forget) · Return to title label (QuickMenu)._

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `scene` | string | title | - | - | - |

### `[r]`

_Category: text · Blocking: no (fire-and-forget) · carriage return_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[random]`

_Category: system · Blocking: no (fire-and-forget) · write a random integer into a variable_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `max` | number | 100 | - | - | - |
| `min` | number | 0 | - | - | - |
| `var` | string | - | - | yes | - |

### `[replay]`

_Category: system · Blocking: no (fire-and-forget) · input recording/playback control_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `file` | string | - | - | - | - |
| `mode` | string | - | - | yes | - |

### `[reset]`

_Category: text · Blocking: no (fire-and-forget) · reset text state_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[rollback]`

_Category: system · Blocking: yes (waits for completion) · pop the newest token-level snapshot and re-run from there_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[ruby]`

_Category: text · Blocking: no (fire-and-forget) · KAG3-compatible ruby command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `ruby` | string |  | - | - | - |
| `ruby_scale` | number | 0.5 | 0.1..2.0 | - | - |
| `text` | string |  | - | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[s]`

_Category: text · Blocking: yes (waits for completion) · KAG3 short-wait_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `ms` | number | 250 | 0..60000 | - | - |

### `[save]`

_Category: save · Blocking: yes (waits for completion) · KAG3-compatible save command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `slot` | number | - | -2..99 | - | - |

### `[saveload]`

_Category: save · Blocking: yes (waits for completion) · open the save/load menu (mode: save|load)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `mode` | string | - | - | - | - |

### `[saveplace]`

_Category: save · Blocking: no (fire-and-forget) · KAG3-compatible saveplace command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[scroll]`

_Category: transition · Blocking: no (fire-and-forget) · KAG3-compatible scroll command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `color` | string | white | - | - | - |
| `size` | number | 28 | 8..128 | - | - |
| `speed` | number | 60 | 1..1000 | - | - |
| `text` | string |  | - | - | - |

### `[select]`

_Category: text · Blocking: no (fire-and-forget) · begin a choice block ([sel] alias)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[set]`

_Category: system · Blocking: no (fire-and-forget) · typed variable assignment (f.x/sf.x/tf.x/mp.x/lf.x)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `value` | string | - | - | yes | - |
| `var` | string | - | - | yes | - |

### `[setbgmvolume]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible setbgmvolume command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `volume` | number | - | 0..1.5 | - | - |

### `[setsevolume]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible setsevolume command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `volume` | number | - | 0..1.5 | - | - |

### `[setvoicevolume]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible setvoicevolume command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `volume` | number | - | 0..1.5 | - | - |

### `[shake]`

_Category: vfx · Blocking: yes (waits for completion) · KAG3-compatible shake command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `amplitude` | number | 6 | 0..100 | - | - |
| `frequency` | number | 20 | 1..120 | - | - |
| `time` | number | 500 | 0..10000 | - | - |

### `[skip]`

_Category: text · Blocking: no (fire-and-forget) · toggle skip mode (mode=seen skips read text only)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `mode` | string | - | - | - | - |

### `[sma_anim]`

_Category: system · Blocking: no (fire-and-forget) · SMA runtime animation switch (round 18; optional blend_time crossfade)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `anim` | string | - | - | - | - |
| `blend_time` | number | - | 0..60 | - | - |
| `loop` | boolean | true | - | - | - |
| `name` | string | - | - | - | - |
| `on_done_anim` | string | - | - | - | - |
| `rate` | number | - | - | - | - |

### `[sma_ik]`

_Category: system · Blocking: no (fire-and-forget) · SMA 2-bone IK constraint (round 18): chain reaches (tx, ty)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `bone0` | number | - | - | - | - |
| `bone1` | number | - | - | - | - |
| `l1` | number | - | - | - | - |
| `l2` | number | - | - | - | - |
| `name` | string | - | - | - | - |
| `tx` | number | - | - | - | - |
| `ty` | number | - | - | - | - |

### `[sma_play]`

_Category: system · Blocking: no (fire-and-forget) · SMA skeletal-mesh actor spawn (Battle 4d S3; loop/rate/on_done round 18)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `anim` | string | - | - | - | - |
| `asset` | string | - | - | - | - |
| `loop` | boolean | true | - | - | - |
| `name` | string | - | - | - | - |
| `on_done_anim` | string | - | - | - | - |
| `opacity` | number | - | - | - | - |
| `rate` | number | 1 | - | - | - |
| `scale` | number | - | - | - | - |
| `tex` | number | - | - | - | - |
| `view` | number | - | - | - | - |
| `x` | number | - | - | - | - |
| `y` | number | - | - | - | - |

### `[sma_stop]`

_Category: system · Blocking: no (fire-and-forget) · SMA skeletal-mesh actor despawn (Battle 4d S3)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `name` | string | - | - | - | - |

### `[sma_variant]`

_Category: system · Blocking: no (fire-and-forget) · SMA part variant switch (round 18; E-mote style)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `name` | string | - | - | - | - |
| `part` | string | - | - | - | - |
| `tex` | number | - | - | - | - |
| `variant` | string | - | - | - | - |

### `[sprite_fade]`

_Category: text · Blocking: yes (waits for completion) · KAG3-compatible sprite_fade command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `speaker` | string | - | - | yes | - |
| `time` | number | 300 | 0..30000 | - | - |
| `to` | number | 255 | 0..255 | - | - |

### `[sprite_move]`

_Category: text · Blocking: yes (waits for completion) · KAG3-compatible sprite_move command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `speaker` | string | - | - | yes | - |
| `time` | number | 400 | 0..30000 | - | - |
| `x` | number | 440 | - | - | - |
| `y` | number | 200 | - | - | - |

### `[sprite_scale]`

_Category: text · Blocking: yes (waits for completion) · KAG3-compatible sprite_scale command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `scale` | number | 1.0 | 0.1..4.0 | - | - |
| `speaker` | string | - | - | yes | - |
| `time` | number | 300 | 0..30000 | - | - |

### `[sprite_swap]`

_Category: text · Blocking: yes (waits for completion) · KAG3-compatible sprite_swap command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `speaker` | string | - | - | yes | - |
| `sprite` | string | - | - | yes | - |

### `[steam_achievement]`

_Category: system · Blocking: no (fire-and-forget) · unlock a Steamworks achievement (warns, never fails, when Steam is absent)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `cond` | string | - | - | - | - |
| `id` | string | - | - | - | - |
| `name` | string | - | - | - | - |
| `silent` | boolean | false | - | - | - |

### `[stopbgm]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible stopbgm command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `fadeout` | number | - | 0..30000 | - | - |
| `time` | number | - | 0..30000 | - | - |

### `[stopse]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible stopse command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `fadeout` | number | - | 0..30000 | - | - |
| `time` | number | - | 0..30000 | - | - |

### `[stopvideo]`

_Category: video · Blocking: no (fire-and-forget) · stop the current video playback_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[stopvoice]`

_Category: audio · Blocking: no (fire-and-forget) · stop the current voice playback_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[sub]`

_Category: system · Blocking: no (fire-and-forget) · KAG3-compatible sub: var -= value_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `name` | string | - | - | yes | - |
| `value` | number | - | - | yes | - |

### `[text]`

_Category: text · Blocking: no (fire-and-forget) · KAG3-compatible text command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `fade` | number | 0 | 0..30000 | - | - |
| `fade_time` | number | 0 | 0..30000 | - | - |
| `text` | string |  | - | - | message, content |

### `[textbox]`

_Category: text · Blocking: no (fire-and-forget) · KAG3-compatible textbox command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `color` | string | 0,0,0 | - | - | - |
| `h` | number | 200 | 32..1024 | - | - |
| `opacity` | number | 200 | 0..255 | - | - |
| `visible` | boolean | true | - | - | - |
| `w` | number | 1280 | 64..4096 | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 520 | - | - | - |

### `[textspeed]`

_Category: text · Blocking: no (fire-and-forget) · KAG3 typewriter speed: chars per second (overrides [pt] ms/char)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `cps` | number | 50 | 1..120 | - | - |

### `[trans]`

_Category: transition · Blocking: yes (waits for completion) · KAG3-compatible trans command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `duration` | number | - | 0..30000 | - | - |
| `kind` | string | - | - | - | - |
| `method` | string | - | - | - | - |
| `time` | number | - | 0..30000 | - | - |
| `type` | string | - | - | - | - |

### `[tween]`

_Category: layer · Blocking: yes (waits for completion) · declaratively tween a layer/sprite attribute from A to B in N ms_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `attr` | enum | - | x, y, alpha, scale | yes | - |
| `delay` | number | 0 | 0..30000 | - | - |
| `dur` | number | - | 100..30000 | yes | - |
| `ease` | enum | linear | linear, ease_in, ease_out, ease_in_out, back_out | - | - |
| `from` | string | - | - | - | - |
| `target` | string | - | - | yes | - |
| `to` | string | - | - | yes | - |
| `wait` | boolean | true | - | - | - |

### `[typewriter]`

_Category: text · Blocking: no (fire-and-forget) · Configure typewriter sound effects on character reveal_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `action` | enum | set | - | - | - |
| `enabled` | boolean | true | - | - | - |
| `file` | string |  | - | - | - |
| `interval` | number | 1 | 1..100 | - | - |
| `sound` | string |  | - | - | file |
| `volume` | number | 1.0 | 0.0..2.0 | - | - |

### `[typewriter_sound]`

_Category: text · Blocking: no (fire-and-forget) · Alias for typewriter sound configuration_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `action` | enum | set | - | - | - |
| `enabled` | boolean | true | - | - | - |
| `file` | string |  | - | - | - |
| `interval` | number | 1 | 1..100 | - | - |
| `sound` | string |  | - | - | file |
| `volume` | number | 1.0 | 0.0..2.0 | - | - |

### `[unlock]`

_Category: system · Blocking: no (fire-and-forget) · unlock a gallery CG or music-room track_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `id` | string | - | - | - | - |
| `name` | string | - | - | - | - |
| `type` | string | cg | - | - | - |

### `[vfx]`

_Category: vfx · Blocking: no (fire-and-forget) · GPU visual effects: particles, quake/shake/flash/fade/blur, and PostFx chain (postfx=)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `action` | string | - | - | - | - |
| `amount` | number | - | 0..1 | - | - |
| `amplitude` | number | - | 0..100 | - | - |
| `amplitudex` | number | - | 0..200 | - | - |
| `amplitudey` | number | - | 0..200 | - | - |
| `angleMax` | number | - | - | - | - |
| `angleMin` | number | - | - | - | - |
| `angle_max` | number | - | - | - | - |
| `b` | number | - | 0..255 | - | - |
| `blue` | number | - | 0..255 | - | - |
| `blurlevel` | number | - | 0..64 | - | - |
| `color` | string | - | - | - | - |
| `count` | number | - | 0..100000 | - | - |
| `decay` | boolean | - | - | - | - |
| `direction` | string | - | - | - | - |
| `effect` | string | - | - | - | - |
| `emitter` | number | - | - | - | - |
| `frequency` | number | - | 1..120 | - | - |
| `g` | number | - | 0..255 | - | - |
| `gravityX` | number | - | - | - | - |
| `gravityY` | number | - | - | - | - |
| `gravity_x` | number | - | - | - | - |
| `gravity_y` | number | - | - | - | - |
| `green` | number | - | 0..255 | - | - |
| `intensity` | number | - | 0..50 | - | - |
| `layer` | string | - | - | - | - |
| `lifeMax` | number | - | 0..60 | - | - |
| `lifeMin` | number | - | 0..60 | - | - |
| `life_max` | number | - | 0..60 | - | - |
| `lutMix` | number | - | 0..1 | - | - |
| `opacity` | number | - | 0..1 | - | - |
| `postfx` | enum | - | - | - | - |
| `power` | number | - | 0..200 | - | - |
| `r` | number | - | 0..255 | - | - |
| `radius` | number | - | 0..64 | - | - |
| `rate` | number | - | 0..1000 | - | - |
| `red` | number | - | 0..255 | - | - |
| `rgb` | string | - | - | - | - |
| `sizeMax` | number | - | 0..512 | - | - |
| `sizeMin` | number | - | 0..512 | - | - |
| `size_max` | number | - | 0..512 | - | - |
| `speed` | number | - | 0..10000 | - | - |
| `speedMax` | number | - | 0..10000 | - | - |
| `speedMin` | number | - | 0..10000 | - | - |
| `speed_max` | number | - | 0..10000 | - | - |
| `strength` | number | - | 0..255 | - | - |
| `time` | number | - | 0..30000 | - | - |
| `type` | string | - | - | - | - |
| `x` | number | - | - | - | - |
| `y` | number | - | - | - | - |

### `[vib]`

_Category: transition · Blocking: yes (waits for completion) · KAG3-compatible vib command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `amplitude` | number | - | 0..50 | - | - |
| `intensity` | number | 3 | 0..50 | - | - |
| `time` | number | 300 | 0..30000 | - | - |

### `[vibrate]`

_Category: vfx · Blocking: yes (waits for completion) · KAG3-compatible alias for [vib] (message-layer vibration)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `amplitude` | number | - | 0..50 | - | - |
| `intensity` | number | 3 | 0..50 | - | - |
| `time` | number | 300 | 0..30000 | - | - |

### `[video]`

_Category: video · Blocking: yes (waits for completion) · KAG3-compatible video command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| **requires one of** | — | — | file, storage | yes | — |
| `file` | string | - | - | - | - |
| `h` | number | 0 | 0..8192 | - | - |
| `loop` | boolean | false | - | - | - |
| `storage` | string | - | - | - | - |
| `volume` | number | 1.0 | 0..1.5 | - | - |
| `w` | number | 0 | 0..8192 | - | - |
| `x` | number | 0 | - | - | - |
| `y` | number | 0 | - | - | - |

### `[voice]`

_Category: audio · Blocking: no (fire-and-forget) · KAG3-compatible voice command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `file` | file | - | - | - | - |
| `path` | file | - | - | - | - |
| `storage` | file | - | - | - | - |
| `volume` | number | 1.0 | 0..1.5 | - | - |

### `[voice_off]`

_Category: text · Blocking: no (fire-and-forget) · KAG3-compatible voice_off command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `on` | boolean | true | - | - | - |

### `[voice_wait]`

_Category: text · Blocking: yes (waits for completion) · wait for a voice line with click-to-skip_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[wait]`

_Category: system · Blocking: yes (waits for completion) · KAG3-compatible wait command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `duration` | number | - | 0..60000 | - | - |
| `ms` | number | - | 0..60000 | - | - |
| `time` | number | 1000 | 0..60000 | - | - |

### `[waitbgm]`

_Category: audio · Blocking: yes (waits for completion) · block until the BGM bus finishes_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[waitclick]`

_Category: audio · Blocking: yes (waits for completion) · block until a click (voice-oriented wait)_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[waitforclick]`

_Category: text · Blocking: yes (waits for completion) · block until the player clicks_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[waitsound]`

_Category: audio · Blocking: yes (waits for completion) · block until the SE bus finishes_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|

### `[xfadebgm]`

_Category: audio · Blocking: yes (waits for completion) · KAG3-compatible xfadebgm command_

| Param | Type | Default | Range / Choices | Required | Aliases |
|---|---|---|---|---|---|
| `file` | file | - | - | - | - |
| `storage` | file | - | - | - | - |
| `time` | number | 2000 | 0..30000 | - | - |

