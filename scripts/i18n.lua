-- ===========================================================================
--  Caesura (AmeKAG) — i18n.lua  [P1.3]
--  Multi-language internationalization.
--  Loads string tables from assets/lang/xx.lua.
--  Two mechanisms (both flow through i18n.localize, applied by the KAG
--  text pipeline before markup parsing):
--    1. Per-line translation: lang file `lines` table maps
--       "<scene>:<fnv1a(message)>" -> localized text (Ren'Py-style line
--       translation; keys are content-addressed so scene reordering or
--       runtime-generated dialogue never shifts translations).
--    2. {key} token expansion: "{key}" inside any text is replaced by
--       i18n.t(key). Markup tag names (b/i/s/color/size) are whitelisted
--       and never treated as keys (inline markup wins).
--  Authoring: scripts/ks_i18n.lua scans scenes and emits/merges the
--  `lines` template into assets/lang/<code>.lua.
-- ===========================================================================

local i18n = {}

-- Current language code
i18n.current = "zh"
-- Default fallback language code (configurable). t()/translate() fall
-- back to this dictionary before returning the raw key. Defaults to "en"
-- (matches the documented "fall back to English" behavior).
i18n.default_language = "en"
-- Loaded string table (flat key->value)
i18n.strings = {}
-- Fallback string table (always English)
i18n.fallback = {}
-- Per-line translations: ["<scene>:<hash>"] = localized text
i18n.lines = {}

-- Markup tag names that must never be treated as {key} translation tokens
-- (inline text markup wins over i18n; see kag/text_layout.lua).
local MARKUP_NAMES = { b = true, i = true, s = true, color = true, size = true }

-- ===========================================================================
-- i18n.fnv1a(text) -> 8-hex FNV-1a 32-bit hash of a string
--  Content-addressed translation keys: stable under scene reordering and
--  immune to runtime-generated dialogue. Same algorithm as
--  compiler.hashFile (offset basis 2166136261, prime 16777619).
-- ===========================================================================
function i18n.fnv1a(text)
    text = tostring(text or "")
    local hash = 2166136261
    for i = 1, #text do
        hash = (hash ~ text:byte(i)) * 16777619
        hash = hash % 4294967296
    end
    return string.format("%08x", hash)
end

-- ===========================================================================
-- i18n.load(langCode) — load language table from assets/lang/<code>.lua
-- ===========================================================================
function i18n.load(langCode)
    langCode = langCode or "zh"
    i18n.current = langCode
    i18n.lines = {}

    -- Try loading the language file
    local path = "assets/lang/" .. langCode .. ".lua"
    local ok, data = pcall(function()
        local f = io.open(path, "r")
        if not f then return nil end
        local txt = f:read("*a")
        f:close()
        -- Hand-written lang files are bare table literals ({...}); tool
        -- generated files (ks_i18n.lua) carry leading comments + an
        -- explicit `return`. Accept both: strip all leading comment
        -- lines, then prepend `return ` only when the body lacks an
        -- anchored top-level `return` (a bare substring test would
        -- misfire on hand-written files whose values mention "return").
        local body = txt
        while body:match("^%s*%-%-") do
            body = body:gsub("^%s*%-%-[^\n]*\n?", "", 1)
        end
        local chunk = body:match("^%s*return") and load(body, "i18n", "t", {})
            or load("return " .. body, "i18n", "t", {})
        return chunk()
    end)

    if ok and type(data) == "table" then
        i18n.strings = data
        if type(data.lines) == "table" then
            i18n.lines = data.lines
        end
        print("[i18n] Loaded " .. langCode .. " (" .. #data .. " keys)")
    else
        -- If file not found, try built-in fallback
        print("[i18n] Language file not found: " .. path .. ". Using built-in.")
        i18n._loadBuiltin(langCode)
    end

    -- Load the default-language dictionary (configurable, default "en") as
    -- the fallback chain's second rung: specified language -> default
    -- language -> raw key. Only loaded once, unless default_language changed.
    local dflt = i18n.default_language or "en"
    if langCode ~= dflt and next(i18n.fallback) == nil then
        local ok2, fb = pcall(function()
            local f = io.open("assets/lang/" .. dflt .. ".lua", "r")
            if not f then return nil end
            local txt = f:read("*a")
            f:close()
            return load("return " .. txt, "i18n", "t", {})()
        end)
        if ok2 and type(fb) == "table" then
            i18n.fallback = fb
        end
    end

    return i18n.strings
end

-- ===========================================================================
-- i18n.plural_category(count) -> string
--  Per-language plural category for a count (CLDR-style categories).
--    en: 1 -> "one", otherwise -> "other".
--    zh / ja (and any unknown language): always -> "other" (no singular/
--  plural distinction; a single unmarked form is the convention). This
--  keeps the runtime safe when a zh/ja dictionary carries only "other".
-- ===========================================================================
function i18n.plural_category(count)
    count = tonumber(count) or 0
    local lang = i18n.current or "zh"
    if lang == "en" then
        return count == 1 and "one" or "other"
    end
    return "other"
end

-- ===========================================================================
-- i18n._plural_form(entry, count) -> string
--  Resolve a dictionary VALUE that may be either a plain string or a
--  plural-variant table ({ one = "...", other = "..." }):
--    - plain string -> returned unchanged.
--    - plural table  -> the variant for this count's category selected by
--      i18n.plural_category(count); falls back to entry.other then
--      entry.one when the exact category is absent (a missing-plural
--      fallback to the bare/most-generic form). With count=nil the generic
--      "other" form is used (safe for t()/expand() with no {n} in scope).
-- ===========================================================================
function i18n._plural_form(entry, count)
    if type(entry) ~= "table" then return entry end
    local v = entry[i18n.plural_category(count)] or entry.other or entry.one
    return v == nil and "" or tostring(v)
end

-- ===========================================================================
-- i18n.t(key) -> translated string
-- Looks up key in current strings, falls back to English, then returns key itself.
-- A plural-variant table value is resolved to its generic ("other") form so
-- plain lookup never leaks a raw table handle.
-- ===========================================================================
function i18n.t(key)
    if not key or #key == 0 then return key end
    -- Direct lookup
    local val = i18n.strings[key]
    if val ~= nil then return tostring(i18n._plural_form(val)) end
    -- Fallback to English
    val = i18n.fallback[key]
    if val ~= nil then return tostring(i18n._plural_form(val)) end
    -- Return key as-is
    return key
end

-- ===========================================================================
-- i18n.expand(text) -> string with {key} replaced by translations
-- Only replaces {key} tokens in text; non-token content passes through.
-- Inline text markup tag names ({b}/{i}/{s}/{color}/{size}) are whitelisted
-- and never treated as keys — the markup parser wins for those.
-- ===========================================================================
function i18n.expand(text)
    if not text or #text == 0 then return text or "" end
    return (text:gsub("{([%w_]+)}", function(key)
        if MARKUP_NAMES[key] then return "{" .. key .. "}" end
        -- [plural-guard] A string-table key whose value is a PLURAL VARIANT
        -- TABLE ({ one=.., other=.. }) can only pick the right form via
        -- i18n.translate(key, { n = <count> }); a bare {key} token resolves
        -- to the generic ("other") form and LEAVES any {n} placeholder
        -- un-interpolated (round 110 regression: "[ch text=...{items}...]"
        -- rendered a literal "{n} items"). Surface a one-shot diagnostic so
        -- authors route such keys through translate() instead of guessing.
        local rv = i18n.strings[key]
        if rv == nil then rv = i18n.fallback[key] end
        if type(rv) == "table" then
            print(string.format("[i18n] WARN: key %q is a plural-variant table; "
                .. "bare {key} expansion resolves the generic form and leaves "
                .. "{n} un-interpolated. Use i18n.translate(%q, { n = <count> }) "
                .. "inside [iscript]/[emb] instead.", key, key))
        end
        local val = i18n.t(key)
        -- Unknown key: keep the braced form so missing translations stay
        -- visible (and literal {word} text is never destructively mangled).
        if val == key then return "{" .. key .. "}" end
        return val
    end))
end

-- ===========================================================================
-- i18n.localize(text, scene) -> localized dialogue text
--  Precedence:
--    1. Per-line translation: lang file `lines["<scene>:<fnv1a(text)>"]`
--       (Ren'Py-style line translation, content-addressed key).
--    2. {key} token expansion (i18n.expand).
--    3. Original text unchanged.
--  Applied by the KAG text pipeline ([ch]/[text]) BEFORE markup parsing,
--  so translated strings may themselves carry {color}/{size}/{b}/{i} markup.
-- ===========================================================================
function i18n.localize(text, scene)
    if not text or #text == 0 then return text or "" end
    local lineKey = tostring(scene or "") .. ":" .. i18n.fnv1a(text)
    local line = i18n.lines[lineKey]
    if line ~= nil and #tostring(line) > 0 then return tostring(line) end
    return i18n.expand(text)
end

-- ===========================================================================
-- i18n.available() -> { "zh", "en", "ja", ... }
-- Scans assets/lang/ for available .lua files.
-- ===========================================================================
function i18n.available()
    local langs = {"zh"}  -- built-in default
    local fileutil = require("fileutil")
    local files = fileutil.scan_dir("assets/lang", "%.lua$")
    for _, fname in ipairs(files) do
        local code = fname:match("^(.-)%.lua$")
        if code and code ~= "zh" then
            table.insert(langs, code)
        end
    end
    return langs
end

-- ===========================================================================
-- i18n._loadBuiltin(langCode) — minimal built-in tables for when no file exists
-- ===========================================================================
function i18n._builtinStrings(langCode)
    local builtins = {
        zh = {
            title_screen = "标题画面",
            autosave_interval = "自动存档间隔",
            new_game = "新游戏",
            continue = "继续",
            load_game = "读取存档",
            settings = "设置",
            gallery = "CG鉴赏",
            music_room = "音乐室",
            back = "返回",
            yes = "是",
            no = "否",
            save = "保存",
            load = "读取",
            volume_bgm = "BGM音量",
            volume_se = "音效音量",
            volume_voice = "语音音量",
            text_speed = "文字速度",
            fullscreen = "全屏",
            language = "语言",
            skip_mode = "跳过模式",
            skip_auto = "强制跳过",
            auto_mode = "自动模式",
            cc_mode = "字幕模式(CC)",
        },
        en = {
            title_screen = "Title Screen",
            autosave_interval = "Auto-Save Interval",
            new_game = "New Game",
            continue = "Continue",
            load_game = "Load Game",
            settings = "Settings",
            gallery = "CG Gallery",
            music_room = "Music Room",
            back = "Back",
            yes = "Yes",
            no = "No",
            save = "Save",
            load = "Load",
            volume_bgm = "BGM Volume",
            volume_se = "SE Volume",
            volume_voice = "Voice Volume",
            text_speed = "Text Speed",
            fullscreen = "Fullscreen",
            language = "Language",
            skip_mode = "Skip Mode",
            skip_auto = "Force Skip",
            auto_mode = "Auto Mode",
            cc_mode = "Closed Captions",
        },
        ja = {
            title_screen = "タイトル",
            autosave_interval = "オートセーブ間隔",
            new_game = "ニューゲーム",
            continue = "コンティニュー",
            load_game = "ロード",
            settings = "設定",
            gallery = "CG鑑賞",
            music_room = "音楽室",
            back = "戻る",
            yes = "はい",
            no = "いいえ",
            save = "セーブ",
            load = "ロード",
            volume_bgm = "BGM音量",
            volume_se = "SE音量",
            volume_voice = "ボイス音量",
            text_speed = "文字速度",
            fullscreen = "フルスクリーン",
            language = "言語",
            skip_mode = "スキップ",
            skip_auto = "強制スキップ",
            auto_mode = "オート",
        },
    }
    return builtins[langCode] or builtins.zh
end

function i18n._loadBuiltin(langCode)
    i18n.strings = i18n._builtinStrings(langCode)
end

local MAX_DICTIONARY_BYTES = 32 * 1024 * 1024

local function copyDictionary(data, depth)
    if type(data) ~= "table" or depth > 1 then error("Invalid language dictionary", 0) end
    local result = {}
    for key, value in pairs(data) do
        if type(key) ~= "string" then error("Language dictionary keys must be strings", 0) end
        local kind = type(value)
        if kind == "table" then result[key] = copyDictionary(value, depth + 1)
        elseif kind == "string" then result[key] = value
        elseif kind == "number" and value == value and math.abs(value) ~= math.huge then
            result[key] = value
        else error("Unsupported language dictionary value", 0) end
    end
    return result
end

local function prepareDictionary(code)
    if type(code) ~= "string" or not code:match("^[%w_-]+$") then
        error("Invalid language code", 0)
    end
    local file, message, errno = io.open("assets/lang/" .. code .. ".lua", "r")
    if not file then
        -- Native libc uses ENOENT=2; hosts such as Wasmoon have their own
        -- error-number domain (where 2 is EACCES). Classify at the host
        -- boundary, and never treat an unknown/permission error as absence.
        local host = rawget(_G, "backend")
        local classify = type(host) == "table" and host.is_file_missing_error
        local missing
        if type(classify) == "function" then missing = classify(errno) == true
        else missing = errno == 2 end
        if not missing then error(message or "Language dictionary read failed", 0) end
        return copyDictionary(i18n._builtinStrings(code), 0)
    end
    local text = file:read(MAX_DICTIONARY_BYTES + 1)
    file:close()
    if type(text) ~= "string" or #text > MAX_DICTIONARY_BYTES then
        error("Language dictionary exceeds its input limit", 0)
    end
    local body = text:gsub("^\239\187\191", "")
    while body:match("^%s*%-%-") do body = body:gsub("^%s*%-%-[^\n]*\n?", "", 1) end
    if not body:match("^%s*return") then body = "return " .. body end
    local chunk, parse_error = load(body, "i18n:" .. code, "t", {})
    if not chunk then error(parse_error, 0) end
    return copyDictionary(chunk(), 0)
end

-- Snapshot both dictionaries without changing the active language or doing I/O
-- at commit. Restore callers can reject malformed resources before closing A.
local function prepareTranslationDictionary(code)
    local strings = prepareDictionary(code)
    if strings.lines ~= nil then
        if type(strings.lines) ~= "table" then error("Invalid line translation dictionary", 0) end
        for _, line in pairs(strings.lines) do
            if type(line) ~= "string" then error("Line translations must be strings", 0) end
        end
    end
    return strings
end

function i18n.prepare(code, fallbackCode)
    code = code or "zh"
    local strings = prepareTranslationDictionary(code)
    local fallback = {}
    local default = fallbackCode or i18n.default_language or "en"
    if code ~= default then fallback = prepareTranslationDictionary(default) end
    return { current = code, default_language = default,
        strings = strings, lines = strings.lines or {}, fallback = fallback }
end

function i18n.commit(prepared)
    assert(type(prepared) == "table" and type(prepared.current) == "string"
        and type(prepared.strings) == "table" and type(prepared.lines) == "table"
        and type(prepared.fallback) == "table" and type(prepared.default_language) == "string",
        "Invalid prepared locale")
    i18n.current, i18n.strings, i18n.lines, i18n.fallback =
        prepared.current, prepared.strings, prepared.lines, prepared.fallback
    i18n.default_language = prepared.default_language
    return i18n.strings
end

-- ===========================================================================
-- i18n.current_language() -> string
--  Returns the currently selected language code (i18n.current).
--  Pairs with set_language(): the runtime API for switching dictionaries
--  mid-scene (the KAG text pipeline re-localizes via relocalize_page).
-- ===========================================================================
function i18n.current_language()
    return i18n.current
end

-- ===========================================================================
-- i18n.set_language(code, opts) -> strings table
--  Per-language dictionary selection with a fallback chain:
--      current language  ->  default_language  ->  raw key.
--  Selects the dictionary for <code> (loading assets/lang/<code>.lua via
--  i18n.load, or falling back to built-ins when the file is absent), sets
--  i18n.current, and returns the active strings table.
--  opts.default may override the fallback default language for this call
--  (and updates i18n.default_language). Pass opts.reload=true to force a
--  re-read even when <code> equals the current language.
-- ===========================================================================
function i18n.set_language(code, opts)
    opts = opts or {}
    code = code or i18n.current
    if opts.default and type(opts.default) == "string" and #opts.default > 0 then
        i18n.default_language = opts.default
    end
    if not opts.reload and code == i18n.current and next(i18n.strings) ~= nil then
        -- Already on this dictionary; just ensure the fallback is loaded.
        i18n._ensureFallback()
        return i18n.strings
    end
    return i18n.load(code)
end

-- ---------------------------------------------------------------------------
-- i18n._ensureFallback() — (re)load the default-language dictionary if the
--  cached fallback is missing (e.g. set_language with a changed default).
-- ---------------------------------------------------------------------------
function i18n._ensureFallback()
    local dflt = i18n.default_language or "en"
    if next(i18n.fallback) ~= nil or i18n.current == dflt then return end
    local ok2, fb = pcall(function()
        local fp = io.open("assets/lang/" .. dflt .. ".lua", "r")
        if not fp then return nil end
        local txt = fp:read("*a")
        fp:close()
        local body = txt
        while body:match("^%s*%-%-") do
            body = body:gsub("^%s*%-%-[^\n]*\n?", "", 1)
        end
        return (body:match("^%s*return") and load(body, "i18n", "t", {})
            or load("return " .. body, "i18n", "t", {}))()
    end)
    if ok2 and type(fb) == "table" then
        i18n.fallback = fb
    end
end

-- ===========================================================================
-- i18n.translate(text, params) -> string with {placeholder}s interpolated
--  Runtime template interpolation, distinct from i18n.expand (dictionary
--  lookup). Resolves the template (bare whole-key = dictionary value,
--  otherwise the localize path), then fills {name} placeholders from the
--  params table. Unknown placeholders and markup tags stay intact.
--  Plural + numeric format (G9): a string-table VALUE may be a plural
--  variant table, e.g. items = { one = "{n} item", other = "{n} items" }.
--  With params.n present the variant for i18n.plural_category(params.n) is
--  picked and its {n} interpolated to the literal number; without params.n
--  the generic ("other") form is resolved, {n} still interpolable.
--    translate("items", { n = 1 }) -> "1 item"   (en one)
--    translate("items", { n = 3 }) -> "3 items"  (en other)
--    translate("items", { n = 5 }) -> "5" ...    (zh/ja single form)
-- ===========================================================================
function i18n.translate(text, params)
    if not text or #text == 0 then return text or "" end
    -- Resolve the template: a bare whole-key text that names a string-table
    -- entry uses that value (so placeholder templating can be authored as a
    -- {key} value); otherwise fall through the normal localize path.
    local template = text
    local dict = i18n.strings[text]
    if dict == nil then dict = i18n.fallback[text] end
    if dict ~= nil then
        if type(dict) == "table" then
            local count = nil
            if params and params.n ~= nil then
                local nc = tonumber(params.n)
                if nc ~= nil then count = nc end
            end
            template = tostring(i18n._plural_form(dict, count))
        else
            template = tostring(dict)
        end
    else
        template = i18n.localize(text)
    end
    if not params or next(params) == nil then return template end
    return (template:gsub("{([%w_]+)}", function(name)
        if MARKUP_NAMES[name] then return "{" .. name .. "}" end
        if params[name] ~= nil and name ~= "" then
            return tostring(params[name])
        end
        local val = i18n.t(name)
        if val == name then return "{" .. name .. "}" end
        return val
    end))
end

-- ===========================================================================
-- i18n.reload(langCode) -> strings table
--  Hot-reload a language dictionary from disk (re-read assets/lang/<code>.lua
--  even if it is already the current language). Editors / dev workflows can
--  re-run this after editing a lang file without restarting the engine. It
--  preserves i18n.current, i18n.default_language and the cached fallback.
-- ===========================================================================
function i18n.reload(langCode)
    langCode = langCode or i18n.current
    local saved_default = i18n.default_language
    local result = i18n.load(langCode)
    i18n.default_language = saved_default
    return result
end

-- Auto-load default language on module load
pcall(function() i18n.load("zh") end)

return i18n
