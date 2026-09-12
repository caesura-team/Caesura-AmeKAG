-- Actual VFX handlers must validate backend results before recording owners.
package.path = "scripts/?.lua;scripts/?/init.lua;scripts/kag/?.lua;" .. package.path

local passed, failed = 0, 0
local function check(name, condition, detail)
    if condition then passed = passed + 1; print("PASS " .. name)
    else failed = failed + 1; print("FAIL " .. name .. (detail and (" -- " .. tostring(detail)) or "")) end
end
local function count_keys(values)
    local count = 0
    for _ in pairs(values or {}) do count = count + 1 end
    return count
end

-- Only the backend boundary is replaced. vfx, layers, rtt, the schema and
-- command handlers below are the production modules.
local create_result, postfx_result, result_detail
local calls = { created = {}, postfx = {}, destroyed = {}, emitted = {} }
local backend = {
    get_resolution = function() return 1920, 1080 end,
    is_postfx_supported = function() return true end,
    set_postfx = function(kind, params)
        calls.postfx[#calls.postfx + 1] = { kind = kind, params = params }
        return postfx_result, result_detail
    end,
    particles_create_emitter = function(config)
        calls.created[#calls.created + 1] = config
        return create_result, result_detail
    end,
    particles_destroy_emitter = function(id)
        calls.destroyed[#calls.destroyed + 1] = id
        return true
    end,
    particles_emit = function(id, count)
        calls.emitted[#calls.emitted + 1] = { id = id, count = count }
        return true
    end,
    clear_particles = function() return true end,
    clear_postfx = function() return true end,
}
package.loaded.backend = backend
_G.backend = backend
local handlers = require("kag.commands.vfx")
local schema = require("kag.schema")

local invalid_postfx = {
    { "nil" }, { "false", false }, { "zero", 0 }, { "negative", -1 },
    { "fractional", 1.5 }, { "nan", 0 / 0 }, { "infinity", math.huge },
    { "negative infinity", -math.huge }, { "uint32 overflow", 4294967296 },
    { "string", "1" }, { "table", {} },
}
for _, route in ipairs({ "postprocess", "vfx" }) do
    for _, invalid in ipairs(invalid_postfx) do
        local old = { handle = 23, params = { strength = 0.5 } }
        local ctx = { _postfx = { bloom = old } }
        local params = schema.coerce(route, route == "vfx" and { postfx = "bloom" } or { effect = "bloom" }, ctx)
        postfx_result, result_detail = invalid[2], { status = "failed" }
        local before = #calls.postfx
        local ok, value, detail = pcall(handlers[route], ctx, params)
        check(route .. " rejects invalid postfx " .. invalid[1], ok and value == false and detail == result_detail, value)
        check(route .. " failed postfx preserves prior state " .. invalid[1],
            ctx._postfx.bloom == old and count_keys(ctx._postfx) == 1 and #calls.postfx == before + 1)
        local empty = {}
        pcall(handlers[route], empty, params)
        check(route .. " failed postfx creates no owner " .. invalid[1], empty._postfx == nil)
    end
    for _, valid in ipairs({ 1, 4294967295, true }) do
        postfx_result, result_detail = valid, { status = "applied" }
        local ctx = {}
        local params = route == "vfx" and { postfx = "bloom", strength = 0.25 }
            or { effect = "bloom", strength = 0.25 }
        local value, detail = handlers[route](ctx, params)
        check(route .. " valid postfx result and detail preserved " .. tostring(valid),
            value == valid and detail == result_detail and ctx._postfx.bloom.handle == valid
                and ctx._postfx.bloom.params.strength == 0.25)
    end
end

local invalid_particles = {
    { "nil" }, { "false", false }, { "true", true }, { "negative", -1 },
    { "fractional", 1.5 }, { "nan", 0 / 0 }, { "infinity", math.huge },
    { "negative infinity", -math.huge }, { "int overflow", 2147483648 },
    { "string", "0" }, { "table", {} },
}
for _, route in ipairs({ "particles", "vfx" }) do
    for _, invalid in ipairs(invalid_particles) do
        create_result, result_detail = invalid[2], { status = "unsupported" }
        local owned = { [7] = true }
        local ctx = { _particleEmitters = owned }
        local before = #calls.created
        local ok, value, detail = pcall(handlers[route], ctx, { type = "particle", action = "create" })
        check(route .. " rejects invalid emitter " .. invalid[1], ok and value == false and detail == result_detail, value)
        check(route .. " invalid emitter preserves prior owners " .. invalid[1],
            ctx._particleEmitters == owned and count_keys(owned) == 1 and owned[7] and #calls.created == before + 1)
        local empty = {}
        pcall(handlers[route], empty, { type = "particle", action = "create" })
        check(route .. " invalid emitter creates no owner " .. invalid[1], empty._particleEmitters == nil)
    end
    for _, valid in ipairs({ 0, 1, 2147483647 }) do
        create_result, result_detail = valid, { status = "applied" }
        local ctx = {}
        local value, detail = handlers[route](ctx, { type = "particle", action = "create" })
        check(route .. " valid emitter including zero retained " .. tostring(valid),
            value == valid and detail == result_detail and ctx._particleEmitters[valid] == true
                and count_keys(ctx._particleEmitters) == 1)
    end
end

for _, invalid in ipairs(invalid_particles) do
    handlers._clear_runtime_state()
    create_result, result_detail = invalid[2], { status = "failed" }
    local ctx = {}
    local emitted_before, destroyed_before = #calls.emitted, #calls.destroyed
    local ok, value, detail = pcall(handlers.particle_weather, ctx, { type = "rain" })
    check("weather rejects invalid emitter " .. invalid[1], ok and value == false and detail == result_detail, value)
    check("weather invalid result has no ctx ownership or emission " .. invalid[1],
        ctx._weatherEmitters == nil and ctx._particleEmitters == nil and #calls.emitted == emitted_before)
    handlers.particle_weather(nil, { action = "stop", type = "rain" })
    check("weather invalid result has no module owner " .. invalid[1], #calls.destroyed == destroyed_before)
end

handlers._clear_runtime_state()
create_result, result_detail = 0, { status = "applied" }
local weather_ctx = {}
local value, detail = handlers.particle_weather(weather_ctx, { type = "rain", count = 3 })
check("weather zero is a valid owned and emitted ID", value == 0 and detail == result_detail
    and weather_ctx._weatherEmitters.rain == 0 and weather_ctx._particleEmitters[0] == true
    and calls.emitted[#calls.emitted].id == 0 and calls.emitted[#calls.emitted].count == 3)

create_result, result_detail = -1, { status = "failed", reason = "quota" }
local destroyed_before, emitted_before = #calls.destroyed, #calls.emitted
value, detail = handlers.particle_weather(weather_ctx, { type = "rain" })
check("failed weather replacement preserves prior emitter and ctx indexes", value == false and detail == result_detail
    and #calls.destroyed == destroyed_before and #calls.emitted == emitted_before
    and weather_ctx._weatherEmitters.rain == 0 and weather_ctx._particleEmitters[0] == true)
handlers.particle_weather(nil, { action = "stop", type = "rain" })
check("failed weather replacement preserves the module fallback index",
    #calls.destroyed == destroyed_before + 1 and calls.destroyed[#calls.destroyed] == 0)

handlers._clear_runtime_state(weather_ctx)
create_result = 0
handlers.particle_weather(weather_ctx, { type = "rain" })
create_result, result_detail = 8, { status = "applied" }
destroyed_before = #calls.destroyed
value, detail = handlers.particle_weather(weather_ctx, { type = "rain" })
check("successful weather replacement destroys old ID once and switches ctx indexes",
    value == 8 and detail == result_detail and #calls.destroyed == destroyed_before + 1
        and calls.destroyed[#calls.destroyed] == 0 and weather_ctx._weatherEmitters.rain == 8
        and weather_ctx._particleEmitters[0] == nil and weather_ctx._particleEmitters[8] == true)
handlers.particle_weather(nil, { action = "stop", type = "rain" })
check("successful weather replacement switches the module fallback index", calls.destroyed[#calls.destroyed] == 8)

handlers._clear_runtime_state(weather_ctx)
create_result = 5
handlers.particle_weather(weather_ctx, { type = "rain" })
destroyed_before = #calls.destroyed
value = handlers.particle_weather(weather_ctx, { type = "rain" })
check("weather backend reusing the same ID does not destroy the new object",
    value == 5 and #calls.destroyed == destroyed_before and weather_ctx._weatherEmitters.rain == 5
        and weather_ctx._particleEmitters[5] == true)
handlers._clear_runtime_state(weather_ctx)

print(string.format("CAPABILITY RESOURCE RESULTS: %d passed, %d failed", passed, failed))
if failed > 0 then os.exit(1) end
print("ALL CAPABILITY RESOURCE RESULT TESTS PASSED")
