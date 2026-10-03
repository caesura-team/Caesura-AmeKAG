#include "AssetDirectoryBinding.h"
#include "BindingAssetPath.h"
#include "../../di/BackendRegistry.h"
#include "../../resource/api/IAssetReader.h"
#include <new>
#include <string>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace Caesura {
namespace {
constexpr const char* kResultBox="Caesura.AssetDirectoryResult";
int release(lua_State* L) {
    auto** box=static_cast<AssetDirectoryResult**>(lua_touserdata(L,1));
    delete *box;*box=nullptr;return 0;
}
int fail(lua_State* L,const char* reason){lua_pushnil(L);lua_pushstring(L,reason);return 2;}
bool limit(lua_State* L,int index,size_t maximum,size_t& output) {
    output=maximum;if(lua_isnoneornil(L,index))return true;
    int integral=0;const auto number=lua_tointegerx(L,index,&integral);
    if(lua_type(L,index)!=LUA_TNUMBER||!integral||number<=0||static_cast<uint64_t>(number)>maximum)return false;
    output=static_cast<size_t>(number);return true;
}
const char* reason(AssetDirectoryStatus status) {
    switch(status) {
        case AssetDirectoryStatus::Unsupported:return "asset listing unsupported by a mounted provider";
        case AssetDirectoryStatus::InvalidPath:return "invalid or linked asset directory path";
        case AssetDirectoryStatus::LimitExceeded:return "asset directory enumeration limit exceeded";
        case AssetDirectoryStatus::IoError:return "asset directory enumeration I/O error";
        default:return "incomplete asset directory enumeration";
    }
}
int list(lua_State* L) {
    if(lua_type(L,1)!=LUA_TSTRING)return fail(L,"asset directory must be a relative string");
    size_t pathBytes=0;const char* path=lua_tolstring(L,1,&pathBytes);
    size_t maxEntries=0,maxNameBytes=0;
    if(!validBindingAssetPath(path,pathBytes))return fail(L,"invalid asset directory path");
    if(!limit(L,2,kMaxAssetDirectoryEntries,maxEntries)||!limit(L,3,kMaxAssetDirectoryNameBytes,maxNameBytes))
        return fail(L,"invalid asset directory limits");
    auto* reader=BackendRegistry::instance().getAssetReader();
    if(!reader)return fail(L,"asset listing unsupported: no asset reader");
    // Lua owns the heap result before any string/table allocation can longjmp
    // on OOM. C++ provider allocations unwind normally before publication.
    auto** box=static_cast<AssetDirectoryResult**>(lua_newuserdatauv(L,sizeof(AssetDirectoryResult*),0));
    *box=nullptr;luaL_setmetatable(L,kResultBox);
    AssetDirectoryStatus status=AssetDirectoryStatus::IoError;
    try {
        *box=new AssetDirectoryResult(reader->listDirectory(std::string(path,pathBytes),maxEntries,maxNameBytes));
        status=(*box)->status;
        if(status==AssetDirectoryStatus::Complete) {
            size_t bytes=0;
            if((*box)->files.size()>maxEntries)status=AssetDirectoryStatus::LimitExceeded;
            for(const auto& name:(*box)->files) {
                if(!validBindingAssetPath(name.data(),name.size())||name.find('/')!=std::string::npos) {
                    status=AssetDirectoryStatus::InvalidPath;break;
                }
                if(name.size()>maxNameBytes-bytes){status=AssetDirectoryStatus::LimitExceeded;break;}
                bytes+=name.size();
            }
        }
    }catch(...){status=AssetDirectoryStatus::IoError;}
    if(status!=AssetDirectoryStatus::Complete){delete *box;*box=nullptr;return fail(L,reason(status));}
    lua_createtable(L,static_cast<int>((*box)->files.size()),0);
    for(size_t i=0;i<(*box)->files.size();++i) {
        const auto& name=(*box)->files[i];lua_pushlstring(L,name.data(),name.size());lua_rawseti(L,-2,static_cast<lua_Integer>(i+1));
    }
    delete *box;*box=nullptr;lua_pushnil(L);return 2;
}
}
void registerAssetDirectoryBinding(lua_State* L) {
    luaL_newmetatable(L,kResultBox);lua_pushcfunction(L,release);lua_setfield(L,-2,"__gc");
    lua_pushboolean(L,false);lua_setfield(L,-2,"__metatable");lua_pop(L,1);
    lua_getglobal(L,"KAG");lua_pushcfunction(L,list);lua_setfield(L,-2,"list_assets");lua_pop(L,1);
}
}
