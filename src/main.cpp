extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

// SDL_GetBasePath: anchors the bundled editor frontend to the EXECUTABLE's own
// directory, so a release ZIP works from any working directory.
#include <SDL3/SDL_filesystem.h>
}
#include "render/BgfxRenderDevice.h"
#include "di/api/ITextureBudget.h"
#include "job/api/IJobSystem.h"
#include "render/api/IMeshRenderer.h"
#include "audio/SoLoudAudioEngine.h"
#include "platform/SDL3PlatformBackend.h"
#include "minigame/BgfxMiniGameBackend.h"
#include "live2d/api/IAnimationBackend.h"
#include "script/vm/LuaManager.h"
#include "script/vm/ManagedCoroutine.h"
#include "entry/Engine.h"
#include "entry/OwnerRpcQueue.h"
#include "debug/DebugProtocol.h"
#include "rpc/EditorServer.h"
#include <nlohmann_json.hpp>
#include "rpc/RpcServer.h"
#include "rpc/api/IRpcDispatcher.h"
#include <atomic>
#include <cmath>
#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
// Device-day bridge: tee native stderr to logcat so engine diagnostics are
// visible via adb logcat (SDL3 redirects stdout but not stderr).
static void* caesura_stderr_bridge(void* arg) {
    char buf[1024];
    ssize_t n;
    int fd = (int)(intptr_t)arg;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        __android_log_write(ANDROID_LOG_ERROR, "engine-stderr", buf);
    }
    return nullptr;
}
static void caesura_tee_stderr() {
    int fds[2];
    pthread_t th;
    if (pipe(fds) != 0) return;
    dup2(fds[1], 2);
    dup2(2, 1);          // stdout joins the logcat pipe too (no console on Android)
    close(fds[1]);
    pthread_create(&th, nullptr, caesura_stderr_bridge, (void*)(intptr_t)fds[0]);
    (void)th;
}
#endif
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace Caesura {
std::string discoverStartupScriptDir();
void configureStartupLuaPath(lua_State* L, const std::string& scriptDir);
void applyDevModeToTextureManager(lua_State* L);
}

namespace {

bool archivePublisherKeyPath(int argc, char* argv[], int optionIndex,
                             std::filesystem::path& path) {
    try {
        if (optionIndex < 1 || optionIndex + 1 >= argc) return false;
#if defined(_WIN32)
        // main() receives code-page converted argv on Windows. Recover only
        // this host-selected path from the original Unicode command line.
        int wideArgc = 0;
        const std::unique_ptr<wchar_t*, decltype(&LocalFree)> wideArgs(
            CommandLineToArgvW(GetCommandLineW(), &wideArgc), &LocalFree);
        if (!wideArgs || wideArgc != argc ||
            std::wstring(wideArgs.get()[optionIndex]) != L"--carc-public-key" ||
            std::string(argv[optionIndex]) != "--carc-public-key") {
            fprintf(stderr, "[main] ERROR: --carc-public-key command-line mapping failed.\n");
            return false;
        }
        path = std::filesystem::path(wideArgs.get()[optionIndex + 1]);
#else
        path = std::filesystem::path(argv[optionIndex + 1]);
#endif
        return !path.empty();
    } catch (const std::exception& error) {
        fprintf(stderr, "[main] ERROR: --carc-public-key path could not be resolved: %s\n",
                error.what());
        return false;
    }
}

std::string archiveKeyPathLabel(const std::filesystem::path& path) {
    try {
        const auto utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    } catch (const std::exception&) {
        // A diagnostic conversion must not prevent opening a valid native path.
        return "<host-selected path>";
    }
}

bool readArchivePublisherKey(const std::filesystem::path& keyPath,
                             Caesura::carc::ArchivePublicKey& key) {
    const std::string pathLabel = archiveKeyPathLabel(keyPath);
    try {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(keyPath, ec)) {
            fprintf(stderr, "[main] ERROR: --carc-public-key is not a readable file: %s\n",
                    pathLabel.c_str());
            return false;
        }
        std::ifstream input(keyPath, std::ios::binary);
        if (!input) {
            fprintf(stderr, "[main] ERROR: --carc-public-key cannot be opened: %s\n",
                    pathLabel.c_str());
            return false;
        }
        input.read(reinterpret_cast<char*>(key.data()),
                   static_cast<std::streamsize>(key.size()));
        char extra = 0;
        if (input.gcount() != static_cast<std::streamsize>(key.size()) ||
            input.get(extra) || !input.eof() || input.bad()) {
            fprintf(stderr, "[main] ERROR: --carc-public-key must contain exactly 32 raw bytes: %s\n",
                    pathLabel.c_str());
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        fprintf(stderr, "[main] ERROR: --carc-public-key could not be read (%s): %s\n",
                pathLabel.c_str(), error.what());
        return false;
    }
}

Caesura::RpcReply rpcError(Caesura::RpcReplyStatus status,
                           const char* code,
                           const char* message) {
    return {status, code, message, {}};
}

// Asset paths for RPC validation are restricted to the repo's asset
// directories: relative, no "..", no absolute paths, no embedded quotes.
bool isSafeAssetPath(const std::string& path) {
    if (path.empty() || path.size() > 256) return false;
    if (path.find('"') != std::string::npos) return false;
    if (path.find("..") != std::string::npos) return false;
    if (path.front() == '/' || path.front() == '\\') return false;
    if (path.size() > 1 && path[1] == ':') return false;
    return path.rfind("assets/", 0) == 0 || path.rfind("demo/assets/", 0) == 0;
}

Caesura::RpcReply rpcOk() {
    return {Caesura::RpcReplyStatus::Ok, {}, {}, {}};
}

class EngineRpcDispatcher final : public Caesura::IRpcDispatcher {
public:
    explicit EngineRpcDispatcher(Caesura::Engine& engine)
        : m_engine(engine),
          m_queue([this](const Caesura::RpcRequest& request) { return executeSafely(request); },
                  [](const Caesura::OwnerRpcQueue::Event& event) {
                      fprintf(stderr,
                          "[RpcRequest] id=%llu op=%s phase=%s status=%d code=%s elapsed_ms=%lld\n",
                          static_cast<unsigned long long>(event.requestId), event.operation,
                          Caesura::OwnerRpcQueue::phaseName(event.phase), static_cast<int>(event.status),
                          event.code.empty() ? "none" : event.code.c_str(),
                          static_cast<long long>(event.elapsed.count()));
                  }) {}

    ~EngineRpcDispatcher() override { close(); }

    Caesura::RpcReply dispatch(const Caesura::RpcRequest& request) override {
        return m_queue.dispatch(request, std::chrono::milliseconds(dispatchTimeoutMs()));
    }

    void pump() {
        if (const char* stall = std::getenv("CAESURA_TEST_STALL_MS")) {
            char* end = nullptr;
            const long v = std::strtol(stall, &end, 10);
            if (end && *end == '\0' && v > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(v));
        }
        m_queue.pump();
        pumpManagedRuns();
    }

    void close() {
        m_queue.close();
        abortManagedRuns();
    }

private:
    // Preserve the existing per-request environment override and 5s default.
    static long dispatchTimeoutMs() {
        const char* env = std::getenv("CAESURA_RPC_DISPATCH_TIMEOUT_MS");
        if (env) {
            char* end = nullptr;
            const long v = std::strtol(env, &end, 10);
            if (end && *end == '\0' && v > 0) return v;
        }
        return 5000L;
    }
    Caesura::RpcReply executeSafely(const Caesura::RpcRequest& request) {
        try {
            return execute(request);
        } catch (const std::exception& error) {
            return {Caesura::RpcReplyStatus::Failed,
                    "owner_dispatch_exception", error.what(), {}};
        } catch (...) {
            return rpcError(Caesura::RpcReplyStatus::Failed,
                            "owner_dispatch_exception",
                            "Owner dispatcher threw an unknown exception");
        }
    }

    Caesura::RpcReply execute(const Caesura::RpcRequest& request) {
        return std::visit([this](const auto& operation) -> Caesura::RpcReply {
            using Operation = std::decay_t<decltype(operation)>;

            if constexpr (std::is_same_v<Operation, Caesura::RpcStatusRequest>) {
                Caesura::RpcReply reply = rpcOk();
                reply.payload = Caesura::RpcStatusResult{
                    m_engine.lua().state() != nullptr};
                return reply;
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcRunScriptRequest>) {
                // Every RPC-driven run/eval opens its own budget window,
                // symmetric with the boot entry-scene reset and the per-frame
                // reset in Engine::processEvents. Without this, cumulative
                // counts within one frame false-kill 12-14.7KB scene loads
                // (t59 audit).
                m_engine.lua().resetInstructionBudget();
                return startManagedRun(operation.script);
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcEvaluateRequest>) {
                m_engine.lua().resetInstructionBudget();
                return evaluate(operation.code);
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcKagDebugRequest>) {
                return kagDebugAction(operation);
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcStopRequest>) {
                m_queue.close(); // Stop retires the unstarted remainder of this batch.
                abortManagedRuns();
                m_engine.quit();
                return rpcOk();
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcGetStateRequest>) {
                lua_State* L = m_engine.lua().state();
                if (!L) return rpcError(Caesura::RpcReplyStatus::Unavailable,
                                        "lua_unavailable", "Lua VM is unavailable");
                // Snapshot the runner ctx through Lua (kag_runner.get_ctx
                // is the authoritative game state the debugger also uses).
                const int stackTop = lua_gettop(L);
                const char* code =
                    "local ctx = require('kag_runner').get_ctx(); "
                    "if not ctx then return '{}' end; "
                    "local ok, layers = pcall(function() "
                    "  return require('layers').count() end); "
                    "local i18n = require('i18n'); "
                    "local bl = type(ctx.backlog) == 'table' and #ctx.backlog or 0; "
                    "local tok = ctx.tokens and ctx.tokens[ctx.token_index]; "
                    "local cur = ''; "
                    "if type(tok) == 'table' then "
                    "  if tok.type == 'command' then cur = '[' .. tostring(tok.cmd or '') .. ']' "
                    "  elseif tok.type == 'label' then cur = '*' .. tostring(tok.name or '') "
                    "  elseif tok.type == 'text' then cur = 'text' "
                    "  else cur = tostring(tok.type or '') end "
                    "end; "
                    "return string.format('{\"scene\":%q,\"token_index\":%d,"
                    "\"nvl_mode\":%s,\"language\":%q,\"backlog_count\":%d,"
                    "\"layer_count\":%d,\"current_cmd\":%q}', "
                    "tostring(ctx.current_scene or ctx.currentScene or ''), "
                    "tonumber(ctx.token_index) or 0, "
                    "ctx.nvl_mode == true and 'true' or 'false', "
                    "tostring(i18n and i18n.current or ''), bl, "
                    "ok and (tonumber(layers) or 0) or 0, cur)";
                Caesura::RpcStateResult state;
                if (luaL_loadstring(L, code) == LUA_OK
                    && lua_pcall(L, 0, 1, 0) == LUA_OK
                    && lua_isstring(L, -1)) {
                    // The snippet returns a JSON object literal.
                    try {
                        auto j = nlohmann::json::parse(lua_tostring(L, -1));
                        state.scene = j.value("scene", std::string());
                        state.tokenIndex = j.value("token_index", 0);
                        state.nvlMode = j.value("nvl_mode", false);
                        state.language = j.value("language", std::string());
                        state.backlogCount = j.value("backlog_count", 0);
                        state.layerCount = j.value("layer_count", 0);
                        state.currentCmd = j.value("current_cmd", std::string());
                    } catch (const std::exception&) {
                        state.scene = lua_tostring(L, -1);
                    }
                }
                lua_settop(L, stackTop);
                Caesura::RpcReply reply = rpcOk();
                reply.payload = std::move(state);
                return reply;
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcSmaValidateRequest>) {
                if (!isSafeAssetPath(operation.path)) {
                    return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                    "unsafe_path",
                                    "Path must be relative under assets/ or demo/assets/");
                }
                lua_State* L = m_engine.lua().state();
                if (!L) return rpcError(Caesura::RpcReplyStatus::Unavailable,
                                        "lua_unavailable", "Lua VM is unavailable");
                const int stackTop = lua_gettop(L);
                // Run the shared checker (kag.sma_check) inside the engine
                // Lua state: same module the runtime loader uses, so the
                // panel and the game agree on what is valid.
                std::string path = operation.path;
                for (char& ch : path) {
                    if (ch == '\\') ch = '/';
                }
                const std::string code =
                    "local ok, checker = pcall(require, 'kag.sma_check')\n"
                    "if not ok or not checker then return '{\"ok\":false,\"errors\":[\"sma_check unavailable\"],\"meta\":{}}' end\n"
                    "local function jl(list) local parts = {} for _, e in ipairs(list) do parts[#parts + 1] = string.format('%q', tostring(e)) end return '[' .. table.concat(parts, ',') .. ']' end\n"
                    "local function jm(meta) local anims = {} for _, a in ipairs(meta.anims or {}) do anims[#anims + 1] = string.format('%q', tostring(a)) end "
                    "local bt = {} for _, b in ipairs(meta.boneTree or {}) do bt[#bt + 1] = string.format('{\"id\":%d,\"parent\":%d,\"pivot\":[%s,%s]}', b.id, b.parent or -1, tostring(b.pivot and b.pivot[1] or 0), tostring(b.pivot and b.pivot[2] or 0)) end "
                    "local ad = {} for _, d in ipairs(meta.animDetails or {}) do ad[#ad + 1] = string.format('{\"name\":%q,\"duration\":%s,\"tracks\":%s}', tostring(d.name), d.duration or 0, jl(d.tracks or {})) end "
                    "return string.format('{\"bones\":%d,\"anims\":[%s],\"parts\":%d,\"verts\":%d,\"tris\":%d,\"boneTree\":[%s],\"animDetails\":[%s]}', "
                    "meta.bones or 0, table.concat(anims, ','), meta.parts or 0, meta.verts or 0, meta.tris or 0, table.concat(bt, ','), table.concat(ad, ',')) end\n"
                    // The path arrives as a CHUNK ARGUMENT (varargs), never
                    // concatenated into the source, so quotes / newlines /
                    // backslashes inside it can no longer alter the program.
                    "local res = checker.validate_file(...)\n"
                    "return string.format('{\"ok\":%s,\"errors\":%s,\"meta\":%s}', "
                    "res.ok and 'true' or 'false', jl(res.errors or {}), jm(res.meta or {}))";
                Caesura::RpcSmaValidateResult result;
                bool smaLuaOk = false;
                if (luaL_loadstring(L, code.c_str()) == LUA_OK) {
                    lua_pushlstring(L, path.data(), path.size());  // -> ...
                    smaLuaOk = lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1);
                }
                if (smaLuaOk) {
                    try {
                        auto j = nlohmann::json::parse(lua_tostring(L, -1));
                        result.ok = j.value("ok", false);
                        for (const auto& e : j.value("errors", std::vector<std::string>{})) {
                            result.errors.push_back(e);
                        }
                        result.meta = j.value("meta", nlohmann::json::object()).dump();
                    } catch (const std::exception&) {
                        result.ok = false;
                        result.errors.push_back("lua_result_parse_failed");
                    }
                } else {
                    result.ok = false;
                    result.errors.push_back("lua_exec_failed");
                }
                lua_settop(L, stackTop);
                Caesura::RpcReply reply = rpcOk();
                reply.payload = std::move(result);
                return reply;
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcPickRequest>) {
                // IDE preview-frame pick: hit-test the Lua layer tree via
                // the shared layers module (same code the game uses).
                lua_State* L = m_engine.lua().state();
                if (!L) return rpcError(Caesura::RpcReplyStatus::Unavailable,
                                        "lua_unavailable", "Lua VM is unavailable");
                const int stackTop = lua_gettop(L);
                const std::string code =
                    "local ok, layers = pcall(require, 'layers')\n"
                    "if not ok or not layers or not layers.pick then return '[]' end\n"
                    "local hits = layers.pick(" + std::to_string(operation.x)
                    + ", " + std::to_string(operation.y) + ")\n"
                    "local parts = {}\n"
                    "for _, h in ipairs(hits) do\n"
                    "  parts[#parts + 1] = string.format('{\"id\":%q,\"name\":%q,\"z\":%d,\"depth\":%d,\"opacity\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}', "
                    "tostring(h.id), tostring(h.name), h.z or 0, h.depth or 0, h.opacity or 255, "
                    "math.floor(h.x or 0), math.floor(h.y or 0), math.floor(h.w or 0), math.floor(h.h or 0))\n"
                    "end\n"
                    "return '[' .. table.concat(parts, ',') .. ']'";
                Caesura::RpcPickResult pick;
                if (luaL_loadstring(L, code.c_str()) == LUA_OK
                    && lua_pcall(L, 0, 1, 0) == LUA_OK
                    && lua_isstring(L, -1)) {
                    pick.hits = lua_tostring(L, -1);
                } else {
                    pick.hits = "[]";
                }
                lua_settop(L, stackTop);
                Caesura::RpcReply reply = rpcOk();
                reply.payload = std::move(pick);
                return reply;
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcSmaSaveRequest>) {
                // Editor save-back: validate the JSON text with the shared
                // checker first; only write to the disk on success. Path is
                // restricted by isSafeAssetPath (assets/ or demo/assets/).
                if (!isSafeAssetPath(operation.path)) {
                    return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                    "unsafe_path",
                                    "Path must be relative under assets/ or demo/assets/");
                }
                lua_State* L = m_engine.lua().state();
                Caesura::RpcSmaSaveResult saveRes;
                if (!L) {
                    saveRes.ok = false;
                    saveRes.errors.push_back("lua_unavailable");
                } else {
                    const int stackTop = lua_gettop(L);
                    // Build a Lua literal for the content (escape backslash
                    // and quotes for the embedded string literal).
                    std::string lit = operation.content;
                    std::string esc;
                    esc.reserve(lit.size() + 16);
                    for (char ch : lit) {
                        if (ch == '\\') esc += "\\\\";
                        else if (ch == '"') esc += "\\\"";
                        else if (ch == '\n') esc += "\\n";
                        else if (ch == '\r') esc += "\\r";
                        else if (ch == '\t') esc += "\\t";
                        else esc += ch;
                    }
                    const std::string code =
                        "local ok, checker = pcall(require, 'kag.sma_check')\n"
                        "if not ok or not checker then return '{\"ok\":false,\"errors\":[\"sma_check unavailable\"]}' end\n"
                        "local res = checker.validate_text(\"" + esc + "\")\n"
                        "local parts = {} for _, e in ipairs(res.errors or {}) do parts[#parts + 1] = string.format('%q', tostring(e)) end\n"
                        "return string.format('{\"ok\":%s,\"errors\":[%s]}', res.ok and 'true' or 'false', table.concat(parts, ','))";
                    if (luaL_loadstring(L, code.c_str()) == LUA_OK
                        && lua_pcall(L, 0, 1, 0) == LUA_OK
                        && lua_isstring(L, -1)) {
                        try {
                            auto j = nlohmann::json::parse(lua_tostring(L, -1));
                            saveRes.ok = j.value("ok", false);
                            for (const auto& e2 : j.value("errors", std::vector<std::string>{})) {
                                saveRes.errors.push_back(e2);
                            }
                        } catch (...) {
                            saveRes.ok = false;
                            saveRes.errors.push_back("lua_result_parse_failed");
                        }
                    } else {
                        saveRes.ok = false;
                        saveRes.errors.push_back("lua_exec_failed");
                    }
                    lua_settop(L, stackTop);
                }
                if (saveRes.ok) {
                    std::string writePath = operation.path;
                    for (char& ch : writePath) {
                        if (ch == '/') ch = '\\';
                    }
                    std::ofstream out(writePath, std::ios::binary);
                    if (!out) {
                        saveRes.ok = false;
                        saveRes.errors.push_back("cannot open file for writing");
                    } else {
                        out.write(operation.content.data(),
                                  static_cast<std::streamsize>(operation.content.size()));
                        out.close();
                        if (!out) {
                            saveRes.ok = false;
                            saveRes.errors.push_back("write failed");
                        }
                    }
                }
                Caesura::RpcReply reply = rpcOk();
                reply.payload = std::move(saveRes);
                return reply;
            } else if constexpr (std::is_same_v<Operation, Caesura::RpcStatsRequest>) {
                Caesura::RpcStatsResult stats;
                stats.textureBudgetMB =
                    static_cast<int>(m_engine.textureBudget().getBudgetMB());
                stats.textureTier = m_engine.textureBudget().getTier();
                stats.textureTierName = m_engine.textureBudget().getTierName();
                stats.meshCount = m_engine.meshRenderer().meshCount();
                stats.jobWorkers = m_engine.jobSystem().workerCount();
                stats.jobPending = m_engine.jobSystem().pendingJobs();
                if (lua_State* L = m_engine.lua().state()) {
                    stats.luaKb = static_cast<int>(lua_gc(L, LUA_GCCOUNT, 0));
                }
                Caesura::RpcReply reply = rpcOk();
                reply.payload = std::move(stats);
                return reply;
            } else if constexpr (
                std::is_same_v<Operation, Caesura::RpcCaptureFrameRequest>) {
                std::string frame = m_engine.captureFrameForRpc(
                    operation.width, operation.height);
                if (frame.empty()) {
                    return rpcError(Caesura::RpcReplyStatus::Failed,
                                    "capture_failed", "Frame capture failed");
                }
                Caesura::RpcReply reply = rpcOk();
                reply.payload = Caesura::RpcFrameResult{std::move(frame)};
                return reply;
            } else if constexpr (
                std::is_same_v<Operation, Caesura::RpcReloadScriptsRequest>) {
                if (!m_engine.reloadScriptsNow()) {
                    return rpcError(Caesura::RpcReplyStatus::Failed,
                                    "reload_rejected",
                                    "Reload rejected (Lua paused or script reload failed)");
                }
                return rpcOk();
            } else if constexpr (
                std::is_same_v<Operation, Caesura::RpcLoadAnimationRequest>) {
                if (operation.modelPath.empty()) {
                    return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                    "invalid_model_path", "Model path is empty");
                }
                if (!std::isfinite(operation.x) || !std::isfinite(operation.y) ||
                    !std::isfinite(operation.scale) || operation.scale <= 0.0f) {
                    return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                    "invalid_animation_transform",
                                    "Animation transform must be finite and scale must be positive");
                }
                const std::string name = std::filesystem::path(
                    operation.modelPath).stem().string();
                const int modelId = m_engine.animation().loadModel(
                    operation.modelPath, name);
                if (modelId <= 0) {
                    return rpcError(Caesura::RpcReplyStatus::Failed,
                                    "animation_load_failed", "Animation load failed");
                }
                if (operation.show) {
                    try {
                        m_engine.animation().showModel(
                            modelId, operation.x, operation.y, operation.scale);
                    } catch (...) {
                        m_engine.animation().unloadModel(modelId);
                        throw;
                    }
                }
                Caesura::RpcReply reply = rpcOk();
                reply.payload = Caesura::RpcAnimationResult{modelId, name};
                return reply;
            } else {
                return executeDebug(operation);
            }
        }, request.payload);
    }

    template <typename Operation>
    Caesura::RpcReply executeDebug(const Operation& operation) {
        Caesura::DebugProtocol* protocol = m_engine.debugProtocol();

        if constexpr (std::is_same_v<Operation, Caesura::RpcGetDebugStateRequest>) {
            Caesura::RpcDebugStateResult result;
            if (protocol) {
                switch (protocol->runState()) {
                    case Caesura::DebugProtocol::RunState::Detached:
                        result.state = Caesura::RpcDebugRunState::Detached;
                        break;
                    case Caesura::DebugProtocol::RunState::Running:
                        result.state = Caesura::RpcDebugRunState::Running;
                        break;
                    case Caesura::DebugProtocol::RunState::Paused:
                        result.state = Caesura::RpcDebugRunState::Paused;
                        break;
                    case Caesura::DebugProtocol::RunState::ResumePending:
                        result.state = Caesura::RpcDebugRunState::ResumePending;
                        break;
                }
                result.source = protocol->currentSource();
                result.line = protocol->currentLine();
                result.pauseId = protocol->currentPauseId();
                result.nonYieldableHitCount = protocol->nonYieldableHitCount();
            }
            Caesura::RpcReply reply = rpcOk();
            reply.payload = std::move(result);
            return reply;
        }

        if (!protocol) {
            return rpcError(Caesura::RpcReplyStatus::Unavailable,
                            "debugger_disabled", "Debugger is not enabled");
        }

        if constexpr (std::is_same_v<Operation, Caesura::RpcSetBreakpointRequest>) {
            if (operation.source.empty() || operation.line <= 0) {
                return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                "invalid_breakpoint", "Breakpoint source or line is invalid");
            }
            protocol->setBreakpoint(operation.source, operation.line);
            return rpcOk();
        } else if constexpr (
            std::is_same_v<Operation, Caesura::RpcRemoveBreakpointRequest>) {
            if (operation.source.empty() || operation.line <= 0) {
                return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                "invalid_breakpoint", "Breakpoint source or line is invalid");
            }
            protocol->removeBreakpoint(operation.source, operation.line);
            return rpcOk();
        } else if constexpr (
            std::is_same_v<Operation, Caesura::RpcClearBreakpointsRequest>) {
            protocol->clearAllBreakpoints();
            return rpcOk();
        } else if constexpr (
            std::is_same_v<Operation, Caesura::RpcDebugResumeRequest>) {
            Caesura::DebugProtocol::Command command =
                Caesura::DebugProtocol::Command::Continue;
            switch (operation.mode) {
                case Caesura::RpcDebugResumeMode::Continue:
                    command = Caesura::DebugProtocol::Command::Continue;
                    break;
                case Caesura::RpcDebugResumeMode::StepInto:
                    command = Caesura::DebugProtocol::Command::StepInto;
                    break;
                case Caesura::RpcDebugResumeMode::StepOver:
                    command = Caesura::DebugProtocol::Command::StepOver;
                    break;
                case Caesura::RpcDebugResumeMode::StepOut:
                    command = Caesura::DebugProtocol::Command::StepOut;
                    break;
            }
            if (!protocol->commandSink()(operation.pauseId, command)) {
                return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                "stale_pause", "Pause id is stale or no pause is active");
            }
            return rpcOk();
        } else if constexpr (
            std::is_same_v<Operation, Caesura::RpcInspectLocalRequest>) {
            if (!protocol->isDebugActive() || operation.frame < 0 ||
                operation.name.empty()) {
                return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                "inspection_unavailable",
                                "Local inspection requires an active pause");
            }
            Caesura::RpcReply reply = rpcOk();
            reply.payload = Caesura::RpcInspectionResult{
                protocol->inspectLocal(operation.frame, operation.name)};
            return reply;
        } else if constexpr (
            std::is_same_v<Operation, Caesura::RpcInspectGlobalRequest>) {
            if (!protocol->isDebugActive() || operation.name.empty()) {
                return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                "inspection_unavailable",
                                "Global inspection requires an active pause");
            }
            Caesura::RpcReply reply = rpcOk();
            reply.payload = Caesura::RpcInspectionResult{
                protocol->inspectGlobal(operation.name)};
            return reply;
        } else {
            return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                            "unknown_owner_command", "Unknown owner command");
        }
    }

    // -- Managed run/eval execution ------------------------------------

    struct ManagedRun {
        Caesura::detail::ManagedCoroutine coroutine;
        std::uint64_t requestId = 0;
        std::chrono::steady_clock::time_point submittedAt = std::chrono::steady_clock::now();
    };

    // All phase/code arguments below are fixed
    // labels; user script, error body and credentials never enter this log.
    static void reportManagedRun(const ManagedRun& run, const char* phase,
                                 const char* code, int status = LUA_OK,
                                 int closeStatus = LUA_OK) noexcept {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - run.submittedAt);
        fprintf(stderr, "[RpcRun] id=%llu phase=%s code=%s lua_status=%d close_status=%d elapsed_ms=%lld\n",
                static_cast<unsigned long long>(run.requestId), phase, code, status, closeStatus,
                static_cast<long long>(elapsed.count()));
    }

    lua_State* managedLuaState() noexcept {
        // Keep this tied to our Engine, not a replacement VM in the global
        // registry. Engine::lua() rejects access after Engine shutdown.
        try { return m_engine.lua().state(); }
        catch (...) { return nullptr; }
    }

    struct RpcStackRestore {
        lua_State* state;
        int top;
        ~RpcStackRestore() { lua_settop(state, top); }
    };

    static int stringifyRpcValue(lua_State* state) {
        // All locals are trivial: user __tostring may longjmp to lua_pcall.
        size_t length = 0;
        luaL_tolstring(state, 1, &length);
        return 1;
    }

    static Caesura::detail::CoroutineDiagnostic formatRpcValue(
        lua_State* state, int index, std::string& value) {
        Caesura::detail::CoroutineDiagnostic result;
        const int top = lua_gettop(state);
        const int absoluteIndex = lua_absindex(state, index);
        RpcStackRestore restore{state, top};
        if (!lua_checkstack(state, 2)) {
            result.status = LUA_ERRMEM;
            constexpr char message[] = "cannot grow RPC result formatting stack";
            Caesura::detail::copyCoroutineMessage(result.message, sizeof(result.message),
                                                  message, sizeof(message) - 1);
            return result;
        }
        lua_pushcfunction(state, &EngineRpcDispatcher::stringifyRpcValue);
        lua_pushvalue(state, absoluteIndex);
        result.status = lua_pcall(state, 1, 1, 0);
        if (result.status != LUA_OK) {
            // Never invoke __tostring again while describing its failure.
            Caesura::detail::copyCoroutineError(state, result.message, sizeof(result.message),
                                                "non-string RPC result formatting error");
            return result;
        }
        size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length); // protected function returned a string
        value.assign(text, length);
        return result;
    }

    Caesura::RpcReply kagDebugAction(const Caesura::RpcKagDebugRequest& op) {
        // Drive the kag_debug.lua API through the Lua state. Each action
        // maps to a small Lua snippet so the editor never touches raw
        // Lua; results (inspect) come back as JSON text.
        lua_State* L = m_engine.lua().state();
        if (!L) {
            return rpcError(Caesura::RpcReplyStatus::Unavailable,
                            "lua_unavailable", "Lua VM is unavailable");
        }
        std::string code;
        if (op.action == "setBreakpoint") {
            if (op.scene.empty()
                || (op.cmd.empty() && op.line <= 0)) {
                return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                                "invalid_kag_breakpoint",
                                "scene plus cmd (string) or line (int) required");
            }
            code = "local kd = require('kag_debug'); "
                   "return tostring(kd.set_breakpoint("
                   + luaQuote(op.scene) + ", "
                   + (op.cmd.empty()
                       ? std::to_string(op.line)
                       : luaQuote(op.cmd))
                   + "))";
        } else if (op.action == "clearBreakpoints") {
            code = "local kd = require('kag_debug'); kd.clear_breakpoints("
                   + (op.scene.empty() ? "nil" : luaQuote(op.scene)) + "); "
                   + "return 'ok'";
        } else if (op.action == "continue") {
            code = "local kr = require('kag_runner'); "
                   "local ok = pcall(kr.continue_scene_debugger); "
                   "return ok and 'ok' or 'runner-not-ready'";
        } else if (op.action == "step") {
            code = "local kr = require('kag_runner'); "
                   "local ok = pcall(kr.debug_step); "
                   "return ok and 'ok' or 'runner-not-ready'";
        } else if (op.action == "reloadScene") {
            // Scene hot reload (editor workflow): re-parse the given (or
            // current) .ks through kag_runner.reload_scene -- preserves
            // game state and remaps the execution position.
            code = "local kr = require('kag_runner'); "
                   "local ok, r = kr.reload_scene("
                   + (op.scene.empty() ? "nil" : luaQuote(op.scene))
                   + "); "
                   "return ok and ('ok:' .. tostring(r)) "
                   "or ('error:' .. tostring(r))";
        } else if (op.action == "inspect") {
            code = "local kd = require('kag_debug'); "
                   "local ctx = require('kag_runner').get_ctx(); "
                   "if not ctx then return '{}' end; "
                   "return kd.serialize_json(ctx, "
                   + (op.scope.empty() ? "nil" : luaQuote(op.scope)) + ")";
        } else {
            return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                            "unknown_kag_debug_action",
                            ("unknown action: " + op.action).c_str());
        }

        const int top = lua_gettop(L);
        RpcStackRestore restore{L, top};
        if (luaL_loadstring(L, code.c_str()) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            const std::string msg = err ? err : "compile error";
            lua_settop(L, top);
            return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                            "kag_debug_compile_error", msg.c_str());
        }
        const int callStatus = lua_pcall(L, 0, 1, 0);
        if (callStatus != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            const std::string msg = err ? err : "kag debug action failed";
            lua_settop(L, top);
            return rpcError(Caesura::RpcReplyStatus::Failed,
                            "kag_debug_error", msg.c_str());
        }
        std::string value;
        const auto formatted = formatRpcValue(L, -1, value);
        lua_settop(L, top);
        if (formatted.status != LUA_OK)
            return rpcError(Caesura::RpcReplyStatus::Failed,
                            "kag_debug_result_error", formatted.message);
        Caesura::RpcReply reply = rpcOk();
        reply.payload = Caesura::RpcKagDebugResult{std::move(value)};
        return reply;
    }

    // Quote a string as a Lua literal (single-quoted with escapes).
    static std::string luaQuote(const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
            }
        }
        out += "'";
        return out;
    }

    Caesura::RpcReply evaluate(const std::string& code) {
        lua_State* L = m_engine.lua().state();
        if (!L) {
            return rpcError(Caesura::RpcReplyStatus::Unavailable,
                            "lua_unavailable", "Lua VM is unavailable");
        }
        const int top = lua_gettop(L);
        RpcStackRestore restore{L, top};
        if (luaL_loadstring(L, code.c_str()) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            const std::string msg = err ? err : "compile error";
            lua_settop(L, top);
            return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                            "eval_compile_error", msg.c_str());
        }
        const int callStatus = lua_pcall(L, 0, 1, 0);
        if (callStatus != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            const std::string msg = (callStatus == LUA_YIELD)
                ? "eval cannot yield"
                : (err ? err : "evaluation failed");
            lua_settop(L, top);
            return rpcError(Caesura::RpcReplyStatus::Failed,
                            "eval_error", msg.c_str());
        }
        std::string value;
        const auto formatted = formatRpcValue(L, -1, value);
        lua_settop(L, top);
        if (formatted.status != LUA_OK)
            return rpcError(Caesura::RpcReplyStatus::Failed,
                            "eval_result_error", formatted.message);
        Caesura::RpcReply reply = rpcOk();
        reply.payload = Caesura::RpcEvaluateResult{std::move(value)};
        return reply;
    }

    Caesura::RpcReply startManagedRun(const std::string& script) {
        lua_State* L = m_engine.lua().state();
        if (!L) {
            return rpcError(Caesura::RpcReplyStatus::Unavailable,
                            "lua_unavailable", "Lua VM is unavailable");
        }
        if (script.empty()) {
            return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                            "empty_script", "Script must not be empty");
        }
        ManagedRun run;
        run.requestId = m_queue.currentRequestId();
        const auto created = Caesura::detail::createManagedCoroutine(L, script.c_str(), run.coroutine);
        if (created.status != LUA_OK) {
            return rpcError(Caesura::RpcReplyStatus::InvalidRequest,
                            "run_compile_error", created.message);
        }
        try {
            m_managedRuns.push_back(run);
        } catch (...) {
            const auto closed = Caesura::detail::closeManagedCoroutine(L, run.coroutine);
            reportManagedRun(run, "failed", "run_registration_failed", LUA_ERRMEM, closed.status);
            throw;
        }
        reportManagedRun(run, "accepted", "run_submitted");
        Caesura::RpcReply reply = rpcOk();
        reply.message = "started"; // Preserve acceptance reply, not final script completion.
        return reply;
    }

    void pumpManagedRuns() {
        if (m_managedRuns.empty()) return;
        lua_State* L = managedLuaState();
        if (!L) {
            for (const auto& run : m_managedRuns)
                reportManagedRun(run, "cancelled", "run_vm_unavailable", LUA_ERRRUN);
            m_managedRuns.clear(); // No Lua access after the VM is gone.
            return;
        }
        for (auto it = m_managedRuns.begin(); it != m_managedRuns.end();) {
            const auto step = Caesura::detail::resumeManagedCoroutine(L, it->coroutine);
            if (step.status == LUA_YIELD) {
                ++it;
                continue;
            }
            if (step.status != LUA_OK)
                reportManagedRun(*it, "failed", "run_error", step.status, step.closed.status);
            else if (step.closed.status != LUA_OK)
                reportManagedRun(*it, "failed", "run_close_error", step.status, step.closed.status);
            else
                reportManagedRun(*it, "completed", "run_completed");
            it = m_managedRuns.erase(it);
        }
    }

    void abortManagedRuns() noexcept {
        if (m_managedRuns.empty()) return;
        lua_State* L = managedLuaState();
        while (!m_managedRuns.empty()) {
            // Retire the record before user __close can reenter cleanup.
            ManagedRun run = m_managedRuns.front();
            m_managedRuns.pop_front();
            const auto closed = Caesura::detail::closeManagedCoroutine(L, run.coroutine);
            if (closed.status != LUA_OK)
                reportManagedRun(run, "failed", "run_cancel_close_error", LUA_OK, closed.status);
            else
                reportManagedRun(run, "cancelled", L ? "run_cancelled" : "run_vm_unavailable");
        }
    }

    Caesura::Engine& m_engine;
    Caesura::OwnerRpcQueue m_queue;
    std::deque<ManagedRun> m_managedRuns;
};

// Explicit instantiations: on the Android NDK toolchain (libc++ `__ndk1` +
// lld) the variant visitor inside execute() can leave these member-template
// instantiations unreferenced-into the link (ld.lld: undefined symbol ...
// executeDebug<...> for the 7 debug request types). Forcing emission keeps
// the debug RPC dispatch linkable under the NDK without changing behaviour
// on MSVC/macOS/Linux, where the implicit instantiations link fine.
template Caesura::RpcReply EngineRpcDispatcher::executeDebug<Caesura::RpcSetBreakpointRequest>(
    Caesura::RpcSetBreakpointRequest const&);
template Caesura::RpcReply EngineRpcDispatcher::executeDebug<Caesura::RpcRemoveBreakpointRequest>(
    Caesura::RpcRemoveBreakpointRequest const&);
template Caesura::RpcReply EngineRpcDispatcher::executeDebug<Caesura::RpcClearBreakpointsRequest>(
    Caesura::RpcClearBreakpointsRequest const&);
template Caesura::RpcReply EngineRpcDispatcher::executeDebug<Caesura::RpcDebugResumeRequest>(
    Caesura::RpcDebugResumeRequest const&);
template Caesura::RpcReply EngineRpcDispatcher::executeDebug<Caesura::RpcInspectLocalRequest>(
    Caesura::RpcInspectLocalRequest const&);
template Caesura::RpcReply EngineRpcDispatcher::executeDebug<Caesura::RpcInspectGlobalRequest>(
    Caesura::RpcInspectGlobalRequest const&);
template Caesura::RpcReply EngineRpcDispatcher::executeDebug<Caesura::RpcGetDebugStateRequest>(
    Caesura::RpcGetDebugStateRequest const&);

bool runStdioRpc(Caesura::Engine& engine) {
    auto dispatcher = std::make_shared<EngineRpcDispatcher>(engine);
    // Preserve the startup banner, then reserve the captured descriptor for
    // protocol JSON. Keep console output on stderr through main's final exit
    // messages and C/C++ teardown, so it cannot refill the protocol pipe.
    fflush(stdout);
    Caesura::RpcServer rpc;
    if (!rpc.outputReady()) {
        fprintf(stderr, "[RpcServer] Could not retain protocol stdout.\n");
        engine.quit();
        engine.shutdown();
        return false;
    }
#if defined(_WIN32)
    const bool redirected = ::_dup2(::_fileno(stderr), ::_fileno(stdout)) == 0
        && SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
#else
    const bool redirected = ::dup2(STDERR_FILENO, STDOUT_FILENO) >= 0;
#endif
    if (!redirected) {
        fprintf(stderr, "[RpcServer] Could not reserve stdout for the protocol.\n");
        engine.quit();
        engine.shutdown();
        return false;
    }
    rpc.setDispatcher(dispatcher);

    std::atomic<bool> transportFinished{false};
    std::thread transport([&rpc, &transportFinished]() {
        rpc.run();
        transportFinished.store(true, std::memory_order_release);
    });

    engine.run([&]() {
        dispatcher->pump();
        if (transportFinished.load(std::memory_order_acquire)) engine.quit();
    });

    dispatcher->close();
    rpc.stop();
    rpc.setDispatcher({});
    if (transport.joinable()) transport.join();
    engine.shutdown();
    return true;
}

bool runHttpEditor(Caesura::Engine& engine, const std::string& authToken,
                   bool insecureNoAuth) {
    auto dispatcher = std::make_shared<EngineRpcDispatcher>(engine);
    Caesura::EditorServer editor;
    editor.setDispatcher(dispatcher);
    if (!authToken.empty()) editor.setAuthToken(authToken);
    // Secure by default: with no configured token EditorServer generates one
    // (printed to stderr + written to .caesura-editor-token). The editor RPC
    // exposes /api/eval and /api/run, i.e. arbitrary Lua, so an unauthenticated
    // listener is a local privilege boundary hole. --editor-insecure is the
    // explicit, loudly-warned escape hatch.
    if (insecureNoAuth) editor.setInsecureNoAuth(true);

    // Composition-root injection (N1): the editor's ProjectContext resolves
    // projects/templates from the EXECUTABLE's own directory first, so a
    // release package resolves to itself even when it was built on a machine
    // whose CAESURA_SOURCE_DIR macro still points at a live source tree.
    // SDL_GetBasePath returns UTF-8 -- convert via std::u8string so non-ASCII
    // paths survive the ANSI code page on Windows (same route the removed
    // executableDirectory() used).
    if (const char* base = SDL_GetBasePath()) {
        const std::string utf8(base);
        std::filesystem::path exeDir = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(utf8.data()),
                          utf8.size())).parent_path();
        editor.setSourceAnchor(std::move(exeDir));
    }
    // Serve the bundled web-editor frontend (web-editor/dist). A release ZIP is
    // extracted anywhere and launched from any working directory, so the
    // EXECUTABLE's own directory is the authoritative anchor (SDL_GetBasePath
    // derives it from the loaded image path; SDL is already initialised here).
    // The CWD walk-up stays as a fallback so the in-tree workflows keep working
    // unchanged (repo root, or build/Debug -> ../../web-editor/dist).
    {
        namespace fs = std::filesystem;
        std::string webRoot;
        std::error_code ec;
        // Three levels up from the binary covers the macOS bundle layout
        // (.app/Contents/MacOS/), which BUNDLE DESTINATION . already produces.
        if (const char* base = SDL_GetBasePath()) {
            // SDL returns UTF-8: route through char8_t so non-ASCII install
            // paths survive the ANSI code page (mirrors setSourceAnchor above).
            // Note the trailing separator is kept here on purpose -- this probe
            // appends subpaths, whereas setSourceAnchor takes parent_path() to
            // get the directory itself. Do not "unify" the two.
            fs::path probe = fs::path(
                std::u8string(reinterpret_cast<const char8_t*>(base)));
            for (int i = 0; i < 3 && webRoot.empty(); ++i) {
                if (fs::exists(probe / "web-editor" / "dist" / "index.html", ec)) {
                    webRoot = (probe / "web-editor" / "dist").string();
                }
                probe = probe.parent_path();
            }
        }
        if (webRoot.empty()) {
            fs::path probe = fs::current_path(ec);
            for (int i = 0; i < 4 && webRoot.empty() && !ec; ++i) {
                if (fs::exists(probe / "web-editor" / "dist" / "index.html", ec)) {
                    webRoot = (probe / "web-editor" / "dist").string();
                }
                probe = probe.parent_path();
            }
        }
        if (!webRoot.empty()) editor.setWebRoot(webRoot);
        else fprintf(stderr, "[EditorServer] web-editor/dist not found; serving API only\n");
    }
    // WinNAT dynamic excluded port ranges can swallow the default port with
    // WSAEACCES even when nothing listens (observed 2026-08-29: range
    // 9813-9912 covered 9876). Devs without admin rights can re-home the
    // editor instead of restarting winnat.
    int editorPort = 9876;
    if (const char* portEnv = std::getenv("CAESURA_EDITOR_PORT")) {
        char* end = nullptr;
        const long parsed = std::strtol(portEnv, &end, 10);
        if (end && *end == '\0' && parsed >= 1 && parsed <= 65535) {
            editorPort = static_cast<int>(parsed);
            fprintf(stderr, "[EditorServer] CAESURA_EDITOR_PORT override: %d\n", editorPort);
        } else {
            fprintf(stderr, "[EditorServer] [ERROR] CAESURA_EDITOR_PORT is invalid ('%s'); using default 9876\n", portEnv);
        }
    }
    if (!editor.start(editorPort)) {
        editor.setDispatcher({});
        dispatcher->close();
        engine.shutdown();
        return false;
    }

    engine.run([&]() { dispatcher->pump(); });

    dispatcher->close();
    editor.setDispatcher({});
    editor.stop();
    engine.shutdown();
    return true;
}

} // namespace

// Track M fix (device-found, round 39): SDL3's Android JNI glue resolves
// the entry via dlsym with the C name "SDL_main". SDL_main.h only renames
// main -> SDL_main when included (this TU does not include SDL.h), so the
// rename is applied explicitly for Android; extern "C" keeps the symbol
// unmangled (C++ linkage would mangle it and nativeRunMain fails with
// "Couldn't find function SDL_main"). Desktop keeps SDL_MAIN_HANDLED and
// is unaffected.
#if defined(__ANDROID__)
#include <android/log.h>
#define main SDL_main
#define CAESURA_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "CaesuraAmeKAG", __VA_ARGS__)
#define CAESURA_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "CaesuraAmeKAG", __VA_ARGS__)
#else
#define CAESURA_LOGI(...) ((void)0)
#define CAESURA_LOGE(...) ((void)0)
#endif
extern "C" int main(int argc, char* argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    CAESURA_LOGI("[main] Starting Caesura (AmeKAG)...");
#if defined(__ANDROID__)
    caesura_tee_stderr();
#endif
    fprintf(stderr, "[main] Starting Caesura (AmeKAG)...\n");

    // -- Parse CLI flags -------------------------------------------------
    bool headless = false;
    bool editorMode = false;
    bool editorStdio = false;
    std::string resourceRoot;
    bool rootExplicit = false;
    Caesura::ArchiveTrustMode archiveTrustMode = Caesura::ArchiveTrustMode::Compatible;
    bool archiveTrustExplicit = false;
    std::filesystem::path publisherKeyPath;
    // Explicit opt-out of the default-deny editor auth gate. Deliberately a
    // FLAG rather than a token: it carries no secret, so argv exposure is fine.
    bool editorInsecure = false;
    // Optional GPU backend override: --backend <opengl|vulkan|dx11|dx12|metal|webgpu>
    std::string renderBackend;
    // Optional deterministic frame limit: --frames N (GPU smoke runs; 0 = unlimited)
    uint32_t frameLimit = 0;
    // Optional demo/video export: --export-replay <replay.json> drives the
    // recorded input while each rendered frame is written as PNG into
    // --export-dir (default export_out). Bounded by --frames N.
    std::string exportReplayFile;
    std::string exportDir = "export_out";
    // All-platform default render resolution is 1920x1080 (games are
    // authored for that canvas; the device window adapts). --resolution
    // overrides per launch.
    int resW = 1920, resH = 1080;
    // Editor auth token comes from the environment, not argv: argv is
    // world-readable via /proc/<pid>/cmdline on Linux, so a CLI flag would
    // not protect against other local users. Set CAESURA_EDITOR_TOKEN to
    // require a bearer token on every HTTP editor request.
    const char* envToken = std::getenv("CAESURA_EDITOR_TOKEN");
    std::string editorToken = envToken ? envToken : "";
    // --help / -h: print usage to stdout and exit 0 before any engine
    // initialization (TTFV dry-run audit P1 -- a bare `--help` must work
    // from any working directory with zero side effects).
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            const std::string argv0 = argv[0] ? argv[0] : "CaesuraAmeKAG";
            const size_t lastSlash = argv0.find_last_of("/\\");
            const std::string prog = lastSlash == std::string::npos ? argv0 : argv0.substr(lastSlash + 1);
            printf("Usage: %s [options]\n", prog.c_str());
            printf("\n");
            printf("Caesura (AmeKAG) -- cross-platform visual novel engine.\n");
            printf("\n");
            printf("Options:\n");
            printf("  --resource-root <dir> resource directory containing assets/\n");
            printf("  --carc-trust <mode>   CARC policy: compatible (default) or pinned\n");
            printf("  --carc-public-key <f> 32 raw public-key bytes; requires --carc-trust pinned\n");
            printf("  --headless            run without a GPU window (headless/stdio mode)\n");
            printf("  --editor              run the HTTP editor server (127.0.0.1:9876, hidden GPU window)\n");
            printf("  --editor-stdio        run the stdin/stdout JSON-RPC editor transport\n");
            printf("  --editor-insecure     disable the default-deny editor auth gate (loud warning; use at your own risk)\n");
            printf("  --backend <name>      GPU backend override (opengl|vulkan|dx11|dx12|metal|webgpu)\n");
            printf("  --frames <N>          deterministic frame limit (0 = unlimited)\n");
            printf("  --resolution <WxH>    render canvas size (default 1920x1080)\n");
            printf("  --export-replay <f>   replay a recorded input JSON while exporting frames\n");
            printf("  --export-dir <dir>    frame export directory (default export_out)\n");
            printf("  --help, -h            print this help and exit\n");
            printf("\n");
            printf("Environment:\n");
            printf("  CAESURA_EDITOR_TOKEN  bearer token for the HTTP editor (default-deny; unset = auto-generated)\n");
            return 0;
        }
    }
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        const bool takesValue = arg == "--resource-root" || arg == "--carc-trust" ||
            arg == "--carc-public-key" || arg == "--backend" || arg == "--frames" ||
            arg == "--export-replay" || arg == "--export-dir" || arg == "--resolution";
        if (takesValue && (i + 1 >= argc || argv[i + 1][0] == '\0' ||
                           std::string(argv[i + 1]).rfind("--", 0) == 0)) {
            fprintf(stderr, "[main] ERROR: %s requires a value.\n", arg.c_str());
            return 1;
        }
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--editor") {
            editorMode = true;
        } else if (arg == "--editor-insecure") {
            editorInsecure = true;
        } else if (arg == "--editor-stdio") {
            editorMode = true;
            editorStdio = true;
        } else if (arg == "--resource-root") {
            const std::string value = argv[++i];
            if (!rootExplicit) resourceRoot = value;
            rootExplicit = true;
        } else if (arg == "--carc-trust") {
            const std::string value = argv[++i];
            if (value != "compatible" && value != "pinned") {
                fprintf(stderr, "[main] ERROR: Invalid --carc-trust value: %s\n", value.c_str());
                return 1;
            }
            const auto mode = value == "pinned" ? Caesura::ArchiveTrustMode::PinnedPublisher
                                                 : Caesura::ArchiveTrustMode::Compatible;
            if (archiveTrustExplicit && mode != archiveTrustMode) {
                fprintf(stderr, "[main] ERROR: Conflicting --carc-trust options.\n");
                return 1;
            }
            archiveTrustMode = mode;
            archiveTrustExplicit = true;
        } else if (arg == "--carc-public-key") {
            std::filesystem::path value;
            if (!archivePublisherKeyPath(argc, argv, i, value)) return 1;
            ++i;
            if (!publisherKeyPath.empty() && value.native() != publisherKeyPath.native()) {
                fprintf(stderr, "[main] ERROR: Conflicting --carc-public-key options.\n");
                return 1;
            }
            publisherKeyPath = std::move(value);
        } else if (arg == "--backend" && i + 1 < argc) {
            renderBackend = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            char* end = nullptr;
            const long v = strtol(argv[++i], &end, 10);
            if (end && *end == '\0' && v > 0 && v <= 1000000L) {
                frameLimit = static_cast<uint32_t>(v);
            } else {
                fprintf(stderr, "Invalid --frames value: %s\n", argv[i]);
                return 1;
            }
        } else if (arg == "--export-replay" && i + 1 < argc) {
            exportReplayFile = argv[++i];
        } else if (arg == "--export-dir" && i + 1 < argc) {
            exportDir = argv[++i];
        } else if (arg == "--resolution" && i + 1 < argc) {
            // --resolution WxH (e.g. 1920x1080 / 1280x720): engine render
            // canvas size, any window adapts. All-platform default is 1920x1080.
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
                resW = w; resH = h;
            } else {
                fprintf(stderr, "Invalid --resolution value: %s\n", argv[i]);
                return 1;
            }
        } else {
            fprintf(stderr, "[main] ERROR: Unknown command-line option: %s\n", arg.c_str());
            return 1;
        }
    }

    if (archiveTrustMode == Caesura::ArchiveTrustMode::PinnedPublisher &&
        publisherKeyPath.empty()) {
        fprintf(stderr, "[main] ERROR: --carc-public-key is required for --carc-trust pinned.\n");
        return 1;
    }
    std::optional<Caesura::carc::ArchivePublicKey> archivePublisherKey;
    if (!publisherKeyPath.empty()) {
        if (!archiveTrustExplicit || archiveTrustMode != Caesura::ArchiveTrustMode::PinnedPublisher) {
            fprintf(stderr, "[main] ERROR: --carc-public-key requires explicit --carc-trust pinned.\n");
            return 1;
        }
        Caesura::carc::ArchivePublicKey key{};
        // Read once before changing CWD. Relative host paths are never resolved
        // against the game's resource root or an automatically selected sidecar.
        if (!readArchivePublisherKey(publisherKeyPath, key)) return 1;
        archivePublisherKey = key;
    }

    // Resource root resolution (Track I3 / Android R6):
    //   1. --resource-root <dir>  (explicit; iOS launcher passes
    //      SDL_GetBasePath(), the Android JNI wrapper its install dir)
    //   2. CAESURA_RESOURCE_ROOT env var
    //   3. legacy walk-up: relative to CWD, walk parents until assets/ is
    //      found and chdir there (unchanged backward-compatible behavior).
    {
        namespace fs = std::filesystem;
        const char* envRoot = std::getenv("CAESURA_RESOURCE_ROOT");
        if (resourceRoot.empty() && envRoot && *envRoot) {
            resourceRoot = envRoot;
            rootExplicit = true;
        }

        fs::path target;
        if (rootExplicit) {
            target = fs::path(resourceRoot);
            if (!fs::is_directory(target) || !fs::is_directory(target / "assets")) {
                fprintf(stderr, "[main] ERROR: --resource-root has no assets/ directory: %s\n",
                        resourceRoot.c_str());
                return 1;
            }
        } else {
            fs::path probe = fs::current_path();
            for (int i = 0; i < 6; ++i) {
                if (fs::exists(probe / "assets") && fs::is_directory(probe / "assets")) {
                    target = probe;
                    break;
                }
                probe = probe.parent_path();
            }
            if (target.empty()) target = fs::current_path();
        }

        std::error_code ec;
        fs::current_path(target, ec);
        if (!ec) {
            fprintf(stderr, "[main] Working directory: %s\n", target.string().c_str());
        }
    }

    // Demo/video export needs a real GPU window (bgfx readback): --headless
    // uses NullRenderDevice and cannot capture frames.
    if (!exportReplayFile.empty() && headless) {
        fprintf(stderr, "[main] --export-replay requires a GPU window;"
                        " ignoring --headless.\n");
        headless = false;
    }

    printf("============================================\n");
    printf("  Caesura (AmeKAG) v1.0.0\n");
    printf("  Cross-platform Visual Novel Engine\n");
    printf("  SDL3 + bgfx + SoLoud + Lua\n");
    if (headless) printf("  [HEADLESS MODE]\n");
    printf("============================================\n\n");

    Caesura::EngineConfig config;
    config.title      = "Caesura (AmeKAG)";
    config.width      = resW;
    config.height     = resH;
    config.headless   = headless;
    config.editorMode = editorMode;
    config.enableDebugger = headless || editorMode;
    config.renderBackend  = renderBackend.empty() ? nullptr : renderBackend.c_str();
    config.frameLimit     = frameLimit;
    config.exportReplayFile = exportReplayFile;
    config.exportDir        = exportDir;
    config.archiveTrustMode = archiveTrustMode;
    config.archivePublisherKey = archivePublisherKey;

    // Create GPU-mode implementations here; Engine supplies safe defaults otherwise.
    if (!headless || editorMode) {
        config.platform = new Caesura::SDL3PlatformBackend();
        config.render   = new Caesura::BgfxRenderDevice();
        config.audio    = new Caesura::SoLoudAudioEngine();
        config.miniGame = new Caesura::BgfxMiniGameBackend();
    }

    CAESURA_LOGI("[main] EngineConfig assembled; constructing Engine...");
    Caesura::Engine engine(std::move(config));

    if (!engine.init()) {
        CAESURA_LOGE("[main] ENGINE INIT FAILED");
        fprintf(stderr, "Failed to initialize engine.\n");
        return 1;
    }
    CAESURA_LOGI("[main] Engine init OK");

    // -- Editor mode: hidden window + owner-thread RPC dispatcher ----------
    if (editorMode) {
        fprintf(stderr, editorStdio
            ? "[main] Editor mode: JSON-RPC on stdin/stdout (GPU enabled)\n"
            : "[main] Editor mode: HTTP editor (default 127.0.0.1:9876; GPU enabled)\n");

        std::string scriptDir = Caesura::discoverStartupScriptDir();
        Caesura::configureStartupLuaPath(engine.lua().state(), scriptDir);
        engine.lua().loadScript((scriptDir + "config.lua").c_str());
        engine.lua().loadScript((scriptDir + "kag/init.lua").c_str());
        engine.lua().lockdownScriptEnv();

        const bool editorOk = editorStdio
            ? runStdioRpc(engine)
            : runHttpEditor(engine, editorToken, editorInsecure);
        if (!editorOk || engine.hasRenderFailure()) return 1;
        printf("Caesura (AmeKAG) shut down cleanly.\n");
        return 0;
    }

    // -- Headless mode: stdin/stdout JSON-RPC -----------------------------
    if (headless) {
        fprintf(stderr, "[main] Headless mode: JSON-RPC on stdin/stdout\n");

        // Load minimal config for Lua VM
        std::string scriptDir = Caesura::discoverStartupScriptDir();
        Caesura::configureStartupLuaPath(engine.lua().state(), scriptDir);
        engine.lua().loadScript((scriptDir + "config.lua").c_str());
        engine.lua().loadScript((scriptDir + "kag/init.lua").c_str());
        engine.lua().lockdownScriptEnv();

        if (!runStdioRpc(engine)) return 1;
        if (engine.hasRenderFailure()) return 1;
        printf("Caesura (AmeKAG) shut down cleanly.\n");
        return 0;
    }

    std::string scriptDir = Caesura::discoverStartupScriptDir();
    lua_State* L = engine.lua().state();
    Caesura::configureStartupLuaPath(L, scriptDir);

    // Load config first (backend selection happens here)
    engine.lua().loadScript((scriptDir + "config.lua").c_str());

    // [10.2.57] Apply dev mode to placeholder texture
    Caesura::applyDevModeToTextureManager(L);

    // Load KAG init (loads all Lua libraries)
    if (!engine.lua().loadScript((scriptDir + "kag/init.lua").c_str())) {
        fprintf(stderr, "Warning: Failed to load KAG init.\n");
    }

    // One-time startup loads are separate budget windows from the per-frame
    // game loop: reset the instruction budget before parsing the entry scene.
    engine.lua().resetInstructionBudget();

    // Load main game logic (entry point from config) [10.2.30]
    std::string entryScript = "game_logic.lua";
    if (L) {
        lua_getglobal(L, "config");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "entry_script");
            if (lua_isstring(L, -1)) {
                entryScript = lua_tostring(L, -1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    if (!engine.lua().loadScript((scriptDir + entryScript).c_str())) {
        fprintf(stderr, "Warning: Failed to load game_logic.lua.\n");
        return 1;
    }

    // Push _CAESURA_CONFIG global for sandbox to read
    lua_getglobal(L, "config");  // config table loaded by config.lua
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "dev_mode");
        bool devMode = lua_toboolean(L, -1);
        lua_pop(L, 1);
        
        lua_newtable(L);
        lua_pushboolean(L, devMode ? 1 : 0);
        lua_setfield(L, -2, "dev_mode");
        lua_setglobal(L, "_CAESURA_CONFIG");
        printf("[main] _CAESURA_CONFIG.dev_mode = %s\n", devMode ? "true" : "false");
    }
    lua_pop(L, 1);


    // C3+W8: lockdown script env after ALL scripts are preloaded
    engine.lua().lockdownScriptEnv();

    // Demo/video export: activate replay playback before the main loop.
    // kag_runner.update() fires the recorded events (same on_click path);
    // Engine::run writes one PNG per frame into the export dir.
    if (!exportReplayFile.empty()) {
        bool directoryReady = false;
        try {
            const auto exportPath = std::filesystem::u8path(exportDir);
            std::error_code error;
            std::filesystem::create_directories(exportPath, error);
            if (!error) directoryReady = std::filesystem::is_directory(exportPath, error);
            if (!directoryReady) {
                fprintf(stderr, "[main] Cannot prepare export directory '%s': %s\n",
                        exportDir.c_str(), error ? error.message().c_str() : "not a directory");
            }
        } catch (const std::exception& error) {
            fprintf(stderr, "[main] Cannot prepare export directory '%s': %s\n",
                    exportDir.c_str(), error.what());
        }
        if (!directoryReady) {
            engine.shutdown();
            return 1;
        }
        lua_State* exL = engine.lua().state();
        if (exL) {
            lua_getglobal(exL, "require");
            lua_pushstring(exL, "replay");
            if (lua_pcall(exL, 1, 1, 0) == LUA_OK && lua_istable(exL, -1)) {
                lua_getfield(exL, -1, "set_mode");
                if (lua_isfunction(exL, -1)) {
                    lua_pushvalue(exL, -2);  // self
                    lua_pushstring(exL, "playback");
                    lua_pushstring(exL, exportReplayFile.c_str());
                    if (lua_pcall(exL, 3, 0, 0) != LUA_OK) {
                        fprintf(stderr, "[main] replay set_mode failed: %s\n",
                                lua_tostring(exL, -1) ? lua_tostring(exL, -1)
                                                      : "unknown");
                    }
                }
            }
            lua_settop(exL, 0);
        }
        printf("[main] Export mode: replay %s -> %s (frames=%u)\n",
               exportReplayFile.c_str(), exportDir.c_str(), frameLimit);
    }

    engine.run();
    engine.shutdown();

    if (engine.hasRenderFailure()) return 1;
    printf("Caesura (AmeKAG) shut down cleanly.\n");
    return 0;
}
