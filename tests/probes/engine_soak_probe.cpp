// Opt-in U27 real-backend workload composition root. Completion here is only
// probe evidence; the external controller accepts duration, workload and budgets.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "entry/Engine.h"
#include "entry/EngineConfig.h"
#include "entry/RuntimeStats.h"
#include "audio/SoLoudAudioEngine.h"
#include "di/BackendRegistry.h"
#include "platform/api/IPlatformBackend.h"
#include "render/BgfxRenderDevice.h"
#include "render/api/ITextureManager.h"
#include "resource/api/IImageDecoder.h"
#include "script/api/ILuaManager.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <windows.h>
#include <psapi.h>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace {
using namespace Caesura;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr int kWidth=640,kHeight=360;
void require(bool ok,const std::string& message) { if(!ok) throw std::runtime_error(message); }
std::string utf8(const fs::path& path) { const auto value=path.u8string(); return {reinterpret_cast<const char*>(value.data()),value.size()}; }
void write(const fs::path& p,const json& j) { std::ofstream f(p,std::ios::binary); f<<j.dump(2)<<'\n'; f.close(); require(bool(f),"Cannot persist report"); }
std::string module(const wchar_t* name) {
 const auto handle=GetModuleHandleW(name); require(handle!=nullptr,"Required runtime module missing");
 std::array<wchar_t,32768> b{}; const auto n=GetModuleFileNameW(handle,b.data(),DWORD(b.size()));
 require(n>0&&n<b.size(),"Cannot observe runtime module");return utf8(fs::canonical(b.data()));
}
class HiddenPlatform final : public IPlatformBackend {
public:
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

json observe(Engine& engine) {
 const auto s=captureRuntimeStats(engine);json j;
 j["jobs"]["supported"]=s.jobs.supported;
 j["jobs"]["running"]=s.jobs.running;
 j["jobs"]["workerPending"]=s.jobs.workerPending;
 j["jobs"]["queuedCompletions"]=s.jobs.queuedCompletions;
 j["jobs"]["dispatchingCompletions"]=s.jobs.dispatchingCompletions;
 j["async"]["supported"]=s.asyncLoader.supported;
 j["async"]["running"]=s.asyncLoader.running;
 j["async"]["pendingWaiters"]=s.asyncLoader.pendingWaiters;
 j["async"]["inflightKeys"]=s.asyncLoader.inflightKeys;
 j["async"]["completedBuffered"]=s.asyncLoader.completedBuffered;
 j["async"]["cacheEntries"]=s.asyncLoader.cacheEntries;
 j["async"]["cacheBytes"]=s.asyncLoader.cacheBytes;
 j["host"]["supported"]=s.host.supported;
 j["host"]["initialized"]=s.host.initialized;
 j["host"]["running"]=s.host.running;
 j["host"]["luaPaused"]=s.host.luaPaused;
 j["host"]["asyncOwnershipComplete"]=s.host.asyncOwnershipComplete;
 j["host"]["audioCompletionTrackingSupported"]=s.host.audioCompletionTrackingSupported;
 j["host"]["completedOwnerFrames"]=s.host.completedOwnerFrames;
 j["host"]["deferredAsyncPayloads"]=s.host.deferredAsyncPayloads;
 j["host"]["drainingAsyncPayloads"]=s.host.drainingAsyncPayloads;
 j["host"]["dispatchingAsyncPayloads"]=s.host.dispatchingAsyncPayloads;
 j["host"]["audioCompletionsPending"]=s.host.audioCompletionsPending;
 j["host"]["audioCompletionsActive"]=s.host.audioCompletionsActive;
 j["host"]["audioCompletionOwnerRefs"]=s.host.audioCompletionOwnerRefs;
 j["audio"]["supported"]=s.audio.supported;
 j["audio"]["running"]=s.audio.running;
 j["audio"]["liveVoices"]=s.audio.liveVoices;
 j["audio"]["busVoices"]=s.audio.busVoices;
 j["audio"]["sessionHandles"]=s.audio.sessionHandles;
 j["audio"]["retiringBGM"]=s.audio.retiringBGM;
 j["audio"]["retiringVoice"]=s.audio.retiringVoice;
 j["audio"]["waveCacheEntries"]=s.audio.waveCacheEntries;
 j["audio"]["rawCacheEntries"]=s.audio.rawCacheEntries;
 j["audio"]["voiceCompletionsPending"]=s.audio.voiceCompletionsPending;
 j["audio"]["restoredSources"]=s.audio.restoredSources;
 j["render"]["supported"]=s.render.supported;
 j["render"]["contextInitialized"]=s.render.contextInitialized;
 j["render"]["renderingAvailable"]=s.render.renderingAvailable;
 j["render"]["resourceCountsAvailable"]=s.render.resourceCountsAvailable;
 j["render"]["backendName"]=s.render.backendName;
 j["render"]["contextGeneration"]=s.render.contextGeneration;
 j["render"]["captureSubmissionFrame"]=s.render.captureSubmissionFrame;
 j["render"]["screenshotOwnershipComplete"]=s.render.screenshotOwnershipComplete;
 j["render"]["screenshotReadbackTrackingSupported"]=s.render.screenshotReadbackTrackingSupported;
 j["render"]["screenshotReadbacksOutstanding"]=s.render.screenshotReadbacksOutstanding;
 j["render"]["resources"]["dynamicIndexBuffers"]=s.render.resources.dynamicIndexBuffers;
 j["render"]["resources"]["dynamicVertexBuffers"]=s.render.resources.dynamicVertexBuffers;
 j["render"]["resources"]["frameBuffers"]=s.render.resources.frameBuffers;
 j["render"]["resources"]["indexBuffers"]=s.render.resources.indexBuffers;
 j["render"]["resources"]["occlusionQueries"]=s.render.resources.occlusionQueries;
 j["render"]["resources"]["programs"]=s.render.resources.programs;
 j["render"]["resources"]["shaders"]=s.render.resources.shaders;
 j["render"]["resources"]["textures"]=s.render.resources.textures;
 j["render"]["resources"]["uniforms"]=s.render.resources.uniforms;
 j["render"]["resources"]["vertexBuffers"]=s.render.resources.vertexBuffers;
 j["render"]["resources"]["vertexLayouts"]=s.render.resources.vertexLayouts;
 j["render"]["screenshots"]["supported"]=s.render.screenshots.supported;
 j["render"]["screenshots"]["waiting"]=s.render.screenshots.waiting;
 j["render"]["screenshots"]["submitted"]=s.render.screenshots.submitted;
 j["render"]["screenshots"]["terminal"]=s.render.screenshots.terminal;
 j["render"]["screenshots"]["reservedBytes"]=s.render.screenshots.reservedBytes;
 j["render"]["screenshots"]["pngBytes"]=s.render.screenshots.pngBytes;

 j["host"]["delivery"]=s.host.delivery==AsyncHostDelivery::DirectDrain?"DirectDrain":"SdlEvents";
 j["audio"]["outputMode"]=s.audio.outputMode==AudioOutputMode::Device?"Device":s.audio.outputMode==AudioOutputMode::ManualMix?"ManualMix":s.audio.outputMode==AudioOutputMode::Software?"Software":"Unknown";
 j["render"]["backendKind"]=s.render.backendKind==RenderBackendKind::GraphicsApi?"GraphicsApi":s.render.backendKind==RenderBackendKind::Noop?"Noop":"Unknown";
 PROCESS_MEMORY_COUNTERS_EX mem{};mem.cb=sizeof(mem);DWORD handles=0;
 require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem),sizeof(mem))!=0,"Memory observation failed");
 require(GetProcessHandleCount(GetCurrentProcess(),&handles)!=0,"Handle observation failed");
 auto* vm=BackendRegistry::instance().getLuaManager();require(vm&&vm->state(),"Missing VM observation");
 auto* L=vm->state();lua_gc(L,LUA_GCCOLLECT,0);
 const uint64_t luaBytes=uint64_t(lua_gc(L,LUA_GCCOUNT,0))*1024+uint64_t(lua_gc(L,LUA_GCCOUNTB,0));
 j["memory"]={{"privateBytes",uint64_t(mem.PrivateUsage)},{"rssBytes",uint64_t(mem.WorkingSetSize)},
  {"osHandles",handles},{"luaBytes",luaBytes},{"textureBytes",BackendRegistry::instance().getTextureManager()->totalTextureBytes()}};
 return j;
}
bool quietDebts(const json& j) {
 if(j["jobs"]["supported"] != true) return false;
 if(j["jobs"]["running"] != true) return false;
 if(j["async"]["supported"] != true) return false;
 if(j["async"]["running"] != true) return false;
 if(j["host"]["supported"] != true) return false;
 if(j["host"]["initialized"] != true) return false;
 if(j["host"]["running"] != true) return false;
 if(j["host"]["luaPaused"] != false) return false;
 if(j["host"]["asyncOwnershipComplete"] != true) return false;
 if(j["host"]["audioCompletionTrackingSupported"] != true) return false;
 if(j["audio"]["supported"] != true) return false;
 if(j["audio"]["running"] != true) return false;
 if(j["render"]["supported"] != true) return false;
 if(j["render"]["contextInitialized"] != true) return false;
 if(j["render"]["renderingAvailable"] != true) return false;
 if(j["render"]["resourceCountsAvailable"] != true) return false;
 if(j["render"]["screenshotOwnershipComplete"] != true) return false;
 if(j["render"]["screenshotReadbackTrackingSupported"] != true) return false;
 if(j["render"]["screenshots"]["supported"] != true) return false;
 if(j["host"]["delivery"] != "DirectDrain") return false;
 if(j["audio"]["outputMode"] != "Device") return false;
 if(j["render"]["backendKind"] != "GraphicsApi") return false;
 if(j["render"]["backendName"] != "Direct3D 11") return false;
 if(j["jobs"]["workerPending"] != 0) return false;
 if(j["jobs"]["queuedCompletions"] != 0) return false;
 if(j["jobs"]["dispatchingCompletions"] != 0) return false;
 if(j["async"]["pendingWaiters"] != 0) return false;
 if(j["async"]["inflightKeys"] != 0) return false;
 if(j["async"]["completedBuffered"] != 0) return false;
 if(j["host"]["deferredAsyncPayloads"] != 0) return false;
 if(j["host"]["drainingAsyncPayloads"] != 0) return false;
 if(j["host"]["dispatchingAsyncPayloads"] != 0) return false;
 if(j["host"]["audioCompletionsPending"] != 0) return false;
 if(j["host"]["audioCompletionsActive"] != 0) return false;
 if(j["host"]["audioCompletionOwnerRefs"] != 0) return false;
 if(j["audio"]["liveVoices"] != 0) return false;
 if(j["audio"]["sessionHandles"] != 0) return false;
 if(j["audio"]["retiringBGM"] != 0) return false;
 if(j["audio"]["retiringVoice"] != 0) return false;
 if(j["audio"]["rawCacheEntries"] != 0) return false;
 if(j["audio"]["voiceCompletionsPending"] != 0) return false;
 if(j["audio"]["restoredSources"] != 0) return false;
 if(j["render"]["screenshotReadbacksOutstanding"] != 0) return false;
 if(j["render"]["screenshots"]["waiting"] != 0) return false;
 if(j["render"]["screenshots"]["submitted"] != 0) return false;
 if(j["render"]["screenshots"]["terminal"] != 0) return false;
 if(j["render"]["screenshots"]["reservedBytes"] != 0) return false;
 if(j["render"]["screenshots"]["pngBytes"] != 0) return false;
 return true;
}

class Workload {
public:
 Workload(fs::path output,unsigned cycles,double seconds) : out_(std::move(output)),wanted_(cycles),seconds_(seconds),events_(out_/"events.jsonl",std::ios::binary) {
  require(bool(events_),"Cannot create event stream");
  EngineConfig c;c.width=kWidth;c.height=kHeight;c.title="Caesura bounded runtime workload";
  c.editorMode=true;c.renderBackend="dx11";c.saveEncryptionPolicy=SaveEncryptionPolicy::RequireEncrypted;
  c.platform=new HiddenPlatform;c.render=new BgfxRenderDevice;c.audio=new SoLoudAudioEngine(SoLoudAudioEngine::OutputMode::Device);
  engine_=std::make_unique<Engine>(std::move(c));require(engine_->init(),"Real Engine initialization failed");
  auto& registry=BackendRegistry::instance();vm_=registry.getLuaManager();audio_=registry.getAudioBackend();render_=registry.getRenderDevice();textures_=registry.getTextureManager();
  require(vm_&&audio_&&render_&&textures_,"Missing real backend");
  audio_->setGlobalVolume(0);require(audio_->getSnapshot().outputMode==AudioOutputMode::Device,"Device audio required");
  require(render_->getSnapshot().backendName=="Direct3D 11"&&render_->getRuntimeInfo().shaderReady,"Actual D3D11 with working shaders required");
  registry.getSaveManager()->init("saves");
  lua("Soak=assert(dofile('tests/projects/engine_soak/workload.lua'));assert(KAG.set_encryption_key(string.rep('S',32)))");
  report_={{"status","RUNNING"},{"pid",GetCurrentProcessId()},{"warm_cycles",20},{"requested_cycles",wanted_},
   {"requested_seconds",seconds_},{"runtime_modules",{{"executable",module(nullptr)},{"sdl",module(L"SDL3.dll")},{"d3d11",module(L"d3d11.dll")}}},
   {"physical_audio","NOT_MEASURED"},{"context_restarts",0},{"cold_restart","NOT_RUN"}};
  auto* native=dynamic_cast<SoLoudAudioEngine*>(registry.getAudioBackend());require(native!=nullptr,"Expected actual registered SoLoud owner");
  report_["device_backend"]={{"id",native->soloud().getBackendId()},{"name",native->soloud().getBackendString()},
   {"sample_rate",native->soloud().getBackendSamplerate()},{"reported_buffer_size",native->soloud().getBackendBufferSize()}};
  write(out_/"initialized.json",report_);
  FILETIME a,b,cpu,d;require(GetProcessTimes(GetCurrentProcess(),&a,&b,&cpu,&d)!=0,"Missing process creation time");
  report_["process_created"]=std::to_string((uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime);
  event("initialized",observe(*engine_));
 }
 void run() {
  start_=Clock::now();cycleStart_=start_;
  try {
   engine_->run([this]{ step(); });
   require(done_,"Engine exited before workload completed");
   report_["status"]="PROBE_COMPLETED";
  } catch(const std::exception& e) {
   report_["status"]="FAIL";report_["error"]=e.what();report_["last_observation"]=observe(*engine_);
   report_["lua_counts"]={{"completed",lua("return Soak.completed")},{"natural",lua("return Soak.natural")},
    {"cancelled_callbacks",lua("return Soak.cancelled_callbacks")}};
   auto* native=dynamic_cast<SoLoudAudioEngine*>(BackendRegistry::instance().getAudioBackend());
   if(native)for(const auto handle:voiceHandles_)report_["voice_observations"].push_back({{"handle",handle},
    {"valid",native->soloud().isValidVoiceHandle(handle)},{"paused",native->soloud().getPause(handle)},
    {"stream_time",native->soloud().getStreamTime(handle)}});
  }
  report_["completed_cycles"]=cycle_;report_["process_seconds"]=elapsed(start_);
  report_["measured_seconds"]=measured_?elapsed(measuredStart_):0.0;
  report_["measured_cycles"]=cycle_>20?cycle_-20:0;
  report_["last_phase"]=phase_;report_["completed_owner_frames"]=engine_->getHostSnapshot().completedOwnerFrames;
  engine_->shutdown();report_["shutdown_host"]={{"initialized",engine_->getHostSnapshot().initialized},{"running",engine_->getHostSnapshot().running}};
  write(out_/"result.json",report_);require(report_["status"]=="PROBE_COMPLETED",report_.value("error","Workload incomplete"));
 }
private:
 double elapsed(Clock::time_point t) const { return std::chrono::duration<double>(Clock::now()-t).count(); }
 void event(const char* name,json detail=json::object()) {
  events_<<json({{"event",name},{"cycle",cycle_},{"owner_frame",engine_->getHostSnapshot().completedOwnerFrames},
   {"seconds",start_==Clock::time_point{}?0:elapsed(start_)},{"detail",std::move(detail)}}).dump()<<'\n';events_.flush();require(bool(events_),"Event stream failed");
 }
 lua_Integer lua(const std::string& source) {
  vm_->resetInstructionBudget();auto* L=vm_->state();const int top=lua_gettop(L);const int status=luaL_dostring(L,source.c_str());
  const std::string error=status==LUA_OK?"":(lua_tostring(L,-1)?lua_tostring(L,-1):"non-string Lua error");
  const lua_Integer value=status==LUA_OK&&lua_isinteger(L,-1)?lua_tointeger(L,-1):0;
  lua_settop(L,top);require(status==LUA_OK,"Workload Lua: "+error);return value;
 }
 void phase(int value) { phase_=value;frame_=engine_->getHostSnapshot().completedOwnerFrames; }
 bool advanced(unsigned n=2) const { return engine_->getHostSnapshot().completedOwnerFrames>=frame_+n; }
 void capture(const char* page) {
  auto q=render_->requestScreenshot(ScreenshotOptions{});require(q.status==ScreenshotStatus::Pending&&bool(q.ticket),"Screenshot admission failed");
  ticket_=q.ticket;capturePage_=page;event("capture_admitted",{{"page",page},{"request_id",ticket_.requestId},{"generation",ticket_.generation}});
 }
 bool take() {
  auto result=render_->takeScreenshot(ticket_);if(result.status==ScreenshotStatus::Pending)return false;
  require(result.status==ScreenshotStatus::Completed&&!result.png.empty(),"Screenshot did not complete");
  require(render_->takeScreenshot(ticket_).status==ScreenshotStatus::Unknown,"Screenshot retained after consumption");
  auto* decoder=BackendRegistry::instance().getImageDecoder();require(decoder!=nullptr,"Missing native image decoder");
  auto image=decoder->decode(result.png.data(),result.png.size(),size_t(kWidth)*kHeight*4);
  require(image.ok&&image.width==kWidth&&image.height==kHeight,"Wrong decoded screenshot");
  const std::array<int,3> expected=capturePage_=="b"?std::array<int,3>{130,35,50}:std::array<int,3>{20,50,90};
  const size_t pixel=(10*kWidth+10)*4;
  for(int c=0;c<3;++c)require(std::abs(int(image.rgba[pixel+c])-expected[c])<=2,"Screenshot background pixel differs from requested page");
  const auto path=out_/("cycle-"+std::to_string(cycle_+1)+"-"+capturePage_+".png");
  std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(result.png.data()),std::streamsize(result.png.size()));file.close();require(bool(file),"Cannot retain PNG");
  event("capture_consumed",{{"page",capturePage_},{"file",utf8(path.filename())},{"png_bytes",result.png.size()},{"frame_id",result.frameId},{"request_id",ticket_.requestId},
   {"pixel",{image.rgba[pixel],image.rgba[pixel+1],image.rgba[pixel+2]}}});ticket_={};return true;
 }
 void step() {
  require(elapsed(cycleStart_)<10,"Cycle watchdog exceeded ten seconds");
  switch(phase_) {
  case 0: {
   cycleStart_=Clock::now();lua("Soak.begin("+std::to_string(cycle_+1)+")");
   texture_=textures_->createSolidTexture(43,170,82,255);target_=render_->createRenderTarget(32,32);
   event("transient_resources",{{"texture",texture_},{"texture_valid",textures_->isValid(texture_)},
    {"target",target_.id},{"logical_texture_bytes",textures_->totalTextureBytes()}});
   require(texture_&&textures_->isValid(texture_)&&target_.id,"Transient real resource allocation failed");
   require(textures_->createSolidTexture(43,170,82,255)==texture_,"Live solid color dedup changed");
   event("cycle_begin",{{"texture",texture_},{"target",target_.id}});phase(1);break;
  }
  case 1:if(advanced()){capture("a");phase(2);}break;
  case 2:if(take()){
   lua("Soak.save()");std::ifstream file("saves/save_39.json",std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(file)),{});
   require(bytes.size()>32&&bytes.substr(0,4)=="CAES"&&bytes.find("soak-encrypted-checkpoint")==std::string::npos,"Save is not encrypted CAES");
   event("save",{{"bytes",bytes.size()}});lua("Soak.change()");phase(3);
  }break;
  case 3:if(advanced()){capture("b");phase(4);}break;
  case 4:if(take()){lua("Soak.load()");event("load");phase(5);}break;
  case 5:if(advanced()){capture("restored");phase(6);}break;
  case 6:if(take()){
   const auto admission=lua("return Soak.async_begin()");
   // Production uses a round-robin overlap pool. Explicit stop implements
   // interruption; three playVoice calls alone would create three live lines.
   const auto a=audio_->playVoice("assets/soak/long-a.wav");audio_->stopVoice();
   const auto b=audio_->playVoice("assets/soak/long-b.wav");audio_->stopVoice();
   const auto c=audio_->playVoice("assets/soak/short.wav");
   require(admission>0&&a&&b&&c&&a!=b&&b!=c&&audio_->isVoicePlaying(),"Real async/voice admissions failed");
   voiceHandles_={a,b,c};
   event("activities_admitted",{{"async_id",admission},{"voice_handles",{a,b,c}}});phase(7);
  }break;
  case 7:if(lua("return Soak.completed")==1&&lua("return Soak.natural")==1){
   lua("Soak.cancel_batch();Soak.rollback_begin()");event("activities_finished",{{"completed",1},{"natural",1},{"cancelled_admissions",8}});phase(8);
  }break;
  case 8:if(lua("return Soak.rollback_ready() and 1 or 0")==1){
   lua("Soak.rollback_commit()");event("rollback");lua("Soak.clean()");
   audio_->stopBGM(0);audio_->stopVoice();audio_->stopSE();
   textures_->destroyTexture(texture_);render_->destroyRenderTarget(target_);texture_=0;target_={};
   quietStart_=Clock::now();consecutive_=0;previousQuiet_=json();phase(9);
  }break;
  case 9: {
   auto observation=observe(*engine_);event("settling",observation);
   require(engine_->getHostSnapshot().completedOwnerFrames-frame_<=120&&elapsed(quietStart_)<4,"Quiet boundary did not settle within fixed budget");
   const bool stable=previousQuiet_.is_object()&&observation["render"]["resources"]==previousQuiet_["render"]["resources"]&&observation["memory"]["textureBytes"]==previousQuiet_["memory"]["textureBytes"];
   consecutive_=quietDebts(observation)?(stable?consecutive_+1:1):0;previousQuiet_=observation;
   if(consecutive_>=3){
    require(lua("return Soak.completed")==1&&lua("return Soak.natural")==1&&lua("return Soak.cancelled_callbacks")==0,"Lost or late callback");
    ++cycle_;event("quiet",observation);
    if(cycle_==20){measured_=true;measuredStart_=Clock::now();event("measurement_begin");}
    if(cycle_>=wanted_&&measured_&&elapsed(measuredStart_)>=seconds_){done_=true;engine_->quit();}
    else phase(0);
   }
  }break;
  default:throw std::runtime_error("Unknown workload phase");
  }
  SDL_Delay(1);
 }
 fs::path out_;unsigned wanted_,cycle_=0,consecutive_=0;double seconds_;std::ofstream events_;json report_,previousQuiet_;
 std::unique_ptr<Engine> engine_;ILuaManager* vm_=nullptr;IAudioBackend* audio_=nullptr;IRenderDevice* render_=nullptr;ITextureManager* textures_=nullptr;
 Clock::time_point start_{},cycleStart_{},quietStart_{},measuredStart_{};bool done_=false,measured_=false;int phase_=0;uint64_t frame_=0;
 uint32_t texture_=0;ViewportHandle target_{};ScreenshotTicket ticket_{};std::string capturePage_;
 std::array<unsigned,3> voiceHandles_{};
};
}
int wmain(int argc,wchar_t** argv) {
 SDL_SetMainReady();
 try {
  require(argc==5,"Usage: probe RUNTIME_ROOT OUTPUT_DIR TOTAL_CYCLES MIN_MEASURED_SECONDS");
  const auto root=fs::canonical(argv[1]),output=fs::canonical(argv[2]);
  require(root!=output&&fs::is_directory(root)&&fs::is_directory(output),"Dedicated existing runtime/output directories required");
  const auto cycles=std::stoul(argv[3]);const auto seconds=std::stod(argv[4]);
  require(cycles>=21&&cycles<=100000&&seconds>=0&&seconds<=7200,"Invalid bounded workload request");
  fs::current_path(root);Workload work(output,unsigned(cycles),seconds);work.run();return 0;
 } catch(const std::exception& error){std::cerr<<"U27 workload: "<<error.what()<<'\n';return 1;}
}
