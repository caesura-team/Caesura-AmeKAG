// IRpcDispatcher - owner-thread command boundary for RPC transports.
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace Caesura {

struct RpcStatusRequest {};

struct RpcRunScriptRequest {
    std::string script;
};

struct RpcStopRequest {};

struct RpcEvaluateRequest {
    std::string code;
};

struct RpcGetStateRequest {};

// SMA asset validation for the IDE asset panel (round 19): validates a
// relative asset path through kag.sma_check — the single source of truth
// shared with the runtime loader — and returns the violation list plus a
// structure summary. The engine restricts paths to assets/ and
// demo/assets/ (no .., no absolute paths).
struct RpcSmaValidateRequest {
    std::string path;
};

// IDE preview-frame pick (round 23): hit-test the Lua layer tree at a
// window pixel (1280x720 engine space). Returns JSON hits (bottom-to-top).
struct RpcPickRequest {
    int x = 0;
    int y = 0;
};

// Editor save-back for SMA assets (round 26): validates the JSON text with
// the shared checker, then writes it to the (safe) assets/ path.
struct RpcSmaSaveRequest {
    std::string path;
    std::string content;
};

// Engine runtime stats for the IDE panel (round 28).
struct RpcStatsRequest {};

struct RpcCaptureFrameRequest {
    int width = 1280;
    int height = 720;
};

struct RpcReloadScriptsRequest {};

struct RpcLoadAnimationRequest {
    std::string modelPath;
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
    bool show = true;
};

struct RpcSetBreakpointRequest {
    std::string source;
    int line = 0;
};

struct RpcRemoveBreakpointRequest {
    std::string source;
    int line = 0;
};

struct RpcClearBreakpointsRequest {};

enum class RpcDebugResumeMode {
    Continue,
    StepInto,
    StepOver,
    StepOut,
};

struct RpcDebugResumeRequest {
    std::uint64_t pauseId = 0;
    RpcDebugResumeMode mode = RpcDebugResumeMode::Continue;
};

struct RpcInspectLocalRequest {
    int frame = 0;
    std::string name;
};

struct RpcInspectGlobalRequest {
    std::string name;
};

struct RpcGetDebugStateRequest {};

// KAG scene-level debugger (Neo-Genesis): breakpoints on scene+command
// or scene+line, continue/step, and variable-scope inspection. The Lua
// debugger (DebugProtocol) cannot see KAG tokens; these operations drive
// the kag_debug.lua API through the Lua state.
struct RpcKagDebugRequest {
    std::string action;   // setBreakpoint | clearBreakpoints | continue | step | inspect
    std::string scene;    // setBreakpoint / clearBreakpoints (exact scheduler scene path)
    std::string cmd;      // setBreakpoint: command name (mutually exclusive with line)
    int line = 0;         // setBreakpoint: 1-based token line
    std::string scope;    // inspect: f | sf | tf | mp | lf | all (default all)
};

using RpcRequestPayload = std::variant<
    RpcStatusRequest,
    RpcRunScriptRequest,
    RpcStopRequest,
    RpcEvaluateRequest,
    RpcGetStateRequest,
    RpcSmaValidateRequest,
    RpcPickRequest,
    RpcSmaSaveRequest,
    RpcStatsRequest,
    RpcCaptureFrameRequest,
    RpcReloadScriptsRequest,
    RpcLoadAnimationRequest,
    RpcSetBreakpointRequest,
    RpcRemoveBreakpointRequest,
    RpcClearBreakpointsRequest,
    RpcDebugResumeRequest,
    RpcInspectLocalRequest,
    RpcInspectGlobalRequest,
    RpcGetDebugStateRequest,
    RpcKagDebugRequest>;

struct RpcRequest {
    RpcRequestPayload payload;
};

enum class RpcReplyStatus {
    Ok,
    InvalidRequest,
    Unavailable,
    Busy, // engine main thread did not service the request in time
    Failed,
};

struct RpcStatusResult {
    bool luaReady = false;
};

struct RpcEvaluateResult {
    std::string value;
};

// KAG scene debugger result: inspect returns a JSON object describing the
// requested variable scope(s); other actions return "ok".
struct RpcKagDebugResult {
    std::string value;
};

// Engine runtime state for the IDE preview panel (round 18): current
// scene + token position, NVL mode, UI language, backlog size and layer
// count. Empty/zero when no game is running.
// currentCmd (round 28): human-readable current execution element, e.g.
// "[ch]", "*start" or "text" — empty when no game is running.
struct RpcStateResult {
    std::string scene;
    int tokenIndex = 0;
    bool nvlMode = false;
    std::string language;
    int backlogCount = 0;
    int layerCount = 0;
    std::string currentCmd;
};

// SMA asset validation result: ok flag, per-field violation strings, and
// a JSON structure summary (bones/anims/parts/verts/tris) for the panel.
struct RpcSmaValidateResult {
    bool ok = false;
    std::vector<std::string> errors;
    std::string meta;   // JSON object text
};

struct RpcPickResult {
    std::string hits;   // JSON array text: [{id,name,z,depth,opacity,x,y,w,h}]
};

struct RpcSmaSaveResult {
    bool ok = false;
    std::vector<std::string> errors;
};

// Copied owner-thread observations. Unsupported zeroes are not measurements;
// phases can overlap and must not be summed into a global idle/resource total.
// RPC owns these value types; transports never retain backend pointers.
enum class RpcAsyncDelivery { Unknown, DirectDrain, SdlEvents };
enum class RpcAudioOutput { Unknown, Device, ManualMix, Software };

struct RpcJobStats {
    bool supported = false;
    bool running = false;
    std::uint64_t workerPending = 0;
    std::uint64_t queuedCompletions = 0;
    std::uint64_t dispatchingCompletions = 0;
};

struct RpcAsyncLoaderStats {
    bool supported = false;
    bool running = false;
    std::uint64_t pendingWaiters = 0;
    std::uint64_t inflightKeys = 0;
    std::uint64_t completedBuffered = 0;
    std::uint64_t cacheEntries = 0;
    std::uint64_t cacheBytes = 0;
};

struct RpcHostStats {
    bool supported = false;
    bool initialized = false;
    bool running = false;
    bool luaPaused = false;
    RpcAsyncDelivery delivery = RpcAsyncDelivery::Unknown;
    // Only the standard host direct-drain route can be complete; SDL events
    // and externally taken payloads cannot be inferred from these counters.
    bool asyncOwnershipComplete = false;
    std::uint64_t completedOwnerFrames = 0;
    std::uint64_t deferredAsyncPayloads = 0;
    std::uint64_t drainingAsyncPayloads = 0;
    std::uint64_t dispatchingAsyncPayloads = 0;
    // Host audio debt is independently supported from the backend observation.
    bool audioCompletionTrackingSupported = false;
    std::uint64_t audioCompletionsPending = 0;
    std::uint64_t audioCompletionsActive = 0;
    std::uint64_t audioCompletionOwnerRefs = 0;
};

struct RpcAudioStats {
    bool supported = false;
    bool running = false;
    RpcAudioOutput outputMode = RpcAudioOutput::Unknown;
    std::uint64_t liveVoices = 0;
    std::uint64_t busVoices = 0;
    std::uint64_t sessionHandles = 0;
    std::uint64_t retiringBGM = 0;
    std::uint64_t retiringVoice = 0;
    std::uint64_t waveCacheEntries = 0;
    std::uint64_t rawCacheEntries = 0;
    std::uint64_t voiceCompletionsPending = 0;
    std::uint64_t restoredSources = 0;
};

struct RpcStatsResult {
    int textureBudgetMB = 0;
    int textureTier = 0;
    std::string textureTierName;
    size_t meshCount = 0;
    int jobWorkers = 0;
    int jobPending = 0;
    int luaKb = 0;
    RpcJobStats jobs;
    RpcAsyncLoaderStats asyncLoader;
    RpcHostStats host;
    RpcAudioStats audio;
};

struct RpcFrameResult {
    std::string base64;
};

struct RpcAnimationResult {
    int modelId = 0;
    std::string name;
};

struct RpcInspectionResult {
    std::string value;
};

enum class RpcDebugRunState {
    Detached,
    Running,
    Paused,
    ResumePending,
};

struct RpcDebugStateResult {
    RpcDebugRunState state = RpcDebugRunState::Detached;
    std::string source;
    int line = 0;
    std::uint64_t pauseId = 0;
    std::uint64_t nonYieldableHitCount = 0;
};

using RpcReplyPayload = std::variant<
    std::monostate,
    RpcStatusResult,
    RpcEvaluateResult,
    RpcStateResult,
    RpcSmaValidateResult,
    RpcPickResult,
    RpcSmaSaveResult,
    RpcStatsResult,
    RpcFrameResult,
    RpcAnimationResult,
    RpcInspectionResult,
    RpcDebugStateResult,
    RpcKagDebugResult>;

struct RpcReply {
    RpcReplyStatus status = RpcReplyStatus::Unavailable;
    std::string code;
    std::string message;
    RpcReplyPayload payload;
};

class IRpcDispatcher {
public:
    virtual ~IRpcDispatcher() = default;

    virtual RpcReply dispatch(const RpcRequest& request) = 0;
};

} // namespace Caesura
