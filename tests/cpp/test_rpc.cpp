// test_rpc.cpp - RPC module tests (RpcServer, EditorServer)
#include "doctest.h"
#include "rpc/RpcServer.h"
#include "rpc/EditorServer.h"
#include "rpc/api/IRpcServer.h"
#include "rpc/api/IEditorServer.h"
#include "rpc/api/IRpcDispatcher.h"
#include "rpc/ConstantTime.h"
#include "rpc/ProjectContext.h"
#include "rpc/services/ProjectService.h"
#include "rpc/services/PackagingService.h"
#include "entry/Engine.h"
#include "script/vm/LuaManager.h"
#include <httplib.h>
#include <nlohmann_json.hpp>

// EditorServer bind-failure regression tests (t88) need a held loopback port.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <lua.h>
}

using namespace Caesura;

namespace {

class RecordingRpcDispatcher final : public IRpcDispatcher {
public:
    using Handler = std::function<RpcReply(const RpcRequest&)>;

    explicit RecordingRpcDispatcher(Handler handler = {})
        : m_handler(std::move(handler)) {}

    RpcReply dispatch(const RpcRequest& request) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_requests.push_back(request);
        }
        if (m_handler) return m_handler(request);
        return {RpcReplyStatus::Ok, {}, {}, {}};
    }

    std::size_t requestCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requests.size();
    }

    RpcRequest requestAt(std::size_t index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requests.at(index);
    }

private:
    Handler m_handler;
    mutable std::mutex m_mutex;
    std::vector<RpcRequest> m_requests;
};

// [Sprint 1 t4] The HTTP editor is default-deny: start() requires a bearer
// token and generates one when none was configured. Route/transport tests
// below are not about authentication, so they opt into the documented escape
// hatch EXPLICITLY (--editor-insecure equivalent) instead of relying on an
// open gate. Auth behavior itself is covered by the dedicated cases at the
// end of section 3.
bool startOpen(EditorServer& es) {
    es.setInsecureNoAuth(true);
    return es.start(0);
}

// Holds a listening loopback socket on an OS-assigned port for the
// "port already in use" editor tests (t88). Windows: SO_EXCLUSIVEADDRUSE makes
// the reservation unbreachable -- deterministically the same bind-failure class
// as the t84 dynamic-exclusion WSAEACCES case; POSIX: a plain listening socket
// refuses any second bind.
class HeldLoopbackPort {
public:
    HeldLoopbackPort() {
#ifdef _WIN32
        static const bool wsa = [] {
            WSADATA d{};
            return WSAStartup(MAKEWORD(2, 2), &d) == 0;
        }();
        (void)wsa;
        m_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_sock == INVALID_SOCKET) return;
        BOOL exclusive = TRUE;
        ::setsockopt(m_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
        m_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_sock < 0) return;
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
        const int addrLen = static_cast<int>(sizeof(addr));
#else
        const socklen_t addrLen = static_cast<socklen_t>(sizeof(addr));
#endif
        if (::bind(m_sock, reinterpret_cast<sockaddr*>(&addr), addrLen) != 0 ||
            ::listen(m_sock, 1) != 0) {
            closeIfOpen();
            return;
        }
#ifdef _WIN32
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (::getsockname(m_sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            closeIfOpen();
            return;
        }
        m_port = static_cast<int>(ntohs(addr.sin_port));
    }
    ~HeldLoopbackPort() { closeIfOpen(); }
    HeldLoopbackPort(const HeldLoopbackPort&) = delete;
    HeldLoopbackPort& operator=(const HeldLoopbackPort&) = delete;
    bool valid() const { return m_valid; }
    int port() const { return m_port; }
private:
    void closeIfOpen() {
#ifdef _WIN32
        if (m_sock != INVALID_SOCKET) {
            ::closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
#else
        if (m_sock >= 0) {
            ::close(m_sock);
            m_sock = -1;
        }
#endif
        m_valid = false;
    }
#ifdef _WIN32
    SOCKET m_sock = INVALID_SOCKET;
#else
    int m_sock = -1;
#endif
    bool m_valid = true;
    int m_port = 0;
};

RpcReply successReply(const RpcRequest& request) {
    if (std::holds_alternative<RpcStatusRequest>(request.payload)) {
        return {RpcReplyStatus::Ok, {}, {}, RpcStatusResult{true}};
    }
    if (std::holds_alternative<RpcEvaluateRequest>(request.payload)) {
        return {RpcReplyStatus::Ok, {}, {}, RpcEvaluateResult{"42"}};
    }
    if (std::holds_alternative<RpcGetStateRequest>(request.payload)) {
        RpcStateResult state{"chapter-1"};
        state.currentCmd = "[ch]";
        return {RpcReplyStatus::Ok, {}, {}, state};
    }
    if (std::holds_alternative<RpcCaptureFrameRequest>(request.payload)) {
        return {RpcReplyStatus::Ok, {}, {}, RpcFrameResult{"ZmFrZS1wbmc="}};
    }
    if (const auto* load = std::get_if<RpcLoadAnimationRequest>(&request.payload)) {
        return {RpcReplyStatus::Ok, {}, {}, RpcAnimationResult{17, load->modelPath}};
    }
    if (std::holds_alternative<RpcInspectLocalRequest>(request.payload) ||
        std::holds_alternative<RpcInspectGlobalRequest>(request.payload)) {
        return {RpcReplyStatus::Ok, {}, {}, RpcInspectionResult{"value"}};
    }
    if (std::holds_alternative<RpcGetDebugStateRequest>(request.payload)) {
        return {RpcReplyStatus::Ok, {}, {}, RpcDebugStateResult{
            RpcDebugRunState::Paused, "scripts/chapter.lua", 27, 91, 2}};
    }
    return {RpcReplyStatus::Ok, {}, {}, {}};
}

class ControlledLineSource {
public:
    RpcLineReadResult readLine(std::string& line) {
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_readCalls;
        m_reading = true;
        m_changed.notify_all();
        m_changed.wait(lock, [this]() {
            return m_cancelled || m_endOfInput || !m_lines.empty();
        });

        if (m_cancelled) return RpcLineReadResult::Cancelled;
        if (!m_lines.empty()) {
            line = std::move(m_lines.front());
            m_lines.pop_front();
            return RpcLineReadResult::Line;
        }
        return RpcLineReadResult::EndOfInput;
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_cancelCalls;
        m_cancelled = true;
        m_changed.notify_all();
    }

    void enqueue(std::string line) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.push_back(std::move(line));
        m_changed.notify_all();
    }

    void finish() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_endOfInput = true;
        m_changed.notify_all();
    }

    bool waitUntilReading(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [this]() { return m_reading; });
    }

    int readCalls() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_readCalls;
    }

    int cancelCalls() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancelCalls;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::string> m_lines;
    bool m_cancelled = false;
    bool m_endOfInput = false;
    bool m_reading = false;
    int m_readCalls = 0;
    int m_cancelCalls = 0;
};

RpcLineSource lineSourceFor(const std::shared_ptr<ControlledLineSource>& source) {
    return {
        [source](std::string& line) { return source->readLine(line); },
        [source]() { source->cancel(); },
    };
}

class ScopedModelFiles {
public:
    bool add(const std::string& name) {
        std::error_code error;
        if (!std::filesystem::exists(m_root, error)) {
            if (!std::filesystem::create_directories(m_root, error) || error) {
                return false;
            }
            m_createdRoot = true;
        }

        const std::filesystem::path path = m_root / name;
        if (std::filesystem::exists(path, error) || error) return false;

        std::ofstream output(path, std::ios::binary);
        output.put('x');
        output.close();
        if (!output) {
            std::filesystem::remove(path, error);
            return false;
        }

        m_files.push_back(path);
        return true;
    }

    ~ScopedModelFiles() {
        std::error_code error;
        for (const auto& path : m_files) {
            std::filesystem::remove(path, error);
            error.clear();
        }
        if (m_createdRoot) std::filesystem::remove(m_root, error);
    }

private:
    std::filesystem::path m_root{"models"};
    std::vector<std::filesystem::path> m_files;
    bool m_createdRoot = false;
};

// Creates disposable files under assets/<subdir>/ in the test CWD (which the
// /api/assets handler scans relative to the process CWD) and cleans them up
// on destruction. Mirrors ScopedModelFiles.
class ScopedAssetFiles {
public:
    bool add(const std::string& subdir, const std::string& name) {
        std::error_code error;
        // Assets live at the repo root (AssetService uses the sourceRoot),
        // not the CWD -- the fixture must match the same contract.
        const auto dir = std::filesystem::path(CAESURA_SOURCE_DIR) / "assets" / subdir;
        if (!std::filesystem::exists(dir, error)) {
            if (!std::filesystem::create_directories(dir, error) || error) {
                return false;
            }
            m_createdDirs.push_back(dir);
        }

        const auto path = dir / name;
        if (std::filesystem::exists(path, error) || error) return false;
        std::ofstream output(path, std::ios::binary);
        output.put('x');
        output.close();
        if (!output) {
            std::filesystem::remove(path, error);
            return false;
        }
        m_files.push_back(path);
        return true;
    }

    ~ScopedAssetFiles() {
        std::error_code error;
        for (const auto& path : m_files) {
            std::filesystem::remove(path, error);
            error.clear();
        }
        for (auto it = m_createdDirs.rbegin(); it != m_createdDirs.rend(); ++it) {
            std::filesystem::remove(*it, error);
            error.clear();
        }
    }

private:
    std::vector<std::filesystem::path> m_files;
    std::vector<std::filesystem::path> m_createdDirs;
};

} // namespace

static_assert(std::is_abstract_v<IRpcDispatcher>);
static_assert(std::is_polymorphic_v<IRpcDispatcher>);
static_assert(std::has_virtual_destructor_v<IRpcDispatcher>);

TEST_CASE("RpcLoadAnimationRequest defaults to a visible origin transform") {
    const RpcLoadAnimationRequest request{"models/hero.png"};

    CHECK(request.modelPath == "models/hero.png");
    CHECK(request.x == doctest::Approx(0.0f));
    CHECK(request.y == doctest::Approx(0.0f));
    CHECK(request.scale == doctest::Approx(1.0f));
    CHECK(request.show);
}

TEST_CASE("RpcServer::construct and stop without run") {
    RpcServer rpc;
    rpc.stop();
}

TEST_CASE("RpcServer::isRunning defaults false") {
    RpcServer rpc;
    CHECK(rpc.isRunning() == false);
}

TEST_CASE("IRpcServer interface completeness") {
    RpcServer rpc;
    IRpcServer* iface = &rpc;
    CHECK(iface != nullptr);
    CHECK(iface->isRunning() == false);
}

TEST_CASE("RpcServer::stop interrupts a blocked line read") {
    auto source = std::make_shared<ControlledLineSource>();
    RpcServer rpc(lineSourceFor(source));
    std::thread transport([&rpc]() { rpc.run(); });

    REQUIRE(source->waitUntilReading(std::chrono::milliseconds(500)));
    const auto started = std::chrono::steady_clock::now();
    rpc.stop();
    transport.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(elapsed < std::chrono::milliseconds(500));
    CHECK_FALSE(rpc.isRunning());
    CHECK(source->cancelCalls() >= 1);
}

TEST_CASE("RpcServer::stop before run is not reset or lost") {
    auto source = std::make_shared<ControlledLineSource>();
    source->finish();
    RpcServer rpc(lineSourceFor(source));

    rpc.stop();
    rpc.run();

    CHECK_FALSE(rpc.isRunning());
    CHECK(source->readCalls() == 0);
    CHECK(source->cancelCalls() >= 1);
}

TEST_CASE("RpcServer::EOF ends transport after queued requests") {
    auto source = std::make_shared<ControlledLineSource>();
    source->enqueue(R"({"id":21,"method":"getState"})");
    source->finish();
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    RpcServer rpc(lineSourceFor(source));
    rpc.setDispatcher(dispatcher);

    rpc.run();

    CHECK_FALSE(rpc.isRunning());
    REQUIRE(dispatcher->requestCount() == 1);
    CHECK(std::holds_alternative<RpcGetStateRequest>(
        dispatcher->requestAt(0).payload));
}

TEST_CASE("RpcServer::unusable output prevents reading or dispatching mutations") {
    auto source = std::make_shared<ControlledLineSource>();
    source->enqueue(R"({"id":24,"method":"eval","code":"return 42"})");
    source->finish();
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    RpcServer rpc(lineSourceFor(source), -1);
    rpc.setDispatcher(dispatcher);
    CHECK_FALSE(rpc.outputReady());
    rpc.run();
    CHECK_FALSE(rpc.isRunning());
    CHECK(source->readCalls() == 0);
    CHECK(dispatcher->requestCount() == 0);
}

TEST_CASE("RpcServer::RPC stop replies and does not read another request") {
    auto source = std::make_shared<ControlledLineSource>();
    source->enqueue(R"({"id":22,"method":"stop"})");
    source->enqueue(R"({"id":23,"method":"getState"})");
    source->finish();
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    RpcServer rpc(lineSourceFor(source));
    rpc.setDispatcher(dispatcher);

    rpc.run();

    CHECK_FALSE(rpc.isRunning());
    CHECK(source->readCalls() == 1);
    REQUIRE(dispatcher->requestCount() == 1);
    CHECK(std::holds_alternative<RpcStopRequest>(
        dispatcher->requestAt(0).payload));
}

TEST_CASE("Engine headless loop honors Lua quit before event early return") {
    EngineConfig config;
    config.headless = true;
    Engine engine(std::move(config));
    REQUIRE(engine.init());

    lua_State* L = engine.lua().state();
    REQUIRE(L != nullptr);
    lua_pushboolean(L, 1);
    lua_setglobal(L, "_CAESURA_QUIT");

    int ownerTicks = 0;
    engine.run([&]() {
        ++ownerTicks;
        if (ownerTicks > 2) engine.quit();
    });

    CHECK(ownerTicks == 1);
    engine.shutdown();
}

// =============================================================================
// Expanded: RpcServer handlers + EditorServer accessors
// =============================================================================

TEST_CASE("RpcServer::pushLog does not crash") {
    RpcServer rpc;
    rpc.pushLog("info", "test");
    rpc.pushLog("error", "oops");
}

TEST_CASE("RpcServer reports structured unavailable without dispatcher") {
    RpcServer rpc;
    const std::string response = rpc.processRequestLine(
        R"({"id":7,"method":"run","script":"return 1"})");
    CHECK(response.find("\"id\":7") != std::string::npos);
    CHECK(response.find("\"status\":\"unavailable\"") != std::string::npos);
    CHECK(response.find("\"code\":\"dispatcher_unavailable\"") != std::string::npos);
}

TEST_CASE("RpcServer accepts a UTF-8 BOM on the first JSON line") {
    RpcServer rpc;
    const std::string request =
        std::string("\xEF\xBB\xBF") + R"({"id":8,"method":"ping"})";
    const std::string response = rpc.processRequestLine(request);
    CHECK(response.find("\"id\":8") != std::string::npos);
    CHECK(response.find("\"result\":\"ok\"") != std::string::npos);
}

TEST_CASE("EditorServer::port returns set port") {
    EditorServer es;
    // port defaults to 0 until start is called
    CHECK(es.port() == 0);
}

TEST_CASE("EditorServer::construct and stop without start") {
    EditorServer es;
    es.stop();
}

TEST_CASE("EditorServer::start and stop joins its worker") {
    EditorServer es;
    REQUIRE(startOpen(es));
    CHECK(es.isRunning());
    CHECK(es.port() > 0);
    es.stop();
    CHECK_FALSE(es.isRunning());
}

TEST_CASE("EditorServer::isRunning defaults false") {
    EditorServer es;
    CHECK(es.isRunning() == false);
}

TEST_CASE("EditorServer::setWebRoot no-crash") {
    EditorServer es;
    es.setWebRoot("web-editor/dist");
    es.setArchiveWriterFactory({});
    es.setDispatcher({});
}

TEST_CASE("EditorServer::pushLog no-crash") {
    EditorServer es;
    es.pushLog("info", "test log message");
}

TEST_CASE("IEditorServer interface completeness") {
    EditorServer es;
    IEditorServer* iface = &es;
    CHECK(iface != nullptr);
    CHECK(iface->isRunning() == false);
}

TEST_CASE("EditorServer::bind failure is loud (lastError) on an occupied port") {
    // t88 regression: a held 127.0.0.1:<port> socket makes the bind fail
    // (same class as the t84 dynamic-exclusion WSAEACCES case). start() must
    // return false AND publish the failure in lastError() -- the silent
    // "engine exits 1 with zero [EditorServer] output" defect.
    HeldLoopbackPort held;
    REQUIRE(held.valid());
    EditorServer es;
    es.setInsecureNoAuth(true);
    CHECK_FALSE(es.start(held.port()));
    CHECK_FALSE(es.isRunning());
    CHECK(es.port() == 0);
    CHECK_FALSE(es.lastError().empty());
    CHECK(es.lastError().find("failed to bind 127.0.0.1:") != std::string::npos);

    // t89 must-fix regression: a later successful start() on the SAME instance
    // clears the stale failure message (lastError() contract).
    REQUIRE(startOpen(es));
    CHECK(es.isRunning());
    CHECK(es.lastError().empty());
    es.stop();
}

TEST_CASE("EditorServer::start succeeds on a free port with empty lastError") {
    EditorServer es;
    es.setInsecureNoAuth(true);
    REQUIRE(startOpen(es));
    CHECK(es.isRunning());
    CHECK(es.port() > 0);
    CHECK(es.lastError().empty());
    es.stop();
    CHECK_FALSE(es.isRunning());
}

TEST_CASE("RpcServer submits runtime DTOs to dispatcher") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    RpcServer rpc;
    rpc.setDispatcher(dispatcher);

    CHECK(rpc.processRequestLine(
        R"({"id":1,"method":"run","script":"return 9"})")
        .find("\"status\":\"ok\"") != std::string::npos);
    CHECK(rpc.processRequestLine(
        R"({"id":2,"method":"eval","code":"return 6*7"})")
        .find("\"result\":\"42\"") != std::string::npos);
    CHECK(rpc.processRequestLine(
        R"({"id":3,"method":"getFrame","w":640,"h":360})")
        .find("\"frame\":\"ZmFrZS1wbmc=\"") != std::string::npos);
    CHECK(rpc.processRequestLine(
        R"({"id":4,"method":"getState"})")
        .find("\"scene\":\"chapter-1\"") != std::string::npos);
    CHECK(rpc.processRequestLine(
        R"({"id":5,"method":"reload"})")
        .find("\"status\":\"ok\"") != std::string::npos);
    CHECK(rpc.processRequestLine(
        R"({"id":6,"method":"stop"})")
        .find("\"result\":\"ok\"") != std::string::npos);
    CHECK(rpc.processRequestLine(
        R"({"id":7,"method":"getState"})")
        .find("\"current_cmd\":\"[ch]\"") != std::string::npos);

    REQUIRE(dispatcher->requestCount() == 7);
    const auto run = dispatcher->requestAt(0);
    REQUIRE(std::holds_alternative<RpcRunScriptRequest>(run.payload));
    CHECK(std::get<RpcRunScriptRequest>(run.payload).script == "return 9");

    const auto frame = dispatcher->requestAt(2);
    REQUIRE(std::holds_alternative<RpcCaptureFrameRequest>(frame.payload));
    CHECK(std::get<RpcCaptureFrameRequest>(frame.payload).width == 640);
    CHECK(std::get<RpcCaptureFrameRequest>(frame.payload).height == 360);
    CHECK(std::holds_alternative<RpcReloadScriptsRequest>(
        dispatcher->requestAt(4).payload));
    CHECK(std::holds_alternative<RpcStopRequest>(
        dispatcher->requestAt(5).payload));
    CHECK(std::holds_alternative<RpcGetStateRequest>(
        dispatcher->requestAt(6).payload));
}

TEST_CASE("RpcServer maps debugger JSON methods to self-contained DTOs") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    RpcServer rpc;
    rpc.setDispatcher(dispatcher);

    rpc.processRequestLine(
        R"({"id":1,"method":"setBreakpoint","source":"scripts/a.lua","line":12})");
    rpc.processRequestLine(
        R"({"id":2,"method":"removeBreakpoint","source":"scripts/a.lua","line":12})");
    rpc.processRequestLine(R"({"id":3,"method":"clearBreakpoints"})");
    rpc.processRequestLine(R"({"id":4,"method":"continue","pauseId":91})");
    rpc.processRequestLine(R"({"id":5,"method":"stepInto","pauseId":91})");
    rpc.processRequestLine(R"({"id":6,"method":"stepOver","pauseId":91})");
    rpc.processRequestLine(R"({"id":7,"method":"stepOut","pauseId":91})");
    CHECK(rpc.processRequestLine(
        R"({"id":8,"method":"inspectLocal","frame":2,"name":"speaker"})")
        .find("\"value\":\"value\"") != std::string::npos);
    rpc.processRequestLine(
        R"({"id":9,"method":"inspectGlobal","name":"chapter"})");
    const std::string state = rpc.processRequestLine(
        R"({"id":10,"method":"getDebugState"})");

    REQUIRE(dispatcher->requestCount() == 10);
    const auto set = dispatcher->requestAt(0);
    REQUIRE(std::holds_alternative<RpcSetBreakpointRequest>(set.payload));
    CHECK(std::get<RpcSetBreakpointRequest>(set.payload).source == "scripts/a.lua");
    CHECK(std::get<RpcSetBreakpointRequest>(set.payload).line == 12);
    CHECK(std::holds_alternative<RpcRemoveBreakpointRequest>(
        dispatcher->requestAt(1).payload));
    CHECK(std::holds_alternative<RpcClearBreakpointsRequest>(
        dispatcher->requestAt(2).payload));

    const RpcDebugResumeMode modes[] = {
        RpcDebugResumeMode::Continue,
        RpcDebugResumeMode::StepInto,
        RpcDebugResumeMode::StepOver,
        RpcDebugResumeMode::StepOut,
    };
    for (std::size_t i = 0; i < 4; ++i) {
        const auto request = dispatcher->requestAt(i + 3);
        REQUIRE(std::holds_alternative<RpcDebugResumeRequest>(request.payload));
        const auto& resume = std::get<RpcDebugResumeRequest>(request.payload);
        CHECK(resume.pauseId == 91);
        CHECK(resume.mode == modes[i]);
    }

    const auto local = dispatcher->requestAt(7);
    REQUIRE(std::holds_alternative<RpcInspectLocalRequest>(local.payload));
    CHECK(std::get<RpcInspectLocalRequest>(local.payload).frame == 2);
    CHECK(std::get<RpcInspectLocalRequest>(local.payload).name == "speaker");
    CHECK(std::holds_alternative<RpcInspectGlobalRequest>(
        dispatcher->requestAt(8).payload));
    CHECK(std::holds_alternative<RpcGetDebugStateRequest>(
        dispatcher->requestAt(9).payload));
    CHECK(state.find("\"state\":\"paused\"") != std::string::npos);
    CHECK(state.find("\"pauseId\":91") != std::string::npos);
}

TEST_CASE("RpcServer preserves stale pause rejection from dispatcher") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(
        [](const RpcRequest&) {
            return RpcReply{RpcReplyStatus::InvalidRequest, "stale_pause_id",
                "Pause generation is stale", {}};
        });
    RpcServer rpc;
    rpc.setDispatcher(dispatcher);

    const std::string response = rpc.processRequestLine(
        R"({"id":44,"method":"continue","pauseId":8})");
    CHECK(response.find("\"status\":\"invalid_request\"") != std::string::npos);
    CHECK(response.find("\"code\":\"stale_pause_id\"") != std::string::npos);
    CHECK(response.find("Pause generation is stale") != std::string::npos);
}

TEST_CASE("RpcServer preserves dispatcher shutdown unavailable reply") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(
        [](const RpcRequest&) {
            return RpcReply{RpcReplyStatus::Unavailable, "engine_closing",
                "Engine command intake is closed", {}};
        });
    RpcServer rpc;
    rpc.setDispatcher(dispatcher);

    const std::string response = rpc.processRequestLine(
        R"({"id":45,"method":"getDebugState"})");
    CHECK(response.find("\"status\":\"unavailable\"") != std::string::npos);
    CHECK(response.find("\"code\":\"engine_closing\"") != std::string::npos);
    CHECK(response.find("Engine command intake is closed") != std::string::npos);
}

TEST_CASE("EditorServer HTTP worker submits DTOs through dispatcher") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    EditorServer es;
    es.setDispatcher(dispatcher);
    REQUIRE(startOpen(es));

    httplib::Client client("127.0.0.1", es.port());
    auto run = client.Post("/api/run", "return 3", "text/plain");
    REQUIRE(run);
    CHECK(run->status == 200);
    CHECK(run->body.find("\"status\":\"ok\"") != std::string::npos);

    auto defaultLoad = client.Post("/api/live2d/load",
        R"({"modelPath":"models/haru.model.json"})", "application/json");
    REQUIRE(defaultLoad);
    CHECK(defaultLoad->status == 200);
    CHECK(defaultLoad->body.find("\"modelId\":17") != std::string::npos);

    auto positionedLoad = client.Post("/api/live2d/load",
        R"({"modelPath":"models/hero \"alt\".PNG","x":-12.5,"y":24,"scale":1.25,"show":false})",
        "application/json");
    REQUIRE(positionedLoad);
    CHECK(positionedLoad->status == 200);
    const nlohmann::json positionedResponse =
        nlohmann::json::parse(positionedLoad->body);
    CHECK(positionedResponse.at("name").get<std::string>() ==
        "models/hero \"alt\".PNG");

    auto reload = client.Post("/api/reload", "", "application/json");
    REQUIRE(reload);
    CHECK(reload->status == 200);
    es.stop();

    REQUIRE(dispatcher->requestCount() == 4);
    const auto runRequest = dispatcher->requestAt(0);
    REQUIRE(std::holds_alternative<RpcRunScriptRequest>(runRequest.payload));
    CHECK(std::get<RpcRunScriptRequest>(runRequest.payload).script == "return 3");
    const auto defaultLoadRequest = dispatcher->requestAt(1);
    REQUIRE(std::holds_alternative<RpcLoadAnimationRequest>(
        defaultLoadRequest.payload));
    const auto& defaultAnimation =
        std::get<RpcLoadAnimationRequest>(defaultLoadRequest.payload);
    CHECK(defaultAnimation.modelPath == "models/haru.model.json");
    CHECK(defaultAnimation.x == doctest::Approx(0.0f));
    CHECK(defaultAnimation.y == doctest::Approx(0.0f));
    CHECK(defaultAnimation.scale == doctest::Approx(1.0f));
    CHECK(defaultAnimation.show);

    const auto positionedLoadRequest = dispatcher->requestAt(2);
    REQUIRE(std::holds_alternative<RpcLoadAnimationRequest>(
        positionedLoadRequest.payload));
    const auto& positionedAnimation =
        std::get<RpcLoadAnimationRequest>(positionedLoadRequest.payload);
    CHECK(positionedAnimation.modelPath == "models/hero \"alt\".PNG");
    CHECK(positionedAnimation.x == doctest::Approx(-12.5f));
    CHECK(positionedAnimation.y == doctest::Approx(24.0f));
    CHECK(positionedAnimation.scale == doctest::Approx(1.25f));
    CHECK_FALSE(positionedAnimation.show);
    CHECK(std::holds_alternative<RpcReloadScriptsRequest>(
        dispatcher->requestAt(3).payload));
}

TEST_CASE("EditorServer rejects invalid animation load transforms before dispatch") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    EditorServer es;
    es.setDispatcher(dispatcher);
    REQUIRE(startOpen(es));

    httplib::Client client("127.0.0.1", es.port());
    const std::vector<std::string> invalidBodies = {
        "{",
        "[]",
        "{}",
        R"({"modelPath":3})",
        R"({"modelPath":"hero.png","x":"left"})",
        R"({"modelPath":"hero.png","y":null})",
        R"({"modelPath":"hero.png","scale":0})",
        R"({"modelPath":"hero.png","scale":-1})",
        R"({"modelPath":"hero.png","scale":1e39})",
        R"({"modelPath":"hero.png","show":1})",
    };

    for (const auto& body : invalidBodies) {
        CAPTURE(body);
        auto response =
            client.Post("/api/live2d/load", body, "application/json");
        REQUIRE(response);
        CHECK(response->status == 400);
        CHECK(response->body.find("\"status\":\"invalid_request\"") !=
            std::string::npos);
        CHECK(response->body.find("\"code\":\"invalid_animation_request\"") !=
            std::string::npos);
    }

    es.stop();
    CHECK(dispatcher->requestCount() == 0);
}

TEST_CASE("EditorServer lists static animation images case-insensitively as valid JSON") {
    ScopedModelFiles files;
    REQUIRE(files.add("caesura_rpc_static_sprite.PnG"));
    REQUIRE(files.add("caesura_rpc_static_portrait.JpG"));
    REQUIRE(files.add("caesura_rpc_static_photo.JpEg"));
    REQUIRE(files.add("caesura_rpc_static_bitmap.BmP"));
    REQUIRE(files.add("caesura_rpc_static_ignore.TxT"));

    EditorServer es;
    REQUIRE(startOpen(es));

    httplib::Client client("127.0.0.1", es.port());
    auto response = client.Get("/api/live2d/models");
    REQUIRE(response);
    CHECK(response->status == 200);
    es.stop();

    const nlohmann::json models = nlohmann::json::parse(response->body);
    REQUIRE(models.is_array());

    std::vector<std::pair<std::string, std::string>> entries;
    std::vector<std::string> paths;
    for (const auto& model : models) {
        REQUIRE(model.is_object());
        REQUIRE(model.contains("name"));
        REQUIRE(model.contains("path"));
        const std::string name = model.at("name").get<std::string>();
        const std::string path = model.at("path").get<std::string>();
        entries.emplace_back(path, name);
        paths.push_back(path);
    }
    CHECK(std::is_sorted(paths.begin(), paths.end()));

    const auto containsModel = [&entries](const std::string& path,
                                          const std::string& name) {
        return std::find(entries.begin(), entries.end(),
            std::pair<std::string, std::string>{path, name}) != entries.end();
    };
    CHECK(containsModel("models/caesura_rpc_static_sprite.PnG",
        "caesura_rpc_static_sprite.PnG"));
    CHECK(containsModel("models/caesura_rpc_static_portrait.JpG",
        "caesura_rpc_static_portrait.JpG"));
    CHECK(containsModel("models/caesura_rpc_static_photo.JpEg",
        "caesura_rpc_static_photo.JpEg"));
    CHECK(containsModel("models/caesura_rpc_static_bitmap.BmP",
        "caesura_rpc_static_bitmap.BmP"));
    CHECK_FALSE(containsModel("models/caesura_rpc_static_ignore.TxT",
        "caesura_rpc_static_ignore.TxT"));
}

TEST_CASE("EditorServer /api/assets lists scanned dirs with kind + type filters") {
    ScopedAssetFiles files;
    REQUIRE(files.add("bg", "caesura_rpc_asset_bg.png"));
    REQUIRE(files.add("fg", "caesura_rpc_asset_fg.png"));
    REQUIRE(files.add("bgm", "caesura_rpc_asset_bgm.ogg"));

    EditorServer es;
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());

    auto all = client.Get("/api/assets");
    REQUIRE(all);
    CHECK(all->status == 200);
    const auto allArr = nlohmann::json::parse(all->body);
    REQUIRE(allArr.is_array());

    const auto findEntry = [&allArr](const std::string& name) {
        for (const auto& e : allArr) {
            if (e.value("name", std::string()) == name) return e;
        }
        return nlohmann::json::object();
    };
    {
        const auto bg = findEntry("caesura_rpc_asset_bg.png");
        CHECK(bg.value("type", std::string()) == "image");
        CHECK(bg.value("kind", std::string()) == "bg");
        CHECK(bg.value("path", std::string()) == "assets/bg/caesura_rpc_asset_bg.png");
    }
    {
        const auto fg = findEntry("caesura_rpc_asset_fg.png");
        CHECK(fg.value("type", std::string()) == "image");
        CHECK(fg.value("kind", std::string()) == "fg");
    }
    {
        const auto bgm = findEntry("caesura_rpc_asset_bgm.ogg");
        CHECK(bgm.value("type", std::string()) == "audio");
        CHECK(bgm.value("kind", std::string()) == "bgm");
    }

    auto fgOnly = client.Get("/api/assets?type=fg");
    REQUIRE(fgOnly);
    const auto fgArr = nlohmann::json::parse(fgOnly->body);
    REQUIRE(fgArr.is_array());
    bool fgHit = false, bgHit = false, bgmHit = false;
    for (const auto& e : fgArr) {
        const std::string kind = e.value("kind", std::string());
        fgHit |= (kind == "fg");
        bgHit |= (kind == "bg");
        bgmHit |= (kind == "bgm");
    }
    CHECK(fgHit);
    CHECK_FALSE(bgHit);
    CHECK_FALSE(bgmHit);

    auto bgmOnly = client.Get("/api/assets?type=bgm");
    REQUIRE(bgmOnly);
    const auto bgmArr = nlohmann::json::parse(bgmOnly->body);
    bgmHit = false; fgHit = false;
    for (const auto& e : bgmArr) {
        const std::string kind = e.value("kind", std::string());
        bgmHit |= (kind == "bgm");
        fgHit |= (kind == "fg");
    }
    CHECK(bgmHit);
    CHECK_FALSE(fgHit);

    auto imageOnly = client.Get("/api/assets?type=image");
    REQUIRE(imageOnly);
    const auto imageArr = nlohmann::json::parse(imageOnly->body);
    bool anyAudio = false; fgHit = false; bgHit = false;
    for (const auto& e : imageArr) {
        const std::string kind = e.value("kind", std::string());
        fgHit |= (kind == "fg");
        bgHit |= (kind == "bg");
        if (e.value("type", std::string()) == "audio") anyAudio = true;
    }
    CHECK(fgHit);
    CHECK(bgHit);
    CHECK_FALSE(anyAudio);

    es.stop();
}

TEST_CASE("EditorServer reports dispatcher unavailable over HTTP") {
    EditorServer es;
    REQUIRE(startOpen(es));

    httplib::Client client("127.0.0.1", es.port());
    auto response = client.Get("/api/status");
    REQUIRE(response);
    CHECK(response->status == 503);
    CHECK(response->body.find("\"status\":\"unavailable\"") != std::string::npos);
    CHECK(response->body.find("\"code\":\"dispatcher_unavailable\"") !=
        std::string::npos);
    es.stop();
}

TEST_CASE("constantTimeEquals matches only byte-identical strings") {
    using Caesura::constantTimeEquals;
    CHECK(constantTimeEquals("abc", "abc"));
    CHECK(constantTimeEquals("", ""));
    CHECK_FALSE(constantTimeEquals("abc", "abd"));
    CHECK_FALSE(constantTimeEquals("abc", "ab"));
    CHECK_FALSE(constantTimeEquals("ab", "abc"));
    CHECK_FALSE(constantTimeEquals("abc", "ABC"));
    // Bearer-prefixed token form used by the editor gate.
    CHECK(constantTimeEquals("Bearer s3cret", "Bearer s3cret"));
    CHECK_FALSE(constantTimeEquals("Bearer s3cret", "Bearer wrong"));
    CHECK_FALSE(constantTimeEquals("", "Bearer s3cret"));
    CHECK_FALSE(constantTimeEquals("Bearer s3cret", ""));
}

// =============================================================================
// Round 80+ unit-level boundary tests: DTO parsing, serialization shape,
// HTTP routing/auth, lifecycle, and error propagation.
// =============================================================================

// --- 1. DTO parsing: unknown method / malformed JSON / missing fields -----

TEST_CASE("RpcServer replies with structured error for unknown method") {
    RpcServer rpc;
    const std::string response = rpc.processRequestLine(
        R"({"id":99,"method":"definitelyNotAMethod"})");
    CHECK(response.find("\"id\":99") != std::string::npos);
    CHECK(response.find("Unknown method") != std::string::npos);
    CHECK(response.find("definitelyNotAMethod") != std::string::npos);
    CHECK(response.find("\"status\"") == std::string::npos);
    CHECK(response.find("\"jsonrpc\"") == std::string::npos);
}

TEST_CASE("RpcServer handles missing id by defaulting to zero") {
    RpcServer rpc;
    const std::string response = rpc.processRequestLine(
        R"({"method":"ping"})");
    CHECK(response.find("\"id\":0") != std::string::npos);
    CHECK(response.find("\"result\":\"ok\"") != std::string::npos);
}

TEST_CASE("RpcServer handles missing method as unknown empty method") {
    RpcServer rpc;
    const std::string response = rpc.processRequestLine(R"({"id":7})");
    CHECK(response.find("\"id\":7") != std::string::npos);
    CHECK(response.find("Unknown method") != std::string::npos);
}

TEST_CASE("RpcServer treats malformed non-JSON input as unknown method") {
    RpcServer rpc;
    const std::string response = rpc.processRequestLine("not-a-json-object");
    CHECK(response.find("Unknown method") != std::string::npos);
    CHECK(response.find("\"id\":0") != std::string::npos);
}

TEST_CASE("RpcServer tolerates truncated JSON once the method is parsed") {
    RpcServer rpc;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    rpc.setDispatcher(dispatcher);
    // Missing closing brace, but the hand-rolled extractor still finds method.
    const std::string response = rpc.processRequestLine(
        R"({"method":"getState")");
    CHECK(response.find("\"id\":0") != std::string::npos);
    CHECK(response.find("\"scene\"") != std::string::npos);
    CHECK(dispatcher->requestCount() == 1);
}

// --- 2. Response serialization shape --------------------------------------

TEST_CASE("RpcServer success reply echoes id and omits the jsonrpc field") {
    RpcServer rpc;
    const std::string response = rpc.processRequestLine(
        R"({"id":1234,"method":"ping"})");
    CHECK(response.find("\"id\":1234") != std::string::npos);
    CHECK(response.find("\"result\":\"ok\"") != std::string::npos);
    CHECK(response.find("\"jsonrpc\"") == std::string::npos);
}

TEST_CASE("RpcServer dispatcher-unavailable reply is a structured error") {
    RpcServer rpc;  // no dispatcher installed
    const std::string response = rpc.processRequestLine(
        R"({"id":5,"method":"getState"})");
    CHECK(response.find("\"id\":5") != std::string::npos);
    CHECK(response.find("\"status\":\"unavailable\"") != std::string::npos);
    CHECK(response.find("\"code\":\"dispatcher_unavailable\"") != std::string::npos);
    CHECK(response.find("\"error\"") != std::string::npos);
    CHECK(response.find("\"message\"") != std::string::npos);
}

TEST_CASE("RpcServer rejects empty run before reaching the dispatcher") {
    RpcServer rpc;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    rpc.setDispatcher(dispatcher);
    const std::string response = rpc.processRequestLine(
        R"({"id":3,"method":"run"})");  // script omitted -> empty
    CHECK(response.find("\"id\":3") != std::string::npos);
    CHECK(response.find("\"error\":\"Empty script\"") != std::string::npos);
    CHECK(dispatcher->requestCount() == 0);
}

// --- 3. HTTP endpoint routing ---------------------------------------------

TEST_CASE("EditorServer returns 404 for unknown route") {
    EditorServer es;
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());
    auto response = client.Get("/api/nonexistent-route");
    if (response) CHECK(response->status == 404);
    es.stop();
}

TEST_CASE("EditorServer returns 404 for GET on a POST-only route") {
    EditorServer es;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    es.setDispatcher(dispatcher);
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());
    auto response = client.Get("/api/stop");  // /api/stop is POST-only
    if (response) CHECK(response->status == 404);
    CHECK(dispatcher->requestCount() == 0);
    es.stop();
}

TEST_CASE("EditorServer returns 404 for POST on a GET-only route") {
    EditorServer es;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    es.setDispatcher(dispatcher);
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());
    auto response = client.Post("/api/status", "", "application/json");
    if (response) CHECK(response->status == 404);
    CHECK(dispatcher->requestCount() == 0);
    es.stop();
}

TEST_CASE("EditorServer enforces a configured bearer token (401)") {
    EditorServer es;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    es.setDispatcher(dispatcher);
    es.setAuthToken("s3cret-token");
    REQUIRE(es.start(0));
    httplib::Client client("127.0.0.1", es.port());

    auto noAuth = client.Get("/api/ping");
    REQUIRE(noAuth);
    CHECK(noAuth->status == 401);
    CHECK(noAuth->body.find("Unauthorized") != std::string::npos);

    httplib::Headers wrong;
    wrong.emplace("Authorization", "Bearer wrong-token");
    auto badAuth = client.Get("/api/ping", wrong);
    REQUIRE(badAuth);
    CHECK(badAuth->status == 401);

    httplib::Headers good;
    good.emplace("Authorization", "Bearer s3cret-token");
    auto okAuth = client.Get("/api/ping", good);
    REQUIRE(okAuth);
    CHECK(okAuth->status == 200);

    auto okStatus = client.Get("/api/status", good);
    REQUIRE(okStatus);
    CHECK(okStatus->status == 200);
    CHECK(dispatcher->requestCount() >= 1);

    es.stop();
}

TEST_CASE("EditorServer rejects non-local CORS origins with 403") {
    EditorServer es;
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());

    httplib::Headers evil;
    evil.emplace("Origin", "http://localhost.evil.com");
    auto response = client.Get("/api/ping", evil);
    REQUIRE(response);
    CHECK(response->status == 403);
    CHECK(response->body.find("Origin not allowed") != std::string::npos);

    // The bare-prefix guard must also reject an attacker-controlled host.
    httplib::Headers evil2;
    evil2.emplace("Origin", "http://localhostevil.com");
    auto response2 = client.Get("/api/ping", evil2);
    REQUIRE(response2);
    CHECK(response2->status == 403);
    es.stop();
}

TEST_CASE("EditorServer allows localhost CORS origin") {
    EditorServer es;
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());

    httplib::Headers local;
    local.emplace("Origin", "http://localhost:5173");
    auto response = client.Get("/api/ping", local);
    REQUIRE(response);
    CHECK(response->status == 200);
    es.stop();
}

// [Sprint 1 t4] Auth is DEFAULT-DENY: an editor started without any explicit
// configuration must not serve /api/* to an unauthenticated caller. start()
// generates a token, reports it, and only that token opens the gate.
TEST_CASE("EditorServer requires a token by default and generates one") {
    namespace fsn = std::filesystem;
    const fsn::path tokenFile = fsn::current_path() / ".caesura-editor-token";
    std::error_code preEc;
    const bool preexisting = fsn::exists(tokenFile, preEc);

    EditorServer es;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    es.setDispatcher(dispatcher);
    REQUIRE_FALSE(es.insecureNoAuth());
    REQUIRE(es.start(0));

    const std::string generated = es.authToken();
    CHECK_FALSE(generated.empty());
    CHECK(generated.size() >= 32);  // 24 random bytes rendered as hex

    httplib::Client client("127.0.0.1", es.port());
    auto anonymous = client.Get("/api/ping");
    REQUIRE(anonymous);
    CHECK(anonymous->status == 401);
    CHECK(anonymous->body.find("Unauthorized") != std::string::npos);

    // Lua execution and packaging routes are closed to anonymous callers too.
    auto anonymousRun = client.Post("/api/run", "return 3", "text/plain");
    REQUIRE(anonymousRun);
    CHECK(anonymousRun->status == 401);
    CHECK(dispatcher->requestCount() == 0);

    httplib::Headers good;
    good.emplace("Authorization", "Bearer " + generated);
    auto authorized = client.Get("/api/ping", good);
    REQUIRE(authorized);
    CHECK(authorized->status == 200);

    // The generated token is handed to the frontend through a file so the
    // editor can pick it up without the operator copying it by hand.
    const std::string reported = es.authTokenFile();
    if (!reported.empty()) {
        std::ifstream in(reported, std::ios::binary);
        REQUIRE(in.good());
        std::string stored((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
        CHECK(stored == generated);
    }

    es.stop();
    if (!preexisting) {
        std::error_code rmEc;
        fsn::remove(tokenFile, rmEc);
    }
}

// A token supplied by the operator (CAESURA_EDITOR_TOKEN via setAuthToken) is
// used as-is; start() must not overwrite it with a generated one.
TEST_CASE("EditorServer keeps a configured token instead of generating one") {
    EditorServer es;
    es.setAuthToken("operator-token");
    REQUIRE(es.start(0));
    CHECK(es.authToken() == "operator-token");
    CHECK(es.authTokenFile().empty());  // nothing written for configured tokens

    httplib::Client client("127.0.0.1", es.port());
    httplib::Headers good;
    good.emplace("Authorization", "Bearer operator-token");
    auto ok = client.Get("/api/ping", good);
    REQUIRE(ok);
    CHECK(ok->status == 200);
    es.stop();
}

// The escape hatch stays available and explicit (--editor-insecure).
TEST_CASE("EditorServer insecure mode serves without authentication") {
    EditorServer es;
    es.setInsecureNoAuth(true);
    CHECK(es.insecureNoAuth());
    REQUIRE(es.start(0));
    CHECK(es.authToken().empty());  // no token established in insecure mode

    httplib::Client client("127.0.0.1", es.port());
    auto ok = client.Get("/api/ping");
    REQUIRE(ok);
    CHECK(ok->status == 200);
    es.stop();
}

// --- 3b. Project import confinement (Sprint 1 t4) --------------------------

// /api/project/import used to accept ANY absolute directory, turning it into an
// arbitrary-directory read/copy primitive. The source must now canonicalize
// inside an allowed import root.
TEST_CASE("ProjectService::importProject refuses sources outside allowed roots") {
    namespace fsn = std::filesystem;
    const ProjectContext ctx = ProjectContext::fromEnvironment();
    rpc::service::ProjectService projects(ctx);

    // A directory that looks like a project but lives outside every allowed
    // root (system temp dir) must be refused, not copied.
    std::error_code ec;
    const fsn::path outside =
        fsn::temp_directory_path(ec) / "caesura_t4_outside_import";
    REQUIRE_FALSE(ec);
    fsn::remove_all(outside, ec);
    REQUIRE(fsn::create_directories(outside, ec));
    {
        std::ofstream story(outside / "story.ks", std::ios::binary);
        story << "; outside import probe\n[p]";
    }

    const std::string destName = "caesura_t4_outside_dest";
    const fsn::path dest = ctx.projectRoot() / destName;
    fsn::remove_all(dest, ec);

    const auto refused = projects.importProject(outside.string(), destName);
    CHECK(refused.status == 403);
    CHECK(refused.body.value("error", std::string()).find("allowed import roots") !=
        std::string::npos);
    CHECK_FALSE(fsn::exists(dest, ec));

    // Traversal out of the projects root is refused by the same check.
    const auto traversal =
        projects.importProject("../../etc", "caesura_t4_traversal_dest");
    CHECK(traversal.status == 403);

    // A non-existent path inside an allowed root still reports 404 (the
    // confinement check must not mask ordinary "not found").
    const auto missing = projects.importProject(
        (ctx.projectRoot() / "caesura_t4_definitely_missing").string(),
        "caesura_t4_missing_dest");
    CHECK(missing.status == 404);

    fsn::remove_all(outside, ec);
}

// Sources inside an allowed root keep working: the fix must not break the
// legitimate editor workflow (importing a project from the engine tree).
TEST_CASE("ProjectService::importProject accepts a source inside the engine root") {
    namespace fsn = std::filesystem;
    const ProjectContext ctx = ProjectContext::fromEnvironment();
    rpc::service::ProjectService projects(ctx);

    std::error_code ec;
    const fsn::path inside = ctx.sourceRoot() / "tmp" / "caesura_t4_inside_import";
    fsn::remove_all(inside, ec);
    REQUIRE(fsn::create_directories(inside, ec));
    {
        std::ofstream story(inside / "story.ks", std::ios::binary);
        story << "; inside import probe\n[p]";
    }

    const std::string destName = "caesura_t4_inside_dest";
    const fsn::path dest = ctx.projectRoot() / destName;
    fsn::remove_all(dest, ec);

    const auto accepted = projects.importProject(inside.string(), destName);
    CHECK(accepted.status == 200);
    CHECK(accepted.body.value("ok", false));
    CHECK(fsn::exists(dest / "story.ks", ec));

    fsn::remove_all(dest, ec);
    fsn::remove_all(inside, ec);
}

// --- N1: composition-root injection (ProjectContext::fromEnvironment(exeDir)) --

// The injected executable-directory anchor must win over the compile-time
// CAESURA_SOURCE_DIR macro: a release package resolves to ITSELF even when it
// was built on a machine whose macro still points at a live source tree
// (Sprint 4 semantics). An empty anchor must keep the macro-first behavior.
TEST_CASE("ProjectContext::fromEnvironment prefers the injected exe anchor over the macro") {
    namespace fsn = std::filesystem;
    std::error_code ec;
    const fsn::path pkg = fsn::temp_directory_path(ec) / "caesura_n1_pkg_anchor";
    fsn::remove_all(pkg, ec);
    // A release-package layout: templates + scripts + demo, NO src/.
    REQUIRE(fsn::create_directories(pkg / "tools" / "project_templates", ec));
    REQUIRE(fsn::create_directories(pkg / "scripts", ec));
    REQUIRE(fsn::create_directories(pkg / "demo", ec));

    // Direct exe-dir anchor (a ZIP extracted anywhere, exe at the top level).
    const ProjectContext ctx = ProjectContext::fromEnvironment(pkg);
    CHECK(ctx.sourceRoot() == pkg);

    // macOS-style bundle layout: .app/Contents/MacOS/<exe> -- the 3-level
    // upward walk must still find the package root.
    const fsn::path bundleExeDir = pkg / "App.app" / "Contents" / "MacOS";
    REQUIRE(fsn::create_directories(bundleExeDir, ec));
    const ProjectContext ctxBundle = ProjectContext::fromEnvironment(bundleExeDir);
    CHECK(ctxBundle.sourceRoot() == pkg);

    // Empty anchor keeps pre-injection behavior: macro-first (this test binary
    // ships CAESURA_SOURCE_DIR pointing at the checkout).
    const ProjectContext ctxDefault = ProjectContext::fromEnvironment();
#ifndef CAESURA_SOURCE_DIR
    CHECK(ctxDefault.sourceRoot() == ctxDefault.sourceRoot());  // no macro: any
#else
    CHECK(ctxDefault.sourceRoot() == fsn::path(CAESURA_SOURCE_DIR));
#endif

    fsn::remove_all(pkg, ec);
}

// --- 4. Lifecycle ----------------------------------------------------------

TEST_CASE("RpcServer dispatches via processRequestLine before run") {
    RpcServer rpc;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    rpc.setDispatcher(dispatcher);

    const std::string ping = rpc.processRequestLine(
        R"({"id":10,"method":"ping"})");
    CHECK(ping.find("\"id\":10") != std::string::npos);
    CHECK(dispatcher->requestCount() == 0);  // ping is handled internally

    const std::string state = rpc.processRequestLine(
        R"({"id":11,"method":"getState"})");
    CHECK(state.find("\"id\":11") != std::string::npos);
    CHECK(dispatcher->requestCount() == 1);
}

TEST_CASE("RpcServer keeps processing request lines after stop") {
    auto source = std::make_shared<ControlledLineSource>();
    source->finish();
    RpcServer rpc(lineSourceFor(source));
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    rpc.setDispatcher(dispatcher);
    rpc.run();  // transport hits EOF; not running afterwards
    CHECK_FALSE(rpc.isRunning());

    const std::string response = rpc.processRequestLine(
        R"({"id":12,"method":"getState"})");
    CHECK(response.find("\"scene\"") != std::string::npos);
    CHECK(dispatcher->requestCount() == 1);
}

TEST_CASE("EditorServer start while running is idempotent") {
    EditorServer es;
    REQUIRE(startOpen(es));
    const int firstPort = es.port();
    CHECK(es.isRunning());
    CHECK(es.start(0));          // second start while running -> no-op success
    CHECK(es.isRunning());
    CHECK(es.port() == firstPort);
    es.stop();
    CHECK_FALSE(es.isRunning());
}

TEST_CASE("EditorServer can start again after stop") {
    EditorServer es;
    REQUIRE(startOpen(es));
    es.stop();
    CHECK_FALSE(es.isRunning());

    REQUIRE(startOpen(es));
    CHECK(es.isRunning());
    CHECK(es.port() > 0);
    httplib::Client client("127.0.0.1", es.port());
    auto response = client.Get("/api/ping");
    REQUIRE(response);
    CHECK(response->status == 200);
    es.stop();
}

// --- 5. Error propagation --------------------------------------------------

TEST_CASE("RpcServer converts a throwing dispatcher into a structured error") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(
        [](const RpcRequest&) -> RpcReply {
            throw std::runtime_error("boom from dispatcher");
        });
    RpcServer rpc;
    rpc.setDispatcher(dispatcher);
    const std::string response = rpc.processRequestLine(
        R"({"id":21,"method":"getState"})");
    CHECK(response.find("\"id\":21") != std::string::npos);
    CHECK(response.find("\"status\":\"error\"") != std::string::npos);
    CHECK(response.find("\"code\":\"dispatcher_exception\"") != std::string::npos);
    CHECK(response.find("boom from dispatcher") != std::string::npos);
}

TEST_CASE("RpcServer converts a non-std exception into a generic error") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(
        [](const RpcRequest&) -> RpcReply {
            throw 42;  // not derived from std::exception
        });
    RpcServer rpc;
    rpc.setDispatcher(dispatcher);
    const std::string response = rpc.processRequestLine(
        R"({"id":22,"method":"getState"})");
    CHECK(response.find("\"status\":\"error\"") != std::string::npos);
    CHECK(response.find("\"code\":\"dispatcher_exception\"") != std::string::npos);
    CHECK(response.find("unknown exception") != std::string::npos);
}

TEST_CASE("EditorServer rejects an empty run body with 400") {
    EditorServer es;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    es.setDispatcher(dispatcher);
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());
    auto response = client.Post("/api/run", "", "text/plain");
    REQUIRE(response);
    CHECK(response->status == 400);
    CHECK(response->body.find("Empty script") != std::string::npos);
    CHECK(dispatcher->requestCount() == 0);
    es.stop();
}

TEST_CASE("EditorServer propagates a throwing dispatcher as HTTP 500") {
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(
        [](const RpcRequest&) -> RpcReply {
            throw std::runtime_error("http boom");
        });
    EditorServer es;
    es.setDispatcher(dispatcher);
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());
    auto response = client.Post("/api/run", "return 1", "text/plain");
    REQUIRE(response);
    CHECK(response->status == 500);
    CHECK(response->body.find("\"code\":\"dispatcher_exception\"") != std::string::npos);
    es.stop();
}

TEST_CASE("EditorServer does not dispatch oversized request bodies") {
    EditorServer es;
    auto dispatcher = std::make_shared<RecordingRpcDispatcher>(successReply);
    es.setDispatcher(dispatcher);
    REQUIRE(startOpen(es));
    httplib::Client client("127.0.0.1", es.port());

    // Far beyond the default httplib payload cap (8 MiB): the body is
    // rejected before the handler runs, so the dispatcher is never called.
    const std::string huge(16 * 1024 * 1024, 'x');
    auto response = client.Post("/api/live2d/load", huge, "application/json");
    CHECK(dispatcher->requestCount() == 0);
    es.stop();
}

TEST_CASE("rpc::service::PackagingService::widenUtf8 decodes real UTF-8 to wchar code points") {
    // U+4E2D (中) is E4 B8 AD in UTF-8; the old widenAscii mapped each byte
    // to a Latin-1 wchar (U+00E4 U+00B8 U+00AD) -- not a Unicode conversion.
    const std::wstring zh = rpc::service::PackagingService::widenUtf8("\xE4\xB8\xAD");
    CHECK(zh.size() == 1);
    CHECK(zh[0] == static_cast<wchar_t>(0x4E2D));

    // ASCII pass-through stays byte-for-byte.
    const std::wstring ascii = rpc::service::PackagingService::widenUtf8("abc/def_09-%");
    CHECK(ascii == L"abc/def_09-%");

    // Malformed bytes never crash and produce a deterministic U+FFFD each.
    const std::wstring broken = rpc::service::PackagingService::widenUtf8("\xFF\xFE");
    CHECK(broken.size() == 2);
    CHECK(broken[0] == static_cast<wchar_t>(0xFFFD));
    CHECK(broken[1] == static_cast<wchar_t>(0xFFFD));

    // Truncated 3-byte sequence: lead byte alone -> one replacement.
    const std::wstring trunc = rpc::service::PackagingService::widenUtf8("\xE4");
    CHECK(trunc.size() == 1);
    CHECK(trunc[0] == static_cast<wchar_t>(0xFFFD));

    // Surrogate halves are invalid code points -> replaced. Note: the
    // Windows lenient fallback may emit more than one replacement unit
    // for a malformed 3-byte lead (deterministic per platform); the
    // contract is: no crash, at least one U+FFFD, bounded length.
    const std::wstring surr = rpc::service::PackagingService::widenUtf8("\xED\xA0\x80");
    CHECK(surr.size() >= 1);
    CHECK(surr.size() <= 3);
    bool surrHasFffd = false;
    for (wchar_t c : surr) {
        if (c == static_cast<wchar_t>(0xFFFD)) surrHasFffd = true;
    }
    CHECK(surrHasFffd);

    // Empty input yields an empty wide string.
    CHECK(rpc::service::PackagingService::widenUtf8("").empty());
}


// U27 copied runtime snapshots: the dispatcher is the only substituted backend
// boundary. The actual production parsers, serializers and transports run below.
#include <cerrno>
#include <sstream>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using U27Json = nlohmann::json;

RpcStatsResult u27WideStats() {
    RpcStatsResult s;
    s.textureBudgetMB = 1536; s.textureTier = 2;
    s.textureTierName = "tier \"quoted\"\nline";
    s.meshCount = 37; s.jobWorkers = 3; s.jobPending = 5; s.luaKb = 123;
    s.jobs = {true, false, 4294967301ULL, 9007199254740993ULL, 18446744073709551615ULL};
    s.asyncLoader = {true, true, 4294967302ULL, 9007199254740995ULL,
        18446744073709551614ULL, 4294967303ULL, 9007199254740997ULL};
    s.host.supported = true; s.host.initialized = true; s.host.running = true;
    s.host.luaPaused = true; s.host.delivery = RpcAsyncDelivery::DirectDrain;
    s.host.asyncOwnershipComplete = true;
    s.host.completedOwnerFrames = 18446744073709551613ULL;
    s.host.deferredAsyncPayloads = 4294967304ULL;
    s.host.drainingAsyncPayloads = 9007199254740999ULL;
    s.host.dispatchingAsyncPayloads = 18446744073709551612ULL;
    s.host.audioCompletionTrackingSupported = true;
    s.host.audioCompletionsPending = 4294967305ULL;
    s.host.audioCompletionsActive = 9007199254741001ULL;
    s.host.audioCompletionOwnerRefs = 1;
    s.audio = {true, false, RpcAudioOutput::ManualMix, 4294967306ULL,
        9007199254741003ULL, 18446744073709551611ULL, 4294967307ULL,
        9007199254741005ULL, 18446744073709551610ULL, 4294967308ULL,
        9007199254741007ULL, 18446744073709551609ULL};
    s.render.supported = true;
    s.render.contextInitialized = true;
    s.render.renderingAvailable = true;
    s.render.resourceCountsAvailable = true;
    s.render.backendKind = RpcRenderBackendKind::GraphicsApi;
    s.render.backendName = "GPU \"quoted\"\nline\\tail";
    s.render.contextGeneration = 4294967296ULL;
    s.render.captureSubmissionFrame = 9007199254740993ULL;
    s.render.resources = {18446744073709551615ULL, 4294967297ULL, 9007199254740995ULL,
        18446744073709551614ULL, 4294967298ULL, 9007199254740997ULL,
        18446744073709551613ULL, 4294967299ULL, 9007199254740999ULL,
        18446744073709551612ULL, 4294967300ULL};
    s.render.screenshots = {true, 9007199254741001ULL, 18446744073709551611ULL,
        4294967301ULL, 9007199254741003ULL, 18446744073709551610ULL};
    s.render.screenshotOwnershipComplete = true;
    s.render.screenshotReadbackTrackingSupported = true;
    s.render.screenshotReadbacksOutstanding = 4294967302ULL;
    return s;
}

// Literal wire oracle: it is not produced by serializing the supplied DTO.
// These intentionally synthetic widths are protocol values, not real telemetry.
U27Json u27ExpectedWideStats() {
    return U27Json::parse(R"({
      "texture_budget_mb":1536,"texture_tier":2,"texture_tier_name":"tier \"quoted\"\nline",
      "mesh_count":37,"job_workers":3,"job_pending":5,"lua_kb":123,
      "jobs":{"supported":true,"running":false,"worker_pending":4294967301,
        "queued_completions":9007199254740993,"dispatching_completions":18446744073709551615},
      "async_loader":{"supported":true,"running":true,"pending_waiters":4294967302,
        "inflight_keys":9007199254740995,"completed_buffered":18446744073709551614,
        "cache_entries":4294967303,"cache_bytes":9007199254740997},
      "host":{"supported":true,"initialized":true,"running":true,"lua_paused":true,
        "delivery":"direct_drain","async_ownership_complete":true,
        "completed_owner_frames":18446744073709551613,"deferred_async_payloads":4294967304,
        "draining_async_payloads":9007199254740999,"dispatching_async_payloads":18446744073709551612,
        "audio_completion_tracking_supported":true,"audio_completions_pending":4294967305,
        "audio_completions_active":9007199254741001,"audio_completion_owner_refs":1},
      "audio":{"supported":true,"running":false,"output_mode":"manual_mix",
        "live_voices":4294967306,"bus_voices":9007199254741003,
        "session_handles":18446744073709551611,"retiring_bgm":4294967307,
        "retiring_voice":9007199254741005,"wave_cache_entries":18446744073709551610,
        "raw_cache_entries":4294967308,"voice_completions_pending":9007199254741007,
        "restored_sources":18446744073709551609},
      "render":{"supported":true,"context_initialized":true,"rendering_available":true,
        "resource_counts_available":true,"backend_kind":"graphics_api",
        "backend_name":"GPU \"quoted\"\nline\\tail",
        "context_generation":4294967296,"capture_submission_frame":9007199254740993,
        "resources":{"dynamic_index_buffers":18446744073709551615,
          "dynamic_vertex_buffers":4294967297,"frame_buffers":9007199254740995,
          "index_buffers":18446744073709551614,"occlusion_queries":4294967298,
          "programs":9007199254740997,"shaders":18446744073709551613,
          "textures":4294967299,"uniforms":9007199254740999,
          "vertex_buffers":18446744073709551612,"vertex_layouts":4294967300},
        "screenshots":{"supported":true,"waiting":9007199254741001,
          "submitted":18446744073709551611,"terminal":4294967301,
          "reserved_bytes":9007199254741003,"png_bytes":18446744073709551610},
        "screenshot_ownership_complete":true,"screenshot_readback_tracking_supported":true,
        "screenshot_readbacks_outstanding":4294967302}
    })");
}

void u27CheckScalar(const U27Json& actual, const U27Json& expected) {
    CHECK(actual.type() == expected.type());
    if (expected.is_number_unsigned()) {
        CHECK(actual.is_number_unsigned());
        if (actual.is_number_unsigned())
            CHECK(actual.get<std::uint64_t>() == expected.get<std::uint64_t>());
    } else CHECK(actual == expected);
}

// The render group adds another object level. Preserve the existing scalar
// checks at every leaf, including unsigned JSON type and exact uint64 value.
void u27CheckNestedStats(const U27Json& actual, const U27Json& expected) {
    if (!expected.is_object()) {
        u27CheckScalar(actual, expected);
        return;
    }
    CHECK(actual.is_object());
    if (!actual.is_object()) return;
    CHECK(actual.size() == expected.size());
    for (auto field = expected.begin(); field != expected.end(); ++field) {
        CAPTURE(field.key());
        CHECK(actual.contains(field.key()));
        if (actual.contains(field.key()))
            u27CheckNestedStats(actual.at(field.key()), field.value());
    }
}

void u27CheckStats(const U27Json& actual, const U27Json& expected) {
    REQUIRE(actual.is_object());
    CHECK(actual.size() == expected.size());
    for (auto field = expected.begin(); field != expected.end(); ++field) {
        CAPTURE(field.key());
        CHECK(actual.contains(field.key()));
        if (!actual.contains(field.key())) continue;
        const auto& value = actual.at(field.key());
        if (!field.value().is_object()) {
            u27CheckScalar(value, field.value());
            continue;
        }
        CHECK(value.is_object());
        if (!value.is_object()) continue;
        CHECK(value.size() == field.value().size());
        for (auto member = field.value().begin(); member != field.value().end(); ++member) {
            CAPTURE(member.key());
            CHECK(value.contains(member.key()));
            if (value.contains(member.key())) u27CheckNestedStats(value.at(member.key()), member.value());
        }
    }
    CHECK_FALSE(actual.contains("idle"));
    CHECK_FALSE(actual.contains("global_idle"));
}

// Independent literal oracles for the new nested renderer group. Do not build
// expected JSON through statsJson or by reflecting the supplied DTO fields.
U27Json u27ExpectedUnsupportedRender() {
    return U27Json::parse(R"({
      "supported":false,"context_initialized":false,"rendering_available":false,
      "resource_counts_available":false,"backend_kind":"unknown","backend_name":"",
      "context_generation":0,"capture_submission_frame":0,
      "resources":{"dynamic_index_buffers":0,"dynamic_vertex_buffers":0,
        "frame_buffers":0,"index_buffers":0,"occlusion_queries":0,"programs":0,
        "shaders":0,"textures":0,"uniforms":0,"vertex_buffers":0,"vertex_layouts":0},
      "screenshots":{"supported":false,"waiting":0,"submitted":0,"terminal":0,
        "reserved_bytes":0,"png_bytes":0},
      "screenshot_ownership_complete":false,"screenshot_readback_tracking_supported":false,
      "screenshot_readbacks_outstanding":0
    })");
}

struct U27RenderMode {
    const char* name;
    RpcStatsResult supplied;
    U27Json expected;
};

std::vector<U27RenderMode> u27RenderModes() {
    std::vector<U27RenderMode> modes;
    auto unsupported = u27WideStats();
    unsupported.render = {};
    auto unsupportedWire = u27ExpectedWideStats();
    unsupportedWire["render"] = u27ExpectedUnsupportedRender();
    modes.push_back({"unsupported zero counts", unsupported, unsupportedWire});

    auto wide = u27WideStats();
    auto wire = u27ExpectedWideStats();
    modes.push_back({"graphics API with distinct wide counters", wide, wire});

    auto noop = wide; auto noopWire = wire;
    noop.render.backendKind = RpcRenderBackendKind::Noop;
    noop.render.backendName = "Noop";
    noop.render.renderingAvailable = false;
    noopWire["render"]["backend_kind"] = "noop";
    noopWire["render"]["backend_name"] = "Noop";
    noopWire["render"]["rendering_available"] = false;
    modes.push_back({"Noop stays explicit with retained counters", noop, noopWire});

    auto unknown = wide; auto unknownWire = wire;
    unknown.render.backendKind = RpcRenderBackendKind::Unknown;
    unknownWire["render"]["backend_kind"] = "unknown";
    modes.push_back({"known Unknown enum", unknown, unknownWire});
    unknown.render.backendKind = static_cast<RpcRenderBackendKind>(777);
    modes.push_back({"future enum degrades only its name", unknown, unknownWire});

    // Each flag flips independently while every other supplied field remains
    // unchanged. Transports must not infer these booleans from another flag.
    auto notSupported = wide; auto notSupportedWire = wire;
    notSupported.render.supported = false;
    notSupportedWire["render"]["supported"] = false;
    modes.push_back({"unsupported retained values", notSupported, notSupportedWire});
    auto noContext = wide; auto noContextWire = wire;
    noContext.render.contextInitialized = false;
    noContextWire["render"]["context_initialized"] = false;
    modes.push_back({"context flag independent", noContext, noContextWire});
    auto notRendering = wide; auto notRenderingWire = wire;
    notRendering.render.renderingAvailable = false;
    notRenderingWire["render"]["rendering_available"] = false;
    modes.push_back({"rendering flag independent", notRendering, notRenderingWire});
    auto noCounts = wide; auto noCountsWire = wire;
    noCounts.render.resourceCountsAvailable = false;
    noCountsWire["render"]["resource_counts_available"] = false;
    modes.push_back({"resource counts flag independent", noCounts, noCountsWire});
    auto noQueue = wide; auto noQueueWire = wire;
    noQueue.render.screenshots.supported = false;
    noQueueWire["render"]["screenshots"]["supported"] = false;
    modes.push_back({"queue flag independent", noQueue, noQueueWire});
    auto incomplete = wide; auto incompleteWire = wire;
    incomplete.render.screenshotOwnershipComplete = false;
    incompleteWire["render"]["screenshot_ownership_complete"] = false;
    modes.push_back({"ownership flag independent", incomplete, incompleteWire});
    auto untracked = wide; auto untrackedWire = wire;
    untracked.render.screenshotReadbackTrackingSupported = false;
    untrackedWire["render"]["screenshot_readback_tracking_supported"] = false;
    modes.push_back({"readback tracking flag independent", untracked, untrackedWire});

    auto empty = unsupported; auto emptyWire = unsupportedWire;
    empty.render.supported = true;
    empty.render.contextInitialized = true;
    empty.render.renderingAvailable = true;
    empty.render.resourceCountsAvailable = true;
    empty.render.backendKind = RpcRenderBackendKind::GraphicsApi;
    empty.render.backendName = "empty graphics fixture";
    empty.render.contextGeneration = 7;
    empty.render.screenshots.supported = true;
    empty.render.screenshotOwnershipComplete = true;
    empty.render.screenshotReadbackTrackingSupported = true;
    emptyWire["render"]["supported"] = true;
    emptyWire["render"]["context_initialized"] = true;
    emptyWire["render"]["rendering_available"] = true;
    emptyWire["render"]["resource_counts_available"] = true;
    emptyWire["render"]["backend_kind"] = "graphics_api";
    emptyWire["render"]["backend_name"] = "empty graphics fixture";
    emptyWire["render"]["context_generation"] = std::uint64_t{7};
    emptyWire["render"]["screenshots"]["supported"] = true;
    emptyWire["render"]["screenshot_ownership_complete"] = true;
    emptyWire["render"]["screenshot_readback_tracking_supported"] = true;
    modes.push_back({"supported zero observations", empty, emptyWire});
    empty.render.contextGeneration = 0;
    emptyWire["render"]["context_generation"] = std::uint64_t{0};
    modes.push_back({"zero context identity is preserved", empty, emptyWire});
    return modes;
}

std::vector<std::pair<RpcStatsResult, U27Json>> u27StatsModes() {
    auto unsupported = u27ExpectedWideStats();
    for (auto& field : unsupported.items()) {
        if (field.key() == "render") continue; // Its two nested objects have a literal default oracle.
        if (field.value().is_object()) {
            for (auto& member : field.value().items()) {
                if (member.value().is_boolean()) member.value() = false;
                else if (member.value().is_string()) member.value() = "unknown";
                else member.value() = std::uint64_t{0};
            }
        } else if (field.value().is_string()) field.value() = "";
        else field.value() = std::uint64_t{0};
    }
    unsupported["render"] = u27ExpectedUnsupportedRender();
    std::vector<std::pair<RpcStatsResult, U27Json>> cases;
    cases.emplace_back(RpcStatsResult{}, std::move(unsupported));
    auto sdl = u27WideStats(); auto sdlWire = u27ExpectedWideStats();
    sdl.host.delivery = RpcAsyncDelivery::SdlEvents;
    sdl.host.asyncOwnershipComplete = false;
    sdl.host.audioCompletionTrackingSupported = false;
    sdl.audio.outputMode = RpcAudioOutput::Software;
    sdlWire["host"]["delivery"] = "sdl_events";
    sdlWire["host"]["async_ownership_complete"] = false;
    sdlWire["host"]["audio_completion_tracking_supported"] = false;
    sdlWire["audio"]["output_mode"] = "software";
    cases.emplace_back(std::move(sdl), std::move(sdlWire));
    auto unknown = u27WideStats(); auto unknownWire = u27ExpectedWideStats();
    unknown.host.delivery = static_cast<RpcAsyncDelivery>(99);
    unknown.host.asyncOwnershipComplete = false;
    unknown.audio.outputMode = static_cast<RpcAudioOutput>(99);
    unknownWire["host"]["delivery"] = "unknown";
    unknownWire["host"]["async_ownership_complete"] = false;
    unknownWire["audio"]["output_mode"] = "unknown";
    cases.emplace_back(std::move(unknown), std::move(unknownWire));
    auto device = u27WideStats(); auto deviceWire = u27ExpectedWideStats();
    device.audio.outputMode = RpcAudioOutput::Device;
    deviceWire["audio"]["output_mode"] = "device";
    cases.emplace_back(std::move(device), std::move(deviceWire));
    return cases;
}

std::shared_ptr<RecordingRpcDispatcher> u27StatsDispatcher(const RpcStatsResult& value) {
    return std::make_shared<RecordingRpcDispatcher>([value](const RpcRequest& request) {
        if (!std::holds_alternative<RpcStatsRequest>(request.payload))
            return RpcReply{RpcReplyStatus::Failed, "unexpected_fixture_request", {}, {}};
        return RpcReply{RpcReplyStatus::Ok, {}, {}, value};
    });
}

// Owns both real pipe descriptors and its reader. Destruction closes the
// original writer before joining; the inner RpcServer first closes its duplicate.
class U27RpcOutputCapture {
public:
    U27RpcOutputCapture() {
#if defined(_WIN32)
        if (::_pipe(m_fds, 4096, _O_BINARY | _O_NOINHERIT) != 0)
#else
        if (::pipe(m_fds) != 0)
#endif
            m_fds[0] = m_fds[1] = -1;
    }
    ~U27RpcOutputCapture() { finish(); close(m_fds[0]); }
    bool valid() const { return m_fds[0] >= 0 && m_fds[1] >= 0; }
    int writer() const { return m_fds[1]; }
    void startReader() {
        m_reader = std::thread([this] {
            try {
                char bytes[4096];
                for (;;) {
#if defined(_WIN32)
                    const int n = ::_read(m_fds[0], bytes, sizeof(bytes));
#else
                    const int n = static_cast<int>(::read(m_fds[0], bytes, sizeof(bytes)));
#endif
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { m_readFailed = n < 0; break; }
                    m_bytes.append(bytes, static_cast<std::size_t>(n));
                }
            } catch (...) { m_readFailed = true; }
        });
    }
    void closeWriter() { close(m_fds[1]); }
    void finish() { closeWriter(); if (m_reader.joinable()) m_reader.join(); }
    const std::string& bytes() const { return m_bytes; }
    bool readFailed() const { return m_readFailed; }
private:
    static void close(int& fd) {
        if (fd < 0) return;
#if defined(_WIN32)
        ::_close(fd);
#else
        ::close(fd);
#endif
        fd = -1;
    }
    int m_fds[2]{-1, -1};
    std::thread m_reader;
    std::string m_bytes;
    bool m_readFailed = false; // inspected only after join
};
} // namespace

TEST_CASE("U27 RPC stats: stdio nested values preserve every counter and legacy field") {
    auto dispatcher = u27StatsDispatcher(u27WideStats());
    RpcServer rpc;
    rpc.setDispatcher(dispatcher);
    const auto response = U27Json::parse(rpc.processRequestLine(R"({"id":2701,"method":"stats"})"));
    REQUIRE(response.contains("stats"));
    CHECK(response.size() == 2);
    CHECK(response.at("id") == 2701);
    u27CheckStats(response.at("stats"), u27ExpectedWideStats());
    REQUIRE(dispatcher->requestCount() == 1);
    CHECK(std::holds_alternative<RpcStatsRequest>(dispatcher->requestAt(0).payload));
}

TEST_CASE("U27 RPC stats: stdio keeps unsupported unknown and SDL incomplete explicit") {
    for (const auto& [value, expected] : u27StatsModes()) {
        RpcServer rpc;
        rpc.setDispatcher(u27StatsDispatcher(value));
        const auto response = U27Json::parse(rpc.processRequestLine(R"({"id":2702,"method":"stats"})"));
        REQUIRE(response.contains("stats"));
        CHECK(response.size() == 2);
        u27CheckStats(response.at("stats"), expected);
    }
}

TEST_CASE("U27 RPC stats: real stdio descriptor emits complete ordered uint64 lines") {
    U27RpcOutputCapture output;
    REQUIRE(output.valid());
    auto source = std::make_shared<ControlledLineSource>();
    source->enqueue(R"({"id":2703,"method":"stats"})");
    source->enqueue(R"({"id":2704,"method":"stats"})");
    source->finish();
    auto dispatcher = u27StatsDispatcher(u27WideStats());
    {
        RpcServer rpc(lineSourceFor(source), output.writer());
        REQUIRE(rpc.outputReady());
        rpc.setDispatcher(dispatcher);
        output.startReader();
        output.closeWriter();
        rpc.run(); // finite real input/EOF, actual StdioRpcOutput writer
        CHECK_FALSE(rpc.isRunning());
    }
    output.finish();
    REQUIRE_FALSE(output.readFailed());
    REQUIRE_FALSE(output.bytes().empty());
    CHECK(output.bytes().back() == '\n');
    std::istringstream lines(output.bytes());
    for (const int id : {2703, 2704}) {
        std::string line;
        REQUIRE(static_cast<bool>(std::getline(lines, line)));
        const auto response = U27Json::parse(line);
        CHECK(response.at("id") == id);
        CHECK(response.size() == 2);
        REQUIRE(response.contains("stats"));
        u27CheckStats(response.at("stats"), u27ExpectedWideStats());
    }
    std::string extra;
    CHECK_FALSE(static_cast<bool>(std::getline(lines, extra)));
    CHECK(dispatcher->requestCount() == 2);
    CHECK(source->readCalls() == 3);
}

TEST_CASE("U27 RPC stats: real HTTP flat response matches stdio without truncation") {
    auto dispatcher = u27StatsDispatcher(u27WideStats());
    EditorServer server;
    server.setDispatcher(dispatcher);
    REQUIRE(startOpen(server));
    httplib::Client client("127.0.0.1", server.port());
    client.set_connection_timeout(2, 0); client.set_read_timeout(3, 0);
    const auto response = client.Get("/api/stats");
    REQUIRE(response);
    REQUIRE(response->status == 200);
    auto body = U27Json::parse(response->body);
    CHECK(body.at("status") == "ok");
    CHECK_FALSE(body.contains("stats"));
    body.erase("status");
    u27CheckStats(body, u27ExpectedWideStats());
    RpcServer rpc;
    rpc.setDispatcher(dispatcher);
    const auto stdio = U27Json::parse(rpc.processRequestLine(R"({"id":2705,"method":"stats"})"));
    REQUIRE(stdio.contains("stats"));
    CHECK(body == stdio.at("stats"));
    CHECK(dispatcher->requestCount() == 2);
    server.stop();
    CHECK_FALSE(server.isRunning());
}

TEST_CASE("U27 RPC stats: real HTTP preserves unsupported and all delivery output modes") {
    for (const auto& [value, expected] : u27StatsModes()) {
        EditorServer server;
        auto dispatcher = u27StatsDispatcher(value);
        server.setDispatcher(dispatcher);
        REQUIRE(startOpen(server));
        httplib::Client client("127.0.0.1", server.port());
        client.set_connection_timeout(2, 0); client.set_read_timeout(3, 0);
        const auto response = client.Get("/api/stats");
        REQUIRE(response);
        REQUIRE(response->status == 200);
        auto body = U27Json::parse(response->body);
        CHECK(body.at("status") == "ok");
        CHECK_FALSE(body.contains("stats"));
        body.erase("status");
        u27CheckStats(body, expected);
        CHECK(dispatcher->requestCount() == 1);
        server.stop();
    }
}

TEST_CASE("U27 RPC stats: failures and wrong payload never become success snapshots") {
    struct Failure { RpcReply reply; const char* code; int http; };
    const std::vector<Failure> failures = {
        {{RpcReplyStatus::Busy, "request_cancelled", "not started", {}}, "request_cancelled", 503},
        {{RpcReplyStatus::Busy, "result_unknown", "already started", {}}, "result_unknown", 503},
        {{RpcReplyStatus::Unavailable, "dispatcher_closed", "closed", {}}, "dispatcher_closed", 503},
        {{RpcReplyStatus::Ok, {}, {}, RpcStatusResult{true}}, "invalid_dispatcher_reply", 500},
    };
    for (const auto& failure : failures) {
        CAPTURE(failure.code);
        auto dispatcher = std::make_shared<RecordingRpcDispatcher>([failure](const RpcRequest&) {
            return failure.reply;
        });
        RpcServer rpc; rpc.setDispatcher(dispatcher);
        const auto stdio = U27Json::parse(rpc.processRequestLine(R"({"id":2706,"method":"stats"})"));
        CHECK(stdio.at("id") == 2706);
        CHECK(stdio.at("code") == failure.code);
        CHECK(stdio.contains("error"));
        CHECK_FALSE(stdio.contains("stats"));
        EditorServer server; server.setDispatcher(dispatcher);
        REQUIRE(startOpen(server));
        httplib::Client client("127.0.0.1", server.port());
        client.set_connection_timeout(2, 0); client.set_read_timeout(3, 0);
        const auto response = client.Get("/api/stats");
        REQUIRE(response);
        CHECK(response->status == failure.http);
        const auto body = U27Json::parse(response->body);
        CHECK(body.at("code") == failure.code);
        CHECK(body.at("status") != "ok");
        for (const char* name : {"stats", "jobs", "async_loader", "host", "audio", "render"}) {
            CHECK_FALSE(body.contains(name));
            CHECK_FALSE(stdio.contains(name));
        }
        CHECK(dispatcher->requestCount() == 2);
        server.stop();
    }
}

TEST_CASE("U27 Render RPC stats: real stdio descriptor preserves raw flags modes and nested uint64 values") {
    for (const auto& mode : u27RenderModes()) {
        CAPTURE(mode.name);
        U27RpcOutputCapture output;
        REQUIRE(output.valid());
        auto source = std::make_shared<ControlledLineSource>();
        source->enqueue(R"({"id":2711,"method":"stats"})");
        source->finish();
        auto dispatcher = u27StatsDispatcher(mode.supplied);
        {
            RpcServer rpc(lineSourceFor(source), output.writer());
            REQUIRE(rpc.outputReady());
            rpc.setDispatcher(dispatcher);
            output.startReader();
            output.closeWriter();
            rpc.run();
            CHECK_FALSE(rpc.isRunning());
        }
        output.finish();
        REQUIRE_FALSE(output.readFailed());
        REQUIRE_FALSE(output.bytes().empty());
        CHECK(output.bytes().back() == '\n');
        std::istringstream lines(output.bytes());
        std::string line;
        REQUIRE(static_cast<bool>(std::getline(lines, line)));
        const auto response = U27Json::parse(line);
        REQUIRE(response.is_object());
        CHECK(response.size() == 2);
        CHECK(response.at("id") == 2711);
        REQUIRE(response.contains("stats"));
        u27CheckStats(response.at("stats"), mode.expected);
        CHECK_FALSE(static_cast<bool>(std::getline(lines, line)));
        CHECK(dispatcher->requestCount() == 1);
        CHECK(source->readCalls() == 2);
    }
}

TEST_CASE("U27 Render RPC stats: real HTTP preserves raw flags modes and nested uint64 values") {
    for (const auto& mode : u27RenderModes()) {
        CAPTURE(mode.name);
        EditorServer server;
        auto dispatcher = u27StatsDispatcher(mode.supplied);
        server.setDispatcher(dispatcher);
        REQUIRE(startOpen(server));
        httplib::Client client("127.0.0.1", server.port());
        client.set_connection_timeout(2, 0); client.set_read_timeout(3, 0);
        const auto response = client.Get("/api/stats");
        REQUIRE(response);
        REQUIRE(response->status == 200);
        auto body = U27Json::parse(response->body);
        CHECK(body.at("status") == "ok");
        CHECK_FALSE(body.contains("stats"));
        body.erase("status");
        u27CheckStats(body, mode.expected);
        CHECK(dispatcher->requestCount() == 1);
        server.stop();
        CHECK_FALSE(server.isRunning());
    }
}
