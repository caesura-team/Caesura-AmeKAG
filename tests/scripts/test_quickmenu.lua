-- test_quickmenu.lua -- QuickMenu command binding tests
--
-- Save/load/jump tests substitute handlers on the existing KAG table.
-- Modal tests run real handlers and temporarily supply the sandbox-approved
-- host bindings and input state, then restore those values.
package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path

local results = {}
local function check(name, cond, detail)
    if cond then print("PASS " .. name) else print("FAIL " .. name .. " -- " .. tostring(detail)) end
    results[#results + 1] = not not cond
end

local ok_kag, kag = pcall(require, "kag")
check("kag command table loads", ok_kag, tostring(kag))

-- Module resolution: in the engine, kag/init.lua preloads kag.quickmenu before
-- the sandbox locks require() down to package.loaded (scripts/sandbox.lua:186).
-- The suite runs this file AFTER test_sandbox, so the plain require would fail
-- with "not preloaded" even though the production wiring is correct. Compile the
-- real production file and register it exactly like the preload does -- no _G
-- writes, no rawset, and the code under test is the shipped file, not a copy.
-- (io.open under scripts/ and load() from a tests/ caller are both sandbox
-- allowlisted: scripts/sandbox.lua io.open allowlist + load_caller_is_trusted.)
local function load_quickmenu()
    if package.loaded["kag.quickmenu"] ~= nil then
        return true, package.loaded["kag.quickmenu"]
    end
    local ok_req, mod = pcall(require, "kag.quickmenu")
    if ok_req then return true, mod end
    local path = "scripts/kag/quickmenu.lua"
    local f, ferr = io.open(path, "r")
    if not f then return false, "cannot open " .. path .. ": " .. tostring(ferr) end
    local src = f:read("*a")
    f:close()
    -- _ENV must be passed EXPLICITLY: the sandbox load() wrapper
    -- (scripts/sandbox.lua:164) always forwards a 4th argument, so an omitted
    -- env arrives as an explicit nil and Lua sets the chunk's _ENV to nil
    -- ("attempt to index a nil value (upvalue '_ENV')" on the first require).
    local chunk, cerr = load(src, "@" .. path, "t", _G)
    if not chunk then return false, "compile failed: " .. tostring(cerr) end
    local ok_run, result = pcall(chunk)
    if not ok_run then return false, "module body failed: " .. tostring(result) end
    package.loaded["kag.quickmenu"] = result
    return true, result
end

local ok, qm = load_quickmenu()
check("quickmenu module loads", ok, tostring(qm))

if ok and ok_kag then
    -- Record the real handlers so each case restores them afterwards.
    local real_save, real_load, real_jump = kag.save, kag.load, kag.jump

    -- ---- quickmenu_auto -------------------------------------------------------
    do
        local ctx = { auto_mode = false }
        qm.quickmenu_auto(ctx, {})
        check("quickmenu_auto toggles on", ctx.auto_mode == true, tostring(ctx.auto_mode))
        qm.quickmenu_auto(ctx, {})
        check("quickmenu_auto toggles off", ctx.auto_mode == false, tostring(ctx.auto_mode))
        qm.quickmenu_auto(ctx, { mode = "on" })
        check("quickmenu_auto mode=on", ctx.auto_mode == true, tostring(ctx.auto_mode))
        qm.quickmenu_auto(ctx, { mode = "off" })
        check("quickmenu_auto mode=off", ctx.auto_mode == false, tostring(ctx.auto_mode))
        check("quickmenu_auto returns state", qm.quickmenu_auto(ctx, { mode = "on" }) == true, "expected true")
        -- No _toast global is required any more: a missing toast module must not
        -- turn the command into an error (the notification is best-effort).
        local okCall = pcall(qm.quickmenu_auto, { auto_mode = false }, {})
        check("quickmenu_auto survives without toast", okCall, "call errored")
    end

    -- ---- quickmenu_skip -------------------------------------------------------
    do
        local ctx1 = { skip_mode = false }
        qm.quickmenu_skip(ctx1, {})
        check("quickmenu_skip toggles on", ctx1.skip_mode == true, tostring(ctx1.skip_mode))
        qm.quickmenu_skip(ctx1, {})
        check("quickmenu_skip toggles off", ctx1.skip_mode == false, tostring(ctx1.skip_mode))

        local ctx2 = { skip_mode = false }
        qm.quickmenu_skip(ctx2, { mode = "seen" })
        check("quickmenu_skip mode=seen sets seen", ctx2.skip_mode == "seen", tostring(ctx2.skip_mode))
        qm.quickmenu_skip(ctx2, { mode = "seen" })
        check("quickmenu_skip mode=seen toggles off", ctx2.skip_mode == false, tostring(ctx2.skip_mode))
    end

    -- Dispatch each modal through the real scheduler. Only the host's render,
    -- audio and input focus calls are recorded.
    do
        local tokenizer = require("tokenizer")
        local compiler = require("kag.compiler")
        local scheduler = require("scheduler")
        local layers = require("layers")
        require("history_ui")
        require("settings")
        local previous_backend = rawget(_G, "_CAESURA_BACKEND")
        local previous_escape = rawget(_G, "_GAME_KEY_ESC")
        local previous_right = rawget(_G, "_GAME_KEY_RIGHT")
        local previous_kag = rawget(_G, "KAG")
        local focus, rendered = "KAG", {}
        local volume_changes = {}
        rawset(_G, "KAG", setmetatable({
            list_assets = function(dir, max_entries, max_bytes)
                assert(dir == "assets/lang" and max_entries == 4096 and max_bytes == 1024 * 1024)
                return {"en.lua", "ja.lua", "zh.lua"}
            end,
        }, {__index = previous_kag}))
        rawset(_G, "_CAESURA_BACKEND", {
            render = function(method, text)
                if method == "render_text" then rendered[#rendered + 1] = text end
                if method == "create_solid_texture" then return 1 end
                return true
            end,
            platform = function(method, value)
                if method == "get_resolution" then return 1280, 720 end
                if method == "get_input_focus" then return focus end
                if method == "set_input_focus" then focus = value end
                return true
            end,
            audio = function(method, bus, value)
                if method == "get_bus_volume" and bus == "bgm" then return 0.30 end
                if method == "set_bus_volume" then volume_changes[#volume_changes + 1] = {bus, value} end
                return true
            end,
        })

        local function run_modal(command, lua_focus, host_focus, panel_name, expected_text, outcome)
            rendered = {}
            volume_changes = {}
            rawset(_G, "_GAME_KEY_ESC", false)
            rawset(_G, "_GAME_KEY_RIGHT", false)
            local tokens = tokenizer.parse("[" .. command .. "][set f.resumed 1][end]")
            compiler.compile(tokens)
            local saved_settings = {volume_bgm = 80}
            local ctx = {
                f = {}, sf = {}, tf = {}, tokens = tokens, current_scene = "menu.ks",
                input_focus = "kag", backlog = {{text = "Previous dialogue", scene = "menu.ks", token_index = 1}},
                auto_mode = true, skip_mode = "seen", text_speed = 85, cc_mode = true,
                settingsValues = saved_settings,
            }
            local co = coroutine.create(function() scheduler.run(ctx, tokens, 1) end)
            local resumed, err = coroutine.resume(co, 16)
            check(command .. " opens a modal with the expected input focus",
                resumed and coroutine.status(co) == "suspended" and ctx.input_focus == lua_focus
                    and focus == host_focus and not ctx._command_error, err or ctx.input_focus)
            local panel = layers.get_layer(panel_name)
            check(command .. " renders the panel",
                panel and panel.visible and table.concat(rendered, "\n"):find(expected_text, 1, true),
                panel_name)
            if coroutine.status(co) == "suspended" then coroutine.resume(co, 16) end
            check(command .. " holds the next command until dismissal", ctx.f.resumed == nil, ctx.f.resumed)

            if lua_focus == "settings" and coroutine.status(co) == "suspended" then
                local competing = coroutine.create(function() kag.history(ctx, {}) end)
                local opened = coroutine.resume(competing)
                check("history cannot take focus from settings",
                    opened and coroutine.status(competing) == "dead" and ctx.input_focus == "settings", ctx.input_focus)
                coroutine.close(competing)
                if outcome ~= "dismiss" then
                    rendered = {}
                    rawset(_G, "_GAME_KEY_RIGHT", true)
                    local adjusted, adjust_error = coroutine.resume(co, 16)
                    check("settings displays the edited BGM volume",
                        adjusted and table.concat(rendered, "\n"):find("35%", 1, true), adjust_error)
                end
            end
            if outcome == "cancel" then
                coroutine.close(co)
                check(command .. " cancellation does not advance the script", ctx.f.resumed == nil, ctx.f.resumed)
            elseif coroutine.status(co) == "suspended" then
                rawset(_G, "_GAME_KEY_ESC", true)
                local closed, close_error = coroutine.resume(co, 16)
                check(command .. " consumes Esc", closed and not rawget(_G, "_GAME_KEY_ESC"), close_error)
                for _ = 1, 3 do
                    if coroutine.status(co) == "dead" then break end
                    closed, close_error = coroutine.resume(co, 16)
                    if not closed then break end
                end
                check(command .. " resumes the script",
                    closed and coroutine.status(co) == "dead" and ctx.f.resumed == 1, close_error)
                coroutine.close(co)
            end
            check(command .. " restores focus and hides its panel",
                ctx.input_focus == "kag" and focus == "KAG" and panel and not panel.visible,
                ctx.input_focus)
            if lua_focus == "settings" then
                check("settings preserves unrelated live dialogue settings",
                    ctx.auto_mode == true and ctx.skip_mode == "seen" and ctx.text_speed == 85 and ctx.cc_mode == true,
                    ctx.skip_mode)
                if outcome == "adjust" then
                    check("settings applies only the edited BGM volume",
                        #volume_changes == 1 and volume_changes[1][1] == "bgm" and volume_changes[1][2] == 0.35,
                        #volume_changes)
                else
                    check("settings without edits leaves volumes unchanged", #volume_changes == 0, #volume_changes)
                    check("settings restores the original values after dismissal or cancellation",
                        ctx.settingsValues == saved_settings and saved_settings.volume_bgm == 80, saved_settings.volume_bgm)
                end
            end
        end

        run_modal("quickmenu_log", "history", "KAG", "_history_bg", "Previous dialogue", "dismiss")
        run_modal("quickmenu_config", "settings", "GAME", "_settings_panel", "Settings /", "cancel")
        run_modal("quickmenu_config", "settings", "GAME", "_settings_panel", "Settings /", "dismiss")
        run_modal("quickmenu_config", "settings", "GAME", "_settings_panel", "Settings /", "adjust")
        rawset(_G, "_GAME_KEY_ESC", previous_escape)
        rawset(_G, "_GAME_KEY_RIGHT", previous_right)
        rawset(_G, "_CAESURA_BACKEND", previous_backend)
        rawset(_G, "KAG", previous_kag)
    end

    -- ---- quickmenu_qsave: routes to the registered [save] handler -------------
    do
        local seen = nil
        kag.save = function(c, params)
            seen = { ctx = c, slot = params and params.slot }
            c.tf = c.tf or {}
            c.tf.save_result = "ok"
        end
        local ctx = {}
        local saved = qm.quickmenu_qsave(ctx, { slot = 3 })
        check("quickmenu_qsave calls [save]", seen ~= nil, "handler not called")
        check("quickmenu_qsave forwards slot", seen and seen.slot == 3, tostring(seen and seen.slot))
        check("quickmenu_qsave reports ok", saved == true, tostring(saved))
        check("quickmenu_qsave leaves save_result", ctx.tf and ctx.tf.save_result == "ok",
              tostring(ctx.tf and ctx.tf.save_result))

        -- Failure must be reported honestly: [save] signals errors through
        -- ctx.tf.save_result, NEVER through its return value (always nil).
        kag.save = function(c, _params)
            c.tf = c.tf or {}
            c.tf.save_result = "error"
        end
        local ctx2 = {}
        check("quickmenu_qsave reports failure", qm.quickmenu_qsave(ctx2, { slot = 1 }) == false,
              "expected false on save_result=error")

        -- A throwing [save] must not escape the command.
        kag.save = function() error("boom") end
        local ctx3 = {}
        local okCall, res = pcall(qm.quickmenu_qsave, ctx3, { slot = 2 })
        check("quickmenu_qsave contains handler error", okCall and res == false, tostring(res))

        -- Slot clamping (schema allows 0..99; direct callers may exceed it).
        kag.save = function(c, params)
            seen = { slot = params and params.slot }
            c.tf = c.tf or {}
            c.tf.save_result = "ok"
        end
        qm.quickmenu_qsave({}, { slot = 250 })
        check("quickmenu_qsave clamps high slot", seen and seen.slot == 99, tostring(seen and seen.slot))
        qm.quickmenu_qsave({}, { slot = -5 })
        check("quickmenu_qsave clamps negative slot", seen and seen.slot == 0, tostring(seen and seen.slot))
        qm.quickmenu_qsave({}, {})
        check("quickmenu_qsave defaults to slot 0", seen and seen.slot == 0, tostring(seen and seen.slot))

        kag.save = real_save
    end

    -- ---- quickmenu_qload: routes to the registered [load] handler -------------
    do
        local seen = nil
        kag.load = function(c, params)
            seen = { slot = params and params.slot }
            c.tf = c.tf or {}
            c.tf.load_result = "ok"
        end
        check("quickmenu_qload reports ok", qm.quickmenu_qload({}, { slot = 7 }) == true, "expected true")
        check("quickmenu_qload forwards slot", seen and seen.slot == 7, tostring(seen and seen.slot))

        kag.load = function(c, _params)
            c.tf = c.tf or {}
            c.tf.load_result = "error"
        end
        check("quickmenu_qload reports failure", qm.quickmenu_qload({}, { slot = 0 }) == false,
              "expected false on load_result=error")

        kag.load = real_load
    end

    -- ---- quickmenu_title: delegates to kag.jump ------------------------------
    do
        local seen = nil
        kag.jump = function(_c, target) seen = target end
        check("quickmenu_title returns true", qm.quickmenu_title({}, {}) == true, "expected true")
        check("quickmenu_title defaults to title label", seen == "title", tostring(seen))
        qm.quickmenu_title({}, { scene = "chapter2" })
        check("quickmenu_title forwards scene", seen == "chapter2", tostring(seen))
        kag.jump = real_jump
    end

    -- ---- Registration into the kag command table -----------------------------
    check("quickmenu_auto registered in kag", type(kag.quickmenu_auto) == "function", type(kag.quickmenu_auto))
    check("quickmenu_skip registered in kag", type(kag.quickmenu_skip) == "function", type(kag.quickmenu_skip))
    check("quickmenu_log registered in kag",  type(kag.quickmenu_log) == "function",  type(kag.quickmenu_log))
    check("quickmenu_qsave registered in kag", type(kag.quickmenu_qsave) == "function", type(kag.quickmenu_qsave))
    check("quickmenu_qload registered in kag", type(kag.quickmenu_qload) == "function", type(kag.quickmenu_qload))
    check("quickmenu_title registered in kag", type(kag.quickmenu_title) == "function", type(kag.quickmenu_title))
    check("quickmenu_config registered in kag", type(kag.quickmenu_config) == "function", type(kag.quickmenu_config))

    -- ---- Schema contracts ----------------------------------------------------
    do
        local ok_schema, schema = pcall(require, "kag.schema")
        check("schema module available", ok_schema, tostring(schema))
        if ok_schema and type(schema.specs) == "function" then
            local spec = schema.specs("quickmenu_qsave")
            check("quickmenu_qsave schema registered", type(spec) == "table", tostring(spec))
            if type(spec) == "table" then
                check("quickmenu_qsave schema has slot", spec.slot ~= nil, "missing slot spec")
            end
            check("quickmenu_auto schema registered", type(schema.specs("quickmenu_auto")) == "table",
                  tostring(schema.specs("quickmenu_auto")))
        end
    end
end

-- Summary
local passed = 0
for _, r in ipairs(results) do if r then passed = passed + 1 end end
print(string.format("Results: %d/%d passed", passed, #results))
if passed < #results then os.exit(1) end
