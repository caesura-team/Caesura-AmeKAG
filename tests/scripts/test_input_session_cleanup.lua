-- U10: real command/Operation scopes release modal input on coroutine close.
-- Load production Lua into a private environment so shared-suite handlers and
-- package.loaded entries cannot hide a stale callback or leak into other tests.
local passed, failed = 0, 0
local function check(name, condition)
    if condition then
        passed = passed + 1
        print("PASS " .. name)
    else
        failed = failed + 1
        print("FAIL " .. name)
    end
end

local handler_names = {
    "_KAG_onTextInput", "_KAG_onTextEditing", "_KAG_onKeyDown", "_KAG_onClick",
}

local function fixture()
    local env = setmetatable({}, { __index = _G })
    env._G = env
    env.Restore = {
        capture_font = function() return {version=1,active=true,font=0,path="",size=16} end,
        prepare_font = function(snapshot) return snapshot end,
        apply_font = function() return true end,
        discard_font = function() end,
    }
    local state = { starts = 0, stops = 0, active = false, forwarded = 0 }
    local originals = {}
    for _, name in ipairs(handler_names) do
        originals[name] = function() state.forwarded = state.forwarded + 1 end
        env[name] = originals[name]
    end
    local modules = {
        backend = {
            create_solid_texture = function() return 101 end,
            create_viewport = function() return 501 end,
            destroy_viewport = function() return true end,
            destroy_texture = function() return true end,
            text_set_font = function() return true end,
            line_height = function() return 24 end,
            get_resolution = function() return 1280, 720 end,
            set_text_input_rect = function(x, y, w, h)
                state.rect = { x = x, y = y, w = w, h = h }
            end,
            start_text_input = function()
                state.starts = state.starts + 1
                state.active = true
                if state.fail_start then error("text input startup failed") end
            end,
            stop_text_input = function()
                state.stops = state.stops + 1
                state.active = false
            end,
        },
    }
    env.require = function(name)
        if modules[name] == nil then
            local path = "scripts/" .. name:gsub("%.", "/") .. ".lua"
            local file = assert(io.open(path, "r"))
            local source = file:read("*a")
            file:close()
            -- Match loadfile's UTF-8 BOM handling while retaining the private env.
            source = source:gsub("^\239\187\191", "")
            modules[name] = assert(load(source, "@" .. path, "t", env))()
        end
        return modules[name]
    end
    return {
        env = env, state = state, originals = originals,
        commands = env.require("kag.commands.text"),
        operation = env.require("kag.operation"),
        scene = env.require("kag.text_scene"),
    }
end

local function context()
    return { f = {}, tf = {}, sf = {}, mp = {}, _session_active = true }
end

local function start(f, command, ctx, params)
    local co = coroutine.create(function() f.commands[command](ctx, params or {}) end)
    local ok, err = coroutine.resume(co)
    assert(ok, tostring(err))
    assert(coroutine.status(co) == "suspended", "command did not wait for input")
    return co
end

local function resume(co)
    local ok, err = coroutine.resume(co)
    assert(ok, tostring(err))
end

local function choice_context()
    local ctx = context()
    ctx._choiceButtons = {
        { text = "Route A", target = "*route_a", x = "f.result" },
        { text = "Route B", target = "*route_b", x = "f.result" },
    }
    return ctx
end

local function click_choice(f, ctx, index, handler)
    f.env._GAME_MOUSE_X = 100
    f.env._GAME_MOUSE_Y = ctx._choiceButtonsActive[index].y + 5
    local click_handler = handler or f.env._KAG_onClick
    click_handler()
end

local function restored(f)
    for _, name in ipairs(handler_names) do
        if f.env[name] ~= f.originals[name] then return false end
    end
    return true
end

local function no_draws(f, ctx)
    return #f.scene.get_state(ctx).draws == 0
end

do
    local f, ctx = fixture(), choice_context()
    local co = start(f, "endbutton", ctx)
    local stale = f.env._KAG_onClick
    check("choice registers an active operation", #(ctx.active_operations or {}) == 1)
    f.env._GAME_MOUSE_X, f.env._GAME_MOUSE_Y = 100, 455
    assert(coroutine.close(co))
    check("choice close restores previous click handler", restored(f))
    check("choice close clears modal state", ctx._choiceMode == false
        and ctx.waiting_input == false and ctx._choiceButtonsActive == nil)
    check("choice close removes its UI and operation", no_draws(f, ctx)
        and #(ctx.active_operations or {}) == 0)
    stale()
    check("closed choice callback cannot select or forward", ctx._selectedChoice == nil
        and ctx._pendingJump == nil and f.state.forwarded == 0)
    local next_ctx = choice_context()
    local next_co = start(f, "endbutton", next_ctx)
    local next_handler = f.env._KAG_onClick
    stale()
    check("old choice callback leaves next session untouched", next_ctx._choiceMode == true
        and next_ctx._selectedChoice == nil and f.env._KAG_onClick == next_handler)
    click_choice(f, next_ctx, 2)
    resume(next_co)
    check("next choice completes normally", next_ctx._pendingJump == "*route_b"
        and next_ctx.f.result == "*route_b" and no_draws(f, next_ctx))
end

do
    local f, ctx = fixture(), choice_context()
    local co = start(f, "endbutton", ctx)
    click_choice(f, ctx, 1)
    local replacement = function() end
    f.env._KAG_onClick = replacement
    resume(co)
    check("choice completion preserves later click owner", f.env._KAG_onClick == replacement)
    check("choice completion saves selected target and releases scope", ctx.f.result == "*route_a"
        and ctx._pendingJump == "*route_a" and #(ctx.active_operations or {}) == 0)
end

do
    local f, ctx = fixture(), choice_context()
    local co = start(f, "endbutton", ctx)
    click_choice(f, ctx, 1)
    f.operation.cancel_all(ctx)
    resume(co)
    check("cancelled choice cannot commit a pending selection", ctx.f.result == nil
        and ctx._pendingJump == nil and ctx._selectedChoice == nil and no_draws(f, ctx))
end

do
    local f, ctx = fixture(), context()
    local co = start(f, "input", ctx, { name = "f.name", default = "A", btn_cancel = "Cancel" })
    local stale = {}
    for _, name in ipairs(handler_names) do stale[name] = f.env[name] end
    check("text input starts native input and an operation", f.state.active
        and f.state.starts == 1 and #(ctx.active_operations or {}) == 1)
    assert(coroutine.close(co))
    check("text input close stops native input exactly once", not f.state.active and f.state.stops == 1)
    check("text input close restores all handlers", restored(f))
    check("text input close clears modes, UI and operation", ctx._inputMode == false
        and ctx.waiting_input == false and no_draws(f, ctx) and #(ctx.active_operations or {}) == 0)
    local next_ctx = context()
    local next_co = start(f, "input", next_ctx, { name = "f.name", default = "B" })
    local next_handler = f.env._KAG_onTextInput
    stale._KAG_onTextInput("late")
    stale._KAG_onTextEditing("composition", 0, 1)
    stale._KAG_onKeyDown(13, "return")
    f.env._GAME_MOUSE_X, f.env._GAME_MOUSE_Y = 0, 0
    stale._KAG_onClick()
    check("closed text input callbacks cannot change A or B", ctx.f.name == nil
        and no_draws(f, ctx) and next_ctx._inputMode == true and next_ctx.f.name == nil
        and f.env._KAG_onTextInput == next_handler and f.state.stops == 1
        and f.state.forwarded == 0)
    f.env._KAG_onTextInput("eta")
    f.env._KAG_onKeyDown(13, "return")
    resume(next_co)
    check("next text input saves only its own result", next_ctx.f.name == "Beta"
        and ctx.f.name == nil and not f.state.active and f.state.stops == 2 and restored(f))
end

do
    local f, ctx = fixture(), context()
    local co = start(f, "input", ctx, { name = "f.name", default = "original" })
    local replacements = {}
    for _, name in ipairs(handler_names) do
        replacements[name] = function() end
        f.env[name] = replacements[name]
    end
    f.operation.cancel_all(ctx)
    assert(coroutine.close(co))
    local preserved = true
    for _, name in ipairs(handler_names) do
        preserved = preserved and f.env[name] == replacements[name]
    end
    check("input cancellation preserves all later callback owners", preserved)
    check("cancel then close stops input only once without saving", f.state.stops == 1
        and ctx.f.name == nil and no_draws(f, ctx))
end

for _, finish_key in ipairs({ "return", "escape" }) do
    local f, ctx = fixture(), context()
    local co = start(f, "input", ctx, { name = "f.name", default = "typed" })
    local stale_key = f.env._KAG_onKeyDown
    stale_key(finish_key == "return" and 13 or 27, finish_key)
    check("input " .. finish_key .. " releases UI and native input", restored(f)
        and f.state.stops == 1 and no_draws(f, ctx) and ctx.waiting_input == false)
    resume(co)
    stale_key(13, "return")
    check("input " .. finish_key .. " preserves result on late callback", ctx.f.name
        == (finish_key == "return" and "typed" or nil) and f.state.stops == 1
        and #(ctx.active_operations or {}) == 0 and f.state.forwarded == 0)
end

do
    local f, ctx = fixture(), context()
    f.state.fail_start = true
    local co = coroutine.create(function()
        f.commands.input(ctx, { name = "f.name", default = "discarded" })
    end)
    local ok, err = coroutine.resume(co)
    check("native input startup failure propagates", not ok
        and tostring(err):find("text input startup failed", 1, true) ~= nil)
    coroutine.close(co)
    check("native input startup failure releases acquired input", f.state.starts == 1
        and f.state.stops == 1 and not f.state.active and restored(f)
        and no_draws(f, ctx) and #(ctx.active_operations or {}) == 0 and ctx.f.name == nil)
end

do
    local f, ctx = fixture(), context()
    local co = coroutine.create(function()
        f.commands.input(ctx, { name = "f.name", default = "discarded", btn_ok = {} })
    end)
    local ok = coroutine.resume(co)
    check("input UI failure propagates", not ok)
    coroutine.close(co)
    check("input UI failure releases partial UI and native input", f.state.starts == 1
        and f.state.stops == 1 and not f.state.active and restored(f)
        and no_draws(f, ctx) and #(ctx.active_operations or {}) == 0 and ctx.f.name == nil)
end

print(string.format("INPUT SESSION CLEANUP TESTS: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
