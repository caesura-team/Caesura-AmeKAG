#include "doctest.h"
#include "di/BackendRegistry.h"
#include "resource/AssetManager.h"
#include "resource/DirAssetProvider.h"
#include "archive/CARCReader.h"
#include "archive/CARCWriter.h"
#include "archive/CarcAssetProvider.h"
#include "script/bindings/KAGBinding.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
namespace {
using namespace Caesura;
namespace fs=std::filesystem;
std::string utf8(const fs::path& path){const auto s=path.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
struct Fixture {
    static inline std::atomic<unsigned> serial{0};
    fs::path root;
    std::string directory;
    AssetManager assets;
    IAssetReader* previous=BackendRegistry::instance().getAssetReader();
    lua_State* L=nullptr;
    Fixture(){
        const auto tag=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"_"+std::to_string(serial++);
        root=fs::absolute(fs::current_path()/fs::u8path("asset_listing_"+tag));
        REQUIRE(fs::create_directory(root));
        directory="assets/catalog_"+tag;
        assets.init();assets.addProvider(std::make_unique<DirAssetProvider>(utf8(root/"dir")));
        BackendRegistry::instance().setAssetReader(&assets);
        L=luaL_newstate();REQUIRE(L);luaL_openlibs(L);registerKAGBinding(L);
        lua_pushlstring(L,directory.data(),directory.size());lua_setglobal(L,"LIST_PATH");
    }
    ~Fixture(){
        if(L)lua_close(L);
        BackendRegistry::instance().setAssetReader(previous);assets.shutdown();
        // This root was created exclusively by this fixture beneath its CWD.
        // remove_all does not follow contained symlinks/junctions to targets.
        std::error_code error;fs::remove_all(root,error);
    }
    void file(const std::string& relative,const std::string& bytes="fixture"){
        auto target=root/"dir"/fs::u8path(relative);fs::create_directories(target.parent_path());
        std::ofstream out(target,std::ios::binary);out<<bytes;out.close();REQUIRE(bool(out));
    }
    void run(const char* code){
        const auto result=luaL_dostring(L,code);
        const std::string error=result==LUA_OK?"":(lua_tostring(L,-1)?lua_tostring(L,-1):"non-string error");
        REQUIRE_MESSAGE(result==LUA_OK,error);
    }
    void strict(){
        // Absent old binding produces a real, compilable RED. This is not an
        // injected enumeration implementation or a compiler-mirroring check.
        run("assert(type(KAG.list_assets)=='function','safe native asset listing missing');assert(debug.getinfo(KAG.list_assets,'S').what=='C')");
        const auto sandbox=utf8(fs::path(CAESURA_SOURCE_DIR)/"scripts/sandbox.lua");
        const auto result=luaL_dofile(L,sandbox.c_str());
        const std::string error=result==LUA_OK?"":(lua_tostring(L,-1)?lua_tostring(L,-1):"sandbox error");
        REQUIRE_MESSAGE(result==LUA_OK,error);run("assert(_SANDBOX_MODE=='strict')");
    }
    void archive(){
        const auto archive=root/"owned.carc";
        {
            carc::CARCWriter writer;REQUIRE(writer.create(utf8(archive)));
            const std::string value="archive-priority";
            REQUIRE(writer.addFile(directory+"/alpha.ogg",reinterpret_cast<const uint8_t*>(value.data()),value.size()));
            REQUIRE(writer.addFile(directory+"/only-in-archive.wav",reinterpret_cast<const uint8_t*>(value.data()),value.size()));
            REQUIRE(writer.finalize());
        }
        auto reader=std::make_unique<carc::CARCReader>();REQUIRE(reader->open(utf8(archive)));
        REQUIRE(reader->hasFile(directory+"/alpha.ogg"));
        assets.addProvider(std::make_unique<carc::CarcAssetProvider>(std::move(reader),40,"owned-catalog-test"));
    }
};
void directoryLink(const fs::path& target,const fs::path& link){
#ifdef _WIN32
    // A junction needs no symlink privilege/elevation. All paths belong to the
    // fixture. This only constructs the actual containment-negative input.
    REQUIRE(fs::create_directory(link));
    const std::wstring printable=fs::absolute(target).native();
    const std::wstring substitute=L"\\??\\"+printable;
    struct Header {DWORD tag;USHORT length,reserved,subOffset,subLength,printOffset,printLength;};
    const size_t pathsBytes=(substitute.size()+1+printable.size()+1)*sizeof(wchar_t);
    std::vector<unsigned char> bytes(sizeof(Header)+pathsBytes,0);
    auto* data=reinterpret_cast<Header*>(bytes.data());
    data->tag=IO_REPARSE_TAG_MOUNT_POINT;data->length=static_cast<USHORT>(8+pathsBytes);
    data->subOffset=0;data->subLength=static_cast<USHORT>(substitute.size()*sizeof(wchar_t));
    data->printOffset=static_cast<USHORT>((substitute.size()+1)*sizeof(wchar_t));
    data->printLength=static_cast<USHORT>(printable.size()*sizeof(wchar_t));
    std::memcpy(bytes.data()+sizeof(Header),substitute.c_str(),(substitute.size()+1)*sizeof(wchar_t));
    std::memcpy(bytes.data()+sizeof(Header)+data->printOffset,printable.c_str(),(printable.size()+1)*sizeof(wchar_t));
    HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    REQUIRE(handle!=INVALID_HANDLE_VALUE);DWORD returned=0;
    const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,bytes.data(),static_cast<DWORD>(bytes.size()),nullptr,0,&returned,nullptr)!=0;
    const DWORD error=ok?ERROR_SUCCESS:GetLastError();CloseHandle(handle);CAPTURE(error);REQUIRE(ok);
#else
    fs::create_directory_symlink(target,link);
#endif
}
}
TEST_CASE("Asset directory contract: strict native Dir listing is complete bounded and nonrecursive"){
    Fixture f;f.file(f.directory+"/alpha.ogg");f.file(f.directory+"/zeta.wav");f.file(f.directory+"/音楽.wav");
    f.file(f.directory+"/nested/hidden.ogg");fs::create_directories(f.root/"dir"/fs::u8path(f.directory)/"directory.wav");
    f.strict();
    f.run(R"(
        local names,err=KAG.list_assets(LIST_PATH,4096,1048576)
        assert(type(names)=='table' and err==nil)
        assert(#names==3 and names[1]=='alpha.ogg' and names[2]=='zeta.wav' and names[3]=='音楽.wav','leaf/sort/nonrecursive contract')
        local again=assert(KAG.list_assets(LIST_PATH..'/',4096,1048576));assert(#again==3)
        local value,reason=KAG.list_assets(LIST_PATH,1,1048576)
        assert(value==nil and type(reason)=='string' and #reason>0,'entry overflow must be explicit')
        value,reason=KAG.list_assets(LIST_PATH,4096,25)
        assert(value==nil and type(reason)=='string' and #reason>0,'name-byte overflow must be explicit')
        assert(not io.popen('never execute a shell'))
    )");
    CHECK_FALSE(fs::exists(f.root/"dir"/fs::u8path(f.directory)/"_dir_test_.tmp"));
}
TEST_CASE("Asset directory contract: strict rejects paths and out of policy limits"){
    Fixture f;f.file(f.directory+"/alpha.ogg");f.strict();
    f.run(R"(
        for _,path in ipairs({'../outside','/outside','C:/outside','https://example.invalid/assets',
            'assets\\outside','assets/../outside','assets'..string.char(0)..'/outside'}) do
            local value,reason=KAG.list_assets(path,4096,1048576)
            assert(value==nil and type(reason)=='string' and #reason>0)
        end
        for _,limit in ipairs({0,-1,4097,0/0,math.huge,false,'4096',1.5}) do
            local value,reason=KAG.list_assets(LIST_PATH,limit,1048576)
            assert(value==nil and type(reason)=='string')
        end
        local value,reason=KAG.list_assets(LIST_PATH,4096,1048577)
        assert(value==nil and type(reason)=='string')
    )");
}
TEST_CASE("Asset directory contract: real CARC reports unsupported without changing read priority"){
    Fixture f;f.file(f.directory+"/alpha.ogg","loose-lower-priority");f.archive();
    const auto bytes=f.assets.readAsset(f.directory+"/alpha.ogg",1024);
    REQUIRE(std::string(bytes.begin(),bytes.end())=="archive-priority");
    f.strict();
    f.run(R"(
        local value,reason=KAG.list_assets(LIST_PATH,4096,1048576)
        assert(value==nil and type(reason)=='string' and reason:find('unsupported',1,true),
            'hashed archive cannot claim complete empty or partial filename listing')
    )");
    const auto after=f.assets.readAsset(f.directory+"/alpha.ogg",1024);
    CHECK(after==bytes);
}
TEST_CASE("Asset directory contract: real directory escape is refused"){
    Fixture f;f.file(f.directory+"/alpha.ogg");
    const auto outside=f.root/"outside";fs::create_directory(outside);
    {std::ofstream out(outside/"outside-secret.wav");out<<"not-an-asset";REQUIRE(out.good());}
    const auto link=f.root/"dir"/fs::u8path(f.directory)/"escape";directoryLink(outside,link);
    f.strict();
    f.run(R"(
        local names,reason=KAG.list_assets(LIST_PATH..'/escape',4096,1048576)
        assert(names==nil and type(reason)=='string' and #reason>0,'escaped enumeration must fail explicitly')
    )");
}

TEST_CASE("Asset directory contract: real gallery and music scanners use strict asset listing"){
    Fixture f;
    // Enumeration/classification fixtures only; no decode/playback claim.
    f.file("assets/cg/catalog_contract.png");f.file("assets/cg/catalog_upper.JPG");f.file("assets/cg/not_an_image.txt");
    f.file("assets/bgm/catalog_track.ogg");f.file("assets/bgm/catalog_upper.WAV");f.file("assets/bgm/not_audio.txt");
    const auto path=utf8(fs::path(CAESURA_SOURCE_DIR)/"scripts")+"/?.lua;";
    lua_pushlstring(f.L,path.data(),path.size());lua_setglobal(f.L,"CATALOG_SCRIPTS");
    f.run("package.path=CATALOG_SCRIPTS..package.path;CatalogGallery=require('gallery');CatalogMusic=require('music_room')");
    f.strict();
    f.run(R"(
        local function contains(entries,want)
            for _,entry in ipairs(entries) do if entry.id==want then return true end end
            return false
        end
        local images=CatalogGallery.scan({})
        assert(contains(images,'catalog_contract') and contains(images,'catalog_upper'),'actual image extensions not discovered')
        assert(not contains(images,'not_an_image'),'non-image leaked into gallery')
        local music=CatalogMusic.scan()
        assert(contains(music,'catalog_track') and contains(music,'catalog_upper'),'actual audio extensions not discovered')
        assert(not contains(music,'not_audio'),'non-audio leaked into music room')
        assert(not io.popen('never execute a shell'))
    )");
    CHECK_FALSE(fs::exists(f.root/"dir/assets/cg/_dir_test_.tmp"));
    CHECK_FALSE(fs::exists(f.root/"dir/assets/bgm/_dir_test_.tmp"));
}

namespace {
struct ScanObservation { bool success=false, found=false;lua_Integer count=0;std::string reason; };
ScanObservation observeScan(Fixture& fixture) {
    const int base=lua_gettop(fixture.L);
    const int result=luaL_loadstring(fixture.L,R"(
        local ok,value=pcall(CatalogScan.scan,{})
        if not ok then return false,0,false,tostring(value) end
        assert(type(value)=='table','scan must return a list on complete success')
        local found=false
        for _,item in ipairs(value) do if item.id==RECOVERED_ID then found=true end end
        return true,#value,found,''
    )");
    REQUIRE(result==LUA_OK);
    const auto called=lua_pcall(fixture.L,0,4,0);
    const std::string error=called==LUA_OK?"":(lua_tostring(fixture.L,-1)?lua_tostring(fixture.L,-1):"scan invocation error");
    REQUIRE_MESSAGE(called==LUA_OK,error);
    ScanObservation observation;
    observation.success=lua_toboolean(fixture.L,-4)!=0;
    observation.count=lua_tointeger(fixture.L,-3);
    observation.found=lua_toboolean(fixture.L,-2)!=0;
    const char* reason=lua_tostring(fixture.L,-1);observation.reason=reason?reason:"";
    lua_settop(fixture.L,base);return observation;
}
void verifyScanCacheRetry(const char* module,const char* assetDirectory,const char* extension) {
    Fixture fixture;
    const std::string recovered="cache_recovered_"+fixture.directory.substr(fixture.directory.find_last_of('/')+1);
    fixture.file(std::string(assetDirectory)+"/"+recovered+extension);
    fixture.archive(); // Real hashed archive causes explicit incomplete listing.
    const auto scripts=utf8(fs::path(CAESURA_SOURCE_DIR)/"scripts")+"/?.lua;";
    lua_pushlstring(fixture.L,scripts.data(),scripts.size());lua_setglobal(fixture.L,"CACHE_SCRIPTS");
    lua_pushstring(fixture.L,module);lua_setglobal(fixture.L,"CACHE_MODULE");
    lua_pushlstring(fixture.L,recovered.data(),recovered.size());lua_setglobal(fixture.L,"RECOVERED_ID");
    fixture.run("package.path=CACHE_SCRIPTS..package.path;CatalogScan=require(CACHE_MODULE)");
    fixture.strict();
    const auto first=observeScan(fixture);
    CHECK_FALSE(first.success);CHECK(first.reason.find("unsupported")!=std::string::npos);
    // CHECK, rather than REQUIRE, preserves the recovery observation even when
    // the old implementation silently returns its prematurely cached {} here.
    const auto second=observeScan(fixture);
    CHECK_FALSE(second.success);CHECK(second.reason.find("unsupported")!=std::string::npos);
    fixture.assets.shutdown();fixture.assets.init();
    fixture.assets.addProvider(std::make_unique<DirAssetProvider>(utf8(fixture.root/"dir")));
    const auto native=fixture.assets.listDirectory(assetDirectory,4096,1048576);
    REQUIRE(native.status==AssetDirectoryStatus::Complete);
    // The Lua state, required module and scan closure are unchanged. Only the
    // real provider set has lost its non-enumerable archive and can now list.
    const auto recoveredScan=observeScan(fixture);
    CHECK(recoveredScan.success);CHECK(recoveredScan.found);CHECK(recoveredScan.count>=1);
    const auto cached=observeScan(fixture);
    CHECK(cached.success);CHECK(cached.found);CHECK(cached.count==recoveredScan.count);
}
}
TEST_CASE("Asset directory cache contract: gallery preserves errors and retries the same module") {
    verifyScanCacheRetry("gallery","assets/cg",".png");
}
TEST_CASE("Asset directory cache contract: music preserves errors and retries the same module") {
    verifyScanCacheRetry("music_room","assets/bgm",".ogg");
}

TEST_CASE("Asset CLI separation contract: engine Lua library does not register standalone filesystem") {
    Fixture fixture;
    fixture.run("assert(package.preload.caesura_cli_fs==nil and package.loaded.caesura_cli_fs==nil)");
    fixture.strict();
    fixture.run("local ok=pcall(require,'caesura_cli_fs');assert(not ok,'engine strict Lua must not gain CLI capability')");
}
