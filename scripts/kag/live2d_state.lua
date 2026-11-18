-- A prepared model is hidden until the presentation transaction commits.
local Backend = require("backend")
local M = {}
local max_models = 8
local max_float = 3.4028234663852886e38

local function text(value)
    return type(value) == "string" and #value > 0 and #value <= 4096 and not value:find("%z")
end

local function number(value, low, high)
    return type(value) == "number" and value >= low and value <= high
end

local function validate(snapshot)
    local entries, orders = {}, {}
    for name, model in pairs(snapshot) do
        if not text(name) or type(model) ~= "table" or not text(model.storage)
            or not model.storage:match("%.model3%.json$") or type(model.visible) ~= "boolean"
            or not number(model.x, -max_float, max_float) or not number(model.y, -max_float, max_float)
            or not number(model.scale, 0.001, max_float)
            or not number(model.order, 1, max_models) or model.order % 1 ~= 0 or orders[model.order] then
            error("Invalid saved Live2D model declaration", 0)
        end
        if model.current_motion ~= nil and (not text(model.current_motion)
            or not number(model.fadein, 0, 10000) or not number(model.fadeout, 0, 10000)) then
            error("Invalid saved Live2D motion", 0)
        end
        if model.expression ~= nil and (not text(model.expression) or not number(model.expression_weight, 0, 1)) then
            error("Invalid saved Live2D expression", 0)
        end
        local source = model.lip_sync_source
        if source ~= nil and source ~= "off" and source ~= "voice"
            and (source ~= "manual" or not number(model.lip_sync, 0, 1)) then
            error("Invalid saved Live2D mouth state", 0)
        end
        orders[model.order] = true
        entries[#entries + 1] = {name=name, model=model}
    end
    table.sort(entries, function(a, b) return a.model.order < b.model.order end)
    return entries
end

function M.capture(ctx)
    local declarations, handles = ctx.live2d or {}, ctx._live2dHandles or {}
    if type(declarations) ~= "table" or type(handles) ~= "table" then
        error("Invalid Live2D model ownership", 0)
    end
    if not next(declarations) and not next(handles) then return nil, 0 end
    local names, seen = {}, {}
    for name, handle in pairs(handles) do
        if not number(handle, 1, 2147483647) or handle % 1 ~= 0 or seen[handle]
            or type(declarations[name]) ~= "table" or #names >= max_models then
            error("Cannot save untracked Live2D models or more than eight models", 0)
        end
        seen[handle] = true
        names[#names + 1] = name
    end
    for name in pairs(declarations) do
        if not handles[name] then error("Cannot save Live2D state without a model handle", 0) end
    end
    table.sort(names, function(a, b) return handles[a] < handles[b] end)
    local snapshot = {}
    for order, name in ipairs(names) do
        local m = declarations[name]
        snapshot[name] = {order=order, storage=m.storage, visible=m.visible, x=m.x, y=m.y, scale=m.scale,
            expression=m.expression, expression_weight=m.expression_weight,
            current_motion=m.current_motion, fadein=m.fadein, fadeout=m.fadeout,
            lip_sync_source=m.lip_sync_source, lip_sync=m.lip_sync}
    end
    validate(snapshot)
    return snapshot, #names
end

function M.discard(prepared)
    local errors = {}
    for index, handle in pairs(prepared.handles) do
        local called, ok, reason = pcall(Backend.live2d_unload, handle)
        if called and ok == true then prepared.handles[index] = nil
        else errors[#errors + 1] = tostring(called and (reason or "Model unload failed") or ok) end
    end
    return #errors == 0, table.concat(errors, ", ")
end

local function applied(ok, reason)
    if ok ~= true then error(tostring(reason or "Live2D restore operation failed"), 0) end
end

function M.retire(ctx)
    local errors = {}
    for name, handle in pairs(ctx._live2dHandles or {}) do
        local called, ok, reason = pcall(Backend.live2d_unload, handle)
        if called and ok == true then
            ctx._live2dHandles[name] = nil
            if ctx.live2d then ctx.live2d[name] = nil end
        else
            errors[#errors + 1] = tostring(called and (reason or "Model unload failed") or ok)
        end
    end
    if #errors > 0 then error(table.concat(errors, ", "), 0) end
end

function M.prepare(snapshot)
    if snapshot == nil then return nil end
    if type(snapshot) ~= "table" then error("Invalid saved Live2D state", 0) end
    local declarations = require("kag.save_state").copy(snapshot)
    local prepared = {entries=validate(declarations), declarations=declarations, handles={}}
    local ok, reason = pcall(function()
        for index, entry in ipairs(prepared.entries) do
            local m = entry.model
            local handle, why = Backend.live2d_load(m.storage, entry.name)
            if not number(handle, 1, 2147483647) or handle % 1 ~= 0 then
                error("Cannot restore Live2D model "..entry.name..": "..tostring(why), 0)
            end
            prepared.handles[index] = handle
            if m.current_motion then
                applied(Backend.live2d_play_motion(handle, m.current_motion, m.fadein / 1000, m.fadeout / 1000))
            end
            if m.expression then applied(Backend.live2d_set_expression(handle, m.expression, m.expression_weight)) end
            if m.lip_sync_source == "manual" then applied(Backend.live2d_set_mouth(handle, m.lip_sync))
            elseif m.lip_sync_source == "voice" then applied(Backend.live2d_set_voice_lipsync(handle, true)) end
        end
    end)
    if not ok then
        local discarded, detail = M.discard(prepared)
        error(tostring(reason)..(discarded and "" or "; discard: "..detail), 0)
    end
    return prepared
end

function M.apply(prepared, ctx)
    if not prepared then return end
    local handles = {}
    for index, entry in ipairs(prepared.entries) do
        local m, handle = entry.model, prepared.handles[index]
        if m.visible then applied(Backend.live2d_show(handle, m.x, m.y, m.scale)) end
        handles[entry.name] = handle
    end
    ctx._live2dHandles, ctx.live2d = handles, prepared.declarations
    prepared.handles = {}
end

return M
