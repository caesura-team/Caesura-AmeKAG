-- Persistent values and control frames. No live context or backend mutations.
local M = { VERSION = 3 }
local WaitState=require('kag.wait_state')
local MAX_DEPTH, MAX_NODES = 64, 100000

local function integer(value, minimum, name)
    if type(value) ~= "number" or value ~= math.floor(value)
        or value < minimum or value == math.huge then
        error("Invalid saved " .. name, 0)
    end
    return value
end

local function clone(value, visiting, budget, depth)
    local kind = type(value)
    if kind == "nil" or kind == "string" or kind == "boolean" then return value end
    if kind == "number" then
        if value ~= value or math.abs(value) == math.huge then
            error("Non-finite saved number", 0)
        end
        return value
    end
    if kind ~= "table" then error("Unsupported saved value: " .. kind, 0) end
    if depth > MAX_DEPTH or visiting[value] then error("Cyclic or deep saved table", 0) end
    visiting[value] = true
    local result, strings, numbers, maximum = {}, 0, 0, 0
    for key, child in pairs(value) do
        budget.count = budget.count + 1
        if budget.count > MAX_NODES then error("Saved state exceeds value budget", 0) end
        if type(key) == "string" then strings = strings + 1
        elseif type(key) == "number" then
            integer(key, 1, "array index")
            numbers, maximum = numbers + 1, math.max(maximum, key)
        else error("Unsupported saved table key", 0) end
        result[key] = clone(child, visiting, budget, depth + 1)
    end
    visiting[value] = nil
    if numbers > 0 and (strings > 0 or maximum ~= numbers) then
        error("Saved tables require string keys or a dense array", 0)
    end
    return result
end

function M.copy(value)
    return clone(value, {}, { count = 0 }, 0)
end

local function table_field(state, key)
    local value = state[key]
    if value == nil then return {} end
    if type(value) ~= "table" then error("Invalid saved " .. key, 0) end
    return M.copy(value)
end

local function textbox_style(value)
    if value == nil then return nil end
    if type(value) ~= "table" then error("Invalid saved textbox_style", 0) end
    local schema = require("kag.schema")
    if not schema.specs("textbox") then require("kag.commands.text") end
    local restored = M.copy(value)
    -- Saved styles are complete handler values. Do not coerce/default a
    -- malformed slot: cl replays this table without scheduler coercion.
    for key, spec in pairs(schema.specs("textbox")) do
        local field = restored[key]
        if type(field) ~= spec.type or (spec.type == "number"
            and ((spec.min and field < spec.min) or (spec.max and field > spec.max))) then
            error("Invalid saved textbox_style." .. key, 0)
        end
    end
    return restored
end

local function array(value, name)
    if value == nil then return {} end
    if type(value) ~= "table" then error("Invalid saved " .. name, 0) end
    local count = 0
    for key in pairs(value) do
        integer(key, 1, name .. " index")
        count = count + 1
    end
    if count ~= #value then error("Sparse saved " .. name, 0) end
    return value
end

function M.capture_seen(seen)
    local result = {}
    for scene, marks in pairs(seen or {}) do
        if type(scene) ~= "string" or type(marks) ~= "table" then
            error("Invalid seen-scene state", 0)
        end
        local positions = {}
        for index, read in pairs(marks) do
            if read == true then positions[#positions + 1] = integer(index, 0, "seen position") end
        end
        table.sort(positions)
        result[scene] = positions
    end
    return result
end

local function restore_seen(value, version)
    if value == nil then return {} end
    if type(value) ~= "table" then error("Invalid saved seen_scenes", 0) end
    local result = {}
    for scene, marks in pairs(value) do
        if type(scene) ~= "string" or type(marks) ~= "table" then
            error("Invalid saved seen-scene entries", 0)
        end
        local restored = {}
        if version >= 3 then
            for _, index in ipairs(array(marks, "seen positions")) do
                restored[integer(index, 0, "seen position")] = true
            end
        else
            -- JSON object keys in old files are strings, including token IDs.
            for index, read in pairs(marks) do
                if type(read) ~= "boolean" then error("Invalid saved seen mark", 0) end
                restored[integer(tonumber(index), 0, "seen position")] = read
            end
        end
        result[scene] = restored
    end
    return result
end

-- Private defaults are only inputs to clone. Its per-branch visiting map
-- gives every missing output field its own empty table, as before.
local EMPTY_CONTROL_FIELD = {}
function M.capture_control(ctx)
    local for_, while_, if_, switch, marks, rewound = ctx._forStack, ctx._whileStack,
        ctx._ifStack, ctx._switchStack, ctx._forStackMarks, ctx._forRewound
    if not (for_ or while_ or if_ or switch or marks or rewound) then
        -- No caller-provided value remains to validate or copy. These six
        -- fresh empty arrays are the complete default result of M.copy.
        return {for_={}, while_={}, if_={}, switch={}, for_marks={}, for_rewound={}}
    end
    return M.copy({
        for_ = for_ or EMPTY_CONTROL_FIELD, while_ = while_ or EMPTY_CONTROL_FIELD,
        if_ = if_ or EMPTY_CONTROL_FIELD, switch = switch or EMPTY_CONTROL_FIELD,
        for_marks = marks or EMPTY_CONTROL_FIELD, for_rewound = rewound or EMPTY_CONTROL_FIELD,
    })
end

local function restore_control(value, tokens, version)
    if value == nil then value = {} end
    if type(value) ~= "table" then error("Invalid saved control stacks", 0) end
    local result = {}
    for _, key in ipairs({ "if_", "switch" }) do
        result[key] = {}
        for i, taken in ipairs(array(value[key], key)) do
            if version >= 3 and type(taken) ~= "boolean" then
                error("Invalid saved " .. key .. " frame", 0)
            end
            -- Earlier schedulers stored the truthy expression value itself.
            result[key][i] = not not taken
        end
    end
    for _, entry in ipairs({ { "for_", "for" }, { "while_", "while" } }) do
        local key, command = entry[1], entry[2]
        result[key] = {}
        for i, frame in ipairs(array(value[key], key)) do
            if type(frame) ~= "table" then error("Invalid saved " .. key .. " frame", 0) end
            local position = integer(frame.pos, 1, key .. " position")
            local token = tokens[position]
            if not token or (token[1] or token.cmd) ~= command then
                error("Saved " .. key .. " frame does not match its scene", 0)
            end
            if type(frame.ended) ~= "boolean" then error("Invalid saved loop outcome", 0) end
            local restored = { pos = position, ended = frame.ended }
            if key == "for_" then
                if type(frame.var) ~= "string" or type(frame.endv) ~= "number"
                    or type(frame.step) ~= "number" or frame.step == 0 then
                    error("Invalid saved for-loop bounds", 0)
                end
                restored.var, restored.endv, restored.step = frame.var, frame.endv, frame.step
            end
            result[key][i] = restored
        end
    end
    result.for_marks = table_field(value, "for_marks")
    result.for_rewound = table_field(value, "for_rewound")
    for name, count in pairs(result.for_marks) do
        if type(name) ~= "string" then error("Invalid saved loop variable", 0) end
        integer(count, 1, "loop variable depth")
    end
    for name, rewound in pairs(result.for_rewound) do
        if type(name) ~= "string" or type(rewound) ~= "boolean" then
            error("Invalid saved loop rewind marker", 0)
        end
    end
    return result
end

local function read_scene(path, safe_path, loader)
    if not safe_path(path) then error("Unsafe saved scene path", 0) end
    local scene, reason = loader(path)
    if type(scene) ~= "table" or type(scene.tokens) ~= "table" then
        error("Cannot prepare saved scene: " .. path .. ": " .. tostring(reason), 0)
    end
    return scene
end

local function position(value, tokens)
    local index = integer(value == nil and 1 or value, 1, "token position")
    if index > #tokens + 1 then error("Saved token position exceeds scene", 0) end
    return index
end

-- Candidate contains only validated values and freshly loaded executable tokens.
function M.prepare(state, safe_path, loader)
    if type(state) ~= "table" then error("Invalid saved state", 0) end
    local version = integer(state.schema_version == nil and 1 or state.schema_version, 1, "data schema")
    if version > M.VERSION then error("Unsupported saved data schema", 0) end
    local candidate = {}
    local scenes = {}
    local function prepared_scene(path)
        if not scenes[path] then scenes[path] = read_scene(path, safe_path, loader) end
        return scenes[path]
    end
    for _, key in ipairs({ "f", "sf", "lf", "mp", "variables", "unlockedCG",
        "unlockedMusic", "seen_endings", "backlog", "text_state", "layers", "characters" }) do
        candidate[key] = table_field(state, key)
    end
    candidate.textbox_style = textbox_style(state.textbox_style)
    candidate.sf.save_list = nil -- derived slot listing, never persistent state
    for _, entry in ipairs(array(candidate.backlog, "backlog")) do
        if type(entry) ~= "table" then error("Invalid saved backlog entry", 0) end
        for _, key in ipairs({"name", "text", "voice", "scene", "src"}) do
            if entry[key] ~= nil and type(entry[key]) ~= "string" then
                error("Invalid saved backlog " .. key, 0)
            end
        end
    end
    candidate.seen_scenes = restore_seen(state.seen_scenes, version)
    candidate.tf = {}
    candidate.current_scene = state.scene_path
    candidate.currentScene = state.scene_path
    local scene = prepared_scene(state.scene_path)
    candidate.tokens, candidate.labelMap = scene.tokens, M.copy(scene.labels or {})
    candidate._resume_index = position(state.token_index, scene.tokens)
    candidate._restoredWait=WaitState.prepare(state.wait_snapshot,
        candidate.current_scene,candidate.tokens,candidate._resume_index)
    candidate.token_index = position(state.display_token_index or state.token_index, scene.tokens)
    candidate.control = restore_control(state.loop_stacks, scene.tokens, version)
    candidate.call_stack = {}
    for i, frame in ipairs(array(state.call_stack, "call stack")) do
        if type(frame) ~= "table" then error("Invalid saved call frame", 0) end
        -- Never execute token tables from a save; old frames need a scene identity.
        local caller = prepared_scene(frame.scene)
        candidate.call_stack[i] = {
            scene = frame.scene, tokens = caller.tokens,
            label_index = M.copy(caller.labels or {}),
            index = position(frame.index, caller.tokens),
            lf = table_field(frame, "lf"), mp = table_field(frame, "mp"),
            control = restore_control(frame.control, caller.tokens, version),
        }
    end
    if state.skip_mode ~= nil and type(state.skip_mode) ~= "boolean" and state.skip_mode ~= "seen" then
        error("Invalid saved skip mode", 0)
    end
    candidate.skip_mode = state.skip_mode or false
    for _, key in ipairs({ "auto_mode", "nvl_mode", "voice_muted" }) do
        if state[key] ~= nil and type(state[key]) ~= "boolean" then
            error("Invalid saved " .. key, 0)
        end
        candidate[key] = state[key] == true
    end
    if state.language ~= nil and (type(state.language) ~= "string"
        or not state.language:match("^[%w_-]+$")) then
        error("Invalid saved language", 0)
    end
    -- Legacy slots omitted the language. Their migration uses the engine's
    -- historical Chinese startup locale, never a later session's global value.
    candidate.language = state.language or "zh"
    if state.language_default ~= nil and (type(state.language_default) ~= "string"
        or not state.language_default:match("^[%w_-]+$")) then
        error("Invalid saved fallback language", 0)
    end
    candidate.language_default = state.language_default or "en"
    candidate._locale = require("i18n").prepare(candidate.language, candidate.language_default)
    candidate.text_snapshot = require("kag.text_scene").prepare_restore(
        state.text_snapshot or {state=candidate.text_state})
    if state.resume_page_wait ~= nil and type(state.resume_page_wait) ~= "boolean" then
        error("Invalid saved page wait marker", 0)
    end
    local resume_token = candidate.tokens[candidate._resume_index]
    local resume_command = resume_token and (resume_token[1] or resume_token.cmd)
    if state.resume_page_wait == true
        and (resume_command ~= "p" or not candidate.text_snapshot.waiting_input) then
        error("Invalid saved page wait cursor", 0)
    end
    -- Legacy slots have no unambiguous page-wait identity; do not infer one
    -- merely because the next token is [p].
    candidate._resumePageWait = state.resume_page_wait == true
    candidate._presentation = require("kag.presentation").prepare(state)
    return candidate
end

function M.apply_values(ctx, candidate)
    ctx._waitState=nil
    ctx._restoredWait=candidate._restoredWait
    ctx._resumePageWait=candidate._resumePageWait
    ctx.textbox_style=candidate.textbox_style -- nil also clears a reused host context
    for key, value in pairs(candidate) do
        if key ~= "control" and key ~= "language" and key ~= "language_default" and key ~= "_locale" and key ~= "text_snapshot"
            and key ~= "_presentation" then ctx[key] = value end
    end
    local control = candidate.control
    ctx._forStack, ctx._whileStack = control.for_, control.while_
    ctx._ifStack, ctx._switchStack = control.if_, control.switch
    ctx._forStackMarks, ctx._forRewound = control.for_marks, control.for_rewound
    ctx._resumeLoopStacks = nil
    ctx._undoStack, ctx._macroStack = {}, nil
    ctx.label_index = nil
    require("kag.text_scene").apply_restore(ctx,candidate.text_snapshot)
end

return M
