// ===========================================================================
//  Caesura (AmeKAG) -- SaveBinding.cpp
//  Phase 6: C++ Lua binding for save/load.
//  Uses nlohmann/json for structured data interchange between Lua and C++.
//  Registers: KAG.save_game(slot, dataTable, sceneName, tokenIndex, [thumbnail])
//             KAG.load_game(slot) -> dataTable, metaTable
//             KAG.list_saves() -> {{slot, timestamp, scene, ...}, ...}
//             KAG.delete_save(slot) -> bool
// ===========================================================================

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
#include "SaveBinding.h"
#include "../../di/BackendRegistry.h"
#include "../../storage/api/ISaveManager.h"
#include "../../render/api/IRenderDevice.h"
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <new>

namespace Caesura {

static ISaveManager* getSaveManager() {
    return BackendRegistry::instance().getSaveManager();
}
// Lua persistence accepts JSON objects or dense arrays, never silent truncation.
static constexpr int kMaxTableDepth = 64;
static constexpr size_t kMaxValueCount = 100000;

static bool validUtf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned value = 0, minimum = 0, remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1f; minimum = 0x80; remaining = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0f; minimum = 0x800; remaining = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 7; minimum = 0x10000; remaining = 3;
        } else return false;
        if (remaining > text.size() - i) return false;
        while (remaining--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

static std::string savedString(lua_State* L, int index) {
    size_t length = 0;
    const char* bytes = lua_tolstring(L, index, &length);
    std::string value(bytes, length);
    if (!validUtf8(value)) throw std::runtime_error("Saved strings require valid UTF-8");
    return value;
}

struct SaveValueTraversal {
    std::unordered_set<const void*> ancestors;
    size_t count = 0;
};

static json luaValueToJson(lua_State* L, int index, SaveValueTraversal& walk, int depth = 0) {
    if (++walk.count > kMaxValueCount) throw std::runtime_error("Save value budget exceeded");
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return lua_toboolean(L, index) != 0;
    case LUA_TNUMBER:
        if (lua_isinteger(L, index)) return static_cast<int64_t>(lua_tointeger(L, index));
        if (!std::isfinite(lua_tonumber(L, index))) throw std::runtime_error("Non-finite saved number");
        return static_cast<double>(lua_tonumber(L, index));
    case LUA_TSTRING: return savedString(L, index);
    case LUA_TTABLE: break;
    default: throw std::runtime_error("Unsupported saved Lua value");
    }
    if (depth > kMaxTableDepth) throw std::runtime_error("Save table depth exceeded");
    if (!lua_checkstack(L, 8)) throw std::runtime_error("Cannot grow save conversion stack");
    const int absolute = lua_absindex(L, index);
    const void* identity = lua_topointer(L, absolute);
    if (!walk.ancestors.insert(identity).second) throw std::runtime_error("Cyclic save table");
    size_t numeric = 0, strings = 0;
    lua_Integer maximum = 0;
    lua_pushnil(L);
    while (lua_next(L, absolute)) {
        if (lua_type(L, -2) == LUA_TSTRING) ++strings;
        else if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
            const lua_Integer key = lua_tointeger(L, -2);
            if (key < 1) throw std::runtime_error("Saved array indices start at one");
            maximum = (std::max)(maximum, key);
            ++numeric;
        } else throw std::runtime_error("Unsupported saved table key");
        if (numeric + strings > kMaxValueCount) throw std::runtime_error("Save table size exceeded");
        lua_pop(L, 1);
    }
    if (numeric && (strings || static_cast<uint64_t>(maximum) != numeric))
        throw std::runtime_error("Saved tables require string keys or dense array indices");
    json result = numeric ? json::array() : json::object();
    if (numeric) {
        for (lua_Integer key = 1; key <= maximum; ++key) {
            lua_rawgeti(L, absolute, key);
            result.push_back(luaValueToJson(L, -1, walk, depth + 1));
            lua_pop(L, 1);
        }
    } else {
        lua_pushnil(L);
        while (lua_next(L, absolute)) {
            result[savedString(L, -2)] = luaValueToJson(L, -1, walk, depth + 1);
            lua_pop(L, 1);
        }
    }
    walk.ancestors.erase(identity);
    return result;
}

static void validateLoadedValue(const json& value, size_t& count, int depth = 0, bool arrayItem = false) {
    if (++count > kMaxValueCount) throw std::runtime_error("Loaded value budget exceeded");
    if (value.is_null()) {
        // Old releases wrote empty state tables as object null members.
        if (arrayItem) throw std::runtime_error("JSON array null cannot become a Lua array hole");
        return;
    }
    if (value.is_number_unsigned() && value.get<uint64_t>() >
        static_cast<uint64_t>((std::numeric_limits<lua_Integer>::max)()))
        throw std::runtime_error("Saved integer exceeds Lua integer range");
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        throw std::runtime_error("Non-finite loaded number");
    if (value.is_string() && !validUtf8(value.get_ref<const std::string&>()))
        throw std::runtime_error("Invalid UTF-8 in loaded string");
    if (!value.is_structured()) return;
    if (depth > kMaxTableDepth) throw std::runtime_error("Loaded table depth exceeded");
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!validUtf8(it.key())) throw std::runtime_error("Invalid UTF-8 in loaded key");
            validateLoadedValue(it.value(), count, depth + 1);
        }
    } else {
        for (const auto& child : value) validateLoadedValue(child, count, depth + 1, true);
    }
}

// This helper runs only inside lua_pcall. It holds no owning C++ temporaries,
// so a Lua allocation failure cannot jump over JSON/string destructors.
static void pushLoadedValue(lua_State* L, const json& value) {
    if (!lua_checkstack(L, 8)) { luaL_error(L, "Cannot grow load conversion stack"); return; }
    switch (value.type()) {
    case json::value_t::null: lua_pushnil(L); return;
    case json::value_t::boolean: lua_pushboolean(L, value.get<bool>()); return;
    case json::value_t::number_integer: lua_pushinteger(L, value.get<int64_t>()); return;
    case json::value_t::number_unsigned: lua_pushinteger(L, value.get<uint64_t>()); return;
    case json::value_t::number_float: lua_pushnumber(L, value.get<double>()); return;
    case json::value_t::string: {
        const auto& text = value.get_ref<const std::string&>();
        lua_pushlstring(L, text.data(), text.size()); return;
    }
    case json::value_t::object:
        lua_createtable(L, 0, static_cast<int>(value.size()));
        for (auto it = value.begin(); it != value.end(); ++it) {
            lua_pushlstring(L, it.key().data(), it.key().size());
            pushLoadedValue(L, it.value());
            lua_rawset(L, -3);
        }
        return;
    case json::value_t::array: {
        lua_createtable(L, static_cast<int>(value.size()), 0);
        lua_Integer index = 1;
        for (const auto& child : value) {
            pushLoadedValue(L, child);
            lua_rawseti(L, -2, index++);
        }
        return;
    }
    default: lua_pushnil(L); return;
    }
}

struct LoadedSaveView { const json* data; const SaveMeta* meta; };

static void pushSaveMeta(lua_State* L, const SaveMeta& meta) {
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, meta.slot); lua_setfield(L, -2, "slot");
    lua_pushinteger(L, static_cast<lua_Integer>(meta.timestamp)); lua_setfield(L, -2, "timestamp");
    lua_pushlstring(L, meta.sceneName.data(), meta.sceneName.size()); lua_setfield(L, -2, "scene");
    lua_pushinteger(L, meta.tokenIndex); lua_setfield(L, -2, "token_index");
    lua_pushinteger(L, meta.schemaVersion); lua_setfield(L, -2, "schema_version");
}

static int pushLoadedSave(lua_State* L) {
    const auto* view = static_cast<const LoadedSaveView*>(lua_touserdata(L, 1));
    pushLoadedValue(L, *view->data);
    pushSaveMeta(L, *view->meta);
    return 2;
}

static int lua_Save_game(lua_State* L) {
    const auto slotValue = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    size_t sceneLength = 0, thumbnailLength = 0;
    const char* sceneBytes = luaL_checklstring(L, 3, &sceneLength);
    const auto tokenValue = luaL_checkinteger(L, 4);
    const char* thumbnailBytes = luaL_optlstring(L, 5, "", &thumbnailLength);
    const int top = lua_gettop(L);
    char error[256] = {};
    bool saved = false;
    try {
        if (slotValue < 0 || slotValue > 99 || tokenValue < (std::numeric_limits<int>::min)()
            || tokenValue > (std::numeric_limits<int>::max)())
            throw std::runtime_error("Save slot or token index is outside its range");
        const std::string sceneName(sceneBytes, sceneLength);
        const std::string thumbnail(thumbnailBytes, thumbnailLength);
        if (!validUtf8(sceneName) || !validUtf8(thumbnail))
            throw std::runtime_error("Saved metadata requires valid UTF-8");
        SaveValueTraversal traversal;
        const auto data = luaValueToJson(L, 2, traversal);
        auto* manager = getSaveManager();
        saved = manager && manager->save(static_cast<int>(slotValue), data, sceneName,
                                         static_cast<int>(tokenValue), thumbnail);
        if (!saved) std::snprintf(error, sizeof(error), "Save backend rejected the write");
    } catch (const std::exception& failure) {
        std::snprintf(error, sizeof(error), "%s", failure.what());
    } catch (...) {
        std::snprintf(error, sizeof(error), "Save conversion failed");
    }
    lua_settop(L, top);
    lua_pushboolean(L, saved);
    if (saved) return 1;
    lua_pushstring(L, error);
    return 2;
}

static int lua_Load_game(lua_State* L) {
    const auto slotValue = luaL_checkinteger(L, 1);
    const int top = lua_gettop(L);
    char error[256] = {};
    try {
        if (slotValue < 0 || slotValue > 99) throw std::runtime_error("Invalid load slot");
        auto* manager = getSaveManager();
        if (!manager) throw std::runtime_error("Save manager is not available");
        SaveMeta meta;
        const auto data = manager->load(static_cast<int>(slotValue), &meta);
        if (!data.is_object() && !data.is_array())
            throw std::runtime_error("Save slot has no representable state table");
        size_t count = 0;
        validateLoadedValue(data, count);
        if (meta.timestamp > static_cast<uint64_t>((std::numeric_limits<lua_Integer>::max)()))
            throw std::runtime_error("Saved timestamp exceeds Lua integer range");
        if (!lua_checkstack(L, 8)) throw std::runtime_error("Cannot grow load conversion stack");
        const LoadedSaveView view{&data, &meta};
        lua_pushcfunction(L, pushLoadedSave);
        lua_pushlightuserdata(L, const_cast<LoadedSaveView*>(&view));
        if (lua_pcall(L, 1, 2, 0) == LUA_OK) return 2;
        std::snprintf(error, sizeof(error), "%s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua load allocation failed");
    } catch (const std::exception& failure) {
        std::snprintf(error, sizeof(error), "%s", failure.what());
    } catch (...) {
        std::snprintf(error, sizeof(error), "Load conversion failed");
    }
    lua_settop(L, top);
    lua_pushnil(L);
    lua_pushstring(L, error);
    return 2;
}


// -- lua_List_saves ---------------------------------------------------------
// KAG.list_saves() -> {{slot=N, timestamp=T, scene=S, token_index=I}, ...}
static int pushSaveList(lua_State* L) {
    const auto& saves = *static_cast<const std::vector<SaveMeta>*>(lua_touserdata(L, 1));
    lua_createtable(L, static_cast<int>(saves.size()), 0);
    int idx = 1;
    for (const auto& meta : saves) {
        pushSaveMeta(L, meta);
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

static int lua_List_saves(lua_State* L) {
    const int top = lua_gettop(L);
    char error[256] = {};
    try {
        auto* manager = getSaveManager();
        const auto saves = manager ? manager->listSaves() : std::vector<SaveMeta>{};
        if (saves.size() > kMaxValueCount) throw std::runtime_error("Save listing size exceeded");
        for (const auto& meta : saves) {
            if (!validUtf8(meta.sceneName) || meta.timestamp >
                static_cast<uint64_t>((std::numeric_limits<lua_Integer>::max)()))
                throw std::runtime_error("Save listing has unrepresentable metadata");
        }
        if (!lua_checkstack(L, 8)) throw std::runtime_error("Cannot grow save listing stack");
        lua_pushcfunction(L, pushSaveList);
        lua_pushlightuserdata(L, const_cast<std::vector<SaveMeta>*>(&saves));
        if (lua_pcall(L, 1, 1, 0) == LUA_OK) return 1;
        std::snprintf(error, sizeof(error), "%s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua list allocation failed");
    } catch (const std::exception& failure) {
        std::snprintf(error, sizeof(error), "%s", failure.what());
    } catch (...) {
        std::snprintf(error, sizeof(error), "Save listing failed");
    }
    lua_settop(L, top);
    lua_pushnil(L);
    lua_pushstring(L, error);
    return 2;
}

// -- lua_Delete_save ---------------------------------------------------------
// KAG.delete_save(slot) -> bool
static int lua_Delete_save(lua_State* L) {
    int slot = (int)luaL_checkinteger(L, 1);
    auto* manager = getSaveManager();
    bool ok = manager && manager->deleteSlot(slot);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// -- lua_Save_exists ---------------------------------------------------------
// KAG.save_exists(slot) -> bool
static int lua_Save_exists(lua_State* L) {
    int slot = (int)luaL_checkinteger(L, 1);
    auto* manager = getSaveManager();
    bool ok = manager && manager->slotExists(slot);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// -- lua_Save_get_dir --------------------------------------------------------

// -- lua_SetEncryptionKey ---------------------------------------------------
static int lua_SetEncryptionKey(lua_State* L) {
    size_t len; const char* keyStr = luaL_checklstring(L, 1, &len);
    if (len != 32) { lua_pushboolean(L, 0); lua_pushstring(L, "Key must be 32 bytes"); return 2; }
    auto* manager = getSaveManager();
    if (!manager) { lua_pushboolean(L, 0); return 1; }
    manager->setEncryptionKey(reinterpret_cast<const uint8_t*>(keyStr));
    lua_pushboolean(L, 1); return 1;
}

// -- lua_ClearEncryptionKey -------------------------------------------------
static int lua_ClearEncryptionKey(lua_State* L) {
    if (auto* manager = getSaveManager()) manager->clearEncryptionKey();
    return 0;
}

// -- lua_CaptureThumbnail ---------------------------------------------------
namespace {
constexpr const char* kCaptureGuard = "Caesura.ThumbnailCapture";

struct CaptureLease {
    ScreenshotTicket ticket;
    ScreenshotResult result;
    std::string base64;
    bool ownsTicket = false;
};

// Lua is compiled as C: its yield/error longjmps do not unwind C++ locals.
// The POD shell is protected before any request; all owning C++ values live
// behind it, including buffers used by allocation-capable Lua push calls.
struct CaptureGuard {
    CaptureLease* lease;
    bool cancelled;
    const char* status;
    char error[256];
};

static void releaseCapture(CaptureGuard* guard) noexcept {
    if (!guard || !guard->lease) return;
    auto* lease = guard->lease;
    if (lease->ownsTicket) {
        lease->ownsTicket = false;
        // Never retain/dereference an old renderer address across a yield.
        // Globally unique tickets let a replacement safely reject this id.
        auto* renderer = BackendRegistry::instance().getRenderDevice();
        if (renderer) {
            try { renderer->cancelScreenshot(lease->ticket); } catch (...) {}
            // Completion may have won the race: still consume OUR terminal.
            try { (void)renderer->takeScreenshot(lease->ticket); } catch (...) {}
        }
    }
    delete lease;
    guard->lease = nullptr;
}

static int closeCapture(lua_State* L) {
    releaseCapture(static_cast<CaptureGuard*>(lua_touserdata(L, 1)));
    return 0;
}

static int cancelCapture(lua_State* L) {
    auto* guard = static_cast<CaptureGuard*>(lua_touserdata(L, lua_upvalueindex(1)));
    guard->cancelled = true;
    releaseCapture(guard);
    return 0;
}

static void startCapture(CaptureGuard* guard) noexcept {
    try {
        guard->lease = new CaptureLease;
        auto* renderer = BackendRegistry::instance().getRenderDevice();
        if (!renderer) {
            guard->status = "unavailable";
            std::snprintf(guard->error, sizeof(guard->error), "renderer-unavailable");
            return;
        }
        auto& lease = *guard->lease;
        lease.result = renderer->requestScreenshot(ScreenshotOptions{320, 180});
        lease.ticket = lease.result.ticket;
        lease.ownsTicket = static_cast<bool>(lease.ticket);
        if (!lease.ownsTicket || lease.result.status != ScreenshotStatus::Pending) {
            guard->status = "unavailable";
            std::snprintf(guard->error, sizeof(guard->error), "%s",
                lease.result.error.empty() ? "screenshot-rejected" : lease.result.error.c_str());
        }
    } catch (const std::exception& error) {
        guard->status = "failed";
        std::snprintf(guard->error, sizeof(guard->error), "%s", error.what());
    } catch (...) {
        guard->status = "failed";
        std::snprintf(guard->error, sizeof(guard->error), "screenshot-request-failed");
    }
}

static void pollCapture(CaptureGuard* guard) noexcept {
    if (guard->status) return;
    try {
        auto& lease = *guard->lease;
        auto* renderer = BackendRegistry::instance().getRenderDevice();
        if (!renderer) {
            guard->status = "cancelled";
            std::snprintf(guard->error, sizeof(guard->error), "renderer-unavailable");
            return;
        }
        lease.result = renderer->takeScreenshot(lease.ticket);
        if (lease.result.status == ScreenshotStatus::Pending) return;
        lease.ownsTicket = false;
        if (lease.result.status == ScreenshotStatus::Completed) {
            if (lease.result.png.empty()) {
                guard->status = "failed";
                std::snprintf(guard->error, sizeof(guard->error), "empty-screenshot");
                return;
            }
            static constexpr char alphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const auto& png = lease.result.png;
            lease.base64.reserve(((png.size() + 2) / 3) * 4);
            for (size_t i = 0; i < png.size(); i += 3) {
                const uint8_t a = png[i];
                const uint8_t b = i + 1 < png.size() ? png[i + 1] : 0;
                const uint8_t c = i + 2 < png.size() ? png[i + 2] : 0;
                lease.base64 += alphabet[a >> 2];
                lease.base64 += alphabet[((a & 3) << 4) | (b >> 4)];
                lease.base64 += i + 1 < png.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
                lease.base64 += i + 2 < png.size() ? alphabet[c & 63] : '=';
            }
            guard->status = "completed";
        } else {
            guard->status = lease.result.status == ScreenshotStatus::Failed ? "failed" : "cancelled";
            std::snprintf(guard->error, sizeof(guard->error), "%s",
                lease.result.error.empty() ? "screenshot-owner-expired" : lease.result.error.c_str());
        }
    } catch (const std::exception& error) {
        guard->status = "failed";
        std::snprintf(guard->error, sizeof(guard->error), "%s", error.what());
    } catch (...) {
        guard->status = "failed";
        std::snprintf(guard->error, sizeof(guard->error), "screenshot-result-failed");
    }
}

static int continueCapture(lua_State* L, int, lua_KContext context) {
    const int index = static_cast<int>(context);
    auto* guard = static_cast<CaptureGuard*>(lua_touserdata(L, index));
    // Resume arguments (e.g. dt) are not results and must not pop the tbc slot.
    lua_settop(L, index);
    if (guard->cancelled) {
        guard->status = "cancelled";
        std::snprintf(guard->error, sizeof(guard->error), "owner-cancelled");
    } else {
        pollCapture(guard);
    }
    if (!guard->status) {
        if (lua_isyieldable(L)) return lua_yieldk(L, 0, context, continueCapture);
        releaseCapture(guard);
        guard->status = "unavailable";
        std::snprintf(guard->error, sizeof(guard->error), "not-yieldable");
    }
    if (guard->lease && !guard->error[0] && !guard->lease->base64.empty())
        lua_pushlstring(L, guard->lease->base64.data(), guard->lease->base64.size());
    else lua_pushnil(L);
    lua_pushstring(L, guard->status);
    if (guard->error[0]) lua_pushstring(L, guard->error);
    else lua_pushnil(L);
    return 3;
}
} // namespace

// KAG.capture_thumbnail([CancelToken]) -> raw base64 or nil, status, reason.
// Pending stays within this C call until its own ticket reaches a terminal.
static int lua_CaptureThumbnail(lua_State* L) {
    if (!lua_isnoneornil(L, 1)) luaL_checktype(L, 1, LUA_TTABLE);
    auto* guard = new (lua_newuserdatauv(L, sizeof(CaptureGuard), 0)) CaptureGuard{};
    const int index = lua_gettop(L);
    luaL_setmetatable(L, kCaptureGuard);
    lua_toclose(L, index);
    if (index > 1 && !lua_isnil(L, 1)) {
        lua_getfield(L, 1, "register");
        luaL_checktype(L, -1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        lua_pushvalue(L, index);
        lua_pushcclosure(L, cancelCapture, 1);
        lua_call(L, 2, 0);
    }
    // Allocation, method lookup and cancellation registration all precede
    // admission. No C++ automatic owner crosses any Lua longjmp boundary.
    if (!guard->cancelled) startCapture(guard);
    return continueCapture(L, LUA_OK, index);
}

// KAG.get_save_dir() -> string
static int lua_Get_save_dir(lua_State* L) {
    lua_pushstring(L, "saves/");
    return 1;
}

// ============================================================================
//  Registration
// ============================================================================

// -- Cloud sync (C7): HTTP cloud-save endpoint + slot push/pull ------------
// KAG.save.configure_cloud(endpoint) -- "" disables (back to local-only)
// KAG.save.cloud_push(slot) / cloud_pull(slot) -- bool, offline-safe
static int lua_ConfigureCloud(lua_State* L) {
    auto* manager = getSaveManager();
    if (!manager) { lua_pushboolean(L, 0); return 1; }
    const char* endpoint = luaL_optstring(L, 1, "");
    lua_pushboolean(L, manager->configureCloudSync(endpoint) ? 1 : 0);
    return 1;
}
static int lua_CloudPush(lua_State* L) {
    auto* manager = getSaveManager();
    if (!manager) { lua_pushboolean(L, 0); return 1; }
    const int slot = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, manager->pushSlotToCloud(slot) ? 1 : 0);
    return 1;
}
static int lua_CloudPull(lua_State* L) {
    auto* manager = getSaveManager();
    if (!manager) { lua_pushboolean(L, 0); return 1; }
    const int slot = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, manager->pullSlotFromCloud(slot) ? 1 : 0);
    return 1;
}

void registerSaveBinding(lua_State* L) {
    if (luaL_newmetatable(L, kCaptureGuard)) {
        lua_pushcfunction(L, closeCapture);
        lua_setfield(L, -2, "__close");
        lua_pushcfunction(L, closeCapture);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    // [R12-FIX] Registration order note:
    // SaveBinding functions are appended to the existing "KAG" global table.
    // This requires that KAGBinding already registered the KAG table first.
    // LuaManager::registerModules() ensures this order (KAGBinding at line ~102,
    // SaveBinding at line ~108). If reordering, maintain this dependency.

    // Get or create the global KAG table
    lua_getglobal(L, "KAG");
    bool hasKAG = lua_istable(L, -1);

    if (!hasKAG) {
        lua_pop(L, 1);
        lua_newtable(L);
    }

static const luaL_Reg saveFuncs[] = {
        { "save_game",    lua_Save_game    },
        { "load_game",    lua_Load_game    },
        { "list_saves",   lua_List_saves   },
        { "delete_save",  lua_Delete_save  },
        { "save_exists",  lua_Save_exists  },
        { "get_save_dir", lua_Get_save_dir },
        { "set_encryption_key",  lua_SetEncryptionKey  },
        { "clear_encryption_key", lua_ClearEncryptionKey },
        { "capture_thumbnail",    lua_CaptureThumbnail },
        { "configure_cloud",     lua_ConfigureCloud },
        { "cloud_push",          lua_CloudPush },
        { "cloud_pull",          lua_CloudPull },
        { nullptr, nullptr }
    };

    luaL_setfuncs(L, saveFuncs, 0);

    if (!hasKAG) {
        lua_setglobal(L, "KAG");
    } else {
        lua_pop(L, 1);
    }

    printf("[SaveBinding] KAG save/load functions registered (9 APIs).\n");
}

} // namespace Caesura
