// Opt-in command corpus host: real engine/bindings/backends, hidden SDL window.
// Receipt success is scoped to the selected script; external owned cleanup,
// source/asset hashes, GPU pixels and full command coverage remain parent checks.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "entry/RuntimeStats.h"
#include "di/BackendRegistry.h"
#include "audio/SoLoudAudioEngine.h"
#include "render/BgfxRenderDevice.h"
#include "render/ScreenshotQueue.h"
#include "render/api/IMeshRenderer.h"
#include "platform/api/IPlatformBackend.h"
#include "script/vm/LuaManager.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <windows.h>
#include <nlohmann_json.hpp>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <functional>
#include <algorithm>
#include <cstdio>
#include <bcrypt.h>
#include <array>
#include <vector>
#include <chrono>
#include <thread>
#include <climits>
#include <dbghelp.h>
#include <atomic>
#include <cstdarg>
#include <memory>
#include <cstring>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace Caesura {
void configureStartupLuaPath(lua_State*, const std::string&);
void applyDevModeToTextureManager(lua_State*);
}
namespace {
using namespace Caesura;
namespace fs = std::filesystem;
using json = nlohmann::json;
constexpr int kWidth = 640, kHeight = 360;
void require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }
std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
void plainAncestors(const fs::path& path) {
    fs::path part;
    for (const auto& component : fs::absolute(path)) {
        part /= component;
        const auto flags = GetFileAttributesW(part.c_str());
        require(flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_REPARSE_POINT),
                "Missing/reparse path: " + utf8(part));
    }
}
struct PngBudgetLimit : std::runtime_error { using std::runtime_error::runtime_error; };
struct OfficialPngBudget {
    static constexpr uint64_t PerCase = 512ull * 1024 * 1024;
    static constexpr uint64_t Total = 4ull * 1024 * 1024 * 1024;
    static constexpr uint64_t OneFrame = ScreenshotQueue::MaxPngBytes;
    fs::path directory;
    uint64_t priorBytes = 0, bytes = 0, limit = 0;
    unsigned files = 0;
    bool incomplete = false;
    std::string reason;
    void admit(const fs::path& root, const fs::path& output) {
        plainAncestors(root);
        require(fs::is_directory(root), "Official output root is not a directory");
        const auto relative = fs::relative(output, root);
        require(!relative.empty() && relative.is_relative() && *relative.begin() != "..",
                "Official output escaped its aggregate budget root");
        size_t entries = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(++entries <= 200000, "Official budget inventory limit exceeded");
            plainAncestors(entry.path());
            if (entry.is_regular_file() && entry.path().extension() == ".png") {
                const auto size = entry.file_size();
                if (size > Total || priorBytes > Total - size)
                    throw PngBudgetLimit("Official total PNG budget already exhausted");
                priorBytes += size;
            }
        }
        limit = std::min(PerCase, Total - priorBytes);
        if (limit == 0) throw PngBudgetLimit("Official PNG budget exhausted before admission");
        directory = output / "png";
    }
    bool write(unsigned index, const std::vector<uint8_t>& png) {
        require(!png.empty() && png.size() <= OneFrame, "PNG exceeds renderer byte contract");
        if (bytes > limit || png.size() > limit - bytes) {
            incomplete=true; reason="Actual next PNG exceeds remaining case/aggregate byte budget";
            return false; // Refuse before opening a file; never delete earlier evidence.
        }
        char name[32]; std::snprintf(name,sizeof(name),"frame_%05u.png",index);
        const auto path=directory/name; plainAncestors(directory);
        const HANDLE output=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(output!=INVALID_HANDLE_VALUE,"Cannot exclusively create official PNG");
        DWORD written=0;
        const bool wrote=WriteFile(output,png.data(),static_cast<DWORD>(png.size()),&written,nullptr)!=0
            && written==png.size();
        const bool closed=CloseHandle(output)!=0;
        require(wrote && closed,"Cannot finish official PNG; partial evidence retained");
        bytes+=png.size();++files;
        return true;
    }
};
void rawGlobal(lua_State* L, const char* key) {
    lua_pushglobaltable(L); lua_pushstring(L, key); lua_rawget(L, -2); lua_remove(L, -2);
}
void rawField(lua_State* L, const char* key) { lua_pushstring(L, key); lua_rawget(L, -2); }
bool exactBoolean(lua_State* L, int index) { return lua_type(L, index) == LUA_TBOOLEAN && lua_toboolean(L, index); }
std::string exactString(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TSTRING) return {};
    size_t size = 0; const char* bytes = lua_tolstring(L, index, &size);
    require(size <= 65536, "Oversize script receipt field"); return {bytes, size};
}
void replay(lua_State* L, const std::string& path) {
    // All module lookups/accessors execute within pcall, as in main startup.
    const char* chunk = "local p=...; local r=require('replay'); local n,e=r.load(p); "
        "assert(type(n)=='number' and n==0,e); local ok,why=r.set_mode('playback',p); assert(ok==true,why)";
    require(luaL_loadstring(L, chunk) == LUA_OK, "Cannot prepare replay initialization");
    lua_pushlstring(L, path.data(), path.size());
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const auto error = exactString(L, -1); lua_pop(L, 1); throw std::runtime_error("Replay: " + error);
    }
}
std::string pngSha(const std::vector<uint8_t>& bytes) {
    struct HashOwner {
        BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
        std::vector<uint8_t> object;
        ~HashOwner(){if(hash) BCryptDestroyHash(hash);if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);}
    } owner;
    require(bytes.size()<=ULONG_MAX,"PNG hash length overflow");
    require(BCryptOpenAlgorithmProvider(&owner.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 provider failed");
    DWORD size=0,copied=0;
    require(BCryptGetProperty(owner.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&copied,0)>=0
        && size<=65536,"SHA256 object bound failed");
    owner.object.resize(size);std::array<uint8_t,32> digest{};
    require(BCryptCreateHash(owner.algorithm,&owner.hash,owner.object.data(),size,nullptr,0,0)>=0,"SHA256 creation failed");
    require(BCryptHashData(owner.hash,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)>=0
        && BCryptFinishHash(owner.hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0,"SHA256 failed");
    std::string hex;static constexpr char digits[]="0123456789abcdef";
    for(auto byte:digest){hex+=digits[byte>>4];hex+=digits[byte&15];}return hex;
}
struct OfficialCapture {
    Engine* engine=nullptr;OfficialPngBudget* budget=nullptr;
    ScreenshotTicket pending{};unsigned index=0,renderCallbacks=0;
    uint64_t lastFrame=0,generation=0;
    bool active=false;
    json requests=json::array(),completed=json::array();
    std::array<bool,64> first{};
    void fail(const std::string& reason) noexcept {
        if (!budget->incomplete || budget->reason.empty()) {
            try { budget->reason=reason; } catch (...) {}
        }
        budget->incomplete=true;
        engine->quit();
    }
    void request(lua_State* L) {
        if(!active) return;
        require(!pending,"Previous official screenshot ticket was not retired");
        const auto frame=engine->getHostSnapshot().completedOwnerFrames;
        require(frame<6000,"Official frame index outside bound");++renderCallbacks;
        rawGlobal(L,"_CAESURA_COMMAND_CONTRACT_RESULT");
        require(lua_istable(L,-1),"Missing official capture selection");
        rawField(L,"capture_requested");const bool selected=exactBoolean(L,-1);lua_pop(L,2);
        if(frame>=64 && !selected) return;
        index=static_cast<unsigned>(frame);
        requests.push_back({{"export_index",index},{"selection",frame<64?"mandatory-first64":"driver-trans-or-page"}});
        auto result=engine->renderDevice().requestScreenshot(ScreenshotOptions{});
        pending=result.ticket;
        require(result.status==ScreenshotStatus::Pending && bool(pending),"Official screenshot admission refused: "+result.error);
    }
    static int render(lua_State* L) {
        auto* self=static_cast<OfficialCapture*>(lua_touserdata(L,lua_upvalueindex(1)));
        const int args=lua_gettop(L);
        rawGlobal(L,"_CAESURA_COMMAND_CONTRACT_RESULT");
        if(lua_istable(L,-1)){lua_pushliteral(L,"capture_requested");lua_pushboolean(L,false);lua_rawset(L,-3);}
        lua_pop(L,1); // Selection belongs to this invocation, never a prior frame.
        lua_pushvalue(L,lua_upvalueindex(2));lua_insert(L,1);
        if(lua_pcall(L,args,0,0)!=LUA_OK) return lua_error(L);
        try{self->request(L);}catch(const std::exception& error){self->fail(error.what());}
        return 0;
    }
    void install(lua_State* L) {
        rawGlobal(L,"engine_render");require(lua_isfunction(L,-1),"Official driver did not install engine_render");
        lua_pushlightuserdata(L,this);lua_pushvalue(L,-2);
        lua_pushcclosure(L,render,2);lua_setglobal(L,"engine_render");lua_pop(L,1);
        active=true;
    }
    void afterFrame() {
        if(!pending) return;
        try{
            const auto ticket=pending;
            auto result=engine->renderDevice().takeScreenshot(ticket);
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(result.status==ScreenshotStatus::Pending && std::chrono::steady_clock::now()<until){
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                result=engine->renderDevice().takeScreenshot(ticket);
            }
            if(result.status==ScreenshotStatus::Pending){
                engine->renderDevice().cancelScreenshot(ticket);engine->renderDevice().takeScreenshot(ticket);
            }
            pending={};
            require(result.status==ScreenshotStatus::Completed && !result.png.empty(),"Official screenshot failed or timed out: "+result.error);
            require(result.ticket.requestId==ticket.requestId && result.ticket.generation==ticket.generation
                && result.frameId>lastFrame && (!generation || generation==ticket.generation),"Official screenshot identity/frame mismatch");
            lastFrame=result.frameId;generation=ticket.generation;
            const auto digest=pngSha(result.png);
            if(!budget->write(index,result.png)){engine->quit();return;}
            if(index<first.size()) first[index]=true;
            char filename[40];std::snprintf(filename,sizeof(filename),"png/frame_%05u.png",index);
            completed.push_back({{"export_index",index},{"request_id",ticket.requestId},{"generation",ticket.generation},
                {"renderer_frame_id",result.frameId},{"bytes",result.png.size()},{"sha256",digest},
                {"png",filename},
                {"width",result.width},{"height",result.height}});
        }catch(const std::exception& error){fail(error.what());}
    }
    void finish() {
        active=false;
        if(pending){engine->renderDevice().cancelScreenshot(pending);engine->renderDevice().takeScreenshot(pending);pending={};fail("Screenshot has no corresponding presented frame");}
        const auto needed=std::min<uint64_t>(64,engine->getHostSnapshot().completedOwnerFrames);
        for(uint64_t i=0;i<needed;++i) if(!first[static_cast<size_t>(i)]){fail("Mandatory first64 continuity missing");break;}
    }
};

// Opt-in, one-shot observer of this host's own main thread. No dump is made.
// All DbgHelp/heap/IO work occurs AFTER ResumeThread, using a bounded stack copy.
class MainThreadStallObserver {
    static constexpr size_t StackBytes=128u*1024u;
    HMODULE library_=nullptr;
    HANDLE process_=GetCurrentProcess(), thread_=nullptr, stop_=nullptr, file_=INVALID_HANDLE_VALUE;
    std::thread worker_;
    std::atomic<uint64_t> progress_{0}, frames_{0};
    std::atomic<bool> sampled_{false}, resumed_{false};
    ULONG_PTR stackLow_=0,stackHigh_=0;
    DWORD64 capturedBase_=0;size_t capturedSize_=0;
    std::unique_ptr<std::array<unsigned char,StackBytes>> stack_;
    bool symbols_=false;
    size_t logBytes_=0;
    decltype(&SymInitializeW) initialize_=nullptr;
    decltype(&SymSetOptions) options_=nullptr;
    decltype(&SymCleanup) cleanup_=nullptr;
    decltype(&StackWalk64) walk_=nullptr;
    decltype(&SymFromAddr) symbol_=nullptr;
    decltype(&SymFunctionTableAccess64) table_=nullptr;
    decltype(&SymGetModuleBase64) module_=nullptr;
    inline static thread_local MainThreadStallObserver* walking_=nullptr;
    template<class T> T api(const char* name){
        auto fn=reinterpret_cast<T>(GetProcAddress(library_,name));require(fn!=nullptr,std::string("Missing DbgHelp API: ")+name);return fn;
    }
    void log(const char* format,...) noexcept {
        if(file_==INVALID_HANDLE_VALUE || logBytes_>=128u*1024u) return;
        char buffer[4096];va_list args;va_start(args,format);const int count=vsnprintf(buffer,sizeof(buffer),format,args);va_end(args);
        if(count<=0)return;DWORD written=0;
        const auto length=std::min<size_t>(std::min<int>(count,sizeof(buffer)-1),128u*1024u-logBytes_);
        WriteFile(file_,buffer,static_cast<DWORD>(length),&written,nullptr);logBytes_+=written;
        FlushFileBuffers(file_);
    }
    static BOOL CALLBACK read(HANDLE process,DWORD64 address,PVOID output,DWORD size,LPDWORD received){
        auto* self=walking_;*received=0;if(!self || size>65536)return FALSE;
        if(address>=self->capturedBase_ && address-self->capturedBase_<=self->capturedSize_
            && size<=self->capturedSize_-(address-self->capturedBase_)){
            std::memcpy(output,self->stack_->data()+static_cast<size_t>(address-self->capturedBase_),size);*received=size;return TRUE;
        }
        // Never mix resumed live-stack/heap contents into the frozen stack.
        // Only PE image unwind/code metadata may be read after resumption.
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<LPCVOID>(address),&info,sizeof(info)) || info.Type!=MEM_IMAGE
            || (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return FALSE;
        const auto base=reinterpret_cast<uintptr_t>(info.BaseAddress);
        if(address<base || address-base>info.RegionSize || size>info.RegionSize-(address-base))return FALSE;
        SIZE_T copied=0;const BOOL ok=ReadProcessMemory(process,reinterpret_cast<LPCVOID>(address),output,size,&copied);
        *received=static_cast<DWORD>(copied);return ok;
    }
    static PVOID CALLBACK table(HANDLE process,DWORD64 address){return walking_->table_(process,address);}
    static DWORD64 CALLBACK module(HANDLE process,DWORD64 address){return walking_->module_(process,address);}
    void sample() noexcept {
#if !defined(_M_X64)
        log("SAMPLE_END status=UNSUPPORTED_ARCHITECTURE\n");
#else
        sampled_=true;
        CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
        log("SAMPLE_BEGIN pid=%lu observed_thread=main frames=%llu no_progress_ms=%llu\n",GetCurrentProcessId(),
            static_cast<unsigned long long>(frames_.load()),static_cast<unsigned long long>(GetTickCount64()-progress_.load()));
        const auto before=GetTickCount64();
        const DWORD previous=SuspendThread(thread_);
        const DWORD suspendError=previous==DWORD(-1)?GetLastError():0;
        BOOL got=FALSE,copied=FALSE;DWORD contextError=0,readError=0,resumeResult=DWORD(-1);SIZE_T size=0;
        if(previous!=DWORD(-1)){
            struct ResumeOnce {
                HANDLE thread;bool armed=true;
                DWORD resume() noexcept {armed=false;return ResumeThread(thread);}
                ~ResumeOnce(){if(armed) ResumeThread(thread);}
            } resume{thread_};
            // No allocation, logging or DbgHelp while the main thread is held.
            got=GetThreadContext(thread_,&context);contextError=got?0:GetLastError();
            if(got && context.Rsp>=stackLow_ && context.Rsp<stackHigh_){
                capturedBase_=context.Rsp;
                const SIZE_T requested=std::min<SIZE_T>(StackBytes,stackHigh_-context.Rsp);
                copied=ReadProcessMemory(process_,reinterpret_cast<LPCVOID>(context.Rsp),stack_->data(),requested,&size);
                readError=copied?0:GetLastError();capturedSize_=std::min<size_t>(size,StackBytes);
            }
            resumeResult=resume.resume(); // Exactly undo this observer's one suspension.
            resumed_=resumeResult!=DWORD(-1);
        }
        log("CONTEXT suspend_previous=%lu suspend_error=%lu got=%d context_error=%lu read_ok=%d read_error=%lu bytes=%llu resume_result=%lu resumed=%d held_ms=%llu RIP=0x%llx RSP=0x%llx\n",
            previous,suspendError,int(got),contextError,int(copied),readError,static_cast<unsigned long long>(capturedSize_),resumeResult,int(resumed_.load()),
            static_cast<unsigned long long>(GetTickCount64()-before),static_cast<unsigned long long>(context.Rip),static_cast<unsigned long long>(context.Rsp));
        if(!got || !resumed_.load() || capturedSize_==0){log("SAMPLE_END status=CONTEXT_OR_RESUME_UNAVAILABLE\n");return;}
        try{
            walking_=this;
            STACKFRAME64 frame{};frame.AddrPC.Offset=context.Rip;frame.AddrFrame.Offset=context.Rbp;frame.AddrStack.Offset=context.Rsp;
            frame.AddrPC.Mode=frame.AddrFrame.Mode=frame.AddrStack.Mode=AddrModeFlat;
            const auto deadline=GetTickCount64()+2000;DWORD64 previousPC=0,previousSP=0;
            unsigned count=0;bool loop=false;
            for(;count<64 && GetTickCount64()<deadline;++count){
                if(!walk_(IMAGE_FILE_MACHINE_AMD64,process_,thread_,&frame,&context,read,table,module,nullptr))break;
                if(!frame.AddrPC.Offset)break;
                if(frame.AddrPC.Offset==previousPC && frame.AddrStack.Offset==previousSP){loop=true;break;}
                previousPC=frame.AddrPC.Offset;previousSP=frame.AddrStack.Offset;
                alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO)+512]{};
                auto* symbol=reinterpret_cast<SYMBOL_INFO*>(storage);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=511;
                DWORD64 displacement=0;const BOOL named=symbol_(process_,frame.AddrPC.Offset,&displacement,symbol);
                log("FRAME index=%u pc=0x%llx sp=0x%llx module_base=0x%llx name=%.*s displacement=0x%llx symbol=%d\n",count,
                    static_cast<unsigned long long>(frame.AddrPC.Offset),static_cast<unsigned long long>(frame.AddrStack.Offset),
                    static_cast<unsigned long long>(module_(process_,frame.AddrPC.Offset)),named?int(std::min<ULONG>(symbol->NameLen,511)):0,
                    named?symbol->Name:"",static_cast<unsigned long long>(displacement),int(named));
            }
            log("SAMPLE_END status=COLLECTED frames=%u loop=%d deadline=%d complete_stack_not_assumed=true\n",count,int(loop),int(GetTickCount64()>=deadline));
        }catch(...){log("SAMPLE_END status=SYMBOLICATION_EXCEPTION\n");}
        walking_=nullptr;
#endif
    }
public:
    ~MainThreadStallObserver(){
        stop();if(symbols_ && cleanup_)cleanup_(process_);
        if(thread_)CloseHandle(thread_);if(stop_)CloseHandle(stop_);if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
        if(library_)FreeLibrary(library_);
    }
    void start(const fs::path& output){
#if !defined(_M_X64)
        throw std::runtime_error("This bounded stack observer currently requires x64");
#else
        require(!worker_.joinable(),"Stack observer already started");
        plainAncestors(output);
        file_=CreateFileW((output/L"native-stack-once.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(file_!=INVALID_HANDLE_VALUE,"Cannot create stack observer log");
        using Limits=void(WINAPI*)(PULONG_PTR,PULONG_PTR);
        auto limits=reinterpret_cast<Limits>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetCurrentThreadStackLimits"));
        require(limits!=nullptr,"Current thread stack limits unavailable");limits(&stackLow_,&stackHigh_);
        require(stackHigh_>stackLow_,"Invalid main-thread stack range");
        require(DuplicateHandle(process_,GetCurrentThread(),process_,&thread_,THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,0)!=0,"Cannot own main-thread observation handle");
        stack_=std::make_unique<std::array<unsigned char,StackBytes>>();
        library_=LoadLibraryExW(L"dbghelp.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(library_!=nullptr,"System DbgHelp unavailable");
        initialize_=api<decltype(initialize_)>("SymInitializeW");options_=api<decltype(options_)>("SymSetOptions");cleanup_=api<decltype(cleanup_)>("SymCleanup");
        walk_=api<decltype(walk_)>("StackWalk64");symbol_=api<decltype(symbol_)>("SymFromAddr");table_=api<decltype(table_)>("SymFunctionTableAccess64");module_=api<decltype(module_)>("SymGetModuleBase64");
        wchar_t modulePath[32768]{};const DWORD length=GetModuleFileNameW(nullptr,modulePath,32768);
        require(length>0 && length<32768,"Current EXE path unavailable");
        const auto directory=fs::path(modulePath).parent_path();plainAncestors(directory);
        require(directory.native().find(L';')==std::wstring::npos,"Ambiguous local symbol search path");
        DWORD flags=SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_EXACT_SYMBOLS|SYMOPT_FAIL_CRITICAL_ERRORS|SYMOPT_NO_PROMPTS|SYMOPT_IGNORE_NT_SYMPATH;
#ifdef SYMOPT_DISABLE_SYMSRV_AUTODETECT
        flags|=SYMOPT_DISABLE_SYMSRV_AUTODETECT;
#endif
        options_(flags);require(initialize_(process_,directory.c_str(),TRUE)!=0,"Local symbol initialization failed");symbols_=true;
        stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(stop_!=nullptr,"Cannot create stack observer stop event");
        log("OBSERVER_READY pid=%lu main_thread=%lu delay_ms=10000 max_samples=1 stack_window=131072 max_frames=64 local_symbols=%s raw_stack_dump=false\n",GetCurrentProcessId(),GetCurrentThreadId(),utf8(directory).c_str());
        progress_=GetTickCount64();
        worker_=std::thread([this]{while(WaitForSingleObject(stop_,250)==WAIT_TIMEOUT){if(GetTickCount64()-progress_.load()>=10000){sample();return;}}});
#endif
    }
    void frame() noexcept {++frames_;progress_=GetTickCount64();}
    void stop() noexcept {if(stop_)SetEvent(stop_);if(worker_.joinable())worker_.join();}
    json receipt() const{return {{"sampled",sampled_.load()},{"resume_confirmed",resumed_.load()},{"observed_post_frames",frames_.load()},
        {"one_shot",true},{"raw_memory_dump",false},{"unwind_complete_not_assumed",true}};}
};

class HiddenPlatform final : public IPlatformBackend {
public:
    std::function<void()> afterFrame;
    void postFrame() override { if (afterFrame) afterFrame(); }
    ~HiddenPlatform() override { shutdown(); }
    bool init(const char* title, int width, int height) override {
        if (window_) return true;
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return false;
        initialized_ = true;
        const SDL_PropertiesID props = SDL_CreateProperties();
        if (!props) return false;
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
        window_ = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        width_ = width; height_ = height;
        return window_ != nullptr;
    }
    void shutdown() override {
        if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
        if (initialized_) { SDL_Quit(); initialized_ = false; }
    }
    bool pollEvent() override { SDL_Event event{}; return SDL_PollEvent(&event); }
    MouseState getMouseState() const override {
        MouseState value;
        const auto buttons = SDL_GetMouseState(&value.x, &value.y);
        value.leftDown = (buttons & SDL_BUTTON_LMASK) != 0;
        return value;
    }
    uint64_t getTicksMs() const override { return SDL_GetTicks(); }
    void* getNativeWindowHandle() const override {
        return window_ ? SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) : nullptr;
    }
    int getWindowWidth() const override { return width_; }
    int getWindowHeight() const override { return height_; }
    void setFullscreen(bool enabled) override {
        if (window_) SDL_SetWindowFullscreen(window_, enabled);
    }
    void resizeWindow(int width, int height) override {
        if (window_ && SDL_SetWindowSize(window_, width, height)) {
            width_ = width; height_ = height;
        }
    }
    bool hidden() const { return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_HIDDEN) != 0; }
    const char* getBackendName() const override { return "SDL3 hidden probe"; }
    bool startTextInput() override { return window_ && SDL_StartTextInput(window_); }
    bool stopTextInput() override { return window_ && SDL_StopTextInput(window_); }
    bool setTextInputRect(int x, int y, int w, int h, int cursor) override {
        SDL_Rect rect{x, y, w, h};
        return window_ && SDL_SetTextInputArea(window_, &rect, cursor);
    }
    bool isTextInputActive() const override { return window_ && SDL_TextInputActive(window_); }
private:
    SDL_Window* window_ = nullptr;
    bool initialized_ = false;
    int width_ = kWidth, height_ = kHeight;
};

} // namespace
int main(int argc, char** argv) {
    using namespace Caesura;
    fs::path output;
    json report = {{"schema", 1}, {"status", "NOT_RUN"}, {"acceptance_scope", "selected script only"},
        {"physical_audibility", false}, {"parent_cleanup_required", true}, {"fixed_step_ms", 16},
        {"argv", json::array()}, {"pid", GetCurrentProcessId()}};
    for (int i = 0; i < argc; ++i) report["argv"].push_back(argv[i]);
    int exitCode = 1;
    try {
        std::string backend, entry, source;
        fs::path officialRoot;
        unsigned frames = 0;
        bool capture = false, official = false, diagnoseStack = false, smaCpuDiagnostic = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--capture-frames") { require(!capture, "Duplicate capture option"); capture = true; continue; }
            if (key == "--official-demo") { require(!official, "Duplicate official option"); official = true; continue; }
            if (key == "--diagnose-stall-stack") {require(!diagnoseStack,"Duplicate stack diagnostic option");diagnoseStack=true;continue;}
            require(i + 1 < argc, "Missing option value"); const std::string value = argv[++i];
            if (key == "--backend") { require(backend.empty(), "Duplicate backend"); backend = value; }
            else if (key == "--entry") { require(entry.empty(), "Duplicate entry"); entry = value; }
            else if (key == "--output") { require(output.empty(), "Duplicate output"); output = fs::u8path(value); }
            else if (key == "--source-sha") { require(source.empty(), "Duplicate source"); source = value; }
            else if (key == "--diagnostic-sma-skin-mode") {
                require(!smaCpuDiagnostic && value=="cpu", "Diagnostic SMA mode must explicitly be cpu and unique");
                smaCpuDiagnostic=true;
            }
            else if (key == "--official-output-root") { require(officialRoot.empty(), "Duplicate official output root"); officialRoot=fs::u8path(value); }
            else if (key == "--frames") {
                require(frames == 0, "Duplicate frames");
                const auto parsed = std::from_chars(value.data(), value.data()+value.size(), frames);
                require(parsed.ec == std::errc{} && parsed.ptr == value.data()+value.size() && frames >= 1 && frames <= 6000,
                        "Frames outside the absolute 1..6000 bound");
            } else throw std::runtime_error("Unknown option: " + key);
        }
        require(backend == "dx11" || backend == "opengl", "Explicit dx11/opengl backend required");
        require(frames && !entry.empty() && output.is_absolute(), "Entry, frames and absolute output required");
        require(frames <= (official ? 6000u : 1800u), "Ordinary command corpus remains limited to 1800 frames");
        require(official ? (capture && officialRoot.is_absolute()) : officialRoot.empty(),
                "Official mode requires capture and an absolute aggregate output root");
        require(source.size() == 40 && source.find_first_not_of("0123456789abcdef") == std::string::npos,
                "Declared source must be lowercase Git SHA; parent verifies its bytes");
        const auto cwd = fs::current_path(); plainAncestors(cwd);
        const auto relativeEntry = fs::u8path(entry);
        require(relativeEntry.is_relative() && !relativeEntry.has_root_name(), "Entry must be relative to runtime CWD");
        for (const auto& item : relativeEntry) require(item != ".." && item != ".", "Dot entry component refused");
        for (const auto& path : {cwd / "scripts/config.lua", cwd / "scripts/kag/init.lua", cwd / relativeEntry}) {
            plainAncestors(path); require(fs::is_regular_file(path), "Startup input must be regular file");
        }
        output = fs::absolute(output).lexically_normal();
        plainAncestors(output.parent_path());
        require(!fs::exists(output), "Output must be fresh; never overwrite an earlier attempt");
        require(fs::create_directory(output), "Cannot create output"); plainAncestors(output);
        report["cwd"] = utf8(cwd); report["entry"] = entry; report["declared_source_sha"] = source;
        report["source_verified_by_host"] = false; report["requested_backend"] = backend;
        report["frame_limit"] = frames; report["capture_every_rendered_frame"] = capture && !official;
        report["capture_policy"] = official ? "first64-continuous+actual-trans+16-tail+complete-pages" : "ordinary-full-export";
        report["official_demo"] = official; report["stack_diagnosis_enabled"]=diagnoseStack;
        OfficialPngBudget pngBudget;
        if (official) {
            pngBudget.admit(fs::canonical(officialRoot), output);
            report["png_budget_root"] = utf8(fs::canonical(officialRoot));
            report["png_prior_bytes"] = pngBudget.priorBytes;
            report["png_case_limit"] = pngBudget.limit;
            report["png_total_limit"] = OfficialPngBudget::Total;
            report["png_check_before_write"] = true;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0, "Cannot read process creation identity");
        report["process_creation_filetime"] = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        EngineConfig config;
        config.width = kWidth; config.height = kHeight; config.title = "Caesura command contract host";
        config.headless = false; config.editorMode = false; config.enableDebugger = false;
        config.renderBackend = backend.c_str(); config.frameLimit = frames; config.fixedStepMs = 16;
        auto* platform = new HiddenPlatform(); config.platform = platform;
        config.render = new BgfxRenderDevice();
        config.audio = new SoLoudAudioEngine(SoLoudAudioEngine::OutputMode::Software);
        std::string replayPath;
        if (capture) require(fs::create_directory(output / "png"), "Cannot create PNG directory");
        if (capture && !official) {
            replayPath = utf8(output / "empty-replay.json");
            std::ofstream stream(output / "empty-replay.json", std::ios::binary); stream << "[]\n"; stream.close();
            require(bool(stream), "Cannot persist empty replay");
            config.exportReplayFile = replayPath; config.exportDir = utf8(output / "png");
        }
        size_t scriptErrors = 0;
        OfficialCapture officialCapture; // Outlives Engine/Lua and any closing callback.
        MainThreadStallObserver stackObserver;
        Engine engine(std::move(config));
        struct StopObserverBeforeEngine {
            MainThreadStallObserver& observer;
            ~StopObserverBeforeEngine(){observer.stop();}
        } stopObserverBeforeEngine{stackObserver};
        require(engine.init(), "Engine initialization failed");
        auto* diagnosticMesh=BackendRegistry::instance().getMeshRenderer();
        report["sma_cpu_diagnostic_override"]=smaCpuDiagnostic;
        if(smaCpuDiagnostic) {
            require(diagnosticMesh!=nullptr,"Missing real SMA renderer");
            report["sma_mode_before_override"]=static_cast<int>(diagnosticMesh->skinMode());
            diagnosticMesh->setSkinMode(SkinMode::Cpu);
            require(diagnosticMesh->skinMode()==SkinMode::Cpu,"Explicit SMA CPU mode was not applied");
            report["sma_skinning_scope"]="EXPLICIT_CPU_DIAGNOSTIC_NOT_GPU_ACCEPTANCE";
        }
        // Keep the real error handler, but answer its owned SDL error UI with
        // Quit so a hidden failing scene never waits for human keyboard input.
        // Such a scene is always rejected, regardless of its exit request.
        const auto originalReporter=BackendRegistry::instance().getErrorReporter();
        BackendRegistry::instance().setErrorReporter(
            [originalReporter,&scriptErrors](const std::string& command,const std::string& error,
                                            const std::string& scene,int line) {
                ++scriptErrors;
                SDL_Event quit{};quit.type=SDL_EVENT_QUIT;SDL_PushEvent(&quit);
                if(originalReporter) originalReporter(command,error,scene,line);
            });
        require(platform->hidden(), "SDL window is not hidden"); report["hidden_window"] = true;
        const auto renderer = engine.renderDevice().getSnapshot();
        report["actual_backend"] = renderer.backendName;
        require(renderer.backendName == (backend == "dx11" ? "Direct3D 11" : "OpenGL"), "Actual backend differs");
        require(engine.renderDevice().getRuntimeInfo().shaderReady, "Real shaders unavailable");
        require(engine.audio().getSnapshot().outputMode == AudioOutputMode::Software, "Software audio required");
        auto* L = engine.lua().state(); require(L != nullptr, "Missing VM");
        lua_newtable(L); lua_setglobal(L, "_CAESURA_COMMAND_CONTRACT_RESULT");
        configureStartupLuaPath(L, "scripts/");
        require(engine.lua().loadScript("scripts/config.lua"), "Config load failed");
        applyDevModeToTextureManager(L);
        require(engine.lua().loadScript("scripts/kag/init.lua"), "KAG init failed");
        engine.lua().resetInstructionBudget();
        require(engine.lua().loadScript(entry.c_str()), "Entry load failed");
        rawGlobal(L, "config"); require(lua_istable(L, -1), "Missing config table");
        rawField(L, "dev_mode"); const bool devMode = lua_toboolean(L, -1) != 0; lua_pop(L, 2);
        lua_newtable(L); lua_pushboolean(L, devMode); lua_setfield(L, -2, "dev_mode"); lua_setglobal(L, "_CAESURA_CONFIG");
        if (capture && !official) replay(L, replayPath);
        if (official) {officialCapture.engine=&engine;officialCapture.budget=&pngBudget;officialCapture.install(L);}
        engine.lua().lockdownScriptEnv();
        if(smaCpuDiagnostic)require(diagnosticMesh==BackendRegistry::instance().getMeshRenderer()
            && diagnosticMesh->skinMode()==SkinMode::Cpu,"Entry changed the diagnostic SMA owner/mode");
        if (diagnoseStack) stackObserver.start(output);
        if (official || diagnoseStack) platform->afterFrame = [&] {
            if (official) officialCapture.afterFrame();
            if (diagnoseStack) stackObserver.frame();
        };
        engine.run();
        if(smaCpuDiagnostic) {
            require(diagnosticMesh==BackendRegistry::instance().getMeshRenderer()
                && diagnosticMesh->skinMode()==SkinMode::Cpu,"Run changed the diagnostic SMA owner/mode");
            report["sma_mode_after_run"]=static_cast<int>(diagnosticMesh->skinMode());
        }
        platform->afterFrame = {};
        stackObserver.stop();
        if(diagnoseStack) report["stack_diagnosis"]=stackObserver.receipt();
        if (official) {
            officialCapture.finish();
            report["capture_requests"]=officialCapture.requests;report["captures"]=officialCapture.completed;
            report["render_callbacks"]=officialCapture.renderCallbacks;
            report["capture_missing_count"]=officialCapture.requests.size()-officialCapture.completed.size();
            report["capture_no_frame_observed"]=officialCapture.completed.empty();
            report["png_bytes"] = pngBudget.bytes; report["png_files"] = pngBudget.files;
            report["png_budget_incomplete"] = pngBudget.incomplete; report["png_budget_reason"] = pngBudget.reason;
        }
        const auto completed = engine.getHostSnapshot().completedOwnerFrames;
        rawGlobal(L, "_CAESURA_QUIT"); const bool quit = exactBoolean(L, -1); lua_pop(L, 1);
        rawGlobal(L, "_CAESURA_COMMAND_CONTRACT_RESULT"); require(lua_istable(L, -1), "Missing result publication");
        rawField(L, "complete"); const bool complete = exactBoolean(L, -1); lua_pop(L, 1);
        rawField(L, "status"); const auto status = exactString(L, -1); lua_pop(L, 1);
        rawField(L, "reason"); report["script_reason"] = exactString(L, -1); lua_pop(L, 1);
        rawField(L, "case_id"); report["case_id"] = exactString(L, -1); lua_pop(L, 2);
        report["completed_owner_frames"] = completed; report["native_quit_observed"] = quit;
        report["script_complete"] = complete; report["script_status"] = status;
        report["frame_limit_reached"] = completed >= frames; report["render_failed"] = engine.hasRenderFailure();
        report["script_error_count"]=scriptErrors;
        report["error_ui_quit_injected"]=scriptErrors!=0;
        const bool accepted = quit && complete && status == "PASS" && completed < frames
            && !engine.hasRenderFailure() && scriptErrors==0 && !pngBudget.incomplete
            && (!official || !officialCapture.completed.empty());
        engine.shutdown(); report["engine_shutdown_returned"] = true;
        report["status"] = pngBudget.incomplete ? "INCOMPLETE" :
            (accepted ? "SCRIPT_COMPLETED_PARENT_ACCEPTANCE_REQUIRED" : "SCRIPT_REJECTED");
        exitCode = accepted ? 0 : 2;
    } catch (const PngBudgetLimit& error) {
        report["status"]="INCOMPLETE"; report["png_budget_reason"]=error.what(); exitCode=2;
    } catch (const std::exception& error) { report["status"] = "HOST_FAILED"; report["error"] = error.what(); }
    report["actual_exit"] = exitCode;
    // A fresh directory is created by this attempt only. Never publish into an
    // existing/rejected output root even on argument or preflight failures.
    if (report.contains("cwd")) {
        try {
            plainAncestors(output);
            require(!fs::exists(output / "host-report.json"), "Report already exists");
            std::ofstream stream(output / "host-report.json", std::ios::binary); stream << report.dump(2) << '\n'; stream.close();
            require(bool(stream), "Report write failed");
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; exitCode = 1; }
    }
    std::cout << "COMMAND_CONTRACT_HOST_JSON:" << report.dump() << '\n';
    return exitCode;
}
