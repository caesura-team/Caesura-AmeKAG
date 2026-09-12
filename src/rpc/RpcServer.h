#pragma once
#include "api/IRpcServer.h"
// ===========================================================================
//  Caesura (AmeKAG) -- RpcServer
//  stdin/stdout JSON-RPC for IDE/Editor integration.
//  Zero network, zero ports, zero config. Just pipe JSON lines.
// ===========================================================================

#include <functional>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>

namespace Caesura {

class StdioRpcOutput;

enum class RpcLineReadResult {
    Line,
    EndOfInput,
    Cancelled,
};

struct RpcLineSource {
    std::function<RpcLineReadResult(std::string&)> readLine;
    std::function<void()> cancel;
};

class RpcServer : public IRpcServer {
public:
    RpcServer();
    explicit RpcServer(RpcLineSource lineSource);
    RpcServer(RpcLineSource lineSource, int outputDescriptor);
    ~RpcServer() override;

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    // Run the JSON-RPC loop (blocks until stop or stdin EOF)
    void run() override;

    // Signal stop from outside
    void stop() override;
    bool isRunning() const override { return m_running.load(); }
    bool outputReady() const noexcept;

    void setDispatcher(std::shared_ptr<IRpcDispatcher> dispatcher) override;

    // Process one JSON-RPC request without owning stdin/stdout.
    std::string processRequestLine(const std::string& jsonLine);

    // Push a log entry to the output stream
    void pushLog(const std::string& level, const std::string& message) override;

private:
    std::string handlePing(int id);
    std::string handleRun(int id, const std::string& script);
    std::string handleStop(int id);
    std::string handleLogs(int id);
    std::string handleAssets(int id, const std::string& type);
    std::string handleEval(int id, const std::string& code);
    std::string handleGetState(int id);
    std::string handleSmaValidate(int id, const std::string& path);
    std::string handlePick(int id, int x, int y);
    std::string handleSmaSave(int id, const std::string& path, const std::string& content);
    std::string handleStats(int id);
    std::string handleGetFrame(int id, int w, int h);
    std::string handleReload(int id);
    std::string handleDebugAction(int id, RpcRequest request);
    std::string handleInspectLocal(int id, int frame, const std::string& name);
    std::string handleInspectGlobal(int id, const std::string& name);
    std::string handleGetDebugState(int id);

    // KAG scene-level debugger: dispatch helper (defined in RpcServer.cpp).
    std::string handleKagDebug(int id, const std::string& scene,
                               const std::string& cmd, int line,
                               const std::string& action,
                               const std::string& scope);

    RpcReply dispatchRequest(RpcRequest request) const;
    std::string replyError(int id, const RpcReply& reply) const;

    // Enqueue a JSON line; a stalled protocol consumer never blocks producers.
    void writeLine(const std::string& json);

    // Escape string for JSON
    static std::string jsonEscape(const std::string& s);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::unique_ptr<StdioRpcOutput> m_output;
    mutable std::mutex m_dispatcherMutex;
    std::shared_ptr<IRpcDispatcher> m_dispatcher;
    RpcLineSource m_lineSource;
};

} // namespace Caesura
