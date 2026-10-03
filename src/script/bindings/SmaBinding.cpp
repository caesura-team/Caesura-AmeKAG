// SmaBinding.cpp — Lua `sma` global for the skeletal-mesh animation
// renderer (SMA Battle 4d S2/S3). Thin pass-through onto IMeshRenderer
// via BackendRegistry; scripts/kag/sma.lua owns data parsing, hierarchy
// resolution and animation driving.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include "SmaBinding.h"
#include "../../render/api/IMeshRenderer.h"
#include "../../render/api/ITextureManager.h"
#include "../../di/BackendRegistry.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <exception>
#include <cstdio>

namespace Caesura {

// ===========================================================================
// Helpers
// ===========================================================================

static IMeshRenderer* getMeshRenderer() {
    return BackendRegistry::instance().getMeshRenderer();
}

// Read a SMAMesh from two Lua tables:
//   verts:   array of {x, y, u, v, bone0, w0, bone1?, w1?}
//   indices: array of numbers
static void pushRecordField(lua_State* L, const char* name, lua_Integer position) {
    // Named records remain canonical. Only an absent named field falls back
    // to the positional records emitted by scripts/kag/sma.lua. Both reads
    // are raw: no author metamethod may run while a mesh/pose vector is owned.
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_rawgeti(L, -1, position);
    }
}

static bool readFloatField(lua_State* L, const char* name, lua_Integer position, float& value) {
    pushRecordField(L, name, position);
    bool valid = lua_isnil(L, -1);
    if (lua_type(L, -1) == LUA_TNUMBER) {
        const lua_Number number = lua_tonumber(L, -1);
        valid = std::isfinite(number)
            && std::abs(number) <= std::numeric_limits<float>::max();
        if (valid) value = static_cast<float>(number);
    }
    lua_pop(L, 1);
    return valid;
}

static bool readBoneField(lua_State* L, const char* name, lua_Integer position, uint16_t& value) {
    pushRecordField(L, name, position);
    bool valid = lua_isnil(L, -1);
    if (lua_type(L, -1) == LUA_TNUMBER) {
        int integral = 0;
        const auto number = lua_tointegerx(L, -1, &integral);
        valid = integral && number >= 0 && number <= UINT16_MAX;
        if (valid) value = static_cast<uint16_t>(number);
    }
    lua_pop(L, 1);
    return valid;
}

static bool readMesh(lua_State* L, int vertIdx, int idxIdx, SMAMesh& out) {
    if (!lua_istable(L, vertIdx) || !lua_istable(L, idxIdx)) return false;
    const size_t vn = lua_rawlen(L, vertIdx);
    const size_t in = lua_rawlen(L, idxIdx);
    // Mesh indices are uint16_t, so no vertex beyond this range is addressable.
    if (vn == 0 || vn > size_t(UINT16_MAX) + 1 || in == 0 || in % 3 != 0) return false;

    out.vertices.resize(vn);
    for (size_t i = 0; i < vn; ++i) {
        lua_rawgeti(L, vertIdx, static_cast<lua_Integer>(i + 1));
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
        SMAMeshVertex& v = out.vertices[i];
        const bool valid = readFloatField(L, "x", 1, v.x) && readFloatField(L, "y", 2, v.y)
            && readFloatField(L, "u", 3, v.u) && readFloatField(L, "v", 4, v.v)
            && readBoneField(L, "bone0", 5, v.bone0) && readFloatField(L, "w0", 6, v.w0)
            && readBoneField(L, "bone1", 7, v.bone1) && readFloatField(L, "w1", 8, v.w1);
        lua_pop(L, 1);
        if (!valid) return false;
    }
    out.indices.resize(in);
    for (size_t i = 0; i < in; ++i) {
        lua_rawgeti(L, idxIdx, static_cast<lua_Integer>(i + 1));
        int integral = 0;
        const lua_Integer raw = lua_tointegerx(L, -1, &integral);
        const bool valid = lua_type(L, -1) == LUA_TNUMBER && integral
            && raw >= 0 && static_cast<size_t>(raw) < vn && raw <= UINT16_MAX;
        lua_pop(L, 1);
        if (!valid) {
            // Out-of-range index would either corrupt the mesh (surviving a
            // uint16 truncation) or read past the vertex buffer (review S1-1).
            out.indices.clear();
            return false;
        }
        out.indices[i] = static_cast<uint16_t>(raw);
    }
    return true;
}

// ===========================================================================
// sma.* functions
// ===========================================================================

static int lua_sma_create_mesh(lua_State* L) {
    IMeshRenderer* r = getMeshRenderer();
    SMAMesh mesh;
    if (!r || !readMesh(L, 1, 2, mesh)) {
        lua_pushinteger(L, 0);
        return 1;
    }
    const MeshHandle h = r->createMesh(mesh);
    lua_pushinteger(L, h ? (lua_Integer)h.id : 0);
    return 1;
}

static int lua_sma_destroy_mesh(lua_State* L) {
    IMeshRenderer* r = getMeshRenderer();
    if (r) r->destroyMesh(MeshHandle{ (uint32_t)luaL_optinteger(L, 1, 0) });
    return 0;
}

static int lua_sma_update_mesh(lua_State* L) {
    IMeshRenderer* r = getMeshRenderer();
    const MeshHandle h{ (uint32_t)luaL_optinteger(L, 1, 0) };
    if (!r || !lua_istable(L, 2)) return 0;
    const size_t n = lua_rawlen(L, 2);
    if (n > size_t(UINT16_MAX) + 1) return 0;
    std::vector<BonePose> poses;
    poses.resize(n);
    for (size_t i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, static_cast<lua_Integer>(i + 1));
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
        BonePose& p = poses[i];
        const bool valid = readFloatField(L, "rot", 1, p.rot)
            && readFloatField(L, "scale", 2, p.scale)
            && readFloatField(L, "ox", 3, p.ox) && readFloatField(L, "oy", 4, p.oy);
        lua_pop(L, 1);
        if (!valid) return 0;
    }
    r->updateMesh(h, poses);
    return 0;
}

static int lua_sma_draw_mesh(lua_State* L) {
    IMeshRenderer* r = getMeshRenderer();
    auto* textures = BackendRegistry::instance().getTextureManager();
    if (!r || !textures) return 0;
    // Lua obtains TextureManager logical IDs from Render.create/load_texture.
    // A missing logical ID must never become a renderer slot (raw slot 0 may
    // be valid, so validate ownership before resolving rather than testing the
    // resolved value for truthiness).
    int integral = 0;
    const lua_Integer logical = lua_tointegerx(L, 3, &integral);
    if (lua_type(L, 3) != LUA_TNUMBER || !integral || logical <= 0
        || static_cast<uint64_t>(logical) > UINT32_MAX) return 0;
    const auto textureId = static_cast<uint32_t>(logical);
    if (!textures->isValid(textureId)) return 0;
    const uint32_t rawTexture = textures->getTextureHandle(textureId);
    if (rawTexture >= std::numeric_limits<uint16_t>::max()) return 0;
    const MeshHandle h{ (uint32_t)luaL_optinteger(L, 1, 0) };
    const uint16_t view = (uint16_t)luaL_optinteger(L, 2, 0);
    const float x = (float)luaL_optnumber(L, 4, 0.0);
    const float y = (float)luaL_optnumber(L, 5, 0.0);
    const float scale = (float)luaL_optnumber(L, 6, 1.0);
    const float opacity = (float)luaL_optnumber(L, 7, 1.0);
    char failure[192] = {};
    try {
        r->drawMesh(view, h, rawTexture, x, y, scale, opacity);
    } catch (const std::exception& error) {
        const char* message = error.what();
        std::snprintf(failure, sizeof(failure), "SMA draw failed: %.160s",
                      message ? message : "renderer exception");
    } catch (...) {
        std::snprintf(failure, sizeof(failure), "SMA draw failed: unknown renderer exception");
    }
    // Leave the catch scope first: Lua's C build uses longjmp, which must not
    // skip the exception object's or renderer-owned C++ resources' destructors.
    if (failure[0]) {
        for (char* p = failure; *p; ++p) {
            const auto c = static_cast<unsigned char>(*p);
            if (c < 32 || c >= 127) *p = '?';
        }
        return luaL_error(L, "%s", failure);
    }
    return 0;
}

static int lua_sma_count(lua_State* L) {
    IMeshRenderer* r = getMeshRenderer();
    lua_pushinteger(L, r ? (lua_Integer)r->meshCount() : 0);
    return 1;
}

static int lua_sma_initialized(lua_State* L) {
    IMeshRenderer* r = getMeshRenderer();
    lua_pushboolean(L, r ? (r->isInitialized() ? 1 : 0) : 0);
    return 1;
}

// S5: skinning mode ("auto" | "cpu" | "gpu").
static int lua_sma_set_skin_mode(lua_State* L) {
    IMeshRenderer* r = getMeshRenderer();
    if (!r) return 0;
    const char* mode = luaL_optstring(L, 1, "auto");
    SkinMode m = SkinMode::Auto;
    if (std::strcmp(mode, "cpu") == 0) m = SkinMode::Cpu;
    else if (std::strcmp(mode, "gpu") == 0) m = SkinMode::Gpu;
    r->setSkinMode(m);
    return 0;
}

static int lua_sma_get_skin_mode(lua_State* L) {
    IMeshRenderer* r = getMeshRenderer();
    if (!r) { lua_pushstring(L, "cpu"); return 1; }
    switch (r->skinMode()) {
        case SkinMode::Cpu: lua_pushstring(L, "cpu"); break;
        case SkinMode::Gpu: lua_pushstring(L, "gpu"); break;
        default:            lua_pushstring(L, "auto"); break;
    }
    return 1;
}

// ===========================================================================

static const luaL_Reg sma_functions[] = {
    { "create_mesh",     lua_sma_create_mesh     },
    { "destroy_mesh",    lua_sma_destroy_mesh    },
    { "update_mesh",     lua_sma_update_mesh     },
    { "draw_mesh",       lua_sma_draw_mesh       },
    { "count",           lua_sma_count           },
    { "initialized",     lua_sma_initialized     },
    { "set_skin_mode",   lua_sma_set_skin_mode   },
    { "get_skin_mode",   lua_sma_get_skin_mode   },
    { nullptr, nullptr },
};

void registerSmaBinding(lua_State* L) {
    luaL_newlib(L, sma_functions);
    lua_setglobal(L, "sma");
}

} // namespace Caesura
