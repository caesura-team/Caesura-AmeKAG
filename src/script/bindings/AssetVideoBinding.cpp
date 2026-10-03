#include "AssetVideoBinding.h"
#include "BindingAssetPath.h"
#include "../../di/BackendRegistry.h"
#include "../../resource/api/IAssetReader.h"
#include "../../render/api/IVideoPlayer.h"
#include "../../render/api/IRenderDevice.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <new>
#include <utility>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace Caesura {
namespace {
constexpr size_t maxBytes = 64u * 1024u * 1024u;
constexpr const char* sessionType = "Caesura.AssetVideoSessions";
struct Session { uint32_t token=0; VideoHandle handle{}; IVideoPlayer* owner=nullptr; uint64_t generation=0; };
struct Sessions { std::array<Session,4> slots{}; };
// Global uniqueness prevents numeric tokens copied between Lua states from
// accidentally selecting a different state-local session. Exhaustion refuses.
std::atomic<uint64_t> nextToken{1};
int fail(lua_State* L,const char* reason,bool boolean=false) {
    if(boolean) lua_pushboolean(L,false); else lua_pushnil(L);
    lua_pushstring(L,reason);return 2;
}
Sessions* sessions(lua_State* L) { return static_cast<Sessions*>(lua_touserdata(L,lua_upvalueindex(1))); }
bool current(const Session& value) {
    auto& reg=BackendRegistry::instance();
    return value.token && value.generation==reg.videoPlayerGeneration() && value.owner==reg.getVideoPlayer();
}
Session* lookup(lua_State* L) {
    if(!lua_isinteger(L,1)) return nullptr;
    const auto token=lua_tointeger(L,1);
    if(token<1 || token>UINT32_MAX) return nullptr;
    for(auto& slot:sessions(L)->slots) if(slot.token==static_cast<uint32_t>(token)) return current(slot)?&slot:nullptr;
    return nullptr;
}
int collect(lua_State* L) {
    auto* set=static_cast<Sessions*>(lua_touserdata(L,1));
    for(auto& slot:set->slots) {
        if(current(slot)) { try { slot.owner->close(slot.handle); } catch(...) {} }
        slot={};
    }
    return 0;
}
void field(lua_State* L,int table,const char* name) { lua_pushstring(L,name);lua_rawget(L,table); }
bool options(lua_State* L,bool& loop,float& volume) {
    if(lua_isnoneornil(L,2)) return true;
    if(lua_type(L,2)!=LUA_TTABLE) return false;
    field(L,2,"loop");
    const bool loopOk=lua_isnil(L,-1)||lua_isboolean(L,-1);
    if(lua_isboolean(L,-1)) loop=lua_toboolean(L,-1)!=0;
    lua_pop(L,1); if(!loopOk) return false;
    field(L,2,"volume");
    if(!lua_isnil(L,-1)) {
        if(lua_type(L,-1)!=LUA_TNUMBER) {lua_pop(L,1);return false;}
        const double value=lua_tonumber(L,-1);
        if(!std::isfinite(value)||value<0||value>1.5) {lua_pop(L,1);return false;}
        volume=static_cast<float>(value);
    }
    lua_pop(L,1);return true;
}
int play(lua_State* L) {
    if(lua_type(L,1)!=LUA_TSTRING) return fail(L,"Video asset path must be a string");
    size_t length=0;const char* path=lua_tolstring(L,1,&length);
    bool loop=false;float volume=1;
    if(!validBindingAssetPath(path,length)||!options(L,loop,volume)) return fail(L,"Invalid video asset path or options");
    auto& reg=BackendRegistry::instance();
    auto* player=reg.getVideoPlayer();auto* reader=reg.getAssetReader();
    if(!player||!reader) return fail(L,"Asset video backend unavailable");
    Session* free=nullptr;
    for(auto& slot:sessions(L)->slots) {
        if(slot.token&&!current(slot)) slot={};
        if(!slot.token&&!free) free=&slot;
    }
    if(!free) return fail(L,"Asset video session limit reached");
    const uint64_t token=nextToken.fetch_add(1);
    if(token>UINT32_MAX) return fail(L,"Asset video token space exhausted");
    const auto generation=reg.videoPlayerGeneration();
    VideoHandle handle{};char error[256]={};
    try {
        auto bytes=reader->readAsset(std::string(path,length),maxBytes);
        if(bytes.empty()||bytes.size()>maxBytes) std::snprintf(error,sizeof(error),"Video asset missing, failed or oversized");
        else if(player!=reg.getVideoPlayer()||generation!=reg.videoPlayerGeneration())
            std::snprintf(error,sizeof(error),"Video backend changed during asset read");
        else {
            handle=player->openMemory(std::move(bytes));
            if(!handle) std::snprintf(error,sizeof(error),"Video asset decoding refused");
            else if(player!=reg.getVideoPlayer()||generation!=reg.videoPlayerGeneration())
                std::snprintf(error,sizeof(error),"Video backend changed during decode");
            else {
                // Establish cleanup ownership before any later backend call.
                *free={static_cast<uint32_t>(token),handle,player,generation};
                player->setLoop(handle,loop);player->setVolume(handle,volume);
            }
        }
    } catch(const std::exception& cause) {std::snprintf(error,sizeof(error),"Asset video: %.200s",cause.what());}
      catch(...) {std::snprintf(error,sizeof(error),"Asset video failed");}
    if(player!=reg.getVideoPlayer()||generation!=reg.videoPlayerGeneration())
        std::snprintf(error,sizeof(error),"Video backend changed during open");
    if(error[0]) {
        if(handle && player==reg.getVideoPlayer()&&generation==reg.videoPlayerGeneration()) {
            // If close throws, retain the slot for GC/engine retirement instead
            // of losing ownership and permitting unbounded repeated opens.
            if(!free->token) *free={static_cast<uint32_t>(token),handle,player,generation};
            try{player->close(handle);*free={};}catch(...){}
        }
        return fail(L,error);
    }
    *free={static_cast<uint32_t>(token),handle,player,generation};
    lua_pushinteger(L,static_cast<lua_Integer>(token));return 1;
}
int stop(lua_State* L) {
    auto* slot=lookup(L);if(!slot) return fail(L,"Unknown or expired asset video session",true);
    char error[128]={};
    try{slot->owner->close(slot->handle);*slot={};}
    catch(...){std::snprintf(error,sizeof(error),"Asset video close failed");}
    if(error[0]) return fail(L,error,true);
    lua_pushboolean(L,true);return 1;
}
int playing(lua_State* L) {
    auto* slot=lookup(L);if(!slot) return fail(L,"Unknown or expired asset video session",true);
    bool value=false;char error[128]={};
    try{value=slot->owner->isPlaying(slot->handle);}catch(...){std::snprintf(error,sizeof(error),"Asset video query failed");}
    if(error[0]) return fail(L,error,true);
    lua_pushboolean(L,value);return 1;
}
int draw(lua_State* L) {
    auto* slot=lookup(L);if(!slot) return fail(L,"Unknown or expired asset video session",true);
    float rect[4]={};
    for(int i=0;i<4;++i) {
        if(lua_isnoneornil(L,i+2)) continue;
        if(lua_type(L,i+2)!=LUA_TNUMBER) return fail(L,"Video rectangle must be numeric",true);
        const auto value=lua_tonumber(L,i+2);
        if(!std::isfinite(value)||std::abs(value)>1048576 || (i>=2 && (value<0 || value>8192)))
            return fail(L,"Invalid video rectangle",true);
        rect[i]=static_cast<float>(value);
    }
    bool drawn=false;char error[128]={};
    try {
        auto* renderer=BackendRegistry::instance().getRenderDevice();
        if(renderer&&slot->owner->isPlaying(slot->handle)) {
            const auto texture=slot->owner->getTexture(slot->handle);
            if(texture) {
                if(rect[2]==0) rect[2]=static_cast<float>(renderer->getBackbufferWidth());
                if(rect[3]==0) rect[3]=static_cast<float>(renderer->getBackbufferHeight());
                renderer->blitTexture(VIEW_MAIN,texture,rect[0],rect[1],rect[2],rect[3],255);drawn=true;
            }
        }
    } catch(...) {std::snprintf(error,sizeof(error),"Asset video draw failed");}
    if(error[0]) return fail(L,error,true);
    lua_pushboolean(L,drawn);return 1;
}
}
void registerAssetVideoBinding(lua_State* L) {
    luaL_newmetatable(L,sessionType);
    lua_pushcfunction(L,collect);lua_setfield(L,-2,"__gc");
    lua_pushboolean(L,false);lua_setfield(L,-2,"__metatable");lua_pop(L,1);
    lua_getglobal(L,"KAG");
    auto* set=static_cast<Sessions*>(lua_newuserdatauv(L,sizeof(Sessions),0));new(set) Sessions{};
    luaL_setmetatable(L,sessionType);
    static const luaL_Reg functions[]={{"video_asset_play",play},{"video_asset_stop",stop},
        {"video_asset_draw",draw},{"video_asset_is_playing",playing},{nullptr,nullptr}};
    // setfuncs copies one shared userdata upvalue into every safe C closure.
    luaL_setfuncs(L,functions,1);lua_pop(L,1);
}
}
