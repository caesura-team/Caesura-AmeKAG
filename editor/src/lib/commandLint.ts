// KAG command-name table + parameter hints for the inspector lint surface.
//
// Single source of truth for the recognized command-name set (mirrors what
// kagLanguage.ts drives Monaco keyword highlighting with). It lives here in a
// pure, DOM/monaco-free module so the Inspector's parameter lint can validate
// a selected [command] row without pulling the Monaco editor bundle into the
// test graph (kagLanguage.ts re-exports it as KAG_COMMANDS).
//
// KNOWN_PARAM_HINTS is a small, curated set of accepted parameters for
// high-frequency commands, transcribed from docs/api/command-contracts.md
// (auto-generated from kag/schema.lua) plus observed KAG3-style flow params
// (jump/call/goto) from the demo scripts. It is deliberately *not* exhaustive:
// a parameter not listed here is reported as "unlisted" (a soft hint, never an
// error), so an unfamiliar but valid param is not flagged as wrong.

/** Recognized KAG Neo-Genesis command names (the full keyword set). */
export const KNOWN_COMMANDS: string[] = [
  // flow
  'if', 'elseif', 'elsif', 'else', 'endif', 'while', 'endwhile', 'for',
  'endfor', 'break', 'continue', 'switch', 'case', 'default', 'endswitch',
  'jump', 'goto', 'call', 'return', 'link', 'label', 'macro', 'endmacro',
  'erasemacro', 'end', 'stop', 'eval', 'emb', 'iscript', 'endscript',
  'until',
  // text
  'ch', 'text', 'l', 'r', 'er', 'p', 'ruby', 'font', 'pt', 'button',
  'endbutton', 'sel', 'select', 'endselect', 'nameplate', 'textbox',
  'sprite_fade', 'sprite_move', 'sprite_scale', 'sprite_swap', 'history',
  'voice_wait', 'waitforclick', 'waitclick', 'reset', 'skip', 'auto', 'nvl',
  'input', 'edit', 'typewriter', 'typewriter_sound',
  // layer
  'bg', 'fg', 'cl', 'image', 'position', 'layopt', 'ld', 'fadeout', 'layfade',
  'scroll', 'trans', 'move', 'moveto', 'quake', 'shake', 'vfx', 'flash', 'blur', 'fade',
  'vib', 'camera', 'particles', 'particle_weather', 'postprocess', 'postprocess_off',
  // audio
  'playbgm', 'playbgmstop', 'playse', 'playvoice', 'stopvoice', 'stopbgm', 'stopse',
  'fadebgm', 'fadevol', 'xfadebgm', 'play', 'bgm', 'se', 'voice',
  'voice_off', 'playstop', 'setbgmvolume', 'setsevolume', 'setvoicevolume',
  'waitsound', 'waitbgm',
  // system / resource / save
  'wait', 'delay', 's', 'chapter', 'ending', 'gallery', 'music', 'unlock',
  'rollback', 'toast', 'replay', 'save', 'load', 'saveload', 'listsaves', 'saveplace',
  'loadplace', 'preload', 'get_texture', 'is_loaded', 'is_pending',
  'flush_cache', 'video', 'stopvideo', 'ai_dialog', 'i18n', 'set', 'inc', 'random',
  'assert', 'sma_play', 'sma_stop', 'sma_anim', 'sma_ik', 'sma_variant',
  'steam_achievement',
  // Existing schema-declared quick-menu commands.
  'quickmenu_auto', 'quickmenu_skip', 'quickmenu_log', 'quickmenu_config',
  'quickmenu_qsave', 'quickmenu_qload', 'quickmenu_title',
  // live2d
  'live2d_expression', 'live2d_hide', 'live2d_lip_sync', 'live2d_load',
  'live2d_motion', 'live2d_show', 'live2d_unload',
  // round 71: KAG3-compat arithmetic + character + effects + notification
  'add', 'sub', 'mul', 'div', 'mod', 'dec', 'csp', 'csd', 'csl',
  'textspeed', 'cps', 'palette', 'vibrate', 'notify',
  'clear', 'ct', 'endtag', 'endform', 'g', 'br', 'hr', 'cancel', 'close',
  // round 106/107: declarative tween + layout containers
  'tween', 'layout', 'layout_slot', 'layout_place',
]

/** A Set view of KNOWN_COMMANDS for O(1) membership checks. */
export const KNOWN_COMMAND_SET: ReadonlySet<string> = new Set(KNOWN_COMMANDS)

/** Accepted parameters for curated high-frequency commands (see header). */
export const KNOWN_PARAM_HINTS: Readonly<Record<string, readonly string[]>> = {
  bg: ['file', 'layer', 'path', 'storage'],
  fg: ['clear', 'file', 'layer', 'path', 'storage'],
  bgm: ['file', 'storage', 'volume'],
  ch: ['chars_per_line', 'max_width', 'name', 'sprite', 'text', 'voice'],
  text: ['fade', 'fade_time', 'text'],
  textbox: ['color', 'h', 'opacity', 'visible', 'w', 'x', 'y'],
  l: [],
  play: ['bus', 'file', 'storage', 'volume'],
  playbgm: ['fadein', 'file', 'loop', 'storage', 'volume'],
  playse: ['fadein', 'file', 'storage', 'volume'],
  wait: ['duration', 'ms', 'time'],
  layopt: ['layer', 'opacity', 'visible'],
  position: ['layer', 'name', 'pos', 'scale', 'x', 'y'],
  fade: ['duration', 'from', 'layer', 'time', 'to'],
  trans: ['duration', 'method', 'time', 'type'],
  camera: ['restore', 'time', 'x', 'y'],
  nameplate: ['color', 'h', 'opacity', 'text_color', 'w', 'x', 'y'],
  eval: ['code', 'exp'],
  set: ['value', 'var'],
  inc: ['by', 'var'],
  // KAG3-style flow (not in schema doc; keep the params seen in demo scripts)
  jump: ['target', 'label', 'storage'],
  goto: ['target', 'label', 'storage'],
  call: ['target', 'label', 'storage'],
  link: ['target', 'label', 'storage', 'text'],
}

/** Per-parameter lint verdict. */
export type ParamVerdict = 'known' | 'unlisted' | 'flag'

export interface CommandLint {
  /** Command name (the parens stripped from the [command] tag text). */
  command: string
  /** Whether the command is a recognized KAG command name. */
  knownCommand: boolean
  /** Per-parameter verdict keyed by parsed param name. */
  params: Record<string, ParamVerdict>
  /** Count of parameters flagged as unlisted for a curated command. */
  unlistedCount: number
}

/**
 * Lint a parsed [command ...] row.
 *  - unknownCommand is true when the command word is not in KNOWN_COMMANDS.
 *  - For commands with a KNOWN_PARAM_HINTS entry, params outside that set are
 *    "unlisted" (soft hint); params whose value is 'true' (bare flags) are
 *    "flag".
 *  - For commands without a hint entry, params get no opinion ('known') but
 *    bare flags are still surfaced as 'flag'.
 * Never throws.
 */
export function lintCommand(command: string, params: Record<string, string>): CommandLint {
  const cmd = command.trim().toLowerCase()
  const knownCommand = knownCommandName(cmd)
  const hints = KNOWN_PARAM_HINTS[cmd]
  const out: Record<string, ParamVerdict> = {}
  let unlistedCount = 0
  for (const [key, value] of Object.entries(params)) {
    let verdict: ParamVerdict = 'known'
    if (value === 'true') {
      verdict = 'flag'
    } else if (hints && !hints.includes(key)) {
      verdict = 'unlisted'
      unlistedCount++
    }
    out[key] = verdict
  }
  return { command: cmd, knownCommand, params: out, unlistedCount }
}

/** Normalize a command word and test membership in the known set. */
export function knownCommandName(command: string): boolean {
  return KNOWN_COMMAND_SET.has(command.trim().toLowerCase())
}
