// SteamBinding -- Lua bindings: steam.unlock_achievement, steam.set_stat,
// steam.get_stat_int/float, steam.store_stats, steam.reset_achievement(s),
// steam.cloud_write/read/list/delete/quota (full ISteamBackend surface).
// Registered unconditionally: without the Steam SDK the Null backend is in
// the registry and every call returns a safe default instead of nil-error.
#include "SteamBinding.h"
#include "../../di/BackendRegistry.h"
#include "../../steam/api/ISteamBackend.h"
#include "../../debug/api/DebugLog.h"
#include <cstdio>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace Caesura {

namespace {
// Steam names cross a NUL-terminated ABI. Validate the complete Lua string
// before any backend call or C++ resource acquisition; a Lua argument error
// must not jump over a live std::string/vector destructor.
const char* checkSteamName(lua_State* L, int index) {
    luaL_argcheck(L, lua_type(L, index) == LUA_TSTRING, index,
                  "Steam name must be a string");
    size_t length = 0;
    const char* value = lua_tolstring(L, index, &length);
    luaL_argcheck(L, length > 0 && std::memchr(value, '\0', length) == nullptr,
                  index, "Steam name must be nonempty and contain no NUL");
    return value;
}
}

#define STEAM_BODY(name, nullFallback, code) \
    static int lua_steam_##name(lua_State* L) { \
        auto* steam = BackendRegistry::instance().getSteamBackend(); \
        if (!steam) { nullFallback return 1; } \
        code \
    }

STEAM_BODY(unlock_achievement, lua_pushboolean(L, 0);, {
    const char* id = checkSteamName(L, 1);
    lua_pushboolean(L, steam->unlockAchievement(id) ? 1 : 0);
    return 1;
})

STEAM_BODY(is_achievement_unlocked, lua_pushboolean(L, 0);, {
    const char* id = checkSteamName(L, 1);
    lua_pushboolean(L, steam->isAchievementUnlocked(id) ? 1 : 0);
    return 1;
})

STEAM_BODY(reset_achievement, lua_pushboolean(L, 0);, {
    const char* id = checkSteamName(L, 1);
    lua_pushboolean(L, steam->resetAchievement(id) ? 1 : 0);
    return 1;
})

STEAM_BODY(reset_all_achievements, lua_pushboolean(L, 0);, {
    lua_pushboolean(L, steam->resetAllAchievements() ? 1 : 0);
    return 1;
})

STEAM_BODY(set_stat_int, lua_pushboolean(L, 0);, {
    const char* name = checkSteamName(L, 1);
    const lua_Integer val = luaL_checkinteger(L, 2);
    luaL_argcheck(L, val >= INT32_MIN && val <= INT32_MAX, 2,
                  "Steam integer stat must fit int32");
    lua_pushboolean(L, steam->setStatInt(name, static_cast<int32_t>(val)) ? 1 : 0);
    return 1;
})

STEAM_BODY(get_stat_int, lua_pushinteger(L, 0);, {
    const char* name = checkSteamName(L, 1);
    lua_pushinteger(L, steam->getStatInt(name));
    return 1;
})

STEAM_BODY(set_stat_float, lua_pushboolean(L, 0);, {
    const char* name = checkSteamName(L, 1);
    const lua_Number val = luaL_checknumber(L, 2);
    constexpr lua_Number limit = (std::numeric_limits<float>::max)();
    luaL_argcheck(L, std::isfinite(val) && val >= -limit && val <= limit, 2,
                  "Steam float stat must be finite and fit float");
    lua_pushboolean(L, steam->setStatFloat(name, static_cast<float>(val)) ? 1 : 0);
    return 1;
})

STEAM_BODY(get_stat_float, lua_pushnumber(L, 0.0);, {
    const char* name = checkSteamName(L, 1);
    lua_pushnumber(L, steam->getStatFloat(name));
    return 1;
})

STEAM_BODY(store_stats, lua_pushboolean(L, 0);, {
    lua_pushboolean(L, steam->storeStats() ? 1 : 0);
    return 1;
})

STEAM_BODY(is_overlay_active, lua_pushboolean(L, 0);, {
    lua_pushboolean(L, steam->isOverlayActive() ? 1 : 0);
    return 1;
})

// ---- Cloud saves (Steam Remote Storage) ----------------------------------

STEAM_BODY(cloud_write, lua_pushboolean(L, 0);, {
    const char* file = checkSteamName(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    if (len > INT32_MAX) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, steam->cloudWrite(file, data, (int32_t)len) ? 1 : 0);
    return 1;
})

STEAM_BODY(cloud_read, lua_pushstring(L, "");, {
    const char* file = checkSteamName(L, 1);
    const int32_t size = steam->cloudFileSize(file);
    // P2-4 (round 37): align with CloudSaveProvider kMaxChunkedSize (64MB);
    // the old 16MB cap rejected legitimate cloud saves the provider allows.
    if (size <= 0 || size > 64 * 1024 * 1024) {
        lua_pushnil(L);
        return 1;
    }
    std::vector<char> buf(static_cast<size_t>(size));
    const int32_t got = steam->cloudRead(file, buf.data(), size);
    if (got <= 0 || got > size) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, buf.data(), static_cast<size_t>(got));
    return 1;
})

STEAM_BODY(cloud_file_size, lua_pushinteger(L, 0);, {
    const char* file = checkSteamName(L, 1);
    lua_pushinteger(L, steam->cloudFileSize(file));
    return 1;
})

STEAM_BODY(cloud_file_exists, lua_pushboolean(L, 0);, {
    const char* file = checkSteamName(L, 1);
    lua_pushboolean(L, steam->cloudFileExists(file) ? 1 : 0);
    return 1;
})

STEAM_BODY(cloud_delete, lua_pushboolean(L, 0);, {
    const char* file = checkSteamName(L, 1);
    lua_pushboolean(L, steam->cloudDelete(file) ? 1 : 0);
    return 1;
})

STEAM_BODY(cloud_quota_total, lua_pushinteger(L, 0);, {
    lua_pushinteger(L, steam->cloudQuotaTotal());
    return 1;
})

STEAM_BODY(cloud_quota_used, lua_pushinteger(L, 0);, {
    lua_pushinteger(L, steam->cloudQuotaUsed());
    return 1;
})

STEAM_BODY(cloud_list, lua_newtable(L);, {
    lua_newtable(L);  // result table (rawseti targets -2 inside the fn stack)
    const int32_t count = steam->cloudFileCount();
    for (int32_t i = 0; i < count && i < 256; ++i) {
        const char* name = steam->cloudFileNameAt(i);
        if (name && name[0]) {
            lua_pushstring(L, name);
            lua_rawseti(L, -2, i + 1);
        }
    }
    return 1;
})

#undef STEAM_BODY

static const luaL_Reg steam_functions[] = {
    {"unlock_achievement",      lua_steam_unlock_achievement},
    // set_achievement is an ALIAS of unlock_achievement (identical C function),
    // matching Steamworks' own SetAchievement naming so scripts written against
    // either vocabulary work. Counted once in the API total below.
    {"set_achievement",         lua_steam_unlock_achievement},
    {"is_achievement_unlocked", lua_steam_is_achievement_unlocked},
    {"reset_achievement",       lua_steam_reset_achievement},
    {"reset_all_achievements",  lua_steam_reset_all_achievements},
    {"set_stat_int",            lua_steam_set_stat_int},
    {"get_stat_int",            lua_steam_get_stat_int},
    {"set_stat_float",          lua_steam_set_stat_float},
    {"get_stat_float",          lua_steam_get_stat_float},
    {"store_stats",             lua_steam_store_stats},
    {"is_overlay_active",       lua_steam_is_overlay_active},
    {"cloud_write",             lua_steam_cloud_write},
    {"cloud_read",              lua_steam_cloud_read},
    {"cloud_file_size",         lua_steam_cloud_file_size},
    {"cloud_file_exists",       lua_steam_cloud_file_exists},
    {"cloud_delete",            lua_steam_cloud_delete},
    {"cloud_quota_total",       lua_steam_cloud_quota_total},
    {"cloud_quota_used",        lua_steam_cloud_quota_used},
    {"cloud_list",              lua_steam_cloud_list},
    {nullptr, nullptr}
};

void registerSteamBinding(lua_State* L) {
    luaL_newlib(L, steam_functions);
    lua_setglobal(L, "steam");
    // 19 distinct functions + 1 alias (set_achievement -> unlock_achievement).
    DEBUG_INFO(SubSys::Scripting, ErrCode::Ok,
               "[Lua] Steam module registered (19 APIs + 1 alias).");
}

} // namespace Caesura
