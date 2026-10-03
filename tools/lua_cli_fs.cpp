// Trusted standalone CLI capability, not an engine/sandbox registration.
// Compile the same small DirAssetProvider implementation into this executable;
// no Engine, BackendRegistry, archive, GPU, SDL or shell dependency is linked.
#include "resource/DirAssetProvider.h"
#include <cstring>
#include <string>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
namespace {
using namespace Caesura;
constexpr const char* boxType="Caesura.CliDirectoryResult";
int release(lua_State* L) {
    auto** box=static_cast<AssetDirectoryResult**>(luaL_testudata(L,1,boxType));
    if(box){delete *box;*box=nullptr;}return 0;
}
int fail(lua_State* L,const char* reason){lua_pushnil(L);lua_pushstring(L,reason);return 2;}
bool bounded(lua_State* L,int index,size_t max,size_t& value) {
    value=max;if(lua_isnoneornil(L,index))return true;
    int valid=0;auto number=lua_tointegerx(L,index,&valid);
    if(lua_type(L,index)!=LUA_TNUMBER||!valid||number<=0||static_cast<uint64_t>(number)>max)return false;
    value=static_cast<size_t>(number);return true;
}
const char* reason(AssetDirectoryStatus status) {
    switch(status){
        case AssetDirectoryStatus::InvalidPath:return "invalid or linked CLI asset directory";
        case AssetDirectoryStatus::LimitExceeded:return "CLI asset directory enumeration limit exceeded";
        case AssetDirectoryStatus::Unsupported:return "CLI asset directory enumeration unsupported";
        default:return "CLI asset directory enumeration I/O error";
    }
}
int list(lua_State* L) {
    lua_getglobal(L,"_SANDBOX_MODE");
    const bool strict=lua_type(L,-1)==LUA_TSTRING && std::strcmp(lua_tostring(L,-1),"strict")==0;
    lua_pop(L,1);
    if(strict)return fail(L,"CLI filesystem capability is disabled in strict mode");
    // Never route around an authoritative runtime asset-reader namespace.
    lua_getglobal(L,"KAG");bool runtime=false;
    if(lua_istable(L,-1)){lua_pushliteral(L,"list_assets");lua_rawget(L,-2);runtime=lua_isfunction(L,-1);lua_pop(L,1);}
    lua_pop(L,1);if(runtime)return fail(L,"Runtime asset listing is authoritative; CLI fallback refused");
    size_t count=0,bytes=0,pathSize=0;
    if(lua_type(L,1)!=LUA_TSTRING)return fail(L,"CLI directory must be a relative string");
    const char* path=lua_tolstring(L,1,&pathSize);
    if(pathSize==0||pathSize>4096)return fail(L,"Invalid CLI directory length");
    if(!bounded(L,2,kMaxAssetDirectoryEntries,count)||!bounded(L,3,kMaxAssetDirectoryNameBytes,bytes))
        return fail(L,"Invalid CLI enumeration limits");
    auto** box=static_cast<AssetDirectoryResult**>(lua_newuserdatauv(L,sizeof(AssetDirectoryResult*),0));
    *box=nullptr;luaL_setmetatable(L,boxType);
    AssetDirectoryStatus status=AssetDirectoryStatus::IoError;
    try {
        DirAssetProvider source(""); // CWD is the explicit standalone CLI root.
        *box=new AssetDirectoryResult(source.listDirectory(std::string(path,pathSize),count,bytes));
        status=(*box)->status;
    }catch(...){status=AssetDirectoryStatus::IoError;}
    if(status!=AssetDirectoryStatus::Complete){delete *box;*box=nullptr;return fail(L,reason(status));}
    lua_createtable(L,static_cast<int>((*box)->files.size()),0);
    for(size_t i=0;i<(*box)->files.size();++i){const auto& name=(*box)->files[i];lua_pushlstring(L,name.data(),name.size());lua_rawseti(L,-2,static_cast<lua_Integer>(i+1));}
    delete *box;*box=nullptr;lua_pushnil(L);return 2;
}
int openModule(lua_State* L) {
    luaL_newmetatable(L,boxType);lua_pushcfunction(L,release);lua_setfield(L,-2,"__gc");
    lua_pushboolean(L,false);lua_setfield(L,-2,"__metatable");lua_pop(L,1);
    static const luaL_Reg functions[]={{"list_dir",list},{nullptr,nullptr}};
    luaL_newlib(L,functions);return 1;
}
}
extern "C" void caesura_cli_openlibs(lua_State* L) {
    luaL_openlibs(L);
    luaL_getsubtable(L,LUA_REGISTRYINDEX,LUA_PRELOAD_TABLE);
    lua_pushcfunction(L,openModule);lua_setfield(L,-2,"caesura_cli_fs");lua_pop(L,1);
}
