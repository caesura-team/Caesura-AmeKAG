-- Caesura (AmeKAG) -- kag/quickmenu.lua
-- QuickMenu command bindings: [quickmenu_auto], [quickmenu_skip], [quickmenu_log],
-- [quickmenu_qsave], [quickmenu_qload], [quickmenu_title], [quickmenu_config]
--
-- Sandbox contract (scripts/sandbox.lua): no bare globals, no _G writes.
-- Every collaborator is reached through require() -- and because the sandboxed
-- require only resolves package.loaded, "toast" must be preloaded by
-- kag/init.lua for the notification path to be live. When it is absent the
-- commands still work, they just stay silent (pcall-guarded require).

local schema = require("kag.schema")
local QuickMenu = {}

-- Optional in-game notification module (scripts/toast.lua). Same pattern as
-- scripts/kag_demo_entry.lua:28 / scripts/title_demo_entry.lua:18.
local ok_toast, toast = pcall(require, "toast")
if not ok_toast then toast = nil end

local function notify(msg)
    if toast and type(toast.show) == "function" then
        toast.show(msg)
    end
end

-- The KAG command table (scripts/kag.lua) owns the registered [save]/[load]/
-- jump handlers. Resolved lazily: quickmenu is preloaded BEFORE some of those
-- command modules, so binding at load time would capture a partial table.
local function kag_table()
    local ok, kag = pcall(require, "kag")
    if ok and type(kag) == "table" then return kag end
    return nil
end

local function clamp_slot(value)
    local slot = tonumber(value) or 0
    if slot < 0 then slot = 0 end
    if slot > 99 then slot = 99 end
    return math.floor(slot)
end

-- [quickmenu_auto mode=on|off|toggle]
schema.define("quickmenu_auto", {
    _meta = { category = "ui", blocking = false, desc = "Toggle auto-advance mode (QuickMenu)." },
    mode = { type = "string", choices = { on=true, off=true, toggle=true }, default = "toggle" },
})
function QuickMenu.quickmenu_auto(ctx, params)
    params = params or {}
    local m = params.mode or "toggle"
    if m == "on" then ctx.auto_mode = true
    elseif m == "off" then ctx.auto_mode = false
    else ctx.auto_mode = not ctx.auto_mode end
    notify(ctx.auto_mode and "Auto ON" or "Auto OFF")
    return ctx.auto_mode
end

-- [quickmenu_skip mode=on|off|toggle|seen]
schema.define("quickmenu_skip", {
    _meta = { category = "ui", blocking = false, desc = "Toggle skip mode (QuickMenu)." },
    mode = { type = "string", choices = { on=true, off=true, toggle=true, seen=true }, default = "toggle" },
})
function QuickMenu.quickmenu_skip(ctx, params)
    params = params or {}
    local m = params.mode or "toggle"
    if m == "seen" then
        if ctx.skip_mode == "seen" then ctx.skip_mode = false else ctx.skip_mode = "seen" end
    elseif m == "on" then ctx.skip_mode = true
    elseif m == "off" then ctx.skip_mode = false
    else ctx.skip_mode = not ctx.skip_mode end
    return ctx.skip_mode
end

-- [quickmenu_log]
schema.define("quickmenu_log", {
    _meta = { category = "ui", blocking = true, desc = "Open the native dialogue backlog until dismissal or jump (QuickMenu)." },
})
function QuickMenu.quickmenu_log(ctx, params)
    if not coroutine.isyieldable() then return false end
    local kag = kag_table()
    if not kag or type(kag.history) ~= "function" then return false end
    return kag.history(ctx, params)
end

-- [quickmenu_qsave slot=0]
-- Routes through the registered [save] handler (kag.save). System.quick_save
-- is NOT usable here: its signature is System.quick_save(ctx) -- it takes no
-- slot and performs an in-memory saveplace bookmark, so a slot passed to it
-- was silently discarded. [save] reports success through ctx.tf.save_result
-- (its return value is always nil), which is what the toast reads.
schema.define("quickmenu_qsave", {
    _meta = { category = "ui", blocking = false, desc = "Quick-save to slot (QuickMenu)." },
    slot = { type = "number", default = 0, min = 0, max = 99 },
})
function QuickMenu.quickmenu_qsave(ctx, params)
    params = params or {}
    local slot = clamp_slot(params.slot)
    local kag = kag_table()
    if not kag or type(kag.save) ~= "function" then
        print("[quickmenu] qsave unavailable: [save] handler not registered")
        return false
    end
    ctx.tf = ctx.tf or {}
    ctx.tf.save_result = nil
    local ok, err = pcall(kag.save, ctx, { slot = slot })
    if not ok then
        print("[quickmenu] qsave failed for slot " .. slot .. ": " .. tostring(err))
        return false
    end
    local saved = (ctx.tf.save_result == "ok")
    if saved then
        notify("Saved (slot " .. slot .. ")")
    else
        print("[quickmenu] qsave rejected for slot " .. slot
            .. " (save_result=" .. tostring(ctx.tf.save_result) .. ")")
    end
    return saved
end

-- [quickmenu_qload slot=0]
schema.define("quickmenu_qload", {
    _meta = { category = "ui", blocking = false, desc = "Quick-load from slot (QuickMenu)." },
    slot = { type = "number", default = 0, min = 0, max = 99 },
})
function QuickMenu.quickmenu_qload(ctx, params)
    params = params or {}
    local slot = clamp_slot(params.slot)
    local kag = kag_table()
    if not kag or type(kag.load) ~= "function" then
        print("[quickmenu] qload unavailable: [load] handler not registered")
        return false
    end
    local ok, err = pcall(kag.load, ctx, { slot = slot })
    if not ok then
        print("[quickmenu] qload failed for slot " .. slot .. ": " .. tostring(err))
        return false
    end
    ctx.tf = ctx.tf or {}
    return ctx.tf.load_result ~= "error"
end

-- [quickmenu_title scene=title]
-- Delegates to kag.jump, whose contract is a label in the CURRENT scene
-- ("*title" and "title" both resolve to the label named title). Cross-scene
-- switching is the scheduler's [jump storage=x.ks] path and is deliberately
-- NOT reimplemented here; kag.jump prints its own not-found diagnostic.
schema.define("quickmenu_title", {
    _meta = { category = "ui", blocking = false, desc = "Return to title label (QuickMenu)." },
    scene = { type = "string", default = "title" },
})
function QuickMenu.quickmenu_title(ctx, params)
    params = params or {}
    local scene = params.scene or "title"
    local kag = kag_table()
    if not kag or type(kag.jump) ~= "function" then
        print("[quickmenu] title unavailable: jump handler not registered")
        return false
    end
    kag.jump(ctx, scene)
    return true
end

-- [quickmenu_config]
schema.define("quickmenu_config", {
    _meta = { category = "ui", blocking = true, desc = "Open the native settings panel until dismissal (QuickMenu)." },
})
function QuickMenu.quickmenu_config(ctx, _params)
    if not coroutine.isyieldable() then return false end
    local ok, settings = pcall(require, "settings")
    if not ok or type(settings) ~= "table" or type(settings.show) ~= "function" then return false end
    return settings.show(ctx, true)
end

-- Register all into the KAG command table
local function register_all()
    local kag = kag_table()
    if not kag then return end
    local cmds = {
        "quickmenu_auto", "quickmenu_skip", "quickmenu_log",
        "quickmenu_qsave", "quickmenu_qload", "quickmenu_title", "quickmenu_config",
    }
    for _, name in ipairs(cmds) do
        if QuickMenu[name] then kag[name] = QuickMenu[name] end
    end
end
register_all()

return QuickMenu
