-- Test-host orchestration; rendering, async reads, saves and rollback use the
-- registered production backends. Assets are fixed generated input copies.
package.path = "scripts/?.lua;scripts/?/init.lua;" .. package.path
local config = require("config")
local runner = require("kag_runner")
local saves = require("kag.commands.save")
local layers = require("layers")
local text = require("kag.text_scene")
local M = {cycle=0, rendered=0, updates=0, completed=0, cancelled_callbacks=0,
           natural=0, rollback_driving=false}
config.accessibility.color_filter = "none"
config.accessibility.cc_mode = false
assert(DevCore.set_resolution(640, 360))

function engine_update(dt)
    M.updates = M.updates + 1
    if runner.get_ctx() then
        if M.rollback_driving then runner.on_click() end
        runner.update(dt)
    end
end

function engine_render()
    layers.render()
    if runner.get_ctx() then assert(runner.render()) end
    M.rendered = M.rendered + 1
end

function _onVoiceComplete() M.natural = M.natural + 1 end

local function page(which)
    local ctx = assert(runner.get_ctx())
    assert(layers.clear_for_restore())
    local node = layers.add_layer(nil, {id="soak_background", name="soak_background",
        x=0, y=0, w=640, h=360, z=0, opacity=255})
    local texture = assert(Render.load_texture("assets/soak/" .. which .. ".bmp"))
    assert(texture > 0)
    layers.set_layer_image(node, texture)
    assert(Render.text_set_font("assets/fonts/NotoSansCJKsc-Regular.otf", 24))
    text.reset(ctx)
    text.add_text(ctx, which == "a" and "SOAK A 保存页面" or "SOAK B 改变页面",
        36, 280, {245,240,200,255}, "body", 1, false, false, true)
    ctx.text_state.reveal_chars = 1000
    ctx.f.page = which
end

function M.begin(cycle)
    assert(M.async_texture == nil, "Previous async texture still owned")
    assert(runner.stop())
    assert(runner.start("tests/projects/engine_soak/base.ks"))
    for _=1,4 do runner.update(0) end
    local ctx = assert(runner.get_ctx())
    assert(ctx.f.ready == 1 and ctx._executing_command == "wait")
    M.cycle, M.completed, M.cancelled_callbacks, M.natural = cycle, 0, 0, 0
    M.rollback_driving = false
    ctx.f.cycle = cycle
    ctx.f.secret = "soak-encrypted-checkpoint"
    page("a")
end

function M.save()
    local ctx = assert(runner.get_ctx())
    saves.save(ctx, {slot=39, thumbnail="soak-fixture-only"})
    assert(ctx.tf.save_result == "ok", ctx.tf.save_error)
end

function M.change() page("b") end

function M.load()
    local old = assert(runner.get_ctx())
    assert(old.f.page == "b")
    local ok, err = saves.load(old, {slot=39})
    assert(ok, err)
    local ctx = assert(runner.get_ctx())
    assert(ctx ~= old and ctx.f.page == "a" and ctx.f.cycle == M.cycle)
    assert(ctx.f.secret == "soak-encrypted-checkpoint")
    return true
end

function M.reject_corrupt()
    local old = assert(runner.get_ctx())
    local ok = saves.load(old, {slot=39})
    assert(not ok and runner.get_ctx() == old and old.f.page == "b")
end

function M.cold_begin(producer)
    M.begin(producer and 1 or 777)
    if not producer then
        runner.get_ctx().f.secret = "consumer-never-saved"
        page("b")
    end
end

function M.cold_field(field)
    local f = assert(runner.get_ctx()).f
    if field == "cycle" then return f.cycle end
    if field == "page" then return f.page == "a" and 1 or f.page == "b" and 2 or 0 end
    assert(field == "secret_code")
    return f.secret == "soak-encrypted-checkpoint" and 1 or f.secret == "consumer-never-saved" and 2 or 0
end

function M.cold_apply(corrupt)
    local old = assert(runner.get_ctx())
    assert(old.f.cycle == 777 and old.f.page == "b" and old.f.secret == "consumer-never-saved")
    local ok, err = saves.load(old, {slot=39})
    local current = assert(runner.get_ctx())
    if corrupt then
        M.cold_error = tostring(err or old.tf.load_error or "")
        assert(not ok and M.cold_error ~= "" and current == old)
        assert(old.f.cycle == 777 and old.f.page == "b" and old.f.secret == "consumer-never-saved")
    else
        assert(ok, err)
        assert(current ~= old and current.f.cycle == 1 and current.f.page == "a")
        assert(current.f.secret == "soak-encrypted-checkpoint")
    end
    return 1
end

function M.async_begin()
    local id = Render.load_texture_async("assets/soak/b.bmp", function(ok, path, tex)
        assert(ok and path == "assets/soak/b.bmp" and tex > 0)
        assert(M.async_texture == nil)
        M.async_texture = tex
        M.completed = M.completed + 1
    end)
    assert(id > 0)
    return id
end

function M.cancel_batch()
    for _=1,8 do
        assert(Render.load_texture_async("assets/soak/a.bmp", function()
            M.cancelled_callbacks = M.cancelled_callbacks + 1
        end) > 0)
    end
    assert(Render.cancel_async_loads())
end

function M.rollback_begin()
    assert(M.completed == 1 and M.cancelled_callbacks == 0 and M.natural == 1)
    assert(runner.stop())
    assert(runner.start("tests/projects/engine_soak/rollback.ks"))
    M.rollback_driving = true
end

function M.rollback_ready()
    local ctx = runner.get_ctx()
    return ctx and ctx.f.rbReady == 1
end

function M.rollback_commit()
    M.rollback_driving = false
    local ctx = assert(runner.get_ctx())
    assert(ctx.f.rbReady == 1 and ctx.f.rb == 2)
    local index = ctx.token_index
    assert(runner.rollback())
    assert(ctx.token_index < index and ctx.f.rb == 2)
    assert(runner.rollback())
    assert(ctx.f.rb == 1)
end

function M.clean()
    assert(runner.stop())
    assert(Render.cancel_async_loads())
    assert(layers.clear_for_restore())
    assert(M.completed == 1 and M.cancelled_callbacks == 0 and M.natural == 1)
    -- Async completion transfers a newly uploaded texture to this callback.
    -- It is separate from the synchronous page cache and must be released.
    assert(M.async_texture and M.async_texture > 0)
    assert(Render.destroy_texture(M.async_texture))
    M.async_texture = nil
end

return M
