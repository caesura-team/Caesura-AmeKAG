extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include "RenderBinding.h"
#include "FontRestoreBinding.h"
#include <cmath>
#include "../../di/BackendRegistry.h"
#include "../../render/api/IRenderDevice.h"
#include "../../render/api/ITextureManager.h"
#include "../../render/api/IVideoPlayer.h"
#include "../../resource/api/IAsyncLoader.h"
#include "../../resource/api/IResourceGenerationTracker.h"
#include "../../debug/api/DebugLog.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Caesura {

// -- Helpers: resolve backend pointers from Lua registry (set by Engine::initScriptingPhase)
// Hot-path optimization: registry string lookups happen on every binding
// call (render_text/submit_batch run thousands of times per frame). Each
// getter caches its pointer after first resolution; the caches are cleared
// by registerRenderBinding (a fresh lua_State in tests must re-resolve
// against its own registry). Backend instances are stable for the engine's
// lifetime (device recovery mutates, never replaces).

static ITextureManager* g_cachedTexture = nullptr;
static IRenderDevice*   g_cachedRender  = nullptr;
static IVideoPlayer*    g_cachedVideo   = nullptr;
static IAsyncLoader*    g_cachedAsync   = nullptr;

static void invalidateBindingCaches() {
    g_cachedTexture = nullptr;
    g_cachedRender  = nullptr;
    g_cachedVideo   = nullptr;
    g_cachedAsync   = nullptr;
}

static ITextureManager* getTexture(lua_State* L) {
    if (g_cachedTexture) return g_cachedTexture;
    lua_getfield(L, LUA_REGISTRYINDEX, "Caesura.TextureManager");
    auto* tm = (ITextureManager*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!tm) tm = BackendRegistry::instance().getTextureManager();
    g_cachedTexture = tm;
    return tm;  // set by Engine::initScriptingPhase; null in test env OK
}

static IRenderDevice* getRender(lua_State* L) {
    if (g_cachedRender) return g_cachedRender;
    lua_getfield(L, LUA_REGISTRYINDEX, "Caesura.RenderDevice");
    auto* dev = (IRenderDevice*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!dev) dev = BackendRegistry::instance().getRenderDevice();
    g_cachedRender = dev;
    return dev;
}

static IVideoPlayer* getVideo(lua_State* L) {
    if (g_cachedVideo) return g_cachedVideo;
    lua_getfield(L, LUA_REGISTRYINDEX, "Caesura.VideoPlayer");
    auto* vp = (IVideoPlayer*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!vp) vp = BackendRegistry::instance().getVideoPlayer();
    g_cachedVideo = vp;
    return vp;  // nullable — headless mode may not have VideoPlayer
}

static IAsyncLoader* getAsync(lua_State* L) {
    if (g_cachedAsync) return g_cachedAsync;
    lua_getfield(L, LUA_REGISTRYINDEX, "Caesura.AsyncLoader");
    auto* al = (IAsyncLoader*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!al) al = BackendRegistry::instance().getAsyncLoader();
    g_cachedAsync = al;
    return al;
}

// -- Internal texture helper: delegates to TextureManager -------------------

// Resolve a texture ID: first try TextureManager, then RTT viewport map.
// This handles the case where Lua passes an RTT view ID as "tex" field.
static RenderTextureHandle textureManagerHandle(uint32_t rawHandle) {
    if (rawHandle == 0 || rawHandle > UINT16_MAX ||
        rawHandle == INVALID_RENDER_HANDLE_INDEX) {
        return {};
    }
    return RenderTextureHandle{static_cast<uint16_t>(rawHandle)};
}

static RenderTextureHandle resolveTexture(lua_State* L, uint32_t id, IRenderDevice* dev) {
    auto* texture = getTexture(L);
    uint32_t rawHandle = texture ? texture->getTextureHandle(id) : 0;
    RenderTextureHandle tex = textureManagerHandle(rawHandle);
    if (!tex.isValid() && dev && id != 0) {
        ViewportHandle vp{ id };
        tex = dev->getViewportTexture(vp);
    }
    return tex;
}

// NOTE: the string-keyed getTableInt/getTableFloat helpers were removed with
// the old submit_batch record format (t11). The batch path now reads positional
// integer slots; see batchNum below. Nothing else in this file read tables by
// string key, so keeping them would only produce unused-function warnings.

// -- Render.load_texture(file) ----------------------------------------------

static int lua_Render_load_texture(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    if (file == nullptr || file[0] == '\0') {
        lua_pushinteger(L, 0);
        return 1;
    }

    auto* texture = getTexture(L);
    if (!texture) {
        lua_pushnil(L);
        lua_pushstring(L, "TextureManager not available");
        return 2;
    }

    uint32_t texId = texture->loadTexture(file);
    if (texId == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Failed to load texture");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)texId);
    return 1;
}

// -- Render.destroy_texture(texId) ------------------------------------------

static int lua_Render_destroy_texture(lua_State* L) {
    uint32_t texId = (uint32_t)luaL_checkinteger(L, 1);
    auto* texture = getTexture(L);
    if (!texture) {
        lua_pushboolean(L, 0);
        return 1;
    }
    texture->destroyTexture(texId);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.create_solid_texture(r, g, b, a) --------------------------------

static int lua_Render_create_solid_texture(lua_State* L) {
    int r = (int)luaL_checkinteger(L, 1);
    int g = (int)luaL_checkinteger(L, 2);
    int b = (int)luaL_checkinteger(L, 3);
    int a = (int)luaL_optinteger(L, 4, 255);

    auto* texture = getTexture(L);
    if (!texture) {
        lua_pushnil(L); lua_pushstring(L, "TextureManager not available"); return 2;
    }

    uint32_t texId = texture->createSolidTexture(
        (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a);
    if (texId == 0) {
        lua_pushnil(L); lua_pushstring(L, "GPU solid tex failed"); return 2;
    }
        lua_pushinteger(L, (lua_Integer)texId);
    return 1;
}

// -- Render.get_resolution() ------------------------------------------------

static int lua_Render_get_resolution(lua_State* L) {
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushinteger(L, 0); lua_pushinteger(L, 0); return 2; }
    lua_pushinteger(L, dev->getBackbufferWidth());
    lua_pushinteger(L, dev->getBackbufferHeight());
    return 2;
}

// -- Render.set_screen_offset(dx, dy) ----------------------------------------
// Screen-offset pan (camera/quakes): shifts VIEW_MAIN's rect. dx/dy are
// pixel ints; Lua's [camera]/[quake] drive this.
static int lua_Render_set_screen_offset(lua_State* L) {
    // Camera/quake pass fractional offsets every frame (ease products,
    // random shakes) -- luaL_checkinteger would reject them. Round.
    int dx = (int)llround(luaL_checknumber(L, 1));
    int dy = (int)llround(luaL_checknumber(L, 2));
    auto* dev = getRender(L);
    if (dev) dev->setScreenOffset(dx, dy);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.set_view_name(id, name) -----------------------------------------

static int lua_Render_set_view_name(lua_State* L) {
    uint16_t viewId = (uint16_t)luaL_checkinteger(L, 1);
    const char* name = luaL_checkstring(L, 2);
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    dev->setDebugName(viewId, name);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.submit_batch(...) -- batch-submit layer quads -------------------

// Positional batch wire format, produced by scripts/layers.lua Layers.render.
// batch[1] = live command count; command i occupies the 16 slots starting at
// 1 + (i-1)*16. Reading by integer index avoids the ~11 string-keyed
// lua_getfield hashes the old per-command-table format paid for every quad.
//
// The producer REUSES one array across frames, so slots past the live count
// hold stale numbers from a busier frame. batch[1] is the only length
// authority: lua_rawlen must never be used here.
namespace batchfmt {
enum : int {
    kCount    = 1,   // batch[1]
    kStride   = 16,
    kViewId   = 1,   // offsets within a command, 1-based
    kTex      = 2,
    kRt       = 3,
    kX        = 4,
    kY        = 5,
    kW        = 6,
    kH        = 7,
    kOpacity  = 8,
    kBlend    = 9,
    kScaleX   = 10,
    kScaleY   = 11,
    kRotation = 12,
    kClipX    = 13,
    kClipY    = 14,
    kClipW    = 15,
    kClipH    = 16,
};
}  // namespace batchfmt

// Read one positional slot as a double. Absent/non-numeric -> def, so a
// truncated array degrades to defaults instead of reading garbage.
static inline double batchNum(lua_State* L, int tableIdx, int slot, double def) {
    lua_rawgeti(L, tableIdx, slot);
    double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static int lua_Render_submit_batch(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }

    // Live length comes from the header slot, NEVER lua_rawlen: the producer's
    // array is reused and its tail intentionally holds stale values.
    int n = (int)batchNum(L, 1, batchfmt::kCount, 0);
    if (n < 0) n = 0;
    if (n > 1024) n = 1024;  // per-frame batch cap: a runaway count must not
                             // submit thousands of GPU draws in one frame
    if (n == 0) { lua_pushboolean(L, 1); return 1; }

    dev->beginBatch();

    // Batch-level texture resolution cache: many quads share the same texId
    // (a layer tree repeats the same image), so resolve each id once instead
    // of hitting the TextureManager map per quad. Hash lookup, not the old
    // 256-entry linear scan (which cost O(distinct textures) per quad).
    // static: the map's buckets survive across frames, so a steady-state
    // scene performs zero allocation here. Cleared every call because texture
    // ids are recycled by the TextureManager and a stale handle would blit a
    // freed texture.
    static std::unordered_map<uint32_t, RenderTextureHandle> texCache;
    texCache.clear();

    for (int i = 0; i < n; i++) {
        const int base = batchfmt::kCount + i * batchfmt::kStride;

        uint32_t texId   = (uint32_t)batchNum(L, 1, base + batchfmt::kTex, 0);
        float    x       = (float)batchNum(L, 1, base + batchfmt::kX, 0);
        float    y       = (float)batchNum(L, 1, base + batchfmt::kY, 0);
        float    w       = (float)batchNum(L, 1, base + batchfmt::kW, 128);
        float    h       = (float)batchNum(L, 1, base + batchfmt::kH, 128);
        int      opacity = (int)batchNum(L, 1, base + batchfmt::kOpacity, 255);
        opacity = std::min(std::max(opacity, 0), 255);  // clamp (RD-5)

        RenderTextureHandle tex;
        if (texId != 0) {
            auto it = texCache.find(texId);
            if (it != texCache.end()) {
                tex = it->second;
            } else {
                auto* texture = getTexture(L);
                uint32_t rawHandle = texture ? texture->getTextureHandle(texId) : 0;
                tex = textureManagerHandle(rawHandle);
                texCache.emplace(texId, tex);
            }
        }
        // If no explicit texture or tex is invalid, check if an RTT viewport handle was supplied
        if (!tex.isValid()) {
            uint32_t rtId = (uint32_t)batchNum(L, 1, base + batchfmt::kRt, 0);
            if (rtId != 0 && dev) {
                tex = dev->getViewportTexture(ViewportHandle{ rtId });
            }
        }
        if (tex.isValid()) {
            dev->blitTexture(VIEW_MAIN, (uint32_t)tex.idx, x, y, w, h, (uint8_t)opacity);
        }
    }

    dev->flushBatch();
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.submit_blend(baseTex, blendTex, mode, baseAlpha, blendAlpha, globalAlpha)

static int lua_Render_submit_blend(lua_State* L) {
    uint32_t baseTexId  = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t blendTexId = (uint32_t)luaL_checkinteger(L, 2);
    int      mode       = (int)luaL_checkinteger(L, 3);
    float    baseAlpha  = (float)luaL_optnumber(L, 4, 1.0);
    float    blendAlpha = (float)luaL_optnumber(L, 5, 1.0);
    float    globalAlpha = (float)luaL_optnumber(L, 6, 1.0);

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    RenderTextureHandle baseTex  = resolveTexture(L,baseTexId, dev);
    RenderTextureHandle blendTex = resolveTexture(L,blendTexId, dev);

    if (!baseTex.isValid() || !blendTex.isValid()) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok, "[Render] submit_blend: invalid texture(s)");
        lua_pushboolean(L, 0); return 1;
    }

    dev->submitBlend(VIEW_MAIN, baseTex, blendTex,
                         mode, baseAlpha, blendAlpha, globalAlpha);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.submit_transition(fromTex, toTex, ruleTex, method, progress) ----

static int lua_Render_submit_transition(lua_State* L) {
    uint32_t fromTexId = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t toTexId   = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t ruleTexId = (uint32_t)luaL_optinteger(L, 3, 0);
    int      method    = (int)luaL_checkinteger(L, 4);
    float    progress  = (float)luaL_checknumber(L, 5);

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    RenderTextureHandle fromTex = resolveTexture(L,fromTexId, dev);
    RenderTextureHandle toTex   = resolveTexture(L,toTexId, dev);

    if (!fromTex.isValid() || !toTex.isValid()) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok, "[Render] submit_transition: invalid texture(s)");
        lua_pushboolean(L, 0); return 1;
    }

    RenderTextureHandle ruleTex;
    if (ruleTexId != 0) {
        ruleTex = resolveTexture(L,ruleTexId, dev);
    }

    dev->submitTransition(VIEW_MAIN, fromTex, toTex, ruleTex,
                               method, progress);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.set_color_filter(preset) — accessibility color filter
// "none" | "deuteranopia" | "protanopia" | "tritanopia" | "grayscale" | "high_contrast"
static int lua_Render_set_color_filter(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    IRenderDevice::ColorFilterPreset preset = IRenderDevice::ColorFilterPreset::None;
    if (strcmp(name, "deuteranopia") == 0) preset = IRenderDevice::ColorFilterPreset::Deuteranopia;
    else if (strcmp(name, "protanopia") == 0) preset = IRenderDevice::ColorFilterPreset::Protanopia;
    else if (strcmp(name, "tritanopia") == 0) preset = IRenderDevice::ColorFilterPreset::Tritanopia;
    else if (strcmp(name, "grayscale") == 0) preset = IRenderDevice::ColorFilterPreset::Grayscale;
    else if (strcmp(name, "high_contrast") == 0) preset = IRenderDevice::ColorFilterPreset::HighContrast;
    else if (strcmp(name, "none") != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, dev->setColorFilter(preset) ? 1 : 0);
    return 1;
}

// -- Render.submit_vfx(srcTex, effect, fadeAlpha, fadeR, fadeG, fadeB, blurRadius, quakeX, quakeY)

static int lua_Render_submit_vfx(lua_State* L) {
    uint32_t srcTexId  = (uint32_t)luaL_checkinteger(L, 1);
    int      effect    = (int)luaL_checkinteger(L, 2);
    float    fadeAlpha = (float)luaL_optnumber(L, 3, 1.0);
    float    fadeR     = (float)luaL_optnumber(L, 4, 0.0);
    float    fadeG     = (float)luaL_optnumber(L, 5, 0.0);
    float    fadeB     = (float)luaL_optnumber(L, 6, 0.0);
    float    blurRadius = (float)luaL_optnumber(L, 7, 0.0);
    float    quakeX    = (float)luaL_optnumber(L, 8, 0.0);
    float    quakeY    = (float)luaL_optnumber(L, 9, 0.0);

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    RenderTextureHandle srcTex = resolveTexture(L,srcTexId, dev);
    if (!srcTex.isValid()) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok, "[Render] submit_vfx: invalid texture");
        lua_pushboolean(L, 0); return 1;
    }

    dev->submitVFX(VIEW_MAIN, srcTex, effect,
                       fadeAlpha, fadeR, fadeG, fadeB,
                       blurRadius, quakeX, quakeY);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Post-expression chain (round 102): Render.set_postfx / clear_postfx ----
// Kind strings: "bloom" | "vignette" | "lut" | "softblur". The binding keeps
// a per-kind handle cache in the Lua registry (_POSTFX_HANDLES), so repeated
// set_postfx(kind, params) updates that effect instead of stacking new ones.
// Null/software renderers report isPostFxSupported=false; set_postfx then
// returns 0 (no-op) without touching the device.

static bool resolvePostFxKind(const char* name, IRenderDevice::PostFxKind& kind) {
    if (strcmp(name, "bloom") == 0)         kind = IRenderDevice::PostFxKind::Bloom;
    else if (strcmp(name, "vignette") == 0) kind = IRenderDevice::PostFxKind::Vignette;
    else if (strcmp(name, "lut") == 0)      kind = IRenderDevice::PostFxKind::LutColorGrade;
    else if (strcmp(name, "softblur") == 0) kind = IRenderDevice::PostFxKind::SoftBlur;
    else if (strcmp(name, "lut3d") == 0)    kind = IRenderDevice::PostFxKind::Lut3D;
    else return false;
    return true;
}

static float postFxField(lua_State* L, int tableIdx, const char* key, float def) {
    lua_getfield(L, tableIdx, key);
    float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static IRenderDevice::PostFxParams resolvePostFxParams(lua_State* L, int tableIdx) {
    IRenderDevice::PostFxParams p;
    p.strength = postFxField(L, tableIdx, "strength", 1.0f);
    p.radius   = postFxField(L, tableIdx, "radius", 0.0f);
    p.amount   = postFxField(L, tableIdx, "amount", 0.0f);
    p.lutMix   = postFxField(L, tableIdx, "lutMix", 0.0f);
    // rgb: "r,g,b" (0..255) or a Lua table {r,g,b}; default white tint.
    lua_getfield(L, tableIdx, "rgb");
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1); p.r = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) / 255.0f : 1.0f; lua_pop(L, 1);
        lua_rawgeti(L, -1, 2); p.g = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) / 255.0f : 1.0f; lua_pop(L, 1);
        lua_rawgeti(L, -1, 3); p.b = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) / 255.0f : 1.0f; lua_pop(L, 1);
    } else if (lua_isstring(L, -1)) {
        const char* s = lua_tostring(L, -1);
        float rr = -1.0f, gg = -1.0f, bb = -1.0f;
        const int n = sscanf(s, "%f,%f,%f", &rr, &gg, &bb);
        if (n == 3 && rr >= 0 && gg >= 0 && bb >= 0) {
            p.r = rr / 255.0f; p.g = gg / 255.0f; p.b = bb / 255.0f;
        }
    }
    lua_pop(L, 1);
    // Clamp to [0,1] for tint components and strength/lutMix (schema clamps too).
    if (p.r < 0) p.r = 0; if (p.r > 1) p.r = 1;
    if (p.g < 0) p.g = 0; if (p.g > 1) p.g = 1;
    if (p.b < 0) p.b = 0; if (p.b > 1) p.b = 1;
    if (p.strength < 0) p.strength = 0; if (p.strength > 1) p.strength = 1;
    if (p.lutMix < 0) p.lutMix = 0; if (p.lutMix > 1) p.lutMix = 1;
    if (p.radius < 0) p.radius = 0;
    if (p.amount < 0) p.amount = 0;
    return p;
}

// t214: Lut3D params -- lutId (TextureManager id) + optional lutSize (0 =
// derive from the texture height; the 2D-packed layout requires
// width == N*N). The texture is BORROWED: the manager stays the owner.
static void resolveLut3D(lua_State* L, int tableIdx, IRenderDevice::PostFxParams& p,
                         bool& isLut3D) {
    p.lutTexture = {};
    p.lutSize = 0;
    if (!isLut3D) return;
    lua_getfield(L, tableIdx, "lutId");
    const lua_Integer lutId = lua_isnumber(L, -1) ? (lua_Integer)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    lua_getfield(L, tableIdx, "lutSize");
    const lua_Integer lutSize = lua_isnumber(L, -1) ? (lua_Integer)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    if (lutId <= 0) return; // clear request: no texture -> stage skipped
    ITextureManager* tm = getTexture(L);
    if (!tm || !tm->isValid((uint32_t)lutId)) return;
    uint16_t w = 0, h = 0;
    tm->getTextureSizeById((uint32_t)lutId, w, h);
    uint16_t n = (lutSize > 0) ? (uint16_t)lutSize : h;
    if (n < 2 || w != (uint16_t)(n * n)) return; // LUT layout contract violated
    const uint32_t bgfxIdx = tm->getTextureHandle((uint32_t)lutId);
    if (bgfxIdx == 0) return; // manager reports invalid (idx 0 = invalid per contract)
    p.lutTexture = RenderTextureHandle{ static_cast<uint16_t>(bgfxIdx) };
    p.lutSize = (uint8_t)n;
}

// Get-or-create the _POSTFX_HANDLES table in the Lua registry, pushing it.
static void postFxHandleTable(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "_POSTFX_HANDLES");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "_POSTFX_HANDLES");
    }
}

// Fetch stored handle for a kind string (arg index 1 holds the kind string).
static uint32_t postFxHandle(lua_State* L, int kindIndex) {
    postFxHandleTable(L);
    lua_pushvalue(L, kindIndex); // key
    lua_rawget(L, -2);           // value
    const uint32_t h = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 2);
    return h;
}

static void postFxStoreHandle(lua_State* L, int kindIndex, uint32_t handle) {
    postFxHandleTable(L);
    lua_pushvalue(L, kindIndex);
    lua_pushinteger(L, (lua_Integer)handle);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

// -- Render.set_postfx(kind, params_table) -> handle (0 = unsupported) -----
static int lua_Render_set_postfx(lua_State* L) {
    const char* kindName = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE); // params table
    IRenderDevice::PostFxKind kind;
    if (!resolvePostFxKind(kindName, kind)) { lua_pushinteger(L, 0); return 1; }
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushinteger(L, 0); return 1; }
    if (!dev->isPostFxSupported(kind)) { lua_pushinteger(L, 0); return 1; }

    IRenderDevice::PostFxParams params = resolvePostFxParams(L, 2);
    bool isLut3D = (kind == IRenderDevice::PostFxKind::Lut3D);
    resolveLut3D(L, 2, params, isLut3D);
    uint32_t handle = postFxHandle(L, 1); // key is the kind string itself
    if (handle == 0) {
        handle = (uint32_t)dev->createPostFx(kind, params);
        if (handle == 0) { lua_pushinteger(L, 0); return 1; }
        postFxStoreHandle(L, 1, handle);
    } else {
        dev->setPostFxParams(handle, params);
    }
    lua_pushinteger(L, (lua_Integer)handle);
    return 1;
}

// -- Render.destroy_postfx(kind) -> bool -----------------------------------
static int lua_Render_destroy_postfx(lua_State* L) {
    const char* kindName = luaL_checkstring(L, 1);
    IRenderDevice::PostFxKind kind;
    if (!resolvePostFxKind(kindName, kind)) { lua_pushboolean(L, 0); return 1; }
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    const uint32_t handle = postFxHandle(L, 1);
    if (handle != 0 && dev->isPostFxSupported(kind)) {
        dev->destroyPostFx(handle);
        postFxHandleTable(L);
        lua_pushvalue(L, 1);
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_pop(L, 1);
    }
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.clear_postfx() -> bool: disable the whole chain ----------------
static int lua_Render_clear_postfx(lua_State* L) {
    IRenderDevice* dev = getRender(L);
    if (dev) dev->clearPostFx();
    // Drop all cached handles (orphan the old table; GC reclaims it).
    lua_getfield(L, LUA_REGISTRYINDEX, "_POSTFX_HANDLES");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "_POSTFX_HANDLES");
    }
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.is_postfx_supported(kind) -> bool ------------------------------
static int lua_Render_is_postfx_supported(lua_State* L) {
    const char* kindName = luaL_checkstring(L, 1);
    IRenderDevice::PostFxKind kind;
    if (!resolvePostFxKind(kindName, kind)) { lua_pushboolean(L, 0); return 1; }
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, dev->isPostFxSupported(kind) ? 1 : 0);
    return 1;
}

// -- Render.is_postfx_active() -> bool -------------------------------------
static int lua_Render_is_postfx_active(lua_State* L) {
    IRenderDevice* dev = getRender(L);
    lua_pushboolean(L, dev && dev->isPostFxActive() ? 1 : 0);
    return 1;
}

// -- Render.stretch_blt(dstTexId, dx,dy,dw,dh, srcTexId, sx,sy,sw,sh, filter) --

static int lua_Render_stretch_blt(lua_State* L) {
    uint32_t dstTexId = (uint32_t)luaL_checkinteger(L, 1);
    float    dx = (float)luaL_checknumber(L, 2);
    float    dy = (float)luaL_checknumber(L, 3);
    float    dw = (float)luaL_checknumber(L, 4);
    float    dh = (float)luaL_checknumber(L, 5);
    uint32_t srcTexId = (uint32_t)luaL_checkinteger(L, 6);
    float    sx = (float)luaL_checknumber(L, 7);
    float    sy = (float)luaL_checknumber(L, 8);
    float    sw = (float)luaL_checknumber(L, 9);
    float    sh = (float)luaL_checknumber(L, 10);
    int      filter = (int)luaL_optinteger(L, 11, 1);

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    RenderTextureHandle srcTex = resolveTexture(L,srcTexId, dev);
    if (!srcTex.isValid()) {
        lua_pushboolean(L, 0); return 1;
    }

    dev->stretchBlt(VIEW_MAIN, dstTexId,
                     dx, dy, dw, dh,
                     (uint32_t)srcTex.idx,
                     sx, sy, sw, sh,
                     filter);
    lua_pushboolean(L, 1); return 1;
}

// -- Render.affine_blt(dstTexId, dx,dy,dw,dh, srcTexId, sx,sy,sw,sh, matrix) --

static int lua_Render_affine_blt(lua_State* L) {
    uint32_t dstTexId = (uint32_t)luaL_checkinteger(L, 1);
    float    dx = (float)luaL_checknumber(L, 2);
    float    dy = (float)luaL_checknumber(L, 3);
    float    dw = (float)luaL_checknumber(L, 4);
    float    dh = (float)luaL_checknumber(L, 5);
    uint32_t srcTexId = (uint32_t)luaL_checkinteger(L, 6);
    float    sx = (float)luaL_checknumber(L, 7);
    float    sy = (float)luaL_checknumber(L, 8);
    float    sw = (float)luaL_checknumber(L, 9);
    float    sh = (float)luaL_checknumber(L, 10);

    float matrix[6] = { 1, 0, 0, 1, 0, 0 };
    bool  namedMatrix = false;

    if (lua_istable(L, 11)) {
        lua_getfield(L, 11, "a");  if (lua_isnumber(L, -1)) { matrix[0] = (float)lua_tonumber(L, -1); namedMatrix = true; } lua_pop(L, 1);
        lua_getfield(L, 11, "b");  if (lua_isnumber(L, -1)) { matrix[1] = (float)lua_tonumber(L, -1); namedMatrix = true; } lua_pop(L, 1);
        lua_getfield(L, 11, "c");  if (lua_isnumber(L, -1)) { matrix[2] = (float)lua_tonumber(L, -1); namedMatrix = true; } lua_pop(L, 1);
        lua_getfield(L, 11, "d");  if (lua_isnumber(L, -1)) { matrix[3] = (float)lua_tonumber(L, -1); namedMatrix = true; } lua_pop(L, 1);
        lua_getfield(L, 11, "tx"); if (lua_isnumber(L, -1)) { matrix[4] = (float)lua_tonumber(L, -1); namedMatrix = true; } lua_pop(L, 1);
        lua_getfield(L, 11, "ty"); if (lua_isnumber(L, -1)) { matrix[5] = (float)lua_tonumber(L, -1); namedMatrix = true; } lua_pop(L, 1);

        // Array form only when no named fields were provided: previously the
        // condition tested the table itself (always true), so the array always
        // overwrote the named matrix.
        if (!namedMatrix) {
            for (int i = 0; i < 6; i++) {
                lua_rawgeti(L, 11, i + 1);
                if (lua_isnumber(L, -1)) matrix[i] = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
        }
    }

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    RenderTextureHandle srcTex = resolveTexture(L,srcTexId, dev);
    if (!srcTex.isValid()) {
        lua_pushboolean(L, 0); return 1;
    }

    dev->affineBlt(VIEW_MAIN, dstTexId,
                    dx, dy, dw, dh,
                    (uint32_t)srcTex.idx,
                    sx, sy, sw, sh,
                    matrix);
    lua_pushboolean(L, 1); return 1;
}

// -- Render.fill_viewport(vpHandle, r, g, b, a) ----------------------------

static int lua_Render_fill_viewport(lua_State* L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    uint8_t r = (uint8_t)luaL_optinteger(L, 2, 0);
    uint8_t g = (uint8_t)luaL_optinteger(L, 3, 0);
    uint8_t b = (uint8_t)luaL_optinteger(L, 4, 0);
    uint8_t a = (uint8_t)luaL_optinteger(L, 5, 255);

    ViewportHandle vp{ id };
    dev->fillViewport(vp, r, g, b, a);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.create_viewport(w, h) -> handle ---------------------------------

static int lua_Render_create_viewport(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    // Reject nonsense dimensions (a huge value would truncate into a
    // plausible small RTT on the uint16 path, silently miscreating it).
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        lua_pushinteger(L, 0);
        return 1;
    }

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushinteger(L, 0); return 1; }

    ViewportHandle vp = dev->createRenderTarget(w, h);
    lua_pushinteger(L, (lua_Integer)vp.id);
    return 1;
}

// -- Render.destroy_viewport(handle) ----------------------------------------

static int lua_Render_destroy_viewport(lua_State* L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    ViewportHandle vp{ id };

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    dev->destroyRenderTarget(vp);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.draw_viewport(handle, x, y, w, h) -------------------------------

static int lua_Render_draw_viewport(lua_State* L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0);
    float y = (float)luaL_optnumber(L, 3, 0);
    float w = (float)luaL_optnumber(L, 4, -1);
    float h = (float)luaL_optnumber(L, 5, -1);

    ViewportHandle vp{ id };
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }

    if (w < 0) w = (float)dev->getBackbufferWidth();
    if (h < 0) h = (float)dev->getBackbufferHeight();

    dev->blitViewport(vp, VIEW_MAIN, x, y, w, h);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.resize(w, h) ----------------------------------------------------

static int lua_Render_resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    dev->resize(w, h);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Render.load_texture_async(path) ----------------------------------------

static int lua_Render_load_texture_async(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    auto* async = getAsync(L);
    if (!async) {
        lua_pushinteger(L, 0);
        return 1;
    }
    int id = async->enqueue(path, "texture");
    if (id < 0) {
        lua_pushinteger(L, id);
        return 1;
    }
    // Optional callback(success, path, texId): store as a registry ref in the
    // _ASYNC_CALLBACKS table; Engine::dispatchAsyncLoad looks it up by id.
    if (lua_gettop(L) >= 2 && lua_isfunction(L, 2)) {
        lua_getglobal(L, "_ASYNC_CALLBACKS");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setglobal(L, "_ASYNC_CALLBACKS");
        }
        // Stack: [callback, callbacks-table]. luaL_ref needs the callback on
        // top, so move the table below it with a single insert.
        lua_insert(L, -2);                 // [callbacks-table, callback]
        int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);  // pops the callback -> [table]
        lua_pushinteger(L, id);
        lua_pushinteger(L, cbRef);
        lua_settable(L, -3);
        lua_pop(L, 1);
    }
    lua_pushinteger(L, id);
    return 1;
}

// -- Render.cancel_async_loads() --------------------------------------------

void cancelRenderAsyncLoads(lua_State* L) {
    auto* async = getAsync(L);
    if (async) async->cancelAll();
    // Release all stored callbacks so they never fire for cancelled loads.
    lua_getglobal(L, "_ASYNC_CALLBACKS");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            int cbRef = (int)lua_tointeger(L, -1);
            luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
            lua_pop(L, 1);
        }
        lua_newtable(L);
        lua_setglobal(L, "_ASYNC_CALLBACKS");
    }
    lua_pop(L, 1);
}

static int lua_Render_cancel_async_loads(lua_State* L) {
    cancelRenderAsyncLoads(L);
    lua_pushboolean(L, 1);
    return 1;
}

// -- Video playback (pl_mpeg default, FFmpeg with CAESURA_VIDEO_FFMPEG) ----

static int lua_Render_video_play(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    IVideoPlayer* vp = getVideo(L);
    if (!vp) { lua_pushnil(L); lua_pushstring(L, "VideoPlayer not available"); return 2; }
    VideoHandle h = vp->open(path);
    if (!h) { lua_pushnil(L); lua_pushstring(L, "Failed to open video"); return 2; }

    // Optional options table: { loop = bool, volume = number }
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "loop");
        if (lua_isboolean(L, -1)) vp->setLoop(h, lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);
        lua_getfield(L, 2, "volume");
        if (lua_isnumber(L, -1)) vp->setVolume(h, (float)lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    lua_pushinteger(L, (lua_Integer)h.id);
    return 1;
}

static int lua_Render_video_stop(lua_State* L) {
    VideoHandle h{ (uint32_t)luaL_checkinteger(L, 1) };
    IVideoPlayer* vp = getVideo(L);
    if (vp) vp->close(h);
    lua_pushboolean(L, 1); return 1;
}

static int lua_Render_video_update(lua_State* L) {
    VideoHandle h{ (uint32_t)luaL_checkinteger(L, 1) };
    IVideoPlayer* vp = getVideo(L);
    if (!vp) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, vp->update(h, 0.0) ? 1 : 0);
    return 1;
}

static int lua_Render_video_get_texture(lua_State* L) {
    VideoHandle h{ (uint32_t)luaL_checkinteger(L, 1) };
    IVideoPlayer* vp = getVideo(L);
    if (!vp) { lua_pushinteger(L, 0); return 1; }
    uint32_t texId = vp->getTexture(h);
    lua_pushinteger(L, (lua_Integer)texId);
    return 1;
}

static int lua_Render_video_is_playing(lua_State* L) {
    VideoHandle h{ (uint32_t)luaL_checkinteger(L, 1) };
    IVideoPlayer* vp = getVideo(L);
    lua_pushboolean(L, vp && vp->isPlaying(h) ? 1 : 0);
    return 1;
}

static int lua_Render_video_has_ended(lua_State* L) {
    VideoHandle h{ (uint32_t)luaL_checkinteger(L, 1) };
    IVideoPlayer* vp = getVideo(L);
    lua_pushboolean(L, vp && vp->hasEnded(h) ? 1 : 0);
    return 1;
}

static int lua_Render_video_get_size(lua_State* L) {
    VideoHandle h{ (uint32_t)luaL_checkinteger(L, 1) };
    IVideoPlayer* vp = getVideo(L);
    if (!vp) { lua_pushinteger(L, 0); lua_pushinteger(L, 0); return 2; }
    lua_pushinteger(L, vp->width(h));
    lua_pushinteger(L, vp->height(h));
    return 2;
}

static int lua_Render_video_pause(lua_State* L) {
    VideoHandle h{ (uint32_t)luaL_checkinteger(L, 1) };
    IVideoPlayer* vp = getVideo(L);
    if (vp) vp->pause(h);
    lua_pushboolean(L, 1); return 1;
}

static int lua_Render_video_resume(lua_State* L) {
    VideoHandle h{ (uint32_t)luaL_checkinteger(L, 1) };
    IVideoPlayer* vp = getVideo(L);
    if (vp) vp->resume(h);
    lua_pushboolean(L, 1); return 1;
}

// -- ResourceHandle validation (Phase 0.5) ---------------------------------

static int lua_Render_is_valid_handle(lua_State* L) {
    int typeInt = (int)luaL_checkinteger(L, 1);
    uint32_t id = (uint32_t)luaL_checkinteger(L, 2);
    if (typeInt < 0 || typeInt > 7) { lua_pushboolean(L, 0); return 1; }

    HandleType type = static_cast<HandleType>(typeInt);
    if (id == 0) { lua_pushboolean(L, 0); return 1; }

    switch (type) {
        case HandleType::TEXTURE: {
            auto* texture = getTexture(L);
            bool valid = texture && texture->isValid(id);
            lua_pushboolean(L, valid ? 1 : 0);
            return 1;
        }
        case HandleType::VIEWPORT:
        case HandleType::RTT: {
            lua_pushboolean(L, 1);
            return 1;
        }
        default:
            lua_pushboolean(L, id != 0 ? 1 : 0);
            return 1;
    }
}

static int lua_Render_invalidate_handles(lua_State* L) {
    int typeInt = (int)luaL_checkinteger(L, 1);
    if (typeInt < 0 || typeInt > 7) { lua_pushboolean(L, 0); return 1; }
    HandleType type = static_cast<HandleType>(typeInt);
    auto* tracker = BackendRegistry::instance().getResourceGenerationTracker();
    if (!tracker) { lua_pushboolean(L, 0); return 1; }
    tracker->invalidate(type);
    printf("[Render] Handles invalidated: %s\n", handleTypeName(type));
    lua_pushboolean(L, 1);
    return 1;
}


// -- text_set_font(face, size, color) — stores font settings for renderText ---------
// -- text_set_font(face, size, color) -- switch font face/size -------------------
static int lua_Render_text_set_font(lua_State* L) {
    size_t faceSize=0;
    const char* face = luaL_optlstring(L, 1, "default", &faceSize);
    const float size = (float)luaL_optnumber(L, 2, 24.0);

    IRenderDevice* dev = getRender(L);
    if (!dev) {
        lua_pushboolean(L, 0);
        return 1;
    }

    bool selected=false;
    try { selected=selectScriptFont(*dev,std::string(face,faceSize),size); }
    catch (...) {}
    lua_pushboolean(L,selected);
    return 1;
}

// -- text_reset_state() — reset text renderer state -------------------------------
static int lua_Render_text_reset_state(lua_State* L) {
    // Resets the text renderer's internal line/char tracking.
    // C++ renderText starts fresh each frame; this is a safe no-op
    // that allows the Lua [reset] command to call backend.text_reset_state without error.
    (void)L;
    IRenderDevice* dev = getRender(L);
    if (dev) {
        if (!selectScriptFont(*dev,"default",22.0f)) {
            dev->setFont(0);  // reset to default font
        }
    }
    return 0;
}

// -- Module registration ----------------------------------------------------

static const luaL_Reg render_functions[] = {
    { "create_viewport",    lua_Render_create_viewport    },
    { "destroy_viewport",   lua_Render_destroy_viewport   },
    { "draw_viewport",      lua_Render_draw_viewport      },
    { "load_texture",       lua_Render_load_texture       },
    { "destroy_texture",    lua_Render_destroy_texture    },
    { "create_solid_texture", lua_Render_create_solid_texture },
    { "get_resolution",     lua_Render_get_resolution     },
    { "set_view_name",      lua_Render_set_view_name      },
    { "set_screen_offset",  lua_Render_set_screen_offset  },
    { "submit_batch",       lua_Render_submit_batch       },
    { "submit_blend",       lua_Render_submit_blend       },
    { "submit_transition",  lua_Render_submit_transition  },
    { "submit_vfx",         lua_Render_submit_vfx         },
    { "set_color_filter",   lua_Render_set_color_filter   },
    { "set_postfx",           lua_Render_set_postfx           },
    { "destroy_postfx",       lua_Render_destroy_postfx       },
    { "clear_postfx",         lua_Render_clear_postfx         },
    { "is_postfx_supported",  lua_Render_is_postfx_supported  },
    { "is_postfx_active",     lua_Render_is_postfx_active     },
    { "stretch_blt",        lua_Render_stretch_blt        },
    { "affine_blt",         lua_Render_affine_blt         },
    { "fill_viewport",      lua_Render_fill_viewport      },
    { "resize",             lua_Render_resize             },
    { "is_valid_handle",    lua_Render_is_valid_handle   },
    { "invalidate_handles", lua_Render_invalidate_handles },
    { "load_texture_async",  lua_Render_load_texture_async  },
    { "cancel_async_loads",  lua_Render_cancel_async_loads  },
    { "video_play",        lua_Render_video_play        },
    { "video_stop",         lua_Render_video_stop         },
    { "video_update",       lua_Render_video_update       },
    { "video_get_texture",  lua_Render_video_get_texture  },
    { "video_is_playing",   lua_Render_video_is_playing   },
    { "video_has_ended",    lua_Render_video_has_ended    },
    { "video_get_size",     lua_Render_video_get_size     },
    { "video_pause",        lua_Render_video_pause        },
    { "video_resume",       lua_Render_video_resume       },
    { "text_set_font",     lua_Render_text_set_font     },
    { "text_reset_state",  lua_Render_text_reset_state  },
    { nullptr, nullptr }
};

void registerRenderBinding(lua_State* L) {
    // A fresh lua_State (test suites create many) must re-resolve backend
    // pointers against its own registry -- clear the hot-path caches.
    invalidateBindingCaches();
    luaL_newlib(L, render_functions);
    lua_setglobal(L, "Render");
    printf("[Lua] Render module registered (via BackendRegistry).\n");
}

} // namespace Caesura
