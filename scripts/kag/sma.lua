-- =============================================================================
--  Caesura (AmeKAG) — kag/sma.lua
--  Skeletal Mesh Animation driver (SMA, Battle 4d S3).
--
--  Loads SMA assets (docs/design/skeletal-mesh-animation.md §2.2 JSON),
--  resolves the bone hierarchy into world poses (parent chain + pivot),
--  LERPs animation tracks over time, and drives the engine IMeshRenderer
--  through the `sma.*` Lua binding (SmaBinding.cpp). The C++ side (SmaSkinner
--  + SmaMeshRenderer, S2) does the per-vertex weight blending + GPU draw.
--
--  Pure math (JSON parse / hierarchy / LERP) is unit-tested in
--  tests/scripts/test_sma.lua; GPU drawing follows the deferred pattern
--  (binding absent in tests -> every call is an inert no-op).
-- =============================================================================

local sma = {}

-- ---------------------------------------------------------------------------
-- Minimal JSON parser (SMA data is fixed-shape; a full parser is overkill).
-- Supports objects / arrays / strings / numbers / true / false / null.
-- ---------------------------------------------------------------------------
local json_ok, json = pcall(require, "json")
if not json_ok or type(json) ~= "table" or not json.decode then
    json = nil
end

if not json then
    local function json_parse(text)
        local pos, n = 1, #text
        local function skipws()
            while pos <= n do
                local c = text:sub(pos, pos)
                if c == " " or c == "\t" or c == "\n" or c == "\r" then
                    pos = pos + 1
                else break end
            end
        end
        local function parse_string()
            assert(text:sub(pos, pos) == '"', "expected string")
            pos = pos + 1
            local out = {}
            while pos <= n do
                local c = text:sub(pos, pos)
                if c == '"' then pos = pos + 1 return table.concat(out) end
                if c == "\\" then
                    pos = pos + 1
                    local e = text:sub(pos, pos)
                    if e == "n" then out[#out + 1] = "\n"
                    elseif e == "t" then out[#out + 1] = "\t"
                    elseif e == "r" then out[#out + 1] = "\r"
                    elseif e == "u" then
                        out[#out + 1] = utf8.char(tonumber(
                            text:sub(pos + 1, pos + 4), 16))
                        pos = pos + 4
                    else out[#out + 1] = e end
                    pos = pos + 1
                else
                    out[#out + 1] = c
                    pos = pos + 1
                end
            end
            error("unterminated string")
        end
        local parse_value
        parse_value = function()
            skipws()
            local c = text:sub(pos, pos)
            if c == "{" then
                pos = pos + 1
                local obj = {}
                skipws()
                if text:sub(pos, pos) == "}" then pos = pos + 1 return obj end
                while true do
                    skipws()
                    local key = parse_string()
                    skipws()
                    assert(text:sub(pos, pos) == ":", "expected :")
                    pos = pos + 1
                    obj[key] = parse_value()
                    skipws()
                    local sep = text:sub(pos, pos)
                    if sep == "," then pos = pos + 1
                    elseif sep == "}" then pos = pos + 1 return obj
                    else error("expected , or }") end
                end
            elseif c == "[" then
                pos = pos + 1
                local arr = {}
                skipws()
                if text:sub(pos, pos) == "]" then pos = pos + 1 return arr end
                while true do
                    arr[#arr + 1] = parse_value()
                    skipws()
                    local sep = text:sub(pos, pos)
                    if sep == "," then pos = pos + 1
                    elseif sep == "]" then pos = pos + 1 return arr
                    else error("expected , or ]") end
                end
            elseif c == '"' then
                return parse_string()
            elseif c == "t" then
                assert(text:sub(pos, pos + 3) == "true", "bad true")
                pos = pos + 4
                return true
            elseif c == "f" then
                assert(text:sub(pos, pos + 4) == "false", "bad false")
                pos = pos + 5
                return false
            elseif c == "n" then
                assert(text:sub(pos, pos + 3) == "null", "bad null")
                pos = pos + 4
                return nil
            else
                local num = text:match("^-?%d+%.?%d*[eE]?[+-]?%d*", pos)
                assert(num and #num > 0, "bad number at " .. pos)
                pos = pos + #num
                return tonumber(num)
            end
        end
        skipws()
        local result = parse_value()
        skipws()
        assert(pos > n, "trailing content")
        return result
    end
    json = { decode = json_parse }
end

sma._json = json

--- sma.load(jsonText, opts) → parsed asset table
--  opts.validate=true runs the static schema checker (kag.sma_check) and
--  raises on violations -- creators can gate asset loading in CI/tests.
function sma.load(jsonText, opts)
    local asset = json.decode(jsonText)
    if opts and opts.validate then
        local ok, checker = pcall(require, "kag.sma_check")
        if ok and checker then
            local res = checker.validate(asset)
            if not res.ok then
                error("sma.load: asset validation failed:\n  "
                      .. table.concat(res.errors, "\n  "), 2)
            end
        end
    end
    return asset
end

--- sma.validate(jsonTextOrTable) → {ok, errors} — asset schema check
--  Wraps kag.sma_check (loaded lazily; without it the check reports
--  _unavailable instead of failing).
function sma.validate(jsonTextOrTable)
    local ok, checker = pcall(require, "kag.sma_check")
    if not ok or not checker then
        return { ok = true, errors = {}, _unavailable = true }
    end
    local t = jsonTextOrTable
    if type(t) == "string" then t = json.decode(t) end
    return checker.validate(t)
end

--- sma.validate_file(path) → {ok, errors, meta} — validate an asset file
--  (read + decode + check + structure summary for tooling/editor panels).
function sma.validate_file(path)
    local ok, checker = pcall(require, "kag.sma_check")
    if not ok or not checker then
        return { ok = false, errors = { "sma_check unavailable" }, meta = {} }
    end
    return checker.validate_file(path)
end

-- ---------------------------------------------------------------------------
-- Bone hierarchy -> world poses (2x3 affine matrices), then extract into
-- the interface BonePose {rot, scale, ox, oy} (SmaSkinner applies that
-- around the origin; the pivot is baked into the offset here).
-- ---------------------------------------------------------------------------

local function local_matrix(pivot, rot, scale, ox, oy)
    local c, s = math.cos(rot), math.sin(rot)
    local a, d = scale * c, scale * c
    local b, cc = scale * s, -scale * s
    -- T(pivot + o) ∘ R·S ∘ T(-pivot)
    local px, py = pivot[1], pivot[2]
    return {
        a, b, cc, d,
        (px + ox) - (a * px + cc * py),
        (py + oy) - (b * px + d * py),
    }
end

local function compose(parent, local_m)
    -- parent ∘ local_m (apply local first, then parent)
    local pa, pb, pc, pd, ptx, pty = table.unpack(parent)
    local la, lb, lc, ld, ltx, lty = table.unpack(local_m)
    return {
        pa * la + pc * lb, pb * la + pd * lb,
        pa * lc + pc * ld, pb * lc + pd * ld,
        pa * ltx + pc * lty + ptx,
        pb * ltx + pd * lty + pty,
    }
end

local function to_bone_pose(m)
    local a, b = m[1], m[2]
    return {
        rot = math.atan(b, a), -- Lua 5.4: atan(y, x)
        scale = math.sqrt(a * a + b * b),
        ox = m[5],
        oy = m[6],
    }
end

local IDENTITY = { 1, 0, 0, 1, 0, 0 }

--- sma.resolve_world(bones, locals) → array of BonePose (world)
--  `locals[i]` = {rot, scale, ox, oy} in the PARENT frame. Bones must be
--  ordered parent-before-child (design contract); roots have parent -1.
function sma.resolve_world(bones, locals)
    local world = {}
    local matrices = {}
    for i, bone in ipairs(bones) do
        local l = locals[i] or { rot = 0, scale = 1, ox = 0, oy = 0 }
        local m = local_matrix(bone.pivot or { 0, 0 }, l.rot, l.scale,
            l.ox, l.oy)
        local parent = bone.parent or -1
        if parent >= 0 and matrices[parent + 1] then
            m = compose(matrices[parent + 1], m)
        end
        matrices[i] = m
        world[i] = to_bone_pose(m)
    end
    return world
end

-- ---------------------------------------------------------------------------
-- Animation tracks: LERP over keyframes.
-- ---------------------------------------------------------------------------

local function lerp(a, b, t) return a + (b - a) * t end

local function sample_track(frames, t, duration, loop)
    if #frames == 1 then
        local f = frames[1]
        return f.rot or 0, f.scale or 1,
            (f.offset and f.offset[1]) or 0, (f.offset and f.offset[2]) or 0
    end
    -- Loop wrap: [sma_play loop=true] (design doc §4 promise) cycles the
    -- clip; non-looping clips clamp at the last frame.
    if loop and duration and duration > 0 and t > duration then
        t = t % duration
    end
    local prev = frames[1]
    for i = 2, #frames do
        local next_f = frames[i]
        if t <= next_f.t then
            local span = next_f.t - prev.t
            local k = (span > 0) and ((t - prev.t) / span) or 1
            k = math.max(0, math.min(1, k))
            return lerp(prev.rot or 0, next_f.rot or 0, k),
                lerp(prev.scale or 1, next_f.scale or 1, k),
                lerp((prev.offset and prev.offset[1]) or 0,
                     (next_f.offset and next_f.offset[1]) or 0, k),
                lerp((prev.offset and prev.offset[2]) or 0,
                     (next_f.offset and next_f.offset[2]) or 0, k)
        end
        prev = next_f
    end
    return prev.rot or 0, prev.scale or 1,
        (prev.offset and prev.offset[1]) or 0, (prev.offset and prev.offset[2]) or 0
end

--- sma.duration(asset, animName) → clip length in seconds (max keyframe
--  time across tracks; 0 when the clip has no frames).
function sma.duration(asset, animName)
    local anim = asset and asset.animations and asset.animations[animName]
    if not anim then return 0 end
    local d = 0
    for _, tr in ipairs(anim.tracks or {}) do
        for _, f in ipairs(tr.frames or {}) do
            if (f.t or 0) > d then d = f.t end
        end
    end
    return d
end

--- sma.sample(asset, animName, t, loop) → array of local BonePose
--  (parent frame). loop=true wraps t over the clip duration.
function sma.sample(asset, animName, t, loop)
    local anim = asset and asset.animations and asset.animations[animName]
    if not anim then return nil end
    local duration = sma.duration(asset, animName)
    local locals = {}
    local bones = asset.bones or {}
    for i, bone in ipairs(bones) do
        local track
        for _, tr in ipairs(anim.tracks or {}) do
            if tr.bone == bone.id then track = tr break end
        end
        if track then
            local rot, scale, ox, oy =
                sample_track(track.frames or {}, t, duration, loop)
            locals[i] = { rot = rot, scale = scale, ox = ox, oy = oy }
        end
    end
    return locals
end

-- ---------------------------------------------------------------------------
-- 2-bone IK (advanced): solve the root/mid rotation angles so the chain
--  reaches a world target. Pure cosine-law math — deterministic, tested.
--  Returns theta1 (root world angle), theta2 (mid world angle) or nil when
--  unreachable (target closer than |l1-l2| or farther than l1+l2).
-- ---------------------------------------------------------------------------
local function clamp01(v)
    return math.max(-1, math.min(1, v))
end

function sma.ik2bones(x1, y1, x2, y2, tx, ty, l1, l2)
    l1 = tonumber(l1) or 0
    l2 = tonumber(l2) or 0
    if l1 <= 0 or l2 <= 0 then return nil end
    local dx, dy = tx - x1, ty - y1
    local d = math.sqrt(dx * dx + dy * dy)
    if d < 1e-9 then return nil end
    if d > l1 + l2 or d < math.abs(l1 - l2) then return nil end
    local base = math.atan(dy, dx)
    local cos_a = clamp01((l1 * l1 + d * d - l2 * l2) / (2 * l1 * d))
    local alpha = math.acos(cos_a)
    local cos_b = clamp01((l1 * l1 + l2 * l2 - d * d) / (2 * l1 * l2))
    local beta = math.acos(cos_b)
    -- Bend "up" (positive alpha): the mid rotates +alpha from the base
    -- direction and the second bone folds back by (pi - beta).
    local theta1 = base + alpha
    local theta2 = theta1 - (math.pi - beta)
    return theta1, theta2
end

-- ---------------------------------------------------------------------------
-- Actor management + engine binding drive
-- ---------------------------------------------------------------------------

-- SmaBinding global (nil in unit tests / degraded). Resolved lazily so
-- tests can inject a recording mock after the module is loaded.
local function binding()
    return _G.sma
end

local function actor_pose_list(world)
    local poses = {}
    for _, p in ipairs(world) do
        poses[#poses + 1] = { p.rot, p.scale, p.ox, p.oy }
    end
    return poses
end

-- Build the engine mesh for one part variant (shared by spawn and
-- set_variant). variant = {positions, uvs, weights, indices}.
-- Weight layout (design doc §2.2): per-vertex entries -- either one entry
-- per vertex (single bone) or two interleaved entries ([2i-1] bone A,
-- [2i] bone B). The legacy single-entry layout keeps working unchanged.
local function part_vertex_weights(weights, i, vertCount)
    if #weights >= vertCount * 2 then
        return weights[(i - 1) * 2 + 1] or {}, weights[(i - 1) * 2 + 2] or {}
    end
    return weights[i] or {}, {}
end

local function create_part_mesh(variant)
    local verts, indices = variant.positions or {}, variant.indices or {}
    local weights = variant.weights or {}
    local luaVerts = {}
    for i, p in ipairs(verts) do
        local w, w2 = part_vertex_weights(weights, i, #verts)
        luaVerts[#luaVerts + 1] = {
            p[1] or 0, p[2] or 0,
            (variant.uvs and variant.uvs[i] and variant.uvs[i][1]) or 0,
            (variant.uvs and variant.uvs[i] and variant.uvs[i][2]) or 0,
            w.bone or 0, w.w or 1,
            w2.bone or 0, w2.w or 0,
        }
    end
    if binding() then
        return binding().create_mesh(luaVerts, indices) or 0
    end
    return 0
end

--- sma.spawn(ctx, name, asset, animName, opts) → actor
--  opts: {x, y, scale, texId, opacity, view, loop=true, rate=1,
--         on_done_anim}. Creates the engine mesh (or one per asset part)
--  and registers the actor for per-frame update/render.
function sma.spawn(ctx, name, asset, animName, opts)
    ctx.sma_actors = ctx.sma_actors or {}
    local old = ctx.sma_actors[name]
    if old then sma.despawn(ctx, name) end

    opts = opts or {}
    local actor = {
        name = name,
        asset = asset,
        anim = animName,
        handle = 0,
        parts = nil,          -- { {id, handle, texId, current}, ... }
        texId = opts.texId or 0,
        x = opts.x or 0,
        y = opts.y or 0,
        scale = opts.scale or 1,
        opacity = opts.opacity or 1,
        view = opts.view or 1, -- VIEW_MAIN; explicit view 0 remains valid.
        t = 0,
        loop = opts.loop ~= false,      -- design doc §4: loop defaults true
        rate = tonumber(opts.rate) or 1,
        paused = false,
        on_done_anim = opts.on_done_anim,  -- non-loop completion fallback
        blend = nil,                       -- crossfade state (play_anim)
        ik = nil,                          -- {chain, tx, ty, l1, l2}
    }
    local parts = asset.parts
    if parts and #parts > 0 then
        -- Multi-part (E-mote style): one mesh per part, current variant.
        actor.parts = {}
        for _, part in ipairs(parts) do
            local variant = (part.variants or {})[part.current or "default"]
                or (part.variants or {})[next(part.variants or {})]
            -- Asset tex=0 is an unspecified logical TextureManager ID, not
            -- GPU slot zero. Keep an explicit nonzero part override intact.
            local partTex = tonumber(part.tex)
            if partTex == nil or partTex == 0 then partTex = actor.texId end
            local entry = {
                id = part.id,
                handle = variant and create_part_mesh(variant) or 0,
                texId = partTex,
                current = part.current or "default",
            }
            actor.parts[#actor.parts + 1] = entry
        end
    else
        local mesh = asset.mesh or {}
        actor.handle = create_part_mesh(mesh)
    end
    ctx.sma_actors[name] = actor
    return actor
end

--- sma.despawn(ctx, name)
function sma.despawn(ctx, name)
    local actors = ctx.sma_actors
    if not actors or not actors[name] then return end
    local actor = actors[name]
    if actor.parts then
        for _, p in ipairs(actor.parts) do
            if binding() and p.handle and p.handle ~= 0 then
                binding().destroy_mesh(p.handle)
            end
        end
    elseif binding() and actor.handle and actor.handle ~= 0 then
        binding().destroy_mesh(actor.handle)
    end
    actors[name] = nil
end

-- Resolve the per-actor world poses (sample → optional crossfade blend →
-- resolve_world → optional IK override), returned as the binding pose list.
local function actor_world_poses(actor)
    local asset = actor.asset
    local locals = sma.sample(asset, actor.anim, actor.t, actor.loop)
    if not locals then return nil end
    if actor.blend then
        -- Crossfade: sample the outgoing clip too and lerp per bone.
        local from = sma.sample(asset, actor.blend.from_anim,
                                actor.blend.from_t, actor.blend.from_loop)
        if from then
            local k = math.max(0, math.min(1,
                actor.blend.elapsed / math.max(1e-6, actor.blend.blend_time)))
            for i = 1, #locals do
                local a, b = locals[i], from[i]
                if a and b then
                    a.rot = a.rot + (b.rot - a.rot) * (1 - k)
                    a.scale = a.scale + (b.scale - a.scale) * (1 - k)
                    a.ox = a.ox + (b.ox - a.ox) * (1 - k)
                    a.oy = a.oy + (b.oy - a.oy) * (1 - k)
                end
            end
        end
    end
    local world = sma.resolve_world(asset.bones or {}, locals)
    if actor.ik and world then
        local c = actor.ik.chain
        local a, b = world[c[1]], world[c[2]]
        if a and b then
            local l1 = actor.ik.l1
                or math.sqrt((b.ox - a.ox) ^ 2 + (b.oy - a.oy) ^ 2)
            local t1, t2 = sma.ik2bones(a.ox, a.oy, b.ox, b.oy,
                actor.ik.tx, actor.ik.ty, l1, actor.ik.l2)
            if t1 and t2 then
                -- Rotate the root by the delta; point the mid at the end.
                local d0 = math.atan(b.oy - a.oy, b.ox - a.ox)
                a.rot = a.rot + (t1 - d0)
                b.rot = t2
            end
        end
    end
    return world
end

--- sma.update(ctx, dt) — advance animation time + re-skin every actor.
--  Call from the entry script's engine_update (KAG mode).
function sma.update(ctx, dt)
    local actors = ctx.sma_actors
    if not actors then return end
    for _, actor in pairs(actors) do
        if not actor.paused then
            actor.t = actor.t + dt * actor.rate
            if actor.blend then
                actor.blend.elapsed = actor.blend.elapsed + dt
                if actor.blend.elapsed >= actor.blend.blend_time then
                    actor.blend = nil  -- crossfade complete
                end
            end
        end
        -- Non-loop completion: fall back to on_done_anim (or freeze at the
        -- last frame when no fallback is set).
        local dur = sma.duration(actor.asset, actor.anim)
        if not actor.loop and not actor.blend and dur > 0
           and actor.t >= dur and actor.on_done_anim then
            sma.play_anim(ctx, actor.name, actor.on_done_anim, { loop = true })
        end
        local world = actor_world_poses(actor)
        if world then
            if actor.parts then
                for _, p in ipairs(actor.parts) do
                    if binding() and p.handle and p.handle ~= 0 then
                        binding().update_mesh(p.handle, actor_pose_list(world))
                    end
                end
            elseif binding() and actor.handle and actor.handle ~= 0 then
                binding().update_mesh(actor.handle, actor_pose_list(world))
            end
        end
    end
end

--- sma.render(ctx) — draw every actor. Call from engine_render (KAG mode).
function sma.render(ctx)
    local actors = ctx.sma_actors
    if not actors then return end
    for _, actor in pairs(actors) do
        if actor.parts then
            for _, p in ipairs(actor.parts) do
                if binding() and p.handle and p.handle ~= 0 then
                    binding().draw_mesh(p.handle, actor.view, p.texId,
                        actor.x, actor.y, actor.scale, actor.opacity)
                end
            end
        elseif binding() and actor.handle and actor.handle ~= 0 then
            binding().draw_mesh(actor.handle, actor.view, actor.texId,
                actor.x, actor.y, actor.scale, actor.opacity)
        end
    end
end

--- Playback controls (round 18).
--- sma.set_skin_mode(mode) / sma.get_skin_mode() — S5 GPU compute
--  skinning switch ("auto" | "cpu" | "gpu"; forwards to the binding).
function sma.set_skin_mode(mode)
    if binding() and binding().set_skin_mode then
        binding().set_skin_mode(mode or "auto")
    end
end

function sma.get_skin_mode()
    if binding() and binding().get_skin_mode then
        return binding().get_skin_mode()
    end
    return "cpu"
end

--- sma.pause(ctx, name) / sma.resume(ctx, name) — freeze / resume the clip.
function sma.pause(ctx, name)
    local a = ctx.sma_actors and ctx.sma_actors[name]
    if a then a.paused = true end
end

function sma.resume(ctx, name)
    local a = ctx.sma_actors and ctx.sma_actors[name]
    if a then a.paused = false end
end

--- sma.seek(ctx, name, t) — jump to a clip time (seconds).
function sma.seek(ctx, name, t)
    local a = ctx.sma_actors and ctx.sma_actors[name]
    if a then a.t = tonumber(t) or 0 end
end

--- sma.set_rate(ctx, name, rate) — playback speed multiplier.
function sma.set_rate(ctx, name, rate)
    local a = ctx.sma_actors and ctx.sma_actors[name]
    if a then a.rate = tonumber(rate) or 1 end
end

--- sma.play_anim(ctx, name, animName, opts) — switch animation without
--  rebuilding the mesh. opts: {loop, rate, on_done_anim, blend_time}.
--  blend_time > 0 crossfades from the current clip (both sampled and
--  lerped per bone); the outgoing clip's loop mode is preserved.
function sma.play_anim(ctx, name, animName, opts)
    local actor = ctx.sma_actors and ctx.sma_actors[name]
    if not actor then return end
    opts = opts or {}
    local blend_time = tonumber(opts.blend_time) or 0
    if blend_time > 0 and actor.anim and actor.anim ~= animName then
        actor.blend = {
            from_anim = actor.anim,
            from_t = actor.t,
            from_loop = actor.loop,
            to_anim = animName,
            blend_time = blend_time,
            elapsed = 0,
        }
    end
    actor.anim = animName
    actor.t = 0
    actor.loop = opts.loop ~= false
    if opts.rate then actor.rate = tonumber(opts.rate) end
    if opts.on_done_anim ~= nil then
        actor.on_done_anim = opts.on_done_anim
    end
end

--- IK constraints (advanced): sma.set_ik(ctx, name, chain, tx, ty, l1?, l2?)
--  chain = {rootBoneId, midBoneId} (bone ids as declared in the asset).
--  l1 defaults to the current root→mid world distance; l2 is required
--  (the mid→end length). The chain bones' world rotations are overridden
--  every frame so the chain reaches (tx, ty).
function sma.set_ik(ctx, name, chain, tx, ty, l1, l2)
    local actor = ctx.sma_actors and ctx.sma_actors[name]
    if not actor then return end
    actor.ik = {
        chain = chain,
        tx = tonumber(tx) or 0,
        ty = tonumber(ty) or 0,
        l1 = tonumber(l1) or nil,
        l2 = tonumber(l2) or 0,
    }
end

--- sma.clear_ik(ctx, name) — remove the IK constraint.
function sma.clear_ik(ctx, name)
    local actor = ctx.sma_actors and ctx.sma_actors[name]
    if actor then actor.ik = nil end
end

--- sma.set_variant(ctx, name, partId, variant, texId?) — switch a part's
--  variant (E-mote style parts/expression switching). The part's mesh is
--  rebuilt from the variant geometry; texId optionally updates the part
--  texture. Unknown variants keep the current one (warning).
function sma.set_variant(ctx, name, partId, variant, texId)
    local actor = ctx.sma_actors and ctx.sma_actors[name]
    if not actor or not actor.parts then return false end
    local parts = actor.asset.parts or {}
    for i, p in ipairs(actor.parts) do
        if p.id == partId then
            local spec = parts[i]
            local v = spec and (spec.variants or {})[variant]
            if not v then
                print("[sma_set_variant] unknown variant '" .. tostring(variant)
                      .. "' for part '" .. tostring(partId) .. "'")
                return false
            end
            if binding() and p.handle and p.handle ~= 0 then
                binding().destroy_mesh(p.handle)
            end
            p.handle = create_part_mesh(v)
            p.current = variant
            if texId ~= nil then p.texId = tonumber(texId) or p.texId end
            return true
        end
    end
    return false
end

-- ---------------------------------------------------------------------------
-- KAG commands: [sma_play] / [sma_stop]
-- ---------------------------------------------------------------------------

--- [sma_play name="hero" asset="hero" anim="idle" x= y= scale= tex= view=
--         loop=true rate=1 on_done_anim="idle"]
--  asset must be registered via sma.register() (Lua/[iscript] side).
local assets = {}

--- sma.register(name, assetTable) — register a parsed SMA asset
function sma.register(name, assetTable)
    assets[name] = assetTable
end

--- sma.get(name) → registered asset or nil
function sma.get(name)
    return assets[name]
end

local sma_commands = {}

local function play_params(params)
    return {
        x = tonumber(params.x) or 0,
        y = tonumber(params.y) or 0,
        scale = tonumber(params.scale) or 1,
        texId = tonumber(params.tex) or 0,
        opacity = tonumber(params.opacity) or 1,
        view = tonumber(params.view), -- Omission inherits spawn's VIEW_MAIN.
        loop = params.loop ~= false and params.loop ~= "false",
        rate = tonumber(params.rate) or 1,
        on_done_anim = params.on_done_anim,
    }
end

function sma_commands.sma_play(ctx, params)
    local name = params.name or params[1] or "actor"
    local asset = assets[params.asset or name]
    if not asset then
        print("[sma_play] unknown asset '" .. tostring(params.asset or name) .. "'")
        return
    end
    local anim = params.anim or "idle"
    sma.spawn(ctx, name, asset, anim, play_params(params))
end

function sma_commands.sma_anim(ctx, params)
    local name = params.name or params[1] or "actor"
    sma.play_anim(ctx, name, params.anim or "idle", {
        loop = params.loop ~= false and params.loop ~= "false",
        rate = tonumber(params.rate),
        on_done_anim = params.on_done_anim,
        blend_time = tonumber(params.blend_time),
    })
end

function sma_commands.sma_ik(ctx, params)
    local name = params.name or params[1] or "actor"
    local chain = { tonumber(params.bone0) or 0, tonumber(params.bone1) or 1 }
    sma.set_ik(ctx, name, chain,
        tonumber(params.tx) or 0, tonumber(params.ty) or 0,
        params.l1, tonumber(params.l2) or 0)
end

function sma_commands.sma_variant(ctx, params)
    local name = params.name or params[1] or "actor"
    sma.set_variant(ctx, name, params.part, params.variant,
        tonumber(params.tex))
end

function sma_commands.sma_stop(ctx, params)
    sma.despawn(ctx, params.name or params[1] or "actor")
end

sma.commands = sma_commands

return sma
