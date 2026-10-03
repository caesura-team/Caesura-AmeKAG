extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include "KAGBinding.h"
#include "AssetVideoBinding.h"
#include "AssetDirectoryBinding.h"
#include "../../audio/api/IAudioBackend.h"
#include "../../render/api/IRenderDevice.h"
#include "../../di/BackendRegistry.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <exception>
#include <cmath>
#include <limits>

namespace Caesura {

// -- Forward declarations --------------------------------------------------
static void invalidateKAGBindingCaches();

static int lua_KAG_play_bgm(lua_State* L);
static int lua_KAG_play_voice(lua_State* L);
static int lua_KAG_play_se_3d(lua_State* L);
static int lua_KAG_play_se(lua_State* L);
static int lua_KAG_stop_bgm(lua_State* L);
static int lua_KAG_stop_voice(lua_State* L);
static int lua_KAG_stop_se(lua_State* L);
static int lua_KAG_set_global_volume(lua_State* L);
static int lua_KAG_get_global_volume(lua_State* L);
static int lua_KAG_replay_voice(lua_State* L);
static int lua_KAG_set_bus_volume(lua_State* L);
static int lua_KAG_get_bus_volume(lua_State* L);
static int lua_KAG_flush_wave_cache(lua_State* L);
static int lua_KAG_show_text(lua_State* L);
static int lua_KAG_show_image(lua_State* L);
static int lua_KAG_clear_screen(lua_State* L);
static int lua_KAG_wait_click(lua_State* L);

static int lua_KAG_render_text(lua_State* L);
static int lua_KAG_render_ruby(lua_State* L);
static int lua_KAG_clear_text(lua_State* L);
static int lua_KAG_set_font(lua_State* L);
static int lua_KAG_line_height(lua_State* L);



static int lua_KAG_set_listener(lua_State* L);
static int lua_KAG_is_voice_playing(lua_State* L);
static int lua_KAG_is_bgm_playing(lua_State* L);
static int lua_KAG_is_se_playing(lua_State* L);
static int lua_KAG_get_active_voices(lua_State* L);
static int lua_KAG_log(lua_State* L);
static int lua_KAG_clear_text_layer(lua_State* L);
static int lua_KAG_set_bgm_volume(lua_State* L);
static int lua_KAG_set_se_volume(lua_State* L);
static int lua_KAG_set_voice_volume(lua_State* L);
static int lua_KAG_audio_get_position(lua_State* L);
static int lua_KAG_audio_get_length(lua_State* L);
static int lua_KAG_audio_fade_volume(lua_State* L);

static int lua_KAG_quake(lua_State* L);

// -- Module registration --------------------------------------------------

static const luaL_Reg kag_functions[] = {
    { "play_bgm",           lua_KAG_play_bgm           },
    { "play_voice",         lua_KAG_play_voice         },
    { "play_se_3d",         lua_KAG_play_se_3d         },
    { "play_se",            lua_KAG_play_se            },
    { "stop_bgm",           lua_KAG_stop_bgm           },
    { "stop_voice",         lua_KAG_stop_voice         },
    { "stop_se",            lua_KAG_stop_se            },
    { "set_global_volume",  lua_KAG_set_global_volume  },
    { "get_global_volume",  lua_KAG_get_global_volume  },
    { "replay_voice",       lua_KAG_replay_voice       },
    { "set_bus_volume",     lua_KAG_set_bus_volume     },
    { "get_bus_volume",     lua_KAG_get_bus_volume     },
    { "flush_wave_cache",   lua_KAG_flush_wave_cache   },    { "render_text",        lua_KAG_render_text        },
    { "render_ruby",        lua_KAG_render_ruby        },
    { "clear_text",         lua_KAG_clear_text         },
    { "set_font",           lua_KAG_set_font           },
    { "line_height",        lua_KAG_line_height        },    { "set_listener",       lua_KAG_set_listener       },
    { "is_voice_playing",   lua_KAG_is_voice_playing   },
    { "is_bgm_playing",     lua_KAG_is_bgm_playing     },
    { "is_se_playing",     lua_KAG_is_se_playing     },
    { "get_active_voices",  lua_KAG_get_active_voices  },
    { "log",                lua_KAG_log                },
    { "clear_text_layer",   lua_KAG_clear_text_layer   },
    { "set_bgm_volume",     lua_KAG_set_bgm_volume     },
    { "set_se_volume",      lua_KAG_set_se_volume      },
    { "set_voice_volume",   lua_KAG_set_voice_volume   },
    { "audio_get_position", lua_KAG_audio_get_position },
    { "audio_get_length",   lua_KAG_audio_get_length   },
    { "audio_fade_volume",  lua_KAG_audio_fade_volume  },

    { "quake",              lua_KAG_quake              },
    { "show_text",         lua_KAG_show_text         },
    { "show_image",        lua_KAG_show_image        },
    { "clear_screen",      lua_KAG_clear_screen      },
    { "wait_click",        lua_KAG_wait_click        },
    { nullptr, nullptr }
};

void registerKAGBinding(lua_State* L) {
    invalidateKAGBindingCaches();
    luaL_newlib(L, kag_functions);
    lua_setglobal(L, "KAG");
    registerAssetVideoBinding(L);
    registerAssetDirectoryBinding(L);
    // P2-7: derive the API count from the table so the log cannot drift
    // from the implementation (was hardcoded "35").
    const size_t apiCount = sizeof(kag_functions) / sizeof(kag_functions[0]) - 1;
    printf("[Lua] KAG module registered (%zu APIs, via BackendRegistry).\n", apiCount);
}

// -- Helpers ---------------------------------------------------------------
// P1-8: hot-path backend lookups are cached (same pattern as RenderBinding).
// Backend instances are stable for the engine's lifetime (device recovery
// mutates, never replaces); registerKAGBinding clears the cache so a fresh
// lua_State in tests re-resolves against its own registry.

static IAudioBackend* g_cachedAudio = nullptr;
static IRenderDevice* g_cachedRender = nullptr;

static void invalidateKAGBindingCaches() {
    g_cachedAudio = nullptr;
    g_cachedRender = nullptr;
}

static IAudioBackend* getAudio(lua_State* L) {
    if (g_cachedAudio) return g_cachedAudio;
    lua_getfield(L, LUA_REGISTRYINDEX, "Caesura.AudioBackend");
    auto* be = (IAudioBackend*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!be) be = BackendRegistry::instance().getAudioBackend();
    g_cachedAudio = be;
    return be;  // set by Engine::initScriptingPhase; null in test env OK
}

static IRenderDevice* getRender(lua_State* L) {
    if (g_cachedRender) return g_cachedRender;
    lua_getfield(L, LUA_REGISTRYINDEX, "Caesura.RenderDevice");
    auto* dev = (IRenderDevice*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!dev) dev = BackendRegistry::instance().getRenderDevice();
    g_cachedRender = dev;
    return dev;  // set by Engine::initScriptingPhase; null in test env OK
}

// -- KAG.play_bgm(file, fadeTime) -----------------------------------------

template <typename Play>
static int checkedAudioPlay(lua_State* L, Play play) {
    bool created = false;
    char error[256] = "Audio backend did not create a source";
    try {
        created = play() != 0;
    } catch (const std::exception& failure) {
        std::snprintf(error, sizeof(error), "%s", failure.what());
    } catch (...) {
        std::snprintf(error, sizeof(error), "Audio source creation failed");
    }
    lua_pushboolean(L, created);
    if (created) return 1;
    lua_pushstring(L, error);
    return 2;
}

static int invalidAudioOptions(lua_State* L) {
    lua_pushboolean(L, false);
    lua_pushliteral(L, "Audio options require finite volume 0..1.5, nonnegative fadein and boolean loop");
    return 2;
}

static bool readPlaybackOptions(lua_State* L, int index, AudioPlaybackOptions& options) {
    if (lua_isnoneornil(L, index)) return true;
    if (!lua_istable(L, index)) return false;
    const auto numberField = [&](const char* name, float& value, float maximum) {
        lua_pushstring(L, name);
        lua_rawget(L, index);
        bool valid = lua_isnil(L, -1);
        if (lua_type(L, -1) == LUA_TNUMBER) {
            const auto number = lua_tonumber(L, -1);
            valid = std::isfinite(number) && number >= 0 && number <= maximum;
            if (valid) value = static_cast<float>(number);
        }
        lua_pop(L, 1);
        return valid;
    };
    if (!numberField("volume", options.volume, 1.5f)
        || !numberField("fadein", options.fadeIn, std::numeric_limits<float>::max())) return false;
    lua_pushliteral(L, "loop");
    lua_rawget(L, index);
    const bool valid = lua_isnil(L, -1) || lua_isboolean(L, -1);
    if (lua_isboolean(L, -1)) options.loop = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return valid;
}

static int lua_KAG_play_bgm(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    if (lua_istable(L, 2)) {
        AudioPlaybackOptions options;
        if (!readPlaybackOptions(L, 2, options)) return invalidAudioOptions(L);
        IAudioBackend* audio = getAudio(L);
        if (!audio) { lua_pushboolean(L, 0); return 1; }
        return checkedAudioPlay(L, [=] { return audio->playBGM(file, options); });
    }
    float fadeTime   = (float)luaL_optnumber(L, 2, 1.0);
    if (!std::isfinite(fadeTime) || fadeTime < 0) return invalidAudioOptions(L);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    return checkedAudioPlay(L, [=] { return audio->playBGM(file, fadeTime); });
}

// -- KAG.play_voice(file) -------------------------------------------------

static int lua_KAG_play_voice(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    AudioPlaybackOptions options;
    if (!readPlaybackOptions(L, 2, options)) return invalidAudioOptions(L);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    if (!lua_isnoneornil(L, 2))
        return checkedAudioPlay(L, [=] { return audio->playVoice(file, options); });
    return checkedAudioPlay(L, [=] { return audio->playVoice(file); });
}

// -- KAG.play_se_3d(file, x, y, z) ----------------------------------------

static int lua_KAG_play_se_3d(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_optnumber(L, 4, 0.0);
    AudioPlaybackOptions options;
    if (!readPlaybackOptions(L, 5, options) || !std::isfinite(x)
        || !std::isfinite(y) || !std::isfinite(z)) return invalidAudioOptions(L);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    if (!lua_isnoneornil(L, 5))
        return checkedAudioPlay(L, [=] { return audio->playSE3D(file, x, y, z, options); });
    return checkedAudioPlay(L, [=] { return audio->playSE3D(file, x, y, z); });
}

// -- KAG.play_se(file) ----------------------------------------------------

static int lua_KAG_play_se(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    AudioPlaybackOptions options;
    if (!readPlaybackOptions(L, 2, options)) return invalidAudioOptions(L);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    if (!lua_isnoneornil(L, 2))
        return checkedAudioPlay(L, [=] { return audio->playSE(file, options); });
    return checkedAudioPlay(L, [=] { return audio->playSE(file); });
}

// -- KAG.stop_bgm(fadeTime) -----------------------------------------------

static int lua_KAG_stop_bgm(lua_State* L) {
    float fadeTime = (float)luaL_optnumber(L, 1, 1.0);
    if (!std::isfinite(fadeTime) || fadeTime < 0) return invalidAudioOptions(L);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->stopBGM(fadeTime);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.stop_voice() -----------------------------------------------------

static int lua_KAG_stop_voice(lua_State* L) {
    IAudioBackend* audio = getAudio(L);
    if (!audio) return 0;
    audio->stopVoice();
    return 0;
}

// -- KAG.stop_se() --------------------------------------------------------
// Stops all active sound effects via the SE bus.

static int lua_KAG_stop_se(lua_State* L) {
    const float fade = static_cast<float>(luaL_optnumber(L, 1, 0));
    if (!std::isfinite(fade) || fade < 0) return invalidAudioOptions(L);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, false); return 1; }
    audio->stopSE(fade);
    lua_pushboolean(L, true);
    return 1;
}

// -- KAG.set_global_volume(volume) ----------------------------------------
// Sets the master volume for all audio buses.

static int lua_KAG_set_global_volume(lua_State* L) {
    float volume = (float)luaL_checknumber(L, 1);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->setGlobalVolume(volume);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.get_global_volume() -> float -------------------------------------
// Returns the current master volume.

static int lua_KAG_get_global_volume(lua_State* L) {
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushnumber(L, 1.0); return 1; }
    lua_pushnumber(L, audio->getGlobalVolume());
    return 1;
}

// -- KAG.replay_voice(file) -- playVoice alias (backlog replay deferred to IAudioBackend v2) -------------------

static int lua_KAG_replay_voice(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->playVoice(file);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.set_bus_volume(bus, volume) -- per-bus volume ---------------------

static int lua_KAG_set_bus_volume(lua_State* L) {
    const char* bus    = luaL_checkstring(L, 1);
    float volume       = (float)luaL_checknumber(L, 2);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->setBusVolume(bus, volume);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.get_bus_volume(bus) -> float --------------------------------------

static int lua_KAG_get_bus_volume(lua_State* L) {
    const char* bus    = luaL_checkstring(L, 1);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushnumber(L, 1.0); return 1; }
    lua_pushnumber(L, audio->getBusVolume(bus));
    return 1;
}

// -- KAG.flush_wave_cache() -- clear C++ wave cache ------------------------

static int lua_KAG_flush_wave_cache(lua_State* L) {
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->flushWaveCache();
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.show_text(text) -- console echo -----------------------------------

static int lua_KAG_show_text(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    printf("[KAG] show_text: %s\n", text);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_KAG_render_text(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    float x    = (float)luaL_optnumber(L, 2, 32.0);
    float y    = (float)luaL_optnumber(L, 3, 48.0);
    auto comp   = [L](int idx, lua_Integer dflt) -> uint8_t {
        const lua_Integer v = luaL_optinteger(L, idx, dflt);
        return static_cast<uint8_t>(std::min<lua_Integer>(255, std::max<lua_Integer>(0, v)));
    };
    uint8_t r  = comp(4, 255);   // clamp (S1-3)
    uint8_t g  = comp(5, 255);
    uint8_t b  = comp(6, 255);
    uint8_t a  = comp(7, 255);
    float scale = (float)luaL_optnumber(L, 8, 1.0);   // {size=N} markup
    bool  bold  = lua_toboolean(L, 9) != 0;           // {b} markup
    bool  italic = lua_toboolean(L, 10) != 0;         // {i} markup
    bool  strike = lua_toboolean(L, 11) != 0;         // {s} markup

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    dev->renderText(VIEW_MAIN, text, x, y, r, g, b, a, scale, bold,
                    italic, strike);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.render_ruby(text, ruby, x, y, r, g, b, a) ------------------------

static int lua_KAG_render_ruby(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    const char* ruby = luaL_checkstring(L, 2);
    float x    = (float)luaL_optnumber(L, 3, 32.0);
    float y    = (float)luaL_optnumber(L, 4, 48.0);
    auto comp   = [L](int idx, lua_Integer dflt) -> uint8_t {
        const lua_Integer v = luaL_optinteger(L, idx, dflt);
        return static_cast<uint8_t>(std::min<lua_Integer>(255, std::max<lua_Integer>(0, v)));
    };
    uint8_t r  = comp(5, 255);   // clamp (S1-3)
    uint8_t g  = comp(6, 255);
    uint8_t b  = comp(7, 255);
    uint8_t a  = comp(8, 255);

    IRenderDevice* dev = getRender(L);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    dev->renderRuby(VIEW_MAIN, text, ruby, x, y, r, g, b, a);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.clear_text() -- reset text cursor ---------------------------------

static int lua_KAG_clear_text(lua_State* L) {
    (void)L;
    // Cursor reset is handled by TextRenderer::clearText()
    return 0;
}

// -- KAG.set_font(id) -- 0=Small 1=Large -----------------------------------

static int lua_KAG_set_font(lua_State* L) {
    int fontId = (int)luaL_optinteger(L, 1, 0);
    IRenderDevice* dev = getRender(L);
    if (dev) dev->setFont(fontId);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.line_height() -> float --------------------------------------------

static int lua_KAG_line_height(lua_State* L) {
    IRenderDevice* dev = getRender(L);
    float lh = dev ? dev->textLineHeight() : 16.0f;
    lua_pushnumber(L, (lua_Number)lh);
    return 1;
}

// -- KAG.wait_click() -- coroutine yield -----------------------------------

static int lua_KAG_wait_click(lua_State* L) {
    // Stub: controlled by engine input loop, not Lua
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_KAG_show_image(lua_State* L) {
    const char* file = luaL_checkstring(L, 1);
    printf("[KAG] show_image: %s\n", file);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_KAG_clear_screen(lua_State* L) {
    printf("[KAG] clear_screen\n");
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_KAG_set_listener(lua_State* L) {
    float px = (float)luaL_checknumber(L, 1);
    float py = (float)luaL_checknumber(L, 2);
    float pz = (float)luaL_checknumber(L, 3);
    float ax = (float)luaL_checknumber(L, 4);
    float ay = (float)luaL_checknumber(L, 5);
    float az = (float)luaL_checknumber(L, 6);
    float ux = (float)luaL_optnumber(L, 7, 0.0);
    float uy = (float)luaL_optnumber(L, 8, 1.0);
    float uz = (float)luaL_optnumber(L, 9, 0.0);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->update3dListener(px, py, pz, ax, ay, az, ux, uy, uz);
    lua_pushboolean(L, 1);
    return 1;
}

// -- State queries ---------------------------------------------------------

static int lua_KAG_is_voice_playing(lua_State* L) {
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, audio->isVoicePlaying() ? 1 : 0);
    return 1;
}

static int lua_KAG_is_bgm_playing(lua_State* L) {
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, audio->isBGMPlaying() ? 1 : 0);
    return 1;
}

static int lua_KAG_is_se_playing(lua_State* L) {
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, audio->isSEPlaying() ? 1 : 0);
    return 1;
}

static int lua_KAG_get_active_voices(lua_State* L) {
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, audio->activeVoiceCount());
    return 1;
}

static int lua_KAG_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    printf("[KAG:LOG] %s\n", msg);
    return 0;
}


// -- KAG.clear_text_layer() -- alias for clear_text -----------------------

static int lua_KAG_clear_text_layer(lua_State* L) {
    return lua_KAG_clear_text(L);
}

// -- KAG.set_bgm_volume(vol) ----------------------------------------------

static int lua_KAG_set_bgm_volume(lua_State* L) {
    float vol = (float)luaL_checknumber(L, 1);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->setBusVolume("bgm", vol);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.set_se_volume(vol) -----------------------------------------------

static int lua_KAG_set_se_volume(lua_State* L) {
    float vol = (float)luaL_checknumber(L, 1);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->setBusVolume("se", vol);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.set_voice_volume(vol) --------------------------------------------

static int lua_KAG_set_voice_volume(lua_State* L) {
    float vol = (float)luaL_checknumber(L, 1);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->setBusVolume("voice", vol);
    lua_pushboolean(L, 1);
    return 1;
}

// -- KAG.quake(time_ms, intensity) ----------------------------------------

static int lua_KAG_quake(lua_State* L) {
    float timeMs    = (float)luaL_checknumber(L, 1);
    float intensity = (float)luaL_optnumber(L, 2, 5.0);
    // Quake parameters stored in Lua registry for the VFX layer to consume
    // The actual screen-shake is applied per-frame by vfx.lua
    lua_pushnumber(L, intensity);
    lua_setfield(L, LUA_REGISTRYINDEX, "Caesura.QuakeIntensity");
    lua_pushnumber(L, timeMs);
    lua_setfield(L, LUA_REGISTRYINDEX, "Caesura.QuakeDuration");
    printf("[KAG] Quake: %.0f ms, intensity %.1f\n", timeMs, intensity);
    lua_pushboolean(L, 1);
    return 1;
}
// -- KAG.audio_get_position(bus) -- Spec [3.3] ---------------------------

static int lua_KAG_audio_get_position(lua_State* L) {
    const char* bus = luaL_checkstring(L, 1);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushnumber(L, 0.0); return 1; }
    lua_pushnumber(L, (lua_Number)audio->getPosition(bus));
    return 1;
}

// -- KAG.audio_get_length(bus) -- Spec [3.3] -----------------------------

static int lua_KAG_audio_get_length(lua_State* L) {
    const char* bus = luaL_checkstring(L, 1);
    IAudioBackend* audio = getAudio(L);
    if (!audio) { lua_pushnumber(L, 0.0); return 1; }
    lua_pushnumber(L, (lua_Number)audio->getLength(bus));
    return 1;
}

// -- KAG.audio_fade_volume(bus, target_vol, fade_time) -- Spec [3.2] ----

static int lua_KAG_audio_fade_volume(lua_State* L) {
    const char* bus       = luaL_checkstring(L, 1);
    float targetVolume    = (float)luaL_checknumber(L, 2);
    float fadeTime        = (float)luaL_checknumber(L, 3);
    IAudioBackend* audio  = getAudio(L);
    if (!audio) { lua_pushboolean(L, 0); return 1; }
    audio->fadeVolume(bus, targetVolume, fadeTime);
    lua_pushboolean(L, 1);
    return 1;
}

} // namespace Caesura
