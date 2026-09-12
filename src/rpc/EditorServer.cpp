// ===========================================================================
//  Caesura (AmeKAG) -- EditorServer implementation (Track 4)
// ===========================================================================

#include "EditorServer.h"
#include "ConstantTime.h"
#include "ProjectContext.h"
#include "services/ProjectService.h"
#include "services/PackagingService.h"
#include "services/AssetService.h"
#include "../../external/cpp-httplib/httplib.h"
#include "../debug/api/DebugLog.h"

// Socket-error capture for the loud bind/listen diagnostics (t88): httplib
// does not expose the OS error, so read it at the failure point. ws2tcpip.h
// after winsock2.h; POSIX uses errno (declared by <cerrno>).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <nlohmann_json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;
namespace Caesura {

namespace {

using Json = nlohmann::json;

std::string dumpJson(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

// The OS error behind a failed httplib bind/listen. Must be read immediately
// at the failure point (before any other Winsock/errno-touching call).
int lastSocketError() {
#ifdef _WIN32
    return static_cast<int>(WSAGetLastError());
#else
    return errno;
#endif
}

// Re-run the failing bind with a raw socket so the OS error code is captured
// reliably: httplib does not surface it, and WSAGetLastError after httplib's
// own cleanup frequently reads 0. Returns the OS error, or -1 when the raw
// bind unexpectedly succeeds (caller then keeps the original capture).
int probeBindError(int port) {
#ifdef _WIN32
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return static_cast<int>(WSAGetLastError());
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int err = (rc == 0) ? -1 : static_cast<int>(WSAGetLastError());
    ::closesocket(s);
    return err;
#else
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return errno;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int rc = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int err = (rc == 0) ? -1 : errno;
    ::close(s);
    return err;
#endif
}

// Loud, actionable bind failure text. t84: Windows dynamic exclusion ranges
// silently covered 9876 -> start() returned false -> engine exited 1 with ZERO
// [EditorServer] output; this message is the paper trail for that class.
std::string bindFailureMessage(int port, int socketError) {
    std::string msg = "[EditorServer] [ERROR] failed to bind 127.0.0.1:";
    msg += std::to_string(port);
    msg += ": socket error ";
    msg += std::to_string(socketError);
#ifdef _WIN32
    msg += " (10013=WSAEACCES access denied / dynamic exclusion, 10048=WSAEADDRINUSE already in use)";
#endif
    msg += "\n[EditorServer] [ERROR]   - port already in use? netstat -ano | grep 127.0.0.1:";
    msg += std::to_string(port);
#ifdef _WIN32
    msg += "\n[EditorServer] [ERROR]   - Windows may reserve the port in a dynamic exclusion range (observed 9813-9912 covering 9876 on 2026-08-29):";
    msg += " netsh int ipv4 show excludedportrange protocol=tcp; unblock with admin: net stop winnat && net start winnat (or pick another port)";
#endif
    return msg;
}

std::string pathToUtf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool isAnimationAsset(const fs::path& path) {
    const std::string extension = lowerAscii(path.extension().string());
    if (extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".bmp" ||
        extension == ".moc" || extension == ".moc3" ||
        extension == ".json") {
        return true;
    }

    return lowerAscii(path.filename().string()).find(".model") !=
        std::string::npos;
}

// --- Web packaging (POST /api/package/web) helpers -----------------------
//
// Whitelist prefixes a packageable story path may live under. Everything
// else -- including absolute paths and ".." escapes -- is rejected so a
// local HTTP caller cannot make the engine shell out against arbitrary
// repository locations.
bool isStoryPathAllowed(const std::string& path) {
    static const char* const kPrefixes[] = {
        "assets/", "demo/", "tests/projects/", "projects/"};
    for (const char* prefix : kPrefixes) {
        if (path.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

// Keep only the last lines of packaging output for the reply payload.
std::string logTailOf(const std::string& log) {
    constexpr size_t kMaxLines = 30;
    std::vector<std::string> lines;
    std::istringstream stream(log);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    if (lines.size() > kMaxLines) {
        lines.erase(lines.begin(),
                    lines.end() - static_cast<std::ptrdiff_t>(kMaxLines));
    }
    std::string joined;
    for (const auto& entry : lines) {
        joined += entry;
        joined += '\n';
    }
    return joined;
}

bool readOptionalFloat(const Json& body,
                       const char* field,
                       float defaultValue,
                       float& result,
                       std::string& error) {
    const auto it = body.find(field);
    if (it == body.end()) {
        result = defaultValue;
        return true;
    }
    if (!it->is_number()) {
        error = std::string(field) + " must be a finite number";
        return false;
    }

    double value = 0.0;
    try {
        value = it->get<double>();
    } catch (const Json::exception&) {
        error = std::string(field) + " must be a finite number";
        return false;
    }

    constexpr double maxFloat =
        static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maxFloat || value > maxFloat) {
        error = std::string(field) + " must be a finite number";
        return false;
    }

    result = static_cast<float>(value);
    return true;
}

RpcReply invalidAnimationRequest(const std::string& message) {
    return {RpcReplyStatus::InvalidRequest, "invalid_animation_request",
        message, {}};
}

void setDispatchError(httplib::Response& response, const RpcReply& reply) {
    const char* status = "error";
    int httpStatus = 500;
    if (reply.status == RpcReplyStatus::InvalidRequest) {
        status = "invalid_request";
        httpStatus = 400;
    } else if (reply.status == RpcReplyStatus::Unavailable) {
        status = "unavailable";
        httpStatus = 503;
    } else if (reply.status == RpcReplyStatus::Busy) {
        status = "busy";
        httpStatus = 503;
    }

    const std::string code = reply.code.empty() ? "rpc_failed" : reply.code;
    const std::string message = reply.message.empty() ? "RPC request failed" : reply.message;
    response.status = httpStatus;
    response.set_content(dumpJson({
        {"status", status},
        {"code", code},
        {"error", message},
        {"message", message},
    }), "application/json");
}

RpcReply invalidDispatcherReply(const std::string& message) {
    return {RpcReplyStatus::Failed, "invalid_dispatcher_reply", message, {}};
}

// =========================================================================
// Project metadata (task book §6.3 PM settings)
// A managed project under ./projects/<name>/ may carry an optional
// caesura.project.json:
//   {name, template, version, language, description, created, modified}
// Like the other Project Manager routes these are editor-oriented meta
// operations handled here instead of the runtime RPC DTO chain.
// =========================================================================

// Current UTC time as an ISO-8601 timestamp.
std::string nowIsoUtc() {
    const std::time_t t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Json defaultProjectMeta(const std::string& name) {
    const std::string now = nowIsoUtc();
    return Json{
        {"name", name},
        {"template", ""},
        {"version", "1.0"},
        {"language", "zh"},
        {"description", ""},
        {"created", now},
        {"modified", now},
    };
}

// Overlay stored meta strings onto the defaults so a partially-written or
// hand-edited document still yields a complete schema. Non-string values are
// ignored (they cannot appear in a well-formed document).
void overlayStringMeta(Json& base, const Json& stored) {
    for (auto it = stored.begin(); it != stored.end(); ++it) {
        if (it.value().is_string()) base[it.key()] = it.value();
    }
}

} // namespace

// =========================================================================
// Lifecycle
// =========================================================================

EditorServer::EditorServer() = default;

EditorServer::~EditorServer() {
    stop();
}

std::string EditorServer::lastError() const {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    return m_lastError;
}

bool EditorServer::start(int port) {
    if (m_running) {
        printf("[EditorServer] Already running on port %d\n", m_port);
        { std::lock_guard<std::mutex> lock(m_dispatcherMutex); m_lastError.clear(); }
        return true;
    }

    if (m_thread.joinable()) stop();

    // Secure by default: establish (or generate) the bearer token BEFORE the
    // socket is bound, so no request can ever be served unauthenticated.
    if (!ensureAuthToken()) {
        const std::string msg =
            "[EditorServer] [ERROR] failed to establish an auth token "
            "(editor refuses to start unauthenticated; check token generation)";
        fprintf(stderr, "%s\n", msg.c_str());
        { std::lock_guard<std::mutex> lock(m_dispatcherMutex); m_lastError = msg; }
        m_port = 0;
        return false;
    }

    m_server = std::make_unique<httplib::Server>();
    const int boundPort = port == 0
        ? m_server->bind_to_any_port("127.0.0.1")
        : (m_server->bind_to_port("127.0.0.1", port) ? port : -1);
    if (boundPort < 0) {
        // t88: loud bind failure -- a silent false here surfaced only as
        // "engine exited 1" (t84: dynamic exclusion range covered 9876).
        int sockErr = lastSocketError();
        if (sockErr == 0) {
            const int probeErr = probeBindError(port);
            if (probeErr > 0) sockErr = probeErr;
        }
        const std::string msg = bindFailureMessage(port, sockErr);
        fprintf(stderr, "%s\n", msg.c_str());
        { std::lock_guard<std::mutex> lock(m_dispatcherMutex); m_lastError = msg; }
        m_server.reset();
        m_port = 0;
        return false;
    }

    m_port = boundPort;
    m_running = true;
    m_thread = std::thread(&EditorServer::serverLoop, this, boundPort);

    // Give the server a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (m_running) {
        printf("[EditorServer] Started on http://127.0.0.1:%d\n", m_port);
        // A stranger who just launched the executable needs an address to open,
        // not a secret to assemble by hand. The static shell accepts ?token= and
        // stores it, so print the complete URL; stderr keeps the raw token and
        // the header form for scripts. Printed here rather than in
        // ensureAuthToken() because m_port is only real after bind.
        // 127.0.0.1 rather than localhost on purpose: the server binds only
        // 127.0.0.1 (see bind_to_port above), while Windows resolves localhost
        // as ::1 first -- a stale twin editor listening on ::1:9876 then serves
        // a DIFFERENT token and the browser sees alternating 200/401. Both t3
        // (release verify) and t5 (browser e2e) independently hit this trap.
        std::string tokenForUrl;
        {
            std::lock_guard<std::mutex> lock(m_dispatcherMutex);
            tokenForUrl = m_authToken;
        }
        if (!tokenForUrl.empty()) {
            printf("[EditorServer] Open the editor: http://127.0.0.1:%d/?token=%s\n",
                   m_port, tokenForUrl.c_str());
        } else {
            printf("[EditorServer] Open the editor: http://127.0.0.1:%d/ "
                   "(INSECURE MODE: no token required)\n", m_port);
        }
        fflush(stdout);
        // t89 must-fix: honor the lastError() contract -- a successful start()
        // clears any stale failure from a previous attempt on this instance.
        { std::lock_guard<std::mutex> lock(m_dispatcherMutex); m_lastError.clear(); }
        return true;
    }
    return false;
}

void EditorServer::setDispatcher(std::shared_ptr<IRpcDispatcher> dispatcher) {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    m_dispatcher = std::move(dispatcher);
}

void EditorServer::setAuthToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    m_authToken = token;
}

void EditorServer::setInsecureNoAuth(bool insecure) {
    m_insecureNoAuth.store(insecure);
}

bool EditorServer::insecureNoAuth() const {
    return m_insecureNoAuth.load();
}

std::string EditorServer::authToken() const {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    return m_authToken;
}

std::string EditorServer::authTokenFile() const {
    std::lock_guard<std::mutex> lock(m_dispatcherMutex);
    return m_authTokenFile;
}

std::string EditorServer::generateAuthToken() {
    // std::random_device is the platform CSPRNG on the toolchains we ship
    // (MSVC: rand_s/RtlGenRandom, libstdc++/libc++: /dev/urandom). It is only
    // used for the loopback editor token, never for archive crypto (that is
    // ICryptoEngine's job), and a failure here fails CLOSED: start() refuses
    // to serve rather than falling back to a predictable token.
    try {
        std::random_device rd;
        std::uniform_int_distribution<unsigned> dist(0, 255);
        static constexpr char kHex[] = "0123456789abcdef";
        std::string token;
        token.reserve(48);
        for (int i = 0; i < 24; ++i) {
            const unsigned byte = dist(rd) & 0xFFu;
            token.push_back(kHex[(byte >> 4) & 0x0Fu]);
            token.push_back(kHex[byte & 0x0Fu]);
        }
        return token;
    } catch (const std::exception&) {
        return {};
    }
}

bool EditorServer::ensureAuthToken() {
    // Explicit, loud escape hatch: --editor-insecure (setInsecureNoAuth) or
    // CAESURA_EDITOR_INSECURE=1. Anything else requires a bearer token.
    if (!m_insecureNoAuth.load()) {
        if (const char* env = std::getenv("CAESURA_EDITOR_INSECURE")) {
            const std::string value(env);
            if (value == "1" || value == "true" || value == "TRUE" ||
                value == "yes" || value == "on") {
                m_insecureNoAuth.store(true);
            }
        }
    }
    if (m_insecureNoAuth.load()) {
        fprintf(stderr,
            "[EditorServer] *** INSECURE MODE: HTTP editor has NO authentication."
            " /api/eval and /api/run execute arbitrary Lua and /api/build,"
            " /api/package/* write files, so ANY local process can drive this"
            " engine. Use only on a trusted machine; prefer"
            " CAESURA_EDITOR_TOKEN. ***\n");
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_dispatcherMutex);
        if (!m_authToken.empty()) return true;  // caller configured one
    }

    const std::string token = generateAuthToken();
    if (token.empty()) {
        fprintf(stderr, "[EditorServer] Refusing to start: no editor token was "
                        "configured (CAESURA_EDITOR_TOKEN) and no system "
                        "entropy source is available to generate one. Pass "
                        "--editor-insecure to start without authentication.\n");
        return false;
    }

    // Hand the generated token to the frontend through a file next to the
    // running editor. Best effort: an unwritable cwd must not stop the editor,
    // the token is still on stderr.
    std::string tokenFile;
    {
        std::error_code ec;
        const fs::path path = fs::current_path(ec) / ".caesura-editor-token";
        if (!ec) {
            std::ofstream out(path, std::ios::trunc | std::ios::binary);
            if (out) {
                out << token;
                out.close();
                std::error_code permEc;
                // Owner-only where the platform honours it (POSIX). Windows
                // ACLs are not modelled by std::filesystem::permissions, so
                // the file inherits the directory ACL there.
                fs::permissions(path,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, permEc);
                tokenFile = path.string();
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_dispatcherMutex);
        m_authToken = token;
        m_authTokenFile = tokenFile;
    }

    fprintf(stderr, "[EditorServer] Generated editor token: %s\n", token.c_str());
    if (!tokenFile.empty()) {
        fprintf(stderr, "[EditorServer] Token written to: %s\n", tokenFile.c_str());
    } else {
        fprintf(stderr, "[EditorServer] Token file could not be written; use the "
                        "value above in the editor connection panel.\n");
    }
    fprintf(stderr, "[EditorServer] Send it as: Authorization: Bearer <token>\n");
    return true;
}

RpcReply EditorServer::dispatchRequest(RpcRequest request) const {
    std::shared_ptr<IRpcDispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(m_dispatcherMutex);
        dispatcher = m_dispatcher;
    }

    if (!dispatcher) {
        return {RpcReplyStatus::Unavailable, "dispatcher_unavailable",
            "RPC dispatcher is unavailable", {}};
    }

    try {
        return dispatcher->dispatch(request);
    } catch (const std::exception& ex) {
        return {RpcReplyStatus::Failed, "dispatcher_exception", ex.what(), {}};
    } catch (...) {
        return {RpcReplyStatus::Failed, "dispatcher_exception",
            "RPC dispatcher threw an unknown exception", {}};
        }
}

void EditorServer::stop() {
    const bool wasRunning = m_running.exchange(false);
    if (m_server) m_server->stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_server.reset();
    if (wasRunning) printf("[EditorServer] Stopped.\n");
}

// =========================================================================
// Log buffer
// =========================================================================

void EditorServer::pushLog(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_logMutex);

    // Timestamp
    time_t now = time(nullptr);
    char timeBuf[32];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", localtime(&now));

    m_logs.push_back({level, message, timeBuf});
    if (m_logs.size() > MAX_LOGS) {
        m_logs.erase(m_logs.begin());
    }
}

// =========================================================================
// Server thread
// =========================================================================

void EditorServer::serverLoop(int port) {
    httplib::Server& svr = *m_server;

    // Single source of truth for filesystem roots (task book §7). All
    // template/project/build/package resolution goes through ctx; business
    // handlers must not guess with fs::current_path(). The composition root
    // injects the EXECUTABLE's own directory (src/main.cpp ->
    // setSourceAnchor, from SDL_GetBasePath); the anchor wins over the
    // compile-time CAESURA_SOURCE_DIR macro so a release package resolves to
    // itself even when built on a dev machine (Sprint 4 -- N1).
    const ProjectContext ctx = ProjectContext::fromEnvironment(m_sourceAnchor);
    // Core Service layer (task book §14): handlers below are thin transport
    // wrappers; business logic lives in the services.
    rpc::service::ProjectService projects(ctx);
    rpc::service::PackagingService packaging(
        ctx, [this] { return m_archiveWriterFactory ? m_archiveWriterFactory() : nullptr; });
    rpc::service::AssetService assets(ctx);

    // ---------------------------------------------------------------------
    // CORS middleware - allow web editor from any origin
    // ---------------------------------------------------------------------
    svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        // Only allow same-origin / localhost web editor frontends. A bare "*"
        // would let any website drive the local editor over CORS.
        const std::string origin = req.get_header_value("Origin");
        // Exact host match: http://localhost[:port] or http://127.0.0.1[:port].
        // A raw prefix match would let http://localhost.evil.com (attacker-owned
        // parent domain) pass and read/drive the local editor from a browser.
        bool localOrigin = origin.empty();
        if (!localOrigin) {
            // Only http origins are eligible (https is rejected by design);
            // guard length so substr cannot throw on short headers such as
            // "Origin: http:/".
            if (origin.size() < 7 || origin.rfind("http://", 0) != 0) {
                localOrigin = false;
            } else {
                const std::string suffix = origin.substr(7);  // strip "http://"
                const auto slash = suffix.find('/');
                const std::string authority = slash == std::string::npos
                    ? suffix : suffix.substr(0, slash);
                const auto colon = authority.find(':');
                const std::string host = colon == std::string::npos
                    ? authority : authority.substr(0, colon);
                localOrigin = (host == "localhost" || host == "127.0.0.1");
            }
        }
        if (!localOrigin) {
            res.status = 403;
            res.set_content("{\"error\":\"Origin not allowed\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (!origin.empty()) {
            res.set_header("Access-Control-Allow-Origin", origin.c_str());
            res.set_header("Vary", "Origin");
        }
        // [Sprint 1 t4] API-token gate, DEFAULT-DENY. The editor binds only to
        // loopback (127.0.0.1), so no external host can reach it — but the
        // residual risk is real: another LOCAL process driving /api/run,
        // /api/eval (arbitrary Lua) or /api/build, /api/package/* (file
        // writes). Therefore every /api/* request must present
        // "Authorization: Bearer <token>" or get 401. start() guarantees a
        // token exists (configured via CAESURA_EDITOR_TOKEN/setAuthToken, or
        // generated and reported on stderr + .caesura-editor-token), so an
        // empty token here can only mean the explicit escape hatch
        // (--editor-insecure / CAESURA_EDITOR_INSECURE=1) — which warns loudly
        // at startup. Fail closed if neither holds.
        // CORS preflight (OPTIONS) carries no Authorization header and must not
        // be gated, or the browser web editor breaks; the actual request behind
        // the preflight is checked.
        //
        // The STATIC SHELL is likewise not gated, and that is the whole point of
        // the gate being scoped to /api/*: a browser navigating to
        // http://localhost:9876/ cannot attach an Authorization header, so
        // gating "/" made the editor unreachable from a browser at all -- the
        // only way in was --editor-insecure, i.e. the security fix pushed people
        // to turn security off. index.html carries no secret and no capability;
        // it reads ?token= (or localStorage) and sends the bearer on every
        // /api/* call it makes, each of which is still default-deny below.
        const bool staticShell = (req.path == "/" || req.path == "/index.html");
        if (req.method != "OPTIONS" && !staticShell) {
            const std::string auth = req.get_header_value("Authorization");
            std::string token;
            {
                std::lock_guard<std::mutex> lock(m_dispatcherMutex);
                token = m_authToken;
            }
            const bool insecure = m_insecureNoAuth.load();
            if (!insecure) {
                // Fail closed: no token configured means no request is served.
                const std::string expected = "Bearer " + token;
                if (token.empty() || !constantTimeEquals(auth, expected)) {
                    res.status = 401;
                    res.set_content("{\"error\":\"Unauthorized\"}", "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
        }
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        if (req.method == "OPTIONS") {
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ---------------------------------------------------------------------
    // GET /api/ping -- health check
    // ---------------------------------------------------------------------
    svr.Get("/api/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\",\"engine\":\"CaesuraAmeKAG\"}", "application/json");
    });

    // ---------------------------------------------------------------------
    // GET /api/status -- detailed engine status (R1.1)
    // ---------------------------------------------------------------------
    svr.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        RpcReply reply = dispatchRequest(RpcRequest{RpcStatusRequest{}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }

        const auto* status = std::get_if<RpcStatusResult>(&reply.payload);
        if (!status) {
            setDispatchError(res, invalidDispatcherReply(
                "Status reply did not contain runtime status"));
            return;
        }

        const char* luaOk = status->luaReady ? "true" : "false";
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"status\":\"ok\",\"engine\":\"CaesuraAmeKAG\",\"lua\":%s,\"port\":%d}",
            luaOk, m_port);
        res.set_content(buf, "application/json");
    });

    // ---------------------------------------------------------------------
    // GET /api/assets -- list project assets
    // ---------------------------------------------------------------------
    svr.Get("/api/assets", [&assets](const httplib::Request& req, httplib::Response& res) {
        const auto r = assets.listAssets(req.get_param_value("type"));
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/run -- execute Lua script
    // ---------------------------------------------------------------------
    svr.Post("/api/run", [this](const httplib::Request& req, httplib::Response& res) {
        std::string script = req.body;
        if (script.empty()) {
            res.set_content("{\"error\":\"Empty script\"}", "application/json");
            res.status = 400;
            return;
        }

        pushLog("info", "Running scene script...");
        RpcReply reply = dispatchRequest(RpcRequest{RpcRunScriptRequest{script}});
        if (reply.status != RpcReplyStatus::Ok) {
            pushLog("error", reply.message);
            setDispatchError(res, reply);
            return;
        }

        pushLog("info", "Scene script submitted.");
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/stop -- stop execution
    // ---------------------------------------------------------------------
    svr.Post("/api/stop", [this](const httplib::Request&, httplib::Response& res) {
        RpcReply reply = dispatchRequest(RpcRequest{RpcStopRequest{}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }

        pushLog("info", "Stop requested.");
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/reload -- request an owner-thread script reload
    // ---------------------------------------------------------------------
    svr.Post("/api/reload", [this](const httplib::Request&, httplib::Response& res) {
        RpcReply reply = dispatchRequest(RpcRequest{RpcReloadScriptsRequest{}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }

        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ---------------------------------------------------------------------
    // GET /api/logs -- recent log entries
    // ---------------------------------------------------------------------
    svr.Get("/api/logs", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(m_logMutex);

        Json arr = Json::array();
        for (const auto& entry : m_logs) {
            Json obj;
            obj["level"] = entry.level;
            obj["message"] = std::string(entry.message);
            obj["time"] = entry.time;
            arr.push_back(obj);
        }
        res.set_content(dumpJson(arr), "application/json");
    });

    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // GET /api/live2d/models -- list available Live2D models (R1.2)
    // ---------------------------------------------------------------------
    svr.Get("/api/live2d/models", [](const httplib::Request&, httplib::Response& res) {
        struct ModelEntry {
            std::string name;
            std::string path;
        };

        std::vector<ModelEntry> models;
        const char* dirs[] = {"models", "assets/models", "assets/live2d"};
        for (const char* dir : dirs) {
            if (!fs::exists(dir)) continue;
            try {
                for (const auto& entry : fs::directory_iterator(dir)) {
                    if (!entry.is_regular_file()) continue;
                    if (!isAnimationAsset(entry.path())) continue;
                    models.push_back({
                        pathToUtf8(entry.path().filename()),
                        pathToUtf8(entry.path()),
                    });
                }
            } catch (...) {}
        }

        std::sort(models.begin(), models.end(),
            [](const ModelEntry& lhs, const ModelEntry& rhs) {
                return lhs.path < rhs.path;
            });

        Json response = Json::array();
        for (const auto& model : models) {
            response.push_back({
                {"name", model.name},
                {"path", model.path},
            });
        }
        res.set_content(dumpJson(response), "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/live2d/load -- load a Live2D model (R1.2)
    // ---------------------------------------------------------------------
    svr.Post("/api/live2d/load", [this](const httplib::Request& req, httplib::Response& res) {
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const Json::exception&) {
            setDispatchError(res, invalidAnimationRequest(
                "Request body must be a valid JSON object"));
            return;
        }

        if (!body.is_object()) {
            setDispatchError(res, invalidAnimationRequest(
                "Request body must be a JSON object"));
            return;
        }

        const auto modelPathIt = body.find("modelPath");
        if (modelPathIt == body.end() || !modelPathIt->is_string() ||
            modelPathIt->get_ref<const std::string&>().empty()) {
            setDispatchError(res, invalidAnimationRequest(
                "modelPath must be a non-empty string"));
            return;
        }

        RpcLoadAnimationRequest request;
        request.modelPath = modelPathIt->get<std::string>();
        std::string error;
        if (!readOptionalFloat(body, "x", 0.0f, request.x, error) ||
            !readOptionalFloat(body, "y", 0.0f, request.y, error) ||
            !readOptionalFloat(body, "scale", 1.0f, request.scale, error)) {
            setDispatchError(res, invalidAnimationRequest(error));
            return;
        }
        if (request.scale <= 0.0f) {
            setDispatchError(res, invalidAnimationRequest(
                "scale must be greater than zero"));
            return;
        }

        const auto showIt = body.find("show");
        if (showIt != body.end()) {
            if (!showIt->is_boolean()) {
                setDispatchError(res, invalidAnimationRequest(
                    "show must be a boolean"));
                return;
            }
            request.show = showIt->get<bool>();
        }

        RpcReply reply = dispatchRequest(RpcRequest{std::move(request)});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }

        const auto* animation = std::get_if<RpcAnimationResult>(&reply.payload);
        if (animation && animation->modelId > 0) {
            res.set_content(dumpJson({
                {"status", "ok"},
                {"modelId", animation->modelId},
                {"name", animation->name},
            }), "application/json");
        } else {
            setDispatchError(res, invalidDispatcherReply(
                "Animation reply did not contain a valid model"));
        }
    });


    // ---------------------------------------------------------------------
    // POST /api/eval -- evaluate Lua code and return its stringified value
    // ---------------------------------------------------------------------
    svr.Post("/api/eval", [this](const httplib::Request& req, httplib::Response& res) {
        std::string code = req.body;
        if (code.empty()) {
            res.set_content("{\"error\":\"Empty code\"}", "application/json");
            res.status = 400;
            return;
        }
        RpcReply reply = dispatchRequest(RpcRequest{RpcEvaluateRequest{code}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        const auto* result = std::get_if<RpcEvaluateResult>(&reply.payload);
        if (!result) {
            setDispatchError(res, invalidDispatcherReply(
                "Eval reply did not contain a result value"));
            return;
        }
        res.set_content(dumpJson({
            {"status", "ok"},
            {"result", result->value},
        }), "application/json");
    });

    // ---------------------------------------------------------------------
    // GET /api/debug/getState -- current scene / debug state
    // ---------------------------------------------------------------------
    svr.Get("/api/debug/getState", [this](const httplib::Request&, httplib::Response& res) {
        RpcReply reply = dispatchRequest(RpcRequest{RpcGetStateRequest{}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        const auto* state = std::get_if<RpcStateResult>(&reply.payload);
        if (!state) {
            setDispatchError(res, invalidDispatcherReply(
                "State reply did not contain a scene"));
            return;
        }
        res.set_content(dumpJson({
            {"status", "ok"},
            {"scene", state->scene},
            {"token_index", state->tokenIndex},
            {"nvl_mode", state->nvlMode},
            {"language", state->language},
            {"backlog_count", state->backlogCount},
            {"layer_count", state->layerCount},
            {"current_cmd", state->currentCmd},
        }), "application/json");
    });

    // ---------------------------------------------------------------------
    // GET /api/state -- canonical engine state for the IDE preview panel
    // (same payload as /api/debug/getState; round 18).
    // ---------------------------------------------------------------------
    svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        RpcReply reply = dispatchRequest(RpcRequest{RpcGetStateRequest{}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        const auto* state = std::get_if<RpcStateResult>(&reply.payload);
        if (!state) {
            setDispatchError(res, invalidDispatcherReply(
                "State reply did not contain engine state"));
            return;
        }
        res.set_content(dumpJson({
            {"status", "ok"},
            {"scene", state->scene},
            {"token_index", state->tokenIndex},
            {"nvl_mode", state->nvlMode},
            {"language", state->language},
            {"backlog_count", state->backlogCount},
            {"layer_count", state->layerCount},
            {"current_cmd", state->currentCmd},
        }), "application/json");
    });

    // ---------------------------------------------------------------------
    // GET /api/sma/validate?path=... -- SMA asset validation through the
    // engine's shared checker (kag.sma_check). Returns ok + violations +
    // a structure summary for the IDE SMA asset panel (round 19).
    // ---------------------------------------------------------------------
    svr.Get("/api/sma/validate", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string path = req.get_param_value("path");
        if (path.empty()) {
            res.set_content("{\"error\":\"Missing path parameter\"}", "application/json");
            res.status = 400;
            return;
        }
        RpcReply reply = dispatchRequest(RpcRequest{RpcSmaValidateRequest{path}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        const auto* v = std::get_if<RpcSmaValidateResult>(&reply.payload);
        if (!v) {
            setDispatchError(res, invalidDispatcherReply(
                "SMA validate reply missing result"));
            return;
        }
        res.set_content(dumpJson({
            {"status", "ok"},
            {"ok", v->ok},
            {"errors", v->errors},
            {"meta", v->meta},
        }), "application/json");
    });

    // ---------------------------------------------------------------------
    // GET /api/pick?x=&y= -- IDE preview-frame hit test (round 23): returns
    // the Lua layer-tree nodes containing the window pixel, bottom-to-top.
    // ---------------------------------------------------------------------
    svr.Get("/api/pick", [this](const httplib::Request& req, httplib::Response& res) {
        int x = 0, y = 0;
        if (req.has_param("x")) x = std::atoi(req.get_param_value("x").c_str());
        if (req.has_param("y")) y = std::atoi(req.get_param_value("y").c_str());
        if (x < 0 || y < 0 || x > 8192 || y > 8192) {
            res.set_content("{\"error\":\"Invalid pick coordinates\"}", "application/json");
            res.status = 400;
            return;
        }
        RpcReply reply = dispatchRequest(RpcRequest{RpcPickRequest{x, y}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        const auto* p = std::get_if<RpcPickResult>(&reply.payload);
        if (!p) {
            setDispatchError(res, invalidDispatcherReply(
                "Pick reply missing result"));
            return;
        }
        res.set_content(dumpJson({
            {"status", "ok"},
            {"hits", p->hits},
        }), "application/json");
    });

    // ---------------------------------------------------------------------
    // GET /api/stats -- engine runtime stats for the IDE panel (round 28).
    // ---------------------------------------------------------------------
    svr.Get("/api/stats", [this](const httplib::Request&, httplib::Response& res) {
        RpcReply reply = dispatchRequest(RpcRequest{RpcStatsRequest{}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        const auto* s = std::get_if<RpcStatsResult>(&reply.payload);
        if (!s) {
            setDispatchError(res, invalidDispatcherReply(
                "Stats reply missing result"));
            return;
        }
        res.set_content(dumpJson({
            {"status", "ok"},
            {"texture_budget_mb", s->textureBudgetMB},
            {"texture_tier", s->textureTier},
            {"texture_tier_name", s->textureTierName},
            {"mesh_count", s->meshCount},
            {"job_workers", s->jobWorkers},
            {"job_pending", s->jobPending},
            {"lua_kb", s->luaKb},
        }), "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/sma/save -- validate + write back an SMA asset (round 26).
    // Body: {"path": "...", "content": "..."}; path restricted to assets/.
    // ---------------------------------------------------------------------
    svr.Post("/api/sma/save", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = nlohmann::json::parse(req.body);
            const std::string path = body.value("path", std::string());
            const std::string content = body.value("content", std::string());
            if (path.empty() || content.empty()) {
                res.set_content("{\"error\":\"Missing path or content\"}", "application/json");
                res.status = 400;
                return;
            }
            RpcReply reply = dispatchRequest(
                RpcRequest{RpcSmaSaveRequest{path, content}});
            if (reply.status != RpcReplyStatus::Ok) {
                setDispatchError(res, reply);
                return;
            }
            const auto* v = std::get_if<RpcSmaSaveResult>(&reply.payload);
            if (!v) {
                setDispatchError(res, invalidDispatcherReply(
                    "SMA save reply missing result"));
                return;
            }
            res.set_content(dumpJson({
                {"status", "ok"},
                {"ok", v->ok},
                {"errors", v->errors},
            }), "application/json");
        } catch (const std::exception&) {
            res.set_content("{\"error\":\"Invalid JSON body\"}", "application/json");
            res.status = 400;
        }
    });

    // ---------------------------------------------------------------------
    // GET /api/debug/getFrame -- capture the current frame as base64 PNG
    // ---------------------------------------------------------------------
    svr.Get("/api/debug/getFrame", [this](const httplib::Request& req, httplib::Response& res) {
        int w = 1280, h = 720;
        if (req.has_param("w")) w = std::atoi(req.get_param_value("w").c_str());
        if (req.has_param("h")) h = std::atoi(req.get_param_value("h").c_str());
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
            res.set_content("{\"error\":\"Invalid frame dimensions\"}", "application/json");
            res.status = 400;
            return;
        }
        RpcReply reply = dispatchRequest(
            RpcRequest{RpcCaptureFrameRequest{w, h}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        const auto* frame = std::get_if<RpcFrameResult>(&reply.payload);
        if (!frame) {
            setDispatchError(res, invalidDispatcherReply(
                "Frame reply did not contain a capture"));
            return;
        }
        res.set_content(dumpJson({
            {"status", "ok"},
            {"base64", frame->base64},
        }), "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/debug/setBreakpoint -- set a source line breakpoint
    // ---------------------------------------------------------------------
    svr.Post("/api/debug/setBreakpoint", [this](const httplib::Request& req, httplib::Response& res) {
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const Json::exception&) {
            setDispatchError(res, invalidAnimationRequest(
                "Request body must be a valid JSON object"));
            return;
        }
        if (!body.is_object() || !body.contains("source") ||
            !body["source"].is_string() || !body.contains("line") ||
            !body["line"].is_number_integer()) {
            setDispatchError(res, invalidAnimationRequest(
                "source (string) and line (integer) are required"));
            return;
        }
        std::int64_t line64 = 0;
        try {
            line64 = body["line"].get<std::int64_t>();
        } catch (const Json::exception&) {
            setDispatchError(res, invalidAnimationRequest(
                "line must be a positive integer in int32 range"));
            return;
        }
        if (line64 <= 0 || line64 > (std::numeric_limits<int>::max)()) {
            setDispatchError(res, invalidAnimationRequest(
                "line must be a positive integer in int32 range"));
            return;
        }
        const int line = static_cast<int>(line64);
        RpcReply reply = dispatchRequest(RpcRequest{RpcSetBreakpointRequest{
            body["source"].get<std::string>(), line}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/debug/removeBreakpoint -- remove a source line breakpoint
    // ---------------------------------------------------------------------
    svr.Post("/api/debug/removeBreakpoint", [this](const httplib::Request& req, httplib::Response& res) {
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const Json::exception&) {
            setDispatchError(res, invalidAnimationRequest(
                "Request body must be a valid JSON object"));
            return;
        }
        if (!body.is_object() || !body.contains("source") ||
            !body["source"].is_string() || !body.contains("line") ||
            !body["line"].is_number_integer()) {
            setDispatchError(res, invalidAnimationRequest(
                "source (string) and line (integer) are required"));
            return;
        }
        std::int64_t line64 = 0;
        try {
            line64 = body["line"].get<std::int64_t>();
        } catch (const Json::exception&) {
            setDispatchError(res, invalidAnimationRequest(
                "line must be a positive integer in int32 range"));
            return;
        }
        if (line64 <= 0 || line64 > (std::numeric_limits<int>::max)()) {
            setDispatchError(res, invalidAnimationRequest(
                "line must be a positive integer in int32 range"));
            return;
        }
        const int line = static_cast<int>(line64);
        RpcReply reply = dispatchRequest(RpcRequest{RpcRemoveBreakpointRequest{
            body["source"].get<std::string>(), line}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/debug/clearBreakpoints -- remove all breakpoints
    // ---------------------------------------------------------------------
    svr.Post("/api/debug/clearBreakpoints", [this](const httplib::Request&, httplib::Response& res) {
        RpcReply reply = dispatchRequest(RpcRequest{RpcClearBreakpointsRequest{}});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/debug/continue -- resume a paused debugger run
    // ---------------------------------------------------------------------
    svr.Post("/api/debug/continue", [this](const httplib::Request& req, httplib::Response& res) {
        RpcDebugResumeRequest request;
        request.mode = RpcDebugResumeMode::Continue;
        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const Json::exception&) {
            // Empty/absent body is allowed; defaults apply.
        }
        if (body.is_object() && body.contains("pauseId") &&
            body["pauseId"].is_number_unsigned()) {
            try {
                request.pauseId = body["pauseId"].get<std::uint64_t>();
            } catch (const Json::exception&) {
                // Malformed pauseId: fall through with default 0.
            }
        }
        RpcReply reply = dispatchRequest(RpcRequest{std::move(request)});
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // ---------------------------------------------------------------------
    // POST /api/debug/stepInto|stepOver|stepOut -- single-step a paused
    // debugger run (Sprint 4b). Same contract as /continue but with the
    // matching RpcDebugResumeMode.
    // ---------------------------------------------------------------------
    const auto stepHandler = [this](RpcDebugResumeMode mode) {
        return [this, mode](const httplib::Request& req, httplib::Response& res) {
            RpcDebugResumeRequest request;
            request.mode = mode;
            Json body;
            try {
                body = Json::parse(req.body);
            } catch (const Json::exception&) {
                // Empty/absent body is allowed; defaults apply.
            }
            if (body.is_object() && body.contains("pauseId") &&
                body["pauseId"].is_number_unsigned()) {
                try {
                    request.pauseId = body["pauseId"].get<std::uint64_t>();
                } catch (const Json::exception&) {
                    // Malformed pauseId: fall through with default 0.
                }
            }
            RpcReply reply = dispatchRequest(RpcRequest{std::move(request)});
            if (reply.status != RpcReplyStatus::Ok) {
                setDispatchError(res, reply);
                return;
            }
            res.set_content("{\"status\":\"ok\"}", "application/json");
        };
    };
    svr.Post("/api/debug/stepInto", stepHandler(RpcDebugResumeMode::StepInto));
    svr.Post("/api/debug/stepOver", stepHandler(RpcDebugResumeMode::StepOver));
    svr.Post("/api/debug/stepOut", stepHandler(RpcDebugResumeMode::StepOut));

    // ---------------------------------------------------------------------
    // GET /api/debug/inspect -- inspect a Lua local or global variable
    // ---------------------------------------------------------------------
    svr.Get("/api/debug/inspect", [this](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("name") || req.get_param_value("name").empty()) {
            res.set_content("{\"error\":\"name parameter is required\"}", "application/json");
            res.status = 400;
            return;
        }
        const std::string name = req.get_param_value("name");
        int frame = 0;
        if (req.has_param("frame")) frame = std::atoi(req.get_param_value("frame").c_str());
        RpcReply reply;
        if (req.has_param("global")) {
            reply = dispatchRequest(RpcRequest{RpcInspectGlobalRequest{name}});
        } else {
            reply = dispatchRequest(RpcRequest{RpcInspectLocalRequest{frame, name}});
        }
        if (reply.status != RpcReplyStatus::Ok) {
            setDispatchError(res, reply);
            return;
        }
        const auto* inspection = std::get_if<RpcInspectionResult>(&reply.payload);
        if (!inspection) {
            setDispatchError(res, invalidDispatcherReply(
                "Inspect reply did not contain a value"));
            return;
        }
        res.set_content(dumpJson({
            {"status", "ok"},
            {"value", inspection->value},
        }), "application/json");
    });
    // ---------------------------------------------------------------------
    // POST /api/build -- one-click CARC packaging (R1.3)
    // ---------------------------------------------------------------------
    svr.Post("/api/build", [&packaging](const httplib::Request& req, httplib::Response& res) {
        Json body = Json::object();
        try { body = req.body.empty() ? Json::object() : Json::parse(req.body); } catch (const Json::exception&) {
            res.set_content("{\"error\":\"Request body must be valid JSON\"}", "application/json"); res.status = 400; return;
        }
        if (!body.is_object()) { res.set_content("{\"error\":\"Request body must be a JSON object\"}", "application/json"); res.status = 400; return; }
        std::string outputPath = "build/game.carc";
        std::string keyPath = "build/game.key";
        if (body.contains("outputPath") && body["outputPath"].is_string()) {
            const std::string v = body["outputPath"].get<std::string>();
            if (!v.empty()) outputPath = v;
        }
        if (body.contains("keyPath") && body["keyPath"].is_string()) {
            const std::string v = body["keyPath"].get<std::string>();
            if (!v.empty()) keyPath = v;
        }
        const auto r = packaging.build(outputPath, keyPath);
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    });
    svr.Post("/api/package/web", [&packaging](const httplib::Request& req, httplib::Response& res) {
        Json body = Json::object();
        try { body = req.body.empty() ? Json::object() : Json::parse(req.body); } catch (const Json::exception&) {
            res.set_content("{\"error\":\"Request body must be valid JSON\"}", "application/json"); res.status = 400; return;
        }
        if (!body.is_object()) { res.set_content("{\"error\":\"Request body must be a JSON object\"}", "application/json"); res.status = 400; return; }
        std::string storyPath = "demo/example_game/story.ks";
        std::string rawOutName;
        if (body.contains("storyPath") && body["storyPath"].is_string()) {
            const std::string v = body["storyPath"].get<std::string>();
            if (!v.empty()) storyPath = v;
        }
        if (body.contains("outName") && body["outName"].is_string()) {
            rawOutName = body["outName"].get<std::string>();
        }
        const auto r = packaging.packageWeb(storyPath, rawOutName);
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    });

    // ---------------------------------------------------------------------
    // Static file serving -- web editor frontend
    // ---------------------------------------------------------------------
    // Serve the single-file editor (web-editor/dist/index.html). We read the
    // file through our own ifstream instead of httplib set_mount_point: on
    // Windows, non-ASCII (e.g. CJK) directory paths in the mount dir are
    // re-encoded by the narrow-string CRT layer and silently 404. The
    // explicit handler keeps the path as-is end to end.
    //
    // [M1-B] CAESURA_EDITOR_WEBROOT override: when set and <dir>/index.html
    // exists, that directory becomes the static root -- the whole multi-file
    // SPA (index.html + assets/*.js/*.css) is mounted with httplib
    // set_mount_point so the assets are served with the correct Content-Type
    // (built-in extension table). The explicit GET / and GET /index.html
    // handlers stay registered with the resolved root so the documented route
    // count (36) is unchanged in both modes; note that httplib dispatches the
    // mount-point file handler BEFORE route handlers (routing()), so in
    // override mode the mount serves "/" and "/index.html" itself -- same
    // file, Content-Type without charset (modern browsers honor the SPA's
    // <meta charset>). An unset or invalid value leaves the probe-derived
    // webRoot untouched: fallback mode is byte-identical with the
    // pre-override build (single-file ifstream panel).
    std::string servedRoot   = m_webRoot;
    bool        overrideActive = false;
    if (const char* envRoot = std::getenv("CAESURA_EDITOR_WEBROOT")) {
        if (envRoot[0] != '\0') {
            std::error_code ec;
            fs::path ep(envRoot);
            if (fs::exists(ep / "index.html", ec)) {
                servedRoot      = ep.string();
                overrideActive  = true;
            } else {
                printf("[EDITOR] webroot override ignored (dir missing index.html): %s\n",
                       envRoot);
            }
        }
    }

    if (!servedRoot.empty() && fs::exists(servedRoot)) {
        const auto indexFile = (fs::path(servedRoot) / "index.html").string();
        auto serveIndex = [indexFile](const httplib::Request&, httplib::Response& res) {
            std::ifstream f(indexFile, std::ios::binary);
            if (!f) {
                res.status = 404;
                res.set_content("index.html not found", "text/plain");
                return;
            }
            std::string body((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
            res.set_content(body, "text/html; charset=utf-8");
        };
        if (overrideActive) {
            // Loud contract: "webroot override active" is only printed when
            // the mount actually registered, so a gate grep on that line
            // cannot be satisfied by a silent set_mount_point failure.
            const bool mounted = svr.set_mount_point("/", servedRoot);
            if (mounted) {
                printf("[EDITOR] webroot override active: %s\n", servedRoot.c_str());
            } else {
                printf("[EDITOR] webroot override mount FAILED: %s (single-file fallback)\n",
                       servedRoot.c_str());
                overrideActive = false;
            }
        }
        svr.Get("/", serveIndex);
        svr.Get("/index.html", serveIndex);
        if (!overrideActive) {
            printf("[EditorServer] Serving web editor from: %s\n", servedRoot.c_str());
        }
    }

    // ---------------------------------------------------------------------
    // Project Manager endpoints (Sprint 2, task book §6.3)
    //  GET    /api/project/templates   -> [{id,name,description,dir}]
    //  GET    /api/project/list        -> [{path,name,template,modified}]
    //  POST   /api/project/create      -> {template,name,path}
    //  POST   /api/project/duplicate   -> {srcPath,name}
    //  POST   /api/project/import      -> {srcPath,name} (srcPath is ANY
    //           on-disk directory; name must be sanitized)
    //  GET    /api/project/meta?path=  -> {ok,path,inferred,meta}
    //           (missing caesura.project.json infers defaults)
    //  POST   /api/project/meta        -> {path,meta{language,description,
    // template}} writes caesura.project.json
    //
    // These are EDITOR-ORIENTED meta operations (they manage filesystem
    // projects/templates, not the runtime protocol), so they are implemented
    // here instead of the engine RPC DTO chain -- consistent with the
    // task-book principle that Editor-internal RPC may evolve freely.
    // ---------------------------------------------------------------------

    // Templates root: tools/project_templates under the resolved engine root
    // (ctx.sourceRoot() -- the injected exe anchor / macro / CWD walk, NEVER a
    // separate resolution that could disagree with the services above). Paths
    // are confined there (no ".." / absolute escapes).
    const auto templatesRoot = [&ctx]() -> fs::path {
        return ctx.sourceRoot() / "tools" / "project_templates";
    };

    const auto confineTemplatePath = [&](const std::string& p) -> fs::path {
        std::string norm = p;
        for (auto& ch : norm) if (ch == char(92)) ch = '/';
        if (norm.find("..") != std::string::npos) return {};
        if (!norm.empty() && norm[0] == '/') return {};
        if (norm.find(':') != std::string::npos) return {};   // drive/scheme
        return templatesRoot() / norm;
    };

    svr.Get("/api/project/templates", [&projects](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        const auto r = projects.listTemplates();
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json");
    });
    svr.Get("/api/project/list", [&projects](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        const auto r = projects.list();
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json");
    });
    svr.Post("/api/project/create", [&projects](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        nlohmann::json body = nlohmann::json::object();
        try { body = nlohmann::json::parse(req.body); } catch (...) { }
        const auto r = projects.create(body.value("template", "basic"),
                                       body.value("name", std::string()));
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json");
    });
    svr.Post("/api/project/duplicate", [&projects](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        auto body = nlohmann::json::object();
        try { body = nlohmann::json::parse(req.body); } catch (...) { }
        const auto r = projects.duplicate(body.value("srcPath", std::string()), body.value("name", std::string()));
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json");
    });
    svr.Post("/api/project/import", [&projects](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        auto body = nlohmann::json::object();
        try { body = nlohmann::json::parse(req.body); } catch (...) { }
        const auto r = projects.importProject(body.value("srcPath", std::string()), body.value("name", std::string()));
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json");
    });
    svr.Get("/api/project/meta", [&projects](const httplib::Request& req, httplib::Response& res) {
        const auto r = projects.metaGet(req.get_param_value("path"));
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json");
    });
    svr.Post("/api/project/meta", [&projects](const httplib::Request& req, httplib::Response& res) {
        (void)req;
        nlohmann::json meta = nlohmann::json::object();
        try { meta = nlohmann::json::parse(req.body); } catch (...) { }
        auto m = meta.is_object() ? meta : nlohmann::json::object();
        const auto r = projects.metaSave(m.value("path", std::string()),
                                          m.value("meta", nlohmann::json::object()));
        res.status = r.status;
        res.set_content(r.body.dump(-1, ' ', false,
                                     nlohmann::json::error_handler_t::replace),
                        "application/json");
    });

    // ---------------------------------------------------------------------
    // Start listening
    // ---------------------------------------------------------------------
    printf("[EditorServer] Listening on port %d...\n", port);
    if (!svr.listen_after_bind()) {
        // t88: loud listen failure too (same silent-death class as bind).
        const std::string msg = "[EditorServer] [ERROR] failed to listen on 127.0.0.1:"
            + std::to_string(port) + ": socket error "
            + std::to_string(lastSocketError());
        fprintf(stderr, "%s\n", msg.c_str());
        { std::lock_guard<std::mutex> lock(m_dispatcherMutex); m_lastError = msg; }
        DEBUG_ERR(SubSys::Platform, ErrCode::Ok,
                  "[EditorServer] Failed to listen on port %d", port);
    }
    m_running = false;
}

} // namespace Caesura
