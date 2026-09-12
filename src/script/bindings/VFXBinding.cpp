extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include "VFXBinding.h"
#include "../../di/BackendRegistry.h"
#include "../../render/api/IRenderDevice.h"
#include "../../render/api/IParticleSystem.h"
#include <cassert>
#include <cstdio>

namespace Caesura {

// ===========================================================================
// Singleton ParticleSystem instance -- managed by VFXBinding lifecycle
// ===========================================================================

// ===========================================================================
// Helper: get IRenderDevice from Lua registry (set by Engine::initScriptingPhase)
// ===========================================================================

static IRenderDevice* getRender(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "Caesura.RenderDevice");
    auto* dev = (IRenderDevice*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return dev;  // nullable — VFX can run without render device
}

static IParticleSystem* getParticleSystem(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "Caesura.ParticleSystem");
    auto* particles = static_cast<IParticleSystem*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return particles ? particles : BackendRegistry::instance().getParticleSystem();
}

// -- VFX.particles_init() ----------------------------------------------------

static int lua_VFX_particles_init(lua_State* L) {
    auto* particles = getParticleSystem(L);
    if (!particles) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (particles->isInitialized()) {
        lua_pushboolean(L, 1);
        return 1;
    }

    bool ok = particles->init();
    if (ok) {
        printf("[VFX] Particle system initialized.\n");
    } else {
        printf("[VFX] Particle system init failed!\n");
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// -- VFX.particles_shutdown() ------------------------------------------------

static int lua_VFX_particles_shutdown(lua_State* L) {
    auto* particles = getParticleSystem(L);
    if (!particles) {
        lua_pushboolean(L, 0);
        return 1;
    }
    particles->shutdown();
    printf("[VFX] Particle system shut down.\n");
    lua_pushboolean(L, 1);
    return 1;
}

// -- VFX.particles_create_emitter(cfg_table) ---------------------------------

static int lua_VFX_particles_create_emitter(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    ParticleEmitterConfig cfg;
    lua_getfield(L, 1, "x");         cfg.x       = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "y");         cfg.y       = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "rate");      cfg.rate    = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 10.0f; lua_pop(L, 1);
    lua_getfield(L, 1, "lifeMin");   cfg.lifeMin = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.5f;  lua_pop(L, 1);
    lua_getfield(L, 1, "lifeMax");   cfg.lifeMax = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 2.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "speedMin");  cfg.speedMin= lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 10.0f; lua_pop(L, 1);
    lua_getfield(L, 1, "speedMax");  cfg.speedMax= lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 50.0f; lua_pop(L, 1);
    lua_getfield(L, 1, "angleMin");  cfg.angleMin=lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "angleMax");  cfg.angleMax=lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 6.283f;lua_pop(L, 1);
    lua_getfield(L, 1, "sizeMin");   cfg.sizeMin =lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 2.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "sizeMax");   cfg.sizeMax =lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 8.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "r");         cfg.r       =lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 1.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "g");         cfg.g       =lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 1.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "b");         cfg.b       =lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 1.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "a");         cfg.a       =lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 1.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "gravityX");  cfg.gravityX=lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;  lua_pop(L, 1);
    lua_getfield(L, 1, "gravityY");  cfg.gravityY=lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 0.0f;  lua_pop(L, 1);

    // Validate — reject negative values that would produce undefined behaviour
    if (cfg.rate < 0.0f || cfg.lifeMin < 0.0f || cfg.lifeMax < 0.0f ||
        cfg.speedMin < 0.0f || cfg.speedMax < 0.0f ||
        cfg.sizeMin < 0.0f || cfg.sizeMax < 0.0f) {
        lua_pushinteger(L, -1);
        return 1;
    }
    if (cfg.lifeMax < cfg.lifeMin) cfg.lifeMax = cfg.lifeMin;
    if (cfg.speedMax < cfg.speedMin) cfg.speedMax = cfg.speedMin;
    if (cfg.sizeMax < cfg.sizeMin) cfg.sizeMax = cfg.sizeMin;

    auto* particles = getParticleSystem(L);
    auto* render = BackendRegistry::instance().getRenderDevice();
    // Public VFX calls must honor the same session availability as the host
    // profile, before claiming quota or creating a CPU-side emitter owner.
    if (!particles || !particles->isInitialized() || !render || !render->isInitialized()
        || !BackendRegistry::instance().tryAlloc("particles_emitters")) {
        lua_pushinteger(L, -1);
        return 1;
    }

    int id = particles->createEmitter(cfg);
    if (id < 0) {
        BackendRegistry::instance().release("particles_emitters");
    }
    lua_pushinteger(L, id);
    return 1;
}

// -- VFX.particles_destroy_emitter(id) ---------------------------------------

static int lua_VFX_particles_destroy_emitter(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    auto* particles = getParticleSystem(L);
    const bool destroyed = particles && particles->destroyEmitter(id);
    if (destroyed) {
        BackendRegistry::instance().release("particles_emitters");
    }
    lua_pushboolean(L, destroyed ? 1 : 0);
    return 1;
}

// -- VFX.particles_emit(emitter_id, count) -----------------------------------

static int lua_VFX_particles_emit(lua_State* L) {
    int emitterId = (int)luaL_checkinteger(L, 1);
    int count     = (int)luaL_checkinteger(L, 2);
    auto* particles = getParticleSystem(L);
    if (!particles || count < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    particles->emit(emitterId, count);
    lua_pushboolean(L, 1);
    return 1;
}

// -- VFX.particles_update(dt) -------------------------------------------------

static int lua_VFX_particles_update(lua_State* L) {
    float dt = (float)luaL_checknumber(L, 1);
    int sw = 1280, sh = 720;
    auto* rd = getRender(L);
    if (rd) { sw = rd->getBackbufferWidth(); sh = rd->getBackbufferHeight(); }
    auto* particles = getParticleSystem(L);
    if (!particles) { lua_pushboolean(L, 0); return 1; }
    particles->update(dt, (uint32_t)sw, (uint32_t)sh);
    lua_pushboolean(L, 1);
    return 1;
}

// -- VFX.particles_render(view_id) --------------------------------------------

static int lua_VFX_particles_render(lua_State* L) {
    uint16_t viewId = (uint16_t)luaL_checkinteger(L, 1);
    auto* particles = getParticleSystem(L);
    if (!particles) { lua_pushboolean(L, 0); return 1; }
    particles->render(viewId);
    lua_pushboolean(L, 1);
    return 1;
}

// -- VFX.particles_alive_count() ----------------------------------------------

static int lua_VFX_particles_alive_count(lua_State* L) {
    auto* particles = getParticleSystem(L);
    lua_pushinteger(L, particles ? particles->aliveCount() : 0);
    return 1;
}

// -- VFX.particles_is_initialized() -------------------------------------------

static int lua_VFX_particles_is_initialized(lua_State* L) {
    auto* particles = getParticleSystem(L);
    lua_pushboolean(L, particles && particles->isInitialized() ? 1 : 0);
    return 1;
}

// -- VFX.particles_clear() -- reset all emitters and particles ----------------

static int lua_VFX_particles_clear(lua_State* L) {
    auto* particles = getParticleSystem(L);
    if (!particles) { lua_pushboolean(L, 0); return 1; }
    particles->shutdown();
    // Return the sandbox quota that shutdown() just freed: emitter counters
    // only track create_emitter/destroy_emitter, so without this the count
    // drifts and later create_emitter calls hit the cap permanently.
    const int emitters = BackendRegistry::instance().count("particles_emitters");
    for (int i = 0; i < emitters; ++i) {
        BackendRegistry::instance().release("particles_emitters");
    }
    const bool initialized = particles->init();
    lua_pushboolean(L, initialized ? 1 : 0);
    return 1;
}

// ===========================================================================
// Module registration
// ===========================================================================

static const luaL_Reg vfx_functions[] = {
    { "particles_init",             lua_VFX_particles_init             },
    { "particles_shutdown",         lua_VFX_particles_shutdown         },
    { "particles_create_emitter",   lua_VFX_particles_create_emitter   },
    { "particles_destroy_emitter",  lua_VFX_particles_destroy_emitter  },
    { "particles_emit",             lua_VFX_particles_emit             },
    { "particles_update",           lua_VFX_particles_update           },
    { "particles_render",           lua_VFX_particles_render           },
    { "particles_alive_count",      lua_VFX_particles_alive_count      },
    { "particles_is_initialized",   lua_VFX_particles_is_initialized   },
    { "particles_clear",            lua_VFX_particles_clear            },
    { nullptr, nullptr }
};

void registerVFXBinding(lua_State* L) {
    luaL_newlib(L, vfx_functions);
    lua_setglobal(L, "VFX");
    printf("[Lua] VFX module registered (10 particle APIs).\n");
}

void VFXBinding_Shutdown(IParticleSystem* particleSystem) {
    if (particleSystem && particleSystem->isInitialized()) {
        particleSystem->shutdown();
        printf("[VFX] Particle system shut down (VFXBinding_Shutdown).\n");
    }
}

} // namespace Caesura
