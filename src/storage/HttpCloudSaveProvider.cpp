// HttpCloudSaveProvider.cpp — see header. Uses httplib (header-only,
// exposed to every module via CaesarBuildOptions).
#include "HttpCloudSaveProvider.h"
#include "LocalFileSaveProvider.h"
#include "CloudSaveSnapshot.h"
#include <algorithm>
#include <charconv>
#include <httplib.h>
#include <cstdio>
#include <memory>

namespace Caesura {

HttpCloudSaveProvider::HttpCloudSaveProvider(std::string endpoint, int timeoutMs,
                                                   std::string bearerToken)
    : m_endpoint(std::move(endpoint))
    , m_timeoutMs(timeoutMs > 0 ? timeoutMs : 8000)
    , m_bearerToken(std::move(bearerToken))
    , m_local(std::make_unique<LocalFileSaveProvider>()) {}

CloudConditionalWriteSupport HttpCloudSaveProvider::conditionalWriteSupport(CloudSide) const {
    return CloudConditionalWriteSupport::Unsupported;
}

std::string HttpCloudSaveProvider::safeName(const std::string& slotPath) {
    // Strip any directory component: "saves/slot_3.json" -> "slot_3.json".
    const auto pos = slotPath.find_last_of("/\\");
    return pos == std::string::npos ? slotPath : slotPath.substr(pos + 1);
}

std::string HttpCloudSaveProvider::readFile(const std::string& path) {
    return m_local->readFile(path);
}

bool HttpCloudSaveProvider::writeFile(const std::string& path,
                                      const std::string& content) {
    return m_local->writeFile(path, content);
}

bool HttpCloudSaveProvider::deleteFile(const std::string& path) {
    return m_local->deleteFile(path);
}

std::vector<std::string> HttpCloudSaveProvider::listFiles(const std::string& pattern) {
    return m_local->listFiles(pattern);
}

// -- Cloud sync -------------------------------------------------------------

namespace {
// Split "{scheme}://host:port/prefix" into (host, port, prefix, tls).
// Returns false on malformed input. Both http:// and https:// are accepted
// (ST-2); https selects httplib::SSLClient and defaults to port 443.
bool splitEndpoint(const std::string& endpoint, std::string& host,
                   int& port, std::string& prefix, bool& tls) {
    host = endpoint;
    tls = false;
    const std::string schemeHttp = "http://";
    const std::string schemeHttps = "https://";
    if (host.rfind(schemeHttps, 0) == 0) {
        tls = true;
        host = host.substr(schemeHttps.size());
    } else if (host.rfind(schemeHttp, 0) == 0) {
        host = host.substr(schemeHttp.size());
    } else {
        return false;
    }
    auto slash = host.find('/');
    if (slash != std::string::npos) {
        prefix = host.substr(slash);
        host = host.substr(0, slash);
    }
    port = tls ? 443 : 80;
    auto colon = host.rfind(':');
    if (colon != std::string::npos) {
        port = std::atoi(host.substr(colon + 1).c_str());
        host = host.substr(0, colon);
    }
    return !host.empty() && port > 0;
}

// Max accepted payload from cloud pulls (ST-2): mirrors the local MAX_SAVE_SIZE
// guard so a hostile/misconfigured server cannot exhaust disk via
// pullFromCloud's direct local write.
constexpr size_t kMaxCloudPayload = 10u * 1024u * 1024u;

// Build an httplib client for the parsed endpoint, applying TLS, timeouts and
// the optional bearer token (ST-2).
std::unique_ptr<httplib::Client> makeClient(const std::string& endpoint,
                                            int timeoutMs,
                                            const std::string& bearer) {
    std::string host, prefix;
    int port = 0;
    bool tls = false;
    if (!splitEndpoint(endpoint, host, port, prefix, tls)) return nullptr;

    std::unique_ptr<httplib::Client> cli;
    if (tls) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        cli = std::make_unique<httplib::SSLClient>(host, port);
#else
        // No OpenSSL linked: an https endpoint must fail closed rather than
        // silently downgrading to plaintext (ST-2).
        return nullptr;
#endif
    } else {
        cli = std::make_unique<httplib::Client>(host, port);
    }
    cli->set_connection_timeout(2, 0);
    cli->set_read_timeout(timeoutMs / 1000, (timeoutMs % 1000) * 1000);
    cli->set_write_timeout(5, 0);
    if (!bearer.empty()) {
        cli->set_default_headers({{"Authorization", "Bearer " + bearer}});
    }
    return cli;
}

} // namespace

CloudSnapshot HttpCloudSaveProvider::readSnapshot(CloudSide side, const std::string& slotPath) {
    if (side == CloudSide::Local) return detail::readLocalCloudSnapshot(slotPath);
    CloudSnapshot snapshot;
    auto fail = [&](CloudReadState state, CloudReadError error) {
        snapshot.state = state;
        snapshot.error = error;
        snapshot.bytes.clear();
        return snapshot;
    };
    const auto name = safeName(slotPath);
    if (slotPath.find('\0') != std::string::npos || name.empty() || name == "." || name == "..")
        return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);

    // The legacy endpoint parser uses atoi; validate the whole authority here
    // without changing the old transport's parsing or copying contract.
    std::string host, prefix;
    int port = 0;
    bool tls = false;
    if (std::any_of(m_endpoint.begin(), m_endpoint.end(), [](unsigned char c) {
            return c <= 32 || c == 127 || c == '\\' || c == '?' || c == '#' || c == '@';
        }) || (m_endpoint.rfind("http://", 0) != 0 && m_endpoint.rfind("https://", 0) != 0))
        return fail(CloudReadState::Invalid, CloudReadError::InvalidEndpoint);
    const auto authorityStart = m_endpoint.find("://") + 3;
    const auto authorityEnd = m_endpoint.find('/', authorityStart);
    const auto authority = m_endpoint.substr(authorityStart, authorityEnd - authorityStart);
    const auto colon = authority.find(':');
    if (colon != std::string::npos) {
        const auto text = authority.substr(colon + 1);
        int parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed < 1 || parsed > 65535)
            return fail(CloudReadState::Invalid, CloudReadError::InvalidEndpoint);
    }
    if (!splitEndpoint(m_endpoint, host, port, prefix, tls))
        return fail(CloudReadState::Invalid, CloudReadError::InvalidEndpoint);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (tls) return fail(CloudReadState::Unsupported, CloudReadError::UnsupportedTransport);
#endif
    auto client = makeClient(m_endpoint, m_timeoutMs, m_bearerToken);
    if (!client || !client->is_valid())
        return fail(CloudReadState::Invalid, CloudReadError::InvalidEndpoint);
    client->set_follow_location(false);
    client->set_max_timeout(m_timeoutMs);
    const int connectMs = (std::min)(m_timeoutMs, 2000);
    client->set_connection_timeout(connectMs / 1000, (connectMs % 1000) * 1000);

    // Encode the single flat key, so '?'/'#'/'%' cannot become URL syntax.
    std::string encodedName;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            encodedName += static_cast<char>(c);
        else {
            encodedName += '%'; encodedName += hex[c >> 4]; encodedName += hex[c & 15];
        }
    }
    bool tooLarge = false;
    bool malformedFraming = false;
    // The streaming callback owns the cap. httplib's fixed-size decoder feeds
    // it without allocating a full body; its generic read error would otherwise
    // erase the distinction between oversized and truncated chunk streams.
    auto response = client->Get(prefix + "/" + encodedName,
        [&](const httplib::Response& headers) {
            snapshot.httpStatus = headers.status;
            const auto count = headers.get_header_value_count("Content-Length");
            const auto transfers = headers.get_header_value_count("Transfer-Encoding");
            if (count > 1 || transfers > 1 || (transfers && count) ||
                (transfers && !httplib::detail::case_ignore::equal(
                    headers.get_header_value("Transfer-Encoding"), "chunked"))) {
                malformedFraming = true; return false;
            }
            if (count == 1) {
                const auto text = headers.get_header_value("Content-Length");
                uint64_t length = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), length);
                if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
                    parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
                    malformedFraming = true; return false;
                }
                if (length > kMaxCloudPayload) { tooLarge = true; return false; }
            }
            return true;
        },
        [&](const char* bytes, size_t count) {
            if (count > kMaxCloudPayload - snapshot.bytes.size()) { tooLarge = true; return false; }
            snapshot.bytes.append(bytes, count);
            snapshot.observedBytes += count;
            return true;
        });
    if (tooLarge) return fail(CloudReadState::Invalid, CloudReadError::TooLarge);
    if (malformedFraming) return fail(CloudReadState::Failed, CloudReadError::Io);
    if (!response) {
        if (snapshot.httpStatus == 0)
            return fail(CloudReadState::Unavailable, CloudReadError::TransportUnavailable);
        return fail(CloudReadState::Failed, CloudReadError::Truncated);
    }
    snapshot.httpStatus = response->status;
    if (response->status == 404) return fail(CloudReadState::Missing, CloudReadError::None);
    if (response->status != 200) return fail(CloudReadState::Failed, CloudReadError::HttpStatus);
    snapshot.state = CloudReadState::Present;
    return snapshot;
}

bool HttpCloudSaveProvider::httpPut(const std::string& name,
                                    const std::string& body) {
    if (m_endpoint.empty()) return false;
    std::string host, prefix;
    int port = 0;
    bool tls = false;
    if (!splitEndpoint(m_endpoint, host, port, prefix, tls)) return false;

    auto cli = makeClient(m_endpoint, m_timeoutMs, m_bearerToken);
    if (!cli) return false;

    auto res = cli->Put(prefix + "/" + name, body, "application/octet-stream");
    return res && res->status == 200;
}

std::string HttpCloudSaveProvider::httpGet(const std::string& name) {
    if (m_endpoint.empty()) return std::string();
    std::string host, prefix;
    int port = 0;
    bool tls = false;
    if (!splitEndpoint(m_endpoint, host, port, prefix, tls)) return std::string();

    auto cli = makeClient(m_endpoint, m_timeoutMs, m_bearerToken);
    if (!cli) return std::string();

    auto res = cli->Get(prefix + "/" + name);
    if (!res || res->status != 200) return std::string();
    if (res->body.size() > kMaxCloudPayload) return std::string();  // ST-2
    return res->body;
}

bool HttpCloudSaveProvider::httpDelete(const std::string& name) {
    if (m_endpoint.empty()) return false;
    std::string host, prefix;
    int port = 0;
    bool tls = false;
    if (!splitEndpoint(m_endpoint, host, port, prefix, tls)) return false;

    auto cli = makeClient(m_endpoint, m_timeoutMs, m_bearerToken);
    if (!cli) return false;

    auto res = cli->Delete(prefix + "/" + name);
    return res && res->status == 200;
}

bool HttpCloudSaveProvider::pushToCloud(const std::string& slotPath) {
    const std::string content = readLocalFile(slotPath);
    if (content.empty()) return false;  // nothing local to push
    return writeCloudFile(slotPath, content);
}

bool HttpCloudSaveProvider::pullFromCloud(const std::string& slotPath) {
    const std::string body = readCloudFile(slotPath);
    if (body.empty()) return false;  // 404 or offline
    return writeLocalFile(slotPath, body);
}

std::string HttpCloudSaveProvider::readLocalFile(const std::string& slotPath) {
    return m_local->readFile(slotPath);
}

bool HttpCloudSaveProvider::writeLocalFile(const std::string& slotPath, const std::string& bytes) {
    return m_local->writeFile(slotPath, bytes);
}

std::string HttpCloudSaveProvider::readCloudFile(const std::string& slotPath) {
    return httpGet(safeName(slotPath));
}

bool HttpCloudSaveProvider::writeCloudFile(const std::string& slotPath, const std::string& bytes) {
    return httpPut(safeName(slotPath), bytes);
}

} // namespace Caesura
