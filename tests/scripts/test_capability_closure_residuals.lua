-- test_capability_closure_residuals.lua — U36 closure-residual runtime evidence
--
-- Scope (docs/plans/2026-09-05-001 §5.1.2/U36):
--   * PARTIAL one-hop chains whose effect surface sits after a nested local
--     function (blur/flash/chapter): real production modules are loaded from
--     disk and executed against a recording backend boundary;
--   * stopvideo owner-handle consumption (v3 conservative hold adjudication);
--   * the 7 dynamically registered quickmenu_* commands resolve through the
--     real scheduler entry (kag.schema.resolveHandler on the kag dispatch table);
--   * the 5 explicitly-rejected commands are refused by the real capability
--     runtime (exact feature/reason, zero effect calls) and the SDK-off
--     steam_achievement handler records "unavailable" without raising.
--
-- This is run-surface evidence, not pixel/GPU evidence. Capability refusal is
-- checked through the shipped capability runtime + generated catalog; the
-- direct handler chains below bypass that guard on purpose (they prove the
-- static wiring the closure matrix grades, while the refusal block proves the
-- runtime outcome).
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path
local passed, failed = 0, 0
local function check(name, cond, detail)
    if cond then passed = passed + 1
    else failed = failed + 1; print("[FAIL] " .. name .. (detail and (" -- " .. tostring(detail)) or "")) end
end

local function read_lua(path)
    local f, err = io.open(path, "r")
    if not f then error("cannot open " .. path .. ": " .. tostring(err)) end
    local src = f:read("*a")
    f:close()
    return src
end

-- Compile the real production file with an explicit _ENV (sandbox load()
-- forwards a 4th argument; tests/scripts callers are trusted, same approach as
-- test_quickmenu.lua).
local function load_fresh(path)
    local src = read_lua(path)
    -- Checkout may preserve a UTF-8 BOM on tracked Lua files; load() rejects it.
    if src:sub(1, 3) == "\239\187\191" then src = src:sub(4) end
    local chunk, err = load(src, "@" .. path, "t", _G)
    if not chunk then error("compile failed for " .. path .. ": " .. tostring(err)) end
    local ok, result = pcall(chunk)
    if not ok then error("module body failed for " .. path .. ": " .. tostring(result)) end
    return result
end

local function load_registered(name, path)
    if package.loaded[name] ~= nil then return package.loaded[name] end
    local ok, mod = pcall(require, name)
    if ok then return mod end
    local fresh = load_fresh(path)
    package.loaded[name] = fresh
    return fresh
end

local function recording_backend()
    local calls = {}
    local stub = {
        render = function(cmd, ...)
            calls[#calls + 1] = { cmd = cmd, args = { ... } }
            if cmd == "create_solid_texture" then return 700 + #calls end
            if cmd == "video_play" then return 77 end
            return true
        end,
        platform = function(cmd, ...)
            if cmd == "get_resolution" or cmd == "get_window_size" then return 1280, 720 end
            if cmd == "get_input_focus" then return "KAG" end
            return true
        end,
    }
    return stub, calls
end

local function count_calls(calls, cmd)
    local n = 0
    for _, call in ipairs(calls) do if call.cmd == cmd then n = n + 1 end end
    return n
end

local function first_call(calls, cmd)
    for _, call in ipairs(calls) do if call.cmd == cmd then return call end end
    return nil
end

local function resume_until_done(co, frame_ms)
    while coroutine.status(co) ~= "dead" do
        local ok, err = coroutine.resume(co, frame_ms or 16)
        if not ok then return false, err end
    end
    return true
end

-- ===========================================================================
-- 1. PARTIAL chains: nested-local-function effect surfaces (U36/B1 scanner fix)
-- ===========================================================================
local VFX = load_fresh("scripts/vfx.lua")
local saved_backend = rawget(_G, "_CAESURA_BACKEND")
local stub, calls = recording_backend()
rawset(_G, "_CAESURA_BACKEND", stub)

-- 1a. [blur] handler -> VFX.blur -> rtt.alloc/free + backend.submit_vfx(effect 2)
do
    local saved_rtt = package.loaded["rtt"]
    local alloc_count, free_count = 0, 0
    package.loaded["rtt"] = {
        alloc = function() alloc_count = alloc_count + 1; return 500 + alloc_count end,
        free = function() free_count = free_count + 1 end,
        destroy = function() free_count = free_count + 1 end,
    }
    local saved_vfx = package.loaded["vfx"]
    package.loaded["vfx"] = VFX
    local TransCommands = load_registered("kag.commands.transition", "scripts/kag/commands/transition.lua")
    local ctx = { f = {}, sf = {}, tf = {}, mp = {}, variables = {}, _bgTexture = 42 }
    local ok, err = pcall(TransCommands.blur, ctx, { time = 32, strength = 3 })
    package.loaded["vfx"] = saved_vfx
    package.loaded["rtt"] = saved_rtt
    local submit = first_call(calls, "submit_vfx")
    check("blur handler executes without raising", ok, err)
    check("blur allocates both RTT temporaries", alloc_count == 2, tostring(alloc_count))
    check("blur frees both RTT temporaries", free_count == 2, tostring(free_count))
    check("blur submits effect 2 to backend.submit_vfx", submit ~= nil
        and submit.args[2] == 2 and submit.args[1] == 42, submit and tostring(submit.args[1]) .. "/" .. tostring(submit.args[2]))
    check("blur submits a positive radius", submit ~= nil and tonumber(submit.args[7]) ~= nil
        and tonumber(submit.args[7]) > 0, submit and tostring(submit.args[7]))
end

-- 1b. [flash] handler -> VFX.flash -> backend.create_solid_texture (clamped)
do
    stub, calls = recording_backend()
    rawset(_G, "_CAESURA_BACKEND", stub)
    local saved_vfx = package.loaded["vfx"]
    package.loaded["vfx"] = VFX
    local VFXCommands = load_fresh("scripts/kag/commands/vfx.lua")
    package.loaded["vfx"] = saved_vfx
    local ctx = { f = {}, sf = {}, tf = {}, mp = {}, variables = {} }
    local co = coroutine.create(function() VFXCommands.flash(ctx, { time = 32, r = 300, g = -5, b = 128 }) end)
    local ok, err = resume_until_done(co)
    local solid = first_call(calls, "create_solid_texture")
    check("flash blocking chain completes", ok, err)
    check("flash creates a solid texture", solid ~= nil and count_calls(calls, "create_solid_texture") == 1,
        tostring(count_calls(calls, "create_solid_texture")))
    check("flash clamps colour bytes", solid ~= nil and solid.args[1] == 255
        and solid.args[2] == 0 and solid.args[3] == 128 and solid.args[4] == 255,
        solid and table.concat({ tostring(solid.args[1]), tostring(solid.args[2]), tostring(solid.args[3]), tostring(solid.args[4]) }, ","))
    check("flash registers the __flash layer", ctx.layers ~= nil and ctx.layers["__flash"] ~= nil
        and ctx.layers["__flash"].texture ~= nil)
end

-- 1c. [chapter] handler -> ChapterSelect.show -> backend.render_text/layers.ensure
do
    stub, calls = recording_backend()
    rawset(_G, "_CAESURA_BACKEND", stub)
    local saved_layers = package.loaded["layers"]
    local fake_layer = { visible = false }
    package.loaded["layers"] = {
        ensure = function() return fake_layer end,
        get_layer = function() return fake_layer end,
        find = function() return fake_layer end,
        set_layer_visible = function() end,
        move_layer = function() end,
        mark_dirty = function() end,
        forEach = function() end,
    }
    local saved_chapter = package.loaded["chapter_select"]
    local ChapterSelect = load_fresh("scripts/chapter_select.lua")
    package.loaded["chapter_select"] = ChapterSelect
    local System = load_registered("kag.commands.system", "scripts/kag/commands/system.lua")
    local ctx = {
        current_scene = "s.ks", labelMap = { chapter_1 = 5, chapter_2 = 40 },
        _visited_labels = {}, seen_scenes = {}, f = {}, sf = {}, tf = {}, mp = {}, variables = {},
        input_focus = "kag",
    }
    _G._GAME_KEY_UP, _G._GAME_KEY_DOWN, _G._GAME_KEY_ENTER, _G._GAME_KEY_ESC = false, false, false, false
    local co = coroutine.create(function() return System.chapter(ctx, {}) end)
    local first_ok, first_err = coroutine.resume(co)
    _G._GAME_KEY_ESC = true
    local second_ok, chosen = coroutine.resume(co)
    while coroutine.status(co) ~= "dead" do coroutine.resume(co) end
    package.loaded["chapter_select"] = saved_chapter
    package.loaded["layers"] = saved_layers
    check("chapter overlay renders the first frame", first_ok and count_calls(calls, "render_text") >= 3,
        tostring(first_err or count_calls(calls, "render_text")))
    check("chapter Esc cancel completes without a jump", second_ok and chosen == nil, tostring(chosen))
    check("chapter cancel leaves no pending jump", ctx._pendingJump == nil)
    check("chapter draws through backend.render_text after nested cleanup", count_calls(calls, "render_text") >= 3)
end

-- 1d. [stopvideo] handler -> close_playback -> backend.video_stop (owner handle)
do
    stub, calls = recording_backend()
    rawset(_G, "_CAESURA_BACKEND", stub)
    local Video = load_registered("kag.commands.video", "scripts/kag/commands/video.lua")
    local ctx = { f = {}, sf = {}, tf = {}, mp = {}, variables = {}, _videoPlayback = { handle = 77 } }
    local ok = pcall(Video.stopvideo, ctx)
    check("stopvideo handler executes", ok)
    local stop = first_call(calls, "video_stop")
    check("stopvideo consumes its owner handle once", stop ~= nil and stop.args[1] == 77
        and count_calls(calls, "video_stop") == 1)
    check("stopvideo clears the playback owner", ctx._videoPlayback == nil)
    pcall(Video.stopvideo, ctx)
    pcall(Video.stopvideo, {})
    check("stopvideo is idempotent and never stops an unrelated owner",
        count_calls(calls, "video_stop") == 1, tostring(count_calls(calls, "video_stop")))
end

rawset(_G, "_CAESURA_BACKEND", saved_backend)

-- ===========================================================================
-- 2. quickmenu_*: real scheduler dispatch table + real scheduler entry
-- ===========================================================================
do
    local kag = load_registered("kag", "scripts/kag.lua")
    local schema = load_registered("kag.schema", "scripts/kag/schema.lua")
    -- kag/init.lua:30 preloads kag.quickmenu; load_registered reproduces that
    -- order (kag already cached) so register_all() writes into the real table.
    local quickmenu = load_registered("kag.quickmenu", "scripts/kag/quickmenu.lua")
    local names = {
        "quickmenu_auto", "quickmenu_skip", "quickmenu_log",
        "quickmenu_qsave", "quickmenu_qload", "quickmenu_title", "quickmenu_config",
    }
    for _, name in ipairs(names) do
        local handler = schema.resolveHandler(name, kag)
        check(name .. " resolves through schema.resolveHandler (scheduler entry)", type(handler) == "function",
            type(handler))
        check(name .. " is registered on the kag dispatch table", type(kag[name]) == "function", type(kag[name]))
        check(name .. " handler is the quickmenu implementation", kag[name] == quickmenu[name])
    end
    -- State-only handlers: the effect is the documented ctx write consumed by
    -- the runner (auto_mode kag_runner.lua:960, skip_mode :888/:923). The other
    -- five delegate to registered kag.save/load/jump handlers.
    local ctx = { auto_mode = false, skip_mode = false }
    quickmenu.quickmenu_auto(ctx, { mode = "on" })
    quickmenu.quickmenu_skip(ctx, { mode = "on" })
    quickmenu.quickmenu_log(ctx, {})
    quickmenu.quickmenu_config(ctx, {})
    check("quickmenu_auto writes the runner-consumed auto_mode flag", ctx.auto_mode == true)
    check("quickmenu_skip writes the runner-consumed skip_mode flag", ctx.skip_mode == true)
    check("quickmenu_log records its state (no consumer yet; tracked as PARTIAL)", ctx.backlog_visible == true)
    check("quickmenu_config records its state (no consumer yet; tracked as PARTIAL)", ctx.config_open == true)
end

-- ===========================================================================
-- 3. Explicitly-rejected commands: real capability runtime refusal
-- ===========================================================================
do
    local json = load_registered("capability_json", "scripts/capability_json.lua")
    local catalog = load_registered("capability_catalog", "scripts/capability_catalog.lua")
    local policy = load_registered("target_capabilities", "scripts/target_capabilities.lua")
    local data = assert(json.decode(rawget(catalog, "json")))
    local facts = {
        schema = 1, target = "native", scope = "runtime", platform = "windows",
        catalog_sha256 = policy.catalog_sha256,
        compiled = { ffmpeg = false, live2d = false, steam = false },
        available = {},
    }
    for _, feature in pairs(data.features) do
        if feature.native and feature.native.available then
            facts.available[feature.native.available] = true
        end
    end
    local saved_engine = rawget(_G, "Engine")
    rawset(_G, "Engine", { get_capability_profile = function() return facts end })
    local runtime = load_fresh("scripts/capability_runtime.lua")
    rawset(_G, "Engine", saved_engine)
    check("capability runtime captures the native host", runtime.has_host() == true)

    local expected = {
        { command = "blur", feature = "render.blur", reason = "command_not_wired" },
        { command = "xfadebgm", feature = "audio.crossfade", reason = "backend_route_not_wired" },
        { command = "live2d_motion", feature = "kag.live2d_motion", reason = "command_not_wired" },
        { command = "live2d_expression", feature = "kag.live2d_expression", reason = "command_not_wired" },
        { command = "steam_achievement", feature = "steam.achievements", reason = "sdk_disabled" },
    }
    local policy_text = '{"capabilities":{"optional":["render.blur",'
        .. '"audio.crossfade","kag.live2d_motion","kag.live2d_expression",'
        .. '"steam.achievements"]}}'
    check("optional capability policy commits for the rejection cases",
        runtime.configure_project_json(policy_text) == true)
    for _, case in ipairs(expected) do
        local query = runtime.query(case.feature)
        check(case.feature .. " catalog status is unsupported", query.status == "unsupported", tostring(query.status))
        check(case.feature .. " catalog reason matches", query.reason == case.reason, tostring(query.reason))
        local features = policy.command_features(case.command, {}, false)
        check(case.command .. " maps to " .. case.feature, #features == 1 and features[1] == case.feature)
        local effect_calls = 0
        local sink = function() effect_calls = effect_calls + 1; return true end
        local ctx = { current_scene = "refusal.ks", token_index = 1 }
        local ok, applied, result = pcall(runtime.invoke_command, sink, ctx, {}, case.command)
        check(case.command .. " refusal does not raise", ok)
        check(case.command .. " refusal never reaches the handler", applied == false and effect_calls == 0,
            tostring(applied) .. "/" .. tostring(effect_calls))
        check(case.command .. " refusal reports the exact feature/reason",
            result and result.status == "unsupported" and result.feature == case.feature
                and result.reason == case.reason,
            result and (tostring(result.status) .. " " .. tostring(result.feature) .. " " .. tostring(result.reason)))
        check(case.command .. " refusal records one capability diagnostic",
            ctx.capability_diagnostics ~= nil and #ctx.capability_diagnostics == 1
                and ctx.capability_diagnostics_dropped == 0)
    end
end

-- ===========================================================================
-- 4. SDK-off steam_achievement handler: records "unavailable", never raises
-- ===========================================================================
do
    local System = load_registered("kag.commands.system", "scripts/kag/commands/system.lua")
    local saved_steam_global = rawget(_G, "steam")
    local saved_steam_module = package.loaded["steam"]
    rawset(_G, "steam", nil)
    package.loaded["steam"] = nil
    local ctx = { f = {}, sf = {}, tf = {}, mp = {}, variables = {} }
    local ok, result = pcall(System.steam_achievement, ctx, { id = "ACH_U36_SDK_OFF", silent = true })
    rawset(_G, "steam", saved_steam_global)
    package.loaded["steam"] = saved_steam_module
    check("SDK-off steam_achievement does not raise", ok, result)
    check("SDK-off steam_achievement returns true (story never stalls)", result == true, tostring(result))
    check("SDK-off steam_achievement records the unavailable outcome",
        ctx.tf ~= nil and ctx.tf.steam_achievement_result == "unavailable",
        ctx.tf and tostring(ctx.tf.steam_achievement_result))
end

print(string.format("Capability Closure Residuals: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
print("CAPABILITY CLOSURE RESIDUALS TESTS DONE")
