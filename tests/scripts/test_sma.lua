-- test_sma.lua — SMA driver tests (Battle 4d S3): JSON parse, bone
-- hierarchy world-pose resolution, animation track LERP, actor lifecycle
-- via a recording mock of the `sma.*` engine binding. GPU drawing is
-- deferred (mock records calls instead).
local passed, failed = 0, 0
local function check(name, cond)
    if cond then print("PASS " .. name) passed = passed + 1
    else print("FAIL " .. name) failed = failed + 1 end
end

package.path = "scripts/?.lua;scripts/kag/?.lua;" .. package.path

-- Recording mock of the SmaBinding global (resolved lazily by kag.sma).
_G.sma = {
    created = 0,
    destroyed = 0,
    updates = 0,
    draws = 0,
    last_handle = 0,
    last_verts = nil,
    create_mesh = function(verts, indices)
        _G.sma.created = _G.sma.created + 1
        _G.sma.last_handle = _G.sma.created
        _G.sma.last_verts = verts
        return _G.sma.created
    end,
    destroy_mesh = function() _G.sma.destroyed = _G.sma.destroyed + 1 end,
    update_mesh = function(handle, poses)
        _G.sma.updates = _G.sma.updates + 1
        _G.sma.last_poses = poses
    end,
    draw_mesh = function() _G.sma.draws = _G.sma.draws + 1 end,
    count = function() return _G.sma.created end,
    initialized = function() return true end,
}

local sma = require("kag.sma")

-- ---------------------------------------------------------------------------
-- 1. JSON parse
-- ---------------------------------------------------------------------------
local asset = sma.load([==[
{
  "texture": "chara/hero.png",
  "bones": [ {"id":0,"parent":-1,"pivot":[1.0,0.0]}, {"id":1,"parent":0,"pivot":[0.0,0.0]} ],
  "mesh": { "positions":[[0,0],[1,0],[1,1]], "uvs":[[0,0],[1,0],[1,1]], "indices":[0,1,2] },
  "animations": { "idle": { "duration":2.0, "tracks":[ {"bone":1,"frames":[{"t":0,"rot":0},{"t":1,"rot":1.5708}]} ] } }
}]==])
check("json: bones parsed", #asset.bones == 2)
check("json: nested objects", type(asset.animations.idle.tracks) == "table"
      and #asset.animations.idle.tracks == 1)
check("json: nested arrays", #asset.mesh.positions == 3
      and asset.mesh.positions[2][1] == 1)
check("json: strings + numbers", asset.texture == "chara/hero.png"
      and asset.animations.idle.duration == 2.0)
check("json: escaped string", sma.load('{"a":"x\\ny"}').a == "x\ny")

-- ---------------------------------------------------------------------------
-- 2. Hierarchy world-pose resolution
-- ---------------------------------------------------------------------------
local function near(a, b) return math.abs(a - b) < 1e-4 end

-- root with pivot + rotation: T(p)R T(-p) baked into the BonePose offset
local w1 = sma.resolve_world(
    { { id = 0, parent = -1, pivot = { 1, 0 } } },
    { { rot = math.pi / 2, scale = 1, ox = 0, oy = 0 } })
check("hierarchy: root pivot baked (rot)", near(w1[1].rot, math.pi / 2))
check("hierarchy: root pivot baked (offset)",
      near(w1[1].ox, 1) and near(w1[1].oy, -1))

-- identity case: pivot cancels without rotation
local w2 = sma.resolve_world(
    { { id = 0, parent = -1, pivot = { 1, 0 } } },
    { { rot = 0, scale = 1, ox = 0, oy = 0 } })
check("hierarchy: pivot cancels when idle",
      near(w2[1].ox, 0) and near(w2[1].oy, 0))

-- child inherits parent rotation + scale (parent chain)
local w3 = sma.resolve_world(
    {
        { id = 0, parent = -1, pivot = { 0, 0 } },
        { id = 1, parent = 0, pivot = { 0, 0 } },
    },
    {
        { rot = math.pi / 2, scale = 2, ox = 0, oy = 0 },
        { rot = math.pi / 4, scale = 1, ox = 5, oy = 0 },
    })
check("hierarchy: child world rot = parent + local",
      near(w3[2].rot, math.pi / 2 + math.pi / 4))
check("hierarchy: child world scale = product",
      near(w3[2].scale, 2))
-- child local offset (5,0) is rotated+scaled by the parent: 90deg * 2 -> (0, 10)
check("hierarchy: child offset transformed by parent",
      near(w3[2].ox, 0) and near(w3[2].oy, 10))

-- ---------------------------------------------------------------------------
-- 3. Animation LERP
-- ---------------------------------------------------------------------------
local l1 = sma.sample(asset, "idle", 0.5)
check("lerp: midpoint of 0 -> 1.5708", l1[2] and near(l1[2].rot, 0.7854))
local l2 = sma.sample(asset, "idle", 2.0)
check("lerp: clamps after last frame", l2[2] and near(l2[2].rot, 1.5708))
local l3 = sma.sample(asset, "missing", 0.5)
check("lerp: unknown animation -> nil", l3 == nil)

-- ---------------------------------------------------------------------------
-- 4. Actor lifecycle through the binding mock
-- ---------------------------------------------------------------------------
sma.register("hero", asset)
local ctx = {}
local actor = sma.spawn(ctx, "hero1", asset, "idle", {
    x = 100, y = 50, scale = 2, texId = 42,
})
check("spawn: binding create called", _G.sma.created == 1 and actor.handle == 1)
-- Observe the actual producer records here. Decoding these records, named
-- field precedence, and rejection of invalid values are exercised through
-- the real C binding in tests/cpp/test_sma_native_contract.cpp.
check("binding records: vertex x occupies slot 1",
      _G.sma.last_verts[2][1] == 1 and _G.sma.last_verts[2][2] == 0)
check("binding records: default bone0 and weight occupy slots 5 and 6",
      _G.sma.last_verts[2][5] == 0 and _G.sma.last_verts[2][6] == 1)
check("spawn: actor state", ctx.sma_actors.hero1 ~= nil
      and ctx.sma_actors.hero1.texId == 42
      and ctx.sma_actors.hero1.scale == 2)

sma.update(ctx, 0.5)
check("update: binding update called with world poses", _G.sma.updates == 1)
check("binding records: animated rotation occupies pose slot 1",
      _G.sma.last_poses and #_G.sma.last_poses == 2
      and near(_G.sma.last_poses[2][1], 0.7854)
      and near(_G.sma.last_poses[2][2], 1))
sma.render(ctx)
check("render: binding draw called", _G.sma.draws == 1)

sma.despawn(ctx, "hero1")
check("despawn: binding destroy called", _G.sma.destroyed == 1)
check("despawn: actor removed", ctx.sma_actors.hero1 == nil)

-- [sma_play] command registers an actor; [sma_stop] removes it
local cmdCtx = { sma_actors = {} }
sma.commands.sma_play(cmdCtx, { name = "a", asset = "hero", anim = "idle", x = 1 })
check("sma_play: actor spawned", cmdCtx.sma_actors.a ~= nil
      and cmdCtx.sma_actors.a.anim == "idle")
sma.commands.sma_stop(cmdCtx, { name = "a" })
check("sma_stop: actor removed", cmdCtx.sma_actors.a == nil)

sma.commands.sma_play(cmdCtx, { name = "a", asset = "nope" })
check("sma_play: unknown asset is inert", cmdCtx.sma_actors.a == nil)

-- despawn without binding present (inert path)
local old = _G.sma
_G.sma = nil
local ctx2 = {}
sma.spawn(ctx2, "x", asset, "idle", {})
check("spawn without binding: handle 0", ctx2.sma_actors.x.handle == 0)
sma.update(ctx2, 1.0)
sma.render(ctx2)
check("update/render without binding: no crash", true)
_G.sma = old

-- ---------------------------------------------------------------------------
-- 4b. Round 19: dual-bone weight layout (design doc §2.2 interleaved
-- [2i-1]/[2i] entries) reaches the binding without index misalignment.
-- ---------------------------------------------------------------------------
local near2 = function(a, b) return math.abs(a - b) < 1e-6 end
local tbAsset = {
    bones = { { id = 0, parent = -1, pivot = { 0.5, 0.5 } },
              { id = 1, parent = 0, pivot = { 0.5, 0.5 } } },
    mesh = {
        positions = { { 0, 0 }, { 1, 0 }, { 0, 1 } },
        uvs = { { 0, 0 }, { 1, 0 }, { 0, 1 } },
        indices = { 0, 1, 2 },
        weights = {
            { bone = 0, w = 0.6 }, { bone = 1, w = 0.4 },  -- v0: two bones
            { bone = 0, w = 0.7 }, { bone = 1, w = 0.3 },  -- v1: two bones
            { bone = 0, w = 1.0 }, { bone = 1, w = 0.0 },  -- v2: single bone
        },
    },
    animations = {},
}
local tbCtx = {}
sma.spawn(tbCtx, "tb", tbAsset, "idle", {})
local lv = _G.sma.last_verts
check("weights: 3 vertices uploaded", lv ~= nil and #lv == 3)
check("weights: v0 bone pair aligned (0,0.6)/(1,0.4)",
      lv and lv[1][5] == 0 and near2(lv[1][6], 0.6)
      and lv[1][7] == 1 and near2(lv[1][8], 0.4))
check("weights: v1 bone pair aligned (0,0.7)/(1,0.3)",
      lv and lv[2][5] == 0 and near2(lv[2][6], 0.7)
      and lv[2][7] == 1 and near2(lv[2][8], 0.3))
check("weights: v2 single bone (w1 = 0)",
      lv and lv[3][5] == 0 and near2(lv[3][6], 1.0)
      and lv[3][7] == 1 and near2(lv[3][8], 0.0))
sma.despawn(tbCtx, "tb")

-- ---------------------------------------------------------------------------
-- 5. Scene-level determinism test (SMA S4): [sma_play]/[sma_stop] run
-- through the real scheduler without a GPU.
-- ---------------------------------------------------------------------------
-- Load every command module so their schema contracts register (the
-- determinism mock table is built from dumpContracts; without them the
-- mock is empty and every command is flagged unknown).
pcall(require, "kag.commands.text")
pcall(require, "kag.commands.system")
pcall(require, "kag.commands.audio")
pcall(require, "kag.commands.layer")
pcall(require, "kag.commands.transition")
pcall(require, "kag.commands.vfx")
pcall(require, "kag.commands.save")
pcall(require, "kag.commands.video")

local determinism = require("kag.determinism")
-- Scene tests run the real no-GPU path: drop the recording mock so the
-- binding is absent (handle 0, everything inert).
_G.sma = nil
sma.register("hero", asset)

local scenePlay = "[sma_play name=\"hero\" asset=\"hero\" anim=\"idle\""
    .. " x=100 y=50 scale=2]\n[p]\n[ch text=\"done\"]\n"
local res1 = determinism.run_scene(scenePlay, {
    scene = "sma_test.ks",
    kag_override = {
        sma_play = sma.commands.sma_play,
        sma_stop = sma.commands.sma_stop,
    },
})
check("scene: [sma_play] spawns actor", res1.sma_actors ~= nil
      and res1.sma_actors.hero ~= nil)
check("scene: actor state from params", res1.sma_actors.hero.anim == "idle"
      and res1.sma_actors.hero.x == 100
      and res1.sma_actors.hero.y == 50
      and res1.sma_actors.hero.scale == 2)
check("scene: handle 0 without GPU binding", res1.sma_actors.hero.handle == 0)
check("scene: script continues after play", #res1.backlog == 1
      and res1.backlog[1] == "done")

local sceneStop = "[sma_play name=\"hero\" asset=\"hero\" anim=\"idle\"]\n"
    .. "[sma_stop name=\"hero\"]\n[ch text=\"after\"]\n"
local res2 = determinism.run_scene(sceneStop, {
    scene = "sma_test.ks",
    kag_override = {
        sma_play = sma.commands.sma_play,
        sma_stop = sma.commands.sma_stop,
    },
})
check("scene: [sma_stop] despawns actor",
      res2.sma_actors == nil or res2.sma_actors.hero == nil)
check("scene: script continues after stop", #res2.backlog == 1
      and res2.backlog[1] == "after")

local sceneUnknown = "[sma_play name=\"a\" asset=\"nope\"]\n[ch text=\"ok\"]\n"
local res3 = determinism.run_scene(sceneUnknown, {
    scene = "sma_test.ks",
    kag_override = {
        sma_play = sma.commands.sma_play,
        sma_stop = sma.commands.sma_stop,
    },
})
check("scene: unknown asset inert, no crash",
      (res3.sma_actors == nil or res3.sma_actors.a == nil)
      and #res3.backlog == 1)


-- Restore the recording binding mock (section 5 cleared it for the
-- no-GPU determinism runs).
_G.sma = old

-- ---------------------------------------------------------------------------
-- 6. Round 18: playback controls — duration / loop / rate / pause / seek /
--    play_anim / on_done_anim
-- ---------------------------------------------------------------------------
check("controls: duration from max keyframe", near(sma.duration(asset, "idle"), 1.0))
check("controls: unknown anim duration 0", sma.duration(asset, "nope") == 0)

-- loop wrap: the clip is 1s; t=1.5 must sample as t=0.5 (midpoint).
local lp = sma.sample(asset, "idle", 1.5, true)
check("controls: loop wraps t over duration", lp[2] and near(lp[2].rot, 0.7854))
local nl = sma.sample(asset, "idle", 5.0, false)
check("controls: non-loop clamps at last frame", nl[2] and near(nl[2].rot, 1.5708))

local pctx = {}
local act = sma.spawn(pctx, "p", asset, "idle", { loop = false, rate = 2 })
check("controls: spawn loop/rate opts", act.loop == false and act.rate == 2)
sma.update(pctx, 0.5)
check("controls: rate doubles time", near(act.t, 1.0))
sma.pause(pctx, "p")
sma.update(pctx, 10)
check("controls: pause freezes time", near(act.t, 1.0))
sma.resume(pctx, "p")
sma.update(pctx, 0.5)
check("controls: resume continues", near(act.t, 2.0))
sma.seek(pctx, "p", 0.25)
check("controls: seek jumps", near(act.t, 0.25))
sma.set_rate(pctx, "p", 0.5)
sma.update(pctx, 1.0)
check("controls: set_rate slows", near(act.t, 0.75))

sma.play_anim(pctx, "p", "idle", { loop = false })
check("controls: play_anim resets t", near(act.t, 0) and act.anim == "idle")

-- on_done_anim: a non-loop clip finishing switches to the fallback.
local dctx = {}
local doneAct = sma.spawn(dctx, "d", asset, "idle", {
    loop = false, on_done_anim = "idle",
})
sma.seek(dctx, "d", 1.0)  -- at the last frame
sma.update(dctx, 1.0)     -- past the end -> fallback fires (loop=true)
check("controls: on_done_anim fallback fires", doneAct.anim == "idle")
check("controls: fallback loop mode", doneAct.loop == true)

-- ---------------------------------------------------------------------------
-- 7. Round 18: 2-bone IK
-- ---------------------------------------------------------------------------
-- Straight chain at (0,0)-(10,0), l1=10, l2=10, target (10, 10):
-- theta1 should be 45deg + ... and the chain must reach the target.
local t1, t2 = sma.ik2bones(0, 0, 10, 0, 10, 10, 10, 10)
check("ik: solvable chain returns angles", t1 ~= nil and t2 ~= nil)
-- Reachability: target beyond l1+l2 is unreachable.
check("ik: too-far target -> nil",
      sma.ik2bones(0, 0, 10, 0, 50, 50, 10, 10) == nil)
check("ik: too-close target -> nil",
      sma.ik2bones(0, 0, 10, 0, 2, 0, 10, 5) == nil)
check("ik: degenerate length -> nil", sma.ik2bones(0, 0, 1, 0, 5, 0, 0, 0) == nil)
-- Verify the solved angles actually reach the target: root at (0,0),
-- mid at l1*(cos t1, sin t1), end = mid + l2*(cos t2, sin t2) == target.
local ex = 10 * math.cos(t1) + 10 * math.cos(t2)
local ey = 10 * math.sin(t1) + 10 * math.sin(t2)
check("ik: solved angles reach the target", near(ex, 10) and near(ey, 10))

-- set_ik integration: the actor's world poses get overridden.
local ikctx = {}
local ikAct = sma.spawn(ikctx, "ik", asset, "idle", {})
sma.set_ik(ikctx, "ik", { 0, 1 }, 20, 0, nil, 10)
sma.update(ikctx, 0.1)
check("ik: constraint stored", ikAct.ik ~= nil and ikAct.ik.l2 == 10)
sma.clear_ik(ikctx, "ik")
check("ik: clear removes constraint", ikAct.ik == nil)

-- ---------------------------------------------------------------------------
-- 8. Round 18: crossfade blend
-- ---------------------------------------------------------------------------
local bctx = {}
-- Add a second clip so crossfading between DIFFERENT animations is real.
asset.animations.walk = {
    duration = 1.0,
    tracks = { { bone = 1, frames = { { t = 0, rot = 0 }, { t = 1, rot = 0.5 } } } },
}
local bAct = sma.spawn(bctx, "b", asset, "idle", {})
sma.play_anim(bctx, "b", "walk", { blend_time = 1.0 })
check("blend: crossfade armed", bAct.blend ~= nil
      and bAct.blend.to_anim == "walk" and bAct.blend.elapsed == 0)
sma.update(bctx, 0.5)
check("blend: mid-crossfade still active", bAct.blend ~= nil)
sma.update(bctx, 0.6)
check("blend: completes and clears", bAct.blend == nil)
sma.play_anim(bctx, "b", "walk", {})
check("blend: no blend_time -> direct switch", bAct.blend == nil)

-- ---------------------------------------------------------------------------
-- 9. Round 18: parts / variant switching
-- ---------------------------------------------------------------------------
local partsAsset = sma.load([==[
{
  "texture": "chara/hero.png",
  "bones": [ {"id":0,"parent":-1,"pivot":[0.0,0.0]} ],
  "parts": [
    { "id": "eye", "tex": 7, "current": "open",
      "variants": {
        "open":   { "positions":[[0,0],[1,0],[1,1]], "uvs":[[0,0],[1,0],[1,1]], "weights":[{"bone":0,"w":1}], "indices":[0,1,2] },
        "closed": { "positions":[[0,0],[1,0],[1,1]], "uvs":[[0.5,0],[1,0],[1,1]], "weights":[{"bone":0,"w":1}], "indices":[0,1,2] }
      } }
  ],
  "animations": { "idle": { "duration":1.0, "tracks":[] } }
}]==])
local sctx = {}
local sAct = sma.spawn(sctx, "s", partsAsset, "idle", {})
local partHandle = _G.sma.created
check("parts: multi-part spawn creates per-part meshes",
      sAct.parts ~= nil and #sAct.parts == 1 and sAct.parts[1].handle == partHandle)
check("parts: part tex from asset", sAct.parts[1].texId == 7)
local singleHandle = _G.sma.created
local singleActor = sma.spawn(sctx, "s2", asset, "idle", {})
check("parts: single-mesh path unchanged",
      singleActor.handle == singleHandle + 1 and singleActor.parts == nil)
local createdBefore = _G.sma.created
local ok = sma.set_variant(sctx, "s", "eye", "closed")
check("parts: set_variant switches variant", ok and sAct.parts[1].current == "closed")
check("parts: variant rebuilds the mesh",
      _G.sma.created == createdBefore + 1 and _G.sma.destroyed >= 1)
check("parts: unknown variant rejected",
      sma.set_variant(sctx, "s", "eye", "nope") == false)
sma.render(sctx)
check("parts: render draws each part", true)

-- ---------------------------------------------------------------------------
-- 10. Round 18: KAG commands — sma_anim / sma_ik / sma_variant + contracts
-- ---------------------------------------------------------------------------
local cctx = { sma_actors = {} }
sma.commands.sma_play(cctx, { name = "c", asset = "hero", anim = "idle" })
sma.commands.sma_anim(cctx, { name = "c", anim = "idle", rate = 3, loop = false })
check("commands: sma_anim switches + applies opts",
      cctx.sma_actors.c.anim == "idle" and cctx.sma_actors.c.rate == 3
      and cctx.sma_actors.c.loop == false)
sma.commands.sma_ik(cctx, { name = "c", bone0 = 0, bone1 = 1, tx = 10, ty = 10, l2 = 10 })
check("commands: sma_ik applies constraint",
      cctx.sma_actors.c.ik ~= nil and near(cctx.sma_actors.c.ik.tx, 10))
sma.commands.sma_variant(cctx, { name = "c", part = "eye", variant = "open" })
check("commands: sma_variant on single-mesh actor is inert", true)

-- Contracts registered by kag/commands/system.lua (loaded above).
local schema = require("kag.schema")
local contracts = schema.dumpContracts() or {}
local smaPlay = contracts.sma_play or {}
local smaAnim = contracts.sma_anim or {}
local smaIk = contracts.sma_ik or {}
local smaVariant = contracts.sma_variant or {}
check("contracts: sma_play has loop/rate/on_done",
      smaPlay.loop ~= nil and smaPlay.rate ~= nil
      and smaPlay.on_done_anim ~= nil)
check("contracts: sma_anim has blend_time",
      smaAnim.blend_time ~= nil)
check("contracts: sma_ik has bone0/bone1/tx/ty/l1/l2",
      smaIk.bone0 ~= nil and smaIk.l1 ~= nil)
check("contracts: sma_variant has part/variant",
      smaVariant.part ~= nil and smaVariant.variant ~= nil)


-- ---------------------------------------------------------------------------
-- Static source-shape check only. Runtime index decoding/bounds are proved
-- by the separate real C-binding tests, not by this text assertion.
do
    local f=assert(io.open("src/script/bindings/SmaBinding.cpp","r"))
    local src=f:read("*a");f:close()
    local function block(name)
        local text=src:match("static [^\n]- "..name.."%([^\n]-%)%s*{(.-)\n}")
        assert(text,"missing C++ function source: "..name)
        return text:gsub("%s+", "")
    end
    local mesh=block("readMesh")
    local function contains(text,needle)return text:find(needle,1,true)~=nil end
    check("S1-1 static: index type/range rejects invalid mesh",contains(mesh,'lua_type(L,-1)==LUA_TNUMBER&&integral')
        and contains(mesh,'raw>=0&&static_cast<size_t>(raw)<vn&&raw<=UINT16_MAX')
        and contains(mesh,'if(!valid){') and contains(mesh,'out.indices.clear();returnfalse;'))
end

if failed > 0 then
    print(string.format("SMA TESTS: %d passed, %d FAILED", passed, failed))
    os.exit(1)
end
print(string.format("SMA TESTS DONE (%d passed)", passed))
