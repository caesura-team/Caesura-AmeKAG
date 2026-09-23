#include "CloudCoordinatorState.h"
#include "CloudConflictStore.h"
#include <limits>
#include "AtomicSaveFile.h"
#include "CloudSaveSnapshot.h"
#include "../archive/api/ICryptoEngine.h"
#include "../di/BackendRegistry.h"
#include <nlohmann_json.hpp>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Caesura::detail {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Code = CloudCoordinatorCode;
using Crypto = carc::ICryptoEngine;
constexpr uint64_t payloadLimit = 10u * 1024u * 1024u;
constexpr uint64_t manifestLimit = 16u * 1024u;
constexpr uint64_t recordLimit = 128;
constexpr uint64_t storageLimit = 256u * 1024u * 1024u;
constexpr size_t scanEntryLimit = 4096;
constexpr size_t scanDepthLimit = 16;

struct CoordinatorError { Code code; };
void require(bool condition, Code code = Code::InvalidInput) {
    if (!condition) throw CoordinatorError{code};
}
bool lowerHex(const std::string& value, size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string hex(const uint8_t* bytes, size_t count) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(count * 2, '0');
    for (size_t i = 0; i < count; ++i) {
        result[2 * i] = digits[bytes[i] >> 4];
        result[2 * i + 1] = digits[bytes[i] & 15];
    }
    return result;
}
std::string digest(Crypto& crypto, const std::string& bytes) {
    std::array<uint8_t, 32> hash{};
    crypto.sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                  hash.data(), hash.size());
    return hex(hash.data(), hash.size());
}
std::string utf8Path(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// The owner and directory namespace must remain exclusive throughout a call.
// Windows handles also deny directory rename; POSIX identity rechecks detect
// replacement but do not turn the existing pathname writer into a dirfd writer.
struct DirectoryIdentity {
    fs::path path;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info{};
    ~DirectoryIdentity() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int handle = -1;
    struct stat info{};
    ~DirectoryIdentity() { if (handle >= 0) ::close(handle); }
#endif
    DirectoryIdentity() = default;
    DirectoryIdentity(const DirectoryIdentity&) = delete;
    DirectoryIdentity& operator=(const DirectoryIdentity&) = delete;
};
fs::file_status statusOf(const fs::path& path) {
    std::error_code error;
    auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return fs::file_status(fs::file_type::not_found);
    require(!error, Code::IoFailed);
#ifdef _WIN32
    if (fs::exists(status)) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES, Code::IoFailed);
        require((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
    }
#endif
    require(!fs::is_symlink(status));
    return status;
}

class DirectoryGuard {
public:
    explicit DirectoryGuard(const fs::path& root) {
        require(root.is_absolute() && !root.empty(), Code::InvalidInput);
        require(root.native().find(fs::path::value_type{}) == fs::path::string_type::npos,
                Code::InvalidInput);
        fs::path current = root.root_path();
#ifdef _WIN32
        // This local store does not accept UNC/device namespaces or ADS paths.
        const auto drive = root.root_name().native();
        require(drive.size() == 2 && drive[1] == L':' &&
                ((drive[0] >= L'A' && drive[0] <= L'Z') ||
                 (drive[0] >= L'a' && drive[0] <= L'z')), Code::InvalidInput);
#endif
        add(current);
        for (const auto& component : root.relative_path()) {
            if (component.empty()) continue; // optional trailing separator
            require(component != "." && component != "..", Code::InvalidInput);
#ifdef _WIN32
            const auto name = component.native();
            require(name.find(L':') == std::wstring::npos && name.back() != L'.' &&
                    name.back() != L' ', Code::InvalidInput);
#endif
            current /= component;
            add(current);
        }
    }
    void add(const fs::path& path) {
        require(fs::is_directory(statusOf(path)));
        auto entry = std::make_unique<DirectoryIdentity>();
        entry->path = path;
#ifdef _WIN32
        entry->handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(entry->handle != INVALID_HANDLE_VALUE, Code::IoFailed);
        require(GetFileInformationByHandle(entry->handle, &entry->info) != 0, Code::IoFailed);
        require((entry->info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (entry->info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
#else
        entry->handle = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        require(entry->handle >= 0, Code::IoFailed);
        require(::fstat(entry->handle, &entry->info) == 0, Code::IoFailed);
        require(S_ISDIR(entry->info.st_mode));
#endif
        m_entries.push_back(std::move(entry));
        verify();
    }
    void verify() const {
        for (const auto& entry : m_entries) {
            require(fs::is_directory(statusOf(entry->path)));
#ifdef _WIN32
            const HANDLE current = CreateFileW(entry->path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            require(current != INVALID_HANDLE_VALUE, Code::IoFailed);
            BY_HANDLE_FILE_INFORMATION info{};
            const bool read = GetFileInformationByHandle(current, &info) != 0;
            CloseHandle(current);
            require(read, Code::IoFailed);
            require((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                    info.dwVolumeSerialNumber == entry->info.dwVolumeSerialNumber &&
                    info.nFileIndexHigh == entry->info.nFileIndexHigh &&
                    info.nFileIndexLow == entry->info.nFileIndexLow);
#else
            struct stat info{};
            require(::lstat(entry->path.c_str(), &info) == 0, Code::IoFailed);
            require(S_ISDIR(info.st_mode) && info.st_dev == entry->info.st_dev &&
                    info.st_ino == entry->info.st_ino);
#endif
        }
    }
private:
    std::vector<std::unique_ptr<DirectoryIdentity>> m_entries;
};

uint64_t regularSize(const fs::path& path) {
    require(fs::is_regular_file(statusOf(path)));
    std::error_code error;
    const auto links = fs::hard_link_count(path, error);
    require(!error, Code::IoFailed);
    require(links == 1); // never alias a slot or an external record via hard link
    const auto size = fs::file_size(path, error);
    require(!error, Code::IoFailed);
    return size;
}
std::string readBytes(const fs::path& path, uint64_t limit) {
    const auto before = regularSize(path);
    require(before <= limit);
    auto snapshot = readLocalCloudSnapshot(utf8Path(path));
    if (snapshot.state == CloudReadState::Failed) throw CoordinatorError{Code::IoFailed};
    require(snapshot.state == CloudReadState::Present && snapshot.error == CloudReadError::None &&
            snapshot.bytes.size() == before && snapshot.bytes.size() <= limit);
    require(regularSize(path) == before);
    return std::move(snapshot.bytes);
}

// Reject duplicate keys, arrays and deep input BEFORE constructing a JSON DOM.
// The subsequent schema check also rejects every unknown or omitted member.
class ManifestSax final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(number_integer_t) override { return true; }
    bool number_unsigned(number_unsigned_t) override { return true; }
    bool number_float(number_float_t, const string_t&) override { return false; }
    bool string(string_t&) override { return true; }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override {
        if (m_keys.size() >= 4) return false;
        m_keys.emplace_back();
        return true;
    }
    bool key(string_t& key) override {
        return !m_keys.empty() && m_keys.back().insert(key).second;
    }
    bool end_object() override { m_keys.pop_back(); return true; }
    bool start_array(std::size_t) override { return false; }
    bool end_array() override { return false; }
    bool parse_error(std::size_t, const std::string&, const Json::exception&) override { return false; }
private:
    std::vector<std::set<std::string>> m_keys;
};
bool keys(const Json& object, std::initializer_list<const char*> names) {
    if (!object.is_object() || object.size() != names.size()) return false;
    for (const auto* name : names) if (!object.contains(name)) return false;
    return true;
}
std::string stringValue(const Json& value) {
    require(value.is_string());
    return value.get<std::string>();
}
uint64_t unsignedValue(const Json& value) {
    require(value.is_number_unsigned());
    return value.get<uint64_t>();
}

std::string identity(const fs::path& path) {
    DirectoryGuard guard(path);
#ifdef _WIN32
    const HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(h != INVALID_HANDLE_VALUE, Code::IoFailed);
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) != 0;
    CloseHandle(h);
    require(ok, Code::IoFailed);
    return std::to_string(info.dwVolumeSerialNumber) + ":" +
        std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow);
#else
    struct stat info{};
    require(::lstat(path.c_str(), &info) == 0, Code::IoFailed);
    return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
}
bool sameStamp(const CloudCoordinatorStamp& a, const CloudCoordinatorStamp& b) {
    return a.sessionId == b.sessionId && a.generation == b.generation;
}
std::string namespaceKey(const CloudCoordinatorBinding& b) {
    return b.scopeId + ":" + std::to_string(b.policyEpoch);
}
// The call guard survives callbacks into SaveManager. Rejected mutation leaves
// active objects alive, revokes their binding and makes every later checkpoint fail.
struct Call {
    CloudCoordinatorState& state;
    CloudCoordinatorStamp start;
    Crypto* crypto;
    std::unique_ptr<DirectoryGuard> directories;
    explicit Call(CloudCoordinatorState& s, bool needBinding = true) : state(s), start(s.stamp),
        crypto(BackendRegistry::instance().getCryptoEngine()) {
        require(s.owner == std::this_thread::get_id(), Code::ContextChanged);
        if (s.active) { s.invalidate(); throw CoordinatorError{Code::ContextChanged}; }
        require(!needBinding || s.bound, Code::NotConfigured);
        require(crypto != nullptr, Code::CryptoUnavailable);
        if (needBinding) {
            directories = std::make_unique<DirectoryGuard>(s.contextRoot);
            for (const char* name : {"records", "selected", "operations", "exports"})
                directories->add(s.contextRoot / name);
        }
        s.active = true;
    }
    ~Call() { state.active = false; }
    void check(bool binding = true) const {
        require(sameStamp(start, state.stamp) && (!binding || state.bound), Code::ContextChanged);
        require(BackendRegistry::instance().getCryptoEngine() == crypto, Code::ContextChanged);
        if (directories) directories->verify();
        if (binding) require(identity(fs::u8path(state.binding.root)) == state.rootIdentity, Code::ContextChanged);
    }
};
void makeDirectory(const fs::path& path, DirectoryGuard& guard) {
    guard.verify();
    if (!fs::exists(statusOf(path))) {
        std::error_code ec;
        require(fs::create_directory(path, ec) && !ec, Code::IoFailed);
    }
    guard.add(path);
}
struct Usage { uint64_t bytes = 0; size_t entries = 0; };
void countBytes(const fs::path& path, size_t depth, Usage& usage) {
    require(depth <= scanDepthLimit && ++usage.entries <= scanEntryLimit, Code::CapacityExceeded);
    if (fs::is_directory(statusOf(path))) {
        for (const auto& e : fs::directory_iterator(path)) countBytes(e.path(), depth + 1, usage);
    } else {
        const auto size = regularSize(path);
        require(size <= storageLimit - usage.bytes, Code::CapacityExceeded);
        usage.bytes += size;
    }
}
void capacity(const fs::path& root, const fs::path& attempts, uint64_t additional) {
    size_t count = 0;
    for (const auto& e : fs::directory_iterator(attempts)) {
        (void)e; require(++count < recordLimit, Code::CapacityExceeded);
    }
    Usage usage;
    countBytes(root, 0, usage);
    require(additional <= storageLimit - usage.bytes, Code::CapacityExceeded);
}
// Unknown/incomplete bytes are retained and count towards the same bounded tree.
void byteCapacity(const fs::path& root, uint64_t additional) {
    Usage usage; countBytes(root, 0, usage);
    require(additional <= storageLimit - usage.bytes, Code::CapacityExceeded);
}
Json strictJson(const std::string& raw) {
    require(raw.size() <= manifestLimit);
    ManifestSax sax;
    require(Json::sax_parse(raw, &sax));
    return Json::parse(raw);
}
Json context(const CloudCoordinatorState& s, int slot) {
    return Json{{"scope_id", s.binding.scopeId}, {"policy_epoch", s.binding.policyEpoch}, {"slot", slot}};
}
int slotFrom(const Json& value, const CloudCoordinatorState& s) {
    require(keys(value, {"scope_id", "policy_epoch", "slot"}));
    require(stringValue(value.at("scope_id")) == s.binding.scopeId &&
        unsignedValue(value.at("policy_epoch")) == s.binding.policyEpoch);
    const auto& n = value.at("slot");
    require(n.is_number_integer());
    const auto slot = n.get<int64_t>();
    require(slot >= -2 && slot <= 99);
    return static_cast<int>(slot);
}
Json refJson(const CloudPreservedRecordRef& r) {
    return Json{{"id", r.id}, {"manifest_sha256", r.manifestSha256}};
}
Json optionalRef(const std::optional<CloudPreservedRecordRef>& r) { return r ? refJson(*r) : Json(nullptr); }
std::optional<CloudPreservedRecordRef> parseRef(const Json& j) {
    if (j.is_null()) return {};
    require(keys(j, {"id", "manifest_sha256"}));
    CloudPreservedRecordRef r{stringValue(j.at("id")), stringValue(j.at("manifest_sha256"))};
    require(lowerHex(r.id, 32) && lowerHex(r.manifestSha256, 64));
    return r;
}
ConflictRecordRef rawRef(const CloudPreservedRecordRef& r) { return {r.id, r.manifestSha256}; }
CloudPreservedRecordRef publicRef(const ConflictRecordRef& r) { return {r.id, r.manifestSha256}; }
Json prepJson(const CloudPreparationRef& p) { return Json{{"token", p.token}, {"receipt_sha256", p.receiptSha256}}; }
CloudPreparationRef prepRef(const Json& j) {
    require(keys(j, {"token", "receipt_sha256"}));
    CloudPreparationRef p{stringValue(j.at("token")), stringValue(j.at("receipt_sha256"))};
    require(lowerHex(p.token, 32) && lowerHex(p.receiptSha256, 64));
    return p;
}
Json sideJson(const CloudObservedSide& side) {
    return Json{{"state", static_cast<int>(side.state)}, {"error", static_cast<int>(side.error)},
        {"validity", static_cast<int>(side.validity)}, {"size", side.byteCount}, {"sha256", side.sha256}};
}
int enumValue(const Json& j, int last) {
    require(j.is_number_integer());
    const auto value = j.get<int64_t>();
    require(value >= 0 && value <= last);
    return static_cast<int>(value);
}
CloudObservedSide sideFrom(const Json& j) {
    require(keys(j, {"state", "error", "validity", "size", "sha256"}));
    CloudObservedSide s;
    s.state = static_cast<CloudReadState>(enumValue(j.at("state"), static_cast<int>(CloudReadState::Unsupported)));
    s.error = static_cast<CloudReadError>(enumValue(j.at("error"), static_cast<int>(CloudReadError::ChangedDuringRead)));
    s.validity = static_cast<CloudSaveValidity>(enumValue(j.at("validity"), static_cast<int>(CloudSaveValidity::InvalidCurrentPolicy)));
    s.byteCount = unsignedValue(j.at("size")); s.sha256 = stringValue(j.at("sha256"));
    if (s.state == CloudReadState::Present) require(s.byteCount <= payloadLimit &&
        s.error == CloudReadError::None && lowerHex(s.sha256, 64));
    else require(s.sha256.empty());
    return s;
}
bool isKnown(const CloudObservedSide& s) { return s.state == CloudReadState::Present || s.state == CloudReadState::Missing; }
bool validBytes(Call& call, int slot, const std::string& bytes, const CloudCoordinatorState::Validate& validate) {
    bool valid = false;
    try { valid = validate(slot, bytes); } catch (...) { /* current policy rejected the payload */ }
    call.check();
    return valid;
}
CloudObservedSide observe(Call& call, int slot, const CloudSnapshot& snapshot,
                          const CloudCoordinatorState::Validate& validate) {
    CloudObservedSide s;
    s.state = snapshot.state; s.error = snapshot.error;
    s.byteCount = snapshot.observedBytes;
    bool malformed = static_cast<unsigned>(s.state) > static_cast<unsigned>(CloudReadState::Unsupported) ||
        static_cast<unsigned>(s.error) > static_cast<unsigned>(CloudReadError::ChangedDuringRead);
    if (s.state == CloudReadState::Present)
        malformed = malformed || s.error != CloudReadError::None || snapshot.bytes.size() > payloadLimit ||
            snapshot.bytes.size() != snapshot.observedBytes;
    else {
        malformed = malformed || !snapshot.bytes.empty();
        if (s.state == CloudReadState::Missing)
            malformed = malformed || s.error != CloudReadError::None || s.byteCount != 0;
    }
    if (malformed) {
        // A contradictory provider result is never authoritative absence. Keep
        // its diagnostic byte count, but expose no usable payload identity.
        s.state = CloudReadState::Invalid;
        s.error = CloudReadError::MalformedMetadata;
        s.validity = CloudSaveValidity::NotChecked;
        return s;
    }
    if (s.state == CloudReadState::Present) {
        s.byteCount = snapshot.bytes.size(); s.sha256 = digest(*call.crypto, snapshot.bytes);
        call.check();
        s.validity = validBytes(call, slot, snapshot.bytes, validate) ?
            CloudSaveValidity::ValidCurrentPolicy : CloudSaveValidity::InvalidCurrentPolicy;
    } else {
        s.validity = s.state == CloudReadState::Missing ? CloudSaveValidity::NotPresent : CloudSaveValidity::NotChecked;
    }
    return s;
}
void writeFile(const fs::path& path, const std::string& bytes, DirectoryGuard& guard,
               bool replace, Code failure, bool& published) {
    guard.verify();
    const bool exists = fs::exists(statusOf(path));
    require(replace || !exists, failure);
    if (exists) (void)regularSize(path);
    const auto native = path.string();
    require(fs::path(native) == path, failure);
    require(writeSaveFileAtomically(native, bytes), failure);
    published = true;
    guard.verify();
    require(readBytes(path, bytes.size()) == bytes, Code::Indeterminate);
}
std::string cursorBytes(const CloudCoordinatorState& s, int slot, const CloudPreparationRef& p,
                        const CloudPreservedRecordRef& r, const Json& validation) {
    return Json{{"schema_version", 1}, {"context", context(s, slot)}, {"record", refJson(r)},
        {"preparation", prepJson(p)}, {"validation", validation}}.dump();
}
fs::path cursorPath(const CloudCoordinatorState& s, int slot) {
    return s.contextRoot / "selected" / ("slot_" + std::to_string(slot) + ".json");
}
std::optional<std::string> cursorRaw(const CloudCoordinatorState& s, int slot) {
    DirectoryGuard guard(s.contextRoot / "selected");
    const auto path = cursorPath(s, slot);
    if (!fs::exists(statusOf(path))) return {};
    return readBytes(path, manifestLimit);
}
void closure(const fs::path& dir, std::initializer_list<const char*> names, bool observed = false) {
    std::set<std::string> allowed;
    for (const auto* name : names) allowed.insert(name);
    if (observed) allowed.insert("observed.bin");
    size_t count = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        require(++count <= allowed.size() && allowed.count(utf8Path(e.path().filename())) == 1);
        (void)regularSize(e.path());
    }
    require(count == allowed.size());
}
Json request(const CloudCoordinatorState& s, int slot) {
    return Json{{"schema_version", 1}, {"purpose", "prepare"}, {"context", context(s, slot)}};
}
struct OpenPreparation {
    CloudPrepareResult result;
    Json document;
    std::string raw;
    std::optional<std::string> local, cloud, base;
};
PreservedConflictRecord openRecord(Call& call, int slot, const CloudPreservedRecordRef& ref) {
    CloudConflictStore store(call.state.contextRoot / "records");
    const auto found = store.readRecord({call.state.binding.scopeId, slot, call.state.binding.policyEpoch}, rawRef(ref));
    call.check();
    require(found.code == ConflictStoreCode::Complete && found.record.has_value());
    return *found.record;
}
OpenPreparation openPreparation(Call& call, const CloudPreparationRef& expected,
                                const CloudCoordinatorState::Validate& validate) {
    require(lowerHex(expected.token, 32) && lowerHex(expected.receiptSha256, 64));
    auto& s = call.state;
    const auto dir = s.contextRoot / "operations" / expected.token;
    DirectoryGuard guard(dir);
    OpenPreparation opened;
    opened.raw = readBytes(dir / "receipt.json", manifestLimit);
    require(digest(*call.crypto, opened.raw) == expected.receiptSha256);
    call.check();
    auto& j = opened.document;
    j = strictJson(opened.raw);
    require(keys(j, {"schema_version", "context", "token", "validation", "code", "comparison",
        "preservation", "local", "cloud", "record", "base", "candidate_record", "old_cursor", "desired"}));
    require(unsignedValue(j.at("schema_version")) == 1 && stringValue(j.at("token")) == expected.token);
    auto& out = opened.result;
    out.slot = slotFrom(j.at("context"), s); out.stamp = s.stamp; out.preparation = expected;
    require(strictJson(readBytes(dir / "request.json", manifestLimit)) == request(s, out.slot));
    const auto& validation = j.at("validation");
    require(keys(validation, {"schema", "policy"}));
    require(unsignedValue(validation.at("schema")) > 0 && enumValue(validation.at("policy"), 1) >= 0);
    out.code = static_cast<Code>(enumValue(j.at("code"), static_cast<int>(Code::UnsupportedConditionalWrite)));
    out.comparison = static_cast<CloudComparison>(enumValue(j.at("comparison"), static_cast<int>(CloudComparison::InvalidSave)));
    out.preservation = static_cast<CloudPreservation>(enumValue(j.at("preservation"), static_cast<int>(CloudPreservation::Indeterminate)));
    out.local = sideFrom(j.at("local")); out.cloud = sideFrom(j.at("cloud"));
    out.record = parseRef(j.at("record")); out.base = parseRef(j.at("base"));
    out.candidateRecord = parseRef(j.at("candidate_record"));
    const auto desired = parseRef(j.at("desired"));
    require(j.at("old_cursor").is_null() || lowerHex(stringValue(j.at("old_cursor")), 64));
    bool observed = false;
    if (out.record) {
        require(out.preservation == CloudPreservation::Complete);
        const auto raw = openRecord(call, out.slot, *out.record);
        opened.local = raw.localBytes; opened.cloud = raw.cloudBytes; opened.base = raw.baseBytes;
        require(optionalRef(out.base) == (raw.baseRef ? refJson(publicRef(*raw.baseRef)) : Json(nullptr)));
    } else if (out.preservation == CloudPreservation::Complete) {
        require((out.local.state == CloudReadState::Present && out.cloud.state == CloudReadState::Missing) ||
            (out.cloud.state == CloudReadState::Present && out.local.state == CloudReadState::Missing));
        observed = true;
        const auto bytes = readBytes(dir / "observed.bin", payloadLimit);
        if (out.local.state == CloudReadState::Present) opened.local = bytes; else opened.cloud = bytes;
    }
    auto verifySide = [&](CloudObservedSide& side, const std::optional<std::string>& bytes) {
        if (!bytes) return;
        require(side.state == CloudReadState::Present && bytes->size() == side.byteCount &&
            digest(*call.crypto, *bytes) == side.sha256);
        side.validity = validBytes(call, out.slot, *bytes, validate) ?
            CloudSaveValidity::ValidCurrentPolicy : CloudSaveValidity::InvalidCurrentPolicy;
    };
    verifySide(out.local, opened.local); verifySide(out.cloud, opened.cloud);
    if (opened.base) require(validBytes(call, out.slot, *opened.base, validate), Code::InvalidSave);
    if (desired) {
        require(out.record && refJson(*desired) == refJson(*out.record) && opened.local && opened.cloud &&
            *opened.local == *opened.cloud && out.local.validity == CloudSaveValidity::ValidCurrentPolicy &&
            out.cloud.validity == CloudSaveValidity::ValidCurrentPolicy);
        out.candidateCursorSha256 = digest(*call.crypto, cursorBytes(s, out.slot, expected, *desired, validation));
    }
    closure(dir, {"request.json", "receipt.json"}, observed);
    require(readBytes(dir / "receipt.json", manifestLimit) == opened.raw);
    guard.verify(); call.check();
    return opened;
}
std::optional<CloudPreservedRecordRef> selected(Call& call, int slot,
    const std::optional<std::string>& raw, const CloudCoordinatorState::Validate& validate) {
    if (!raw) return {};
    try {
        const auto j = strictJson(*raw);
        require(keys(j, {"schema_version", "context", "record", "preparation", "validation"}));
        require(unsignedValue(j.at("schema_version")) == 1 && slotFrom(j.at("context"), call.state) == slot);
        const auto r = parseRef(j.at("record")); require(r.has_value());
        const auto p = prepRef(j.at("preparation"));
        const auto opened = openPreparation(call, p, validate);
        require(opened.result.slot == slot && opened.document.at("desired") == refJson(*r) &&
            *raw == cursorBytes(call.state, slot, p, *r, opened.document.at("validation")));
        require(cursorRaw(call.state, slot) == raw);
        return r;
    } catch (const CoordinatorError& e) {
        if (e.code == Code::ContextChanged) throw;
        throw CoordinatorError{Code::InvalidAncestor};
    } catch (...) { throw CoordinatorError{Code::InvalidAncestor}; }
}
void recovered(Call& call, OpenPreparation& opened, const CloudCoordinatorState::Validate& validate) {
    auto& r = opened.result;
    r.code = Code::Replayed;
    if (opened.document.at("desired").is_null()) { r.ancestor = CloudAncestorSelection::NotRequested; return; }
    const auto raw = cursorRaw(call.state, r.slot);
    const auto current = selected(call, r.slot, raw, validate);
    const Json sha = raw ? Json(digest(*call.crypto, *raw)) : Json(nullptr);
    if (sha == r.candidateCursorSha256) r.ancestor = CloudAncestorSelection::RecoveredSelected;
    else if (sha == opened.document.at("old_cursor")) r.ancestor = CloudAncestorSelection::RecoveredNotSelected;
    else { require(current.has_value()); r.ancestor = CloudAncestorSelection::Superseded; }
}
std::string variantBytes(const OpenPreparation& p, CloudPreservedVariant variant) {
    const std::optional<std::string>* bytes = nullptr;
    switch (variant) {
    case CloudPreservedVariant::Local: bytes = &p.local; break;
    case CloudPreservedVariant::Cloud: bytes = &p.cloud; break;
    case CloudPreservedVariant::Base: bytes = &p.base; break;
    default: throw CoordinatorError{Code::InvalidInput};
    }
    require(bytes->has_value(), Code::MissingVariant);
    return **bytes;
}
void choiceContext(const CloudCoordinatorState& s, const CloudHistorySelection& choice) {
    require(s.bound && sameStamp(s.stamp, choice.stamp), Code::StaleContext);
}
} // namespace

bool CloudCoordinatorState::allowMutation() {
    if (owner != std::this_thread::get_id()) return false;
    if (active) { invalidate(); return false; }
    return true;
}
void CloudCoordinatorState::invalidate() {
    if (bound) revoked.insert(namespaceKey(binding));
    bound = false;
    if (stamp.generation != std::numeric_limits<uint64_t>::max()) ++stamp.generation;
}
CloudCoordinatorBindResult CloudCoordinatorState::bind(const CloudCoordinatorBinding& next) {
    CloudCoordinatorBindResult out;
    try {
        Call call(*this, false);
        require(lowerHex(next.scopeId, 32) && next.policyEpoch != 0 && !next.root.empty(), Code::InvalidInput);
        require(stamp.generation < std::numeric_limits<uint64_t>::max(), Code::ContextChanged);
        require(revoked.count(namespaceKey(next)) == 0, Code::ContextChanged);
        const auto root = fs::u8path(next.root);
        DirectoryGuard guard(root);
        const auto physical = identity(root);
        if (bound && (namespaceKey(binding) != namespaceKey(next) || physical != rootIdentity)) invalidate();
        call.start = stamp;
        if (stamp.sessionId.empty()) {
            std::array<uint8_t, 16> random{};
            call.crypto->generateNonce(random.data(), random.size());
            call.check(false);
            stamp.sessionId = hex(random.data(), random.size());
            call.start = stamp;
        }
        const auto scope = root / next.scopeId;
        makeDirectory(scope, guard);
        const auto contextDir = scope / std::to_string(next.policyEpoch);
        makeDirectory(contextDir, guard);
        for (const char* name : {"records", "selected", "operations", "exports"}) makeDirectory(contextDir / name, guard);
        call.check(false); guard.verify();
        binding = next; contextRoot = contextDir; rootIdentity = physical;
        bound = true;
        ++stamp.generation;
        out.code = Code::Ready; out.stamp = stamp;
    } catch (const CoordinatorError& e) { out.code = e.code; }
      catch (...) { out.code = Code::IoFailed; }
    return out;
}
CloudPrepareResult CloudCoordinatorState::prepare(int slot, const std::string& token,
    ICloudSaveSnapshotTransport* transport, const std::string& path,
    int schema, int policy, const Validate& validate) {
    CloudPrepareResult out;
    out.slot = slot;
    try {
        Call call(*this);
        out.stamp = stamp;
        call.check();
        require(slot >= -2 && slot <= 99 && lowerHex(token, 32) && !path.empty(), Code::InvalidInput);
        require(transport != nullptr, Code::UnsupportedSnapshot);
        DirectoryGuard guard(contextRoot);
        const auto dir = contextRoot / "operations" / token;
        const auto requestRaw = request(*this, slot).dump();
        if (fs::exists(statusOf(dir))) {
            try {
                DirectoryGuard existing(dir);
                const auto original = strictJson(readBytes(dir / "request.json", manifestLimit));
                require(original == request(*this, slot), Code::TokenMismatch);
                const auto raw = readBytes(dir / "receipt.json", manifestLimit);
                auto opened = openPreparation(call, {token, digest(*call.crypto, raw)}, validate);
                recovered(call, opened, validate);
                return opened.result;
            } catch (const CoordinatorError& e) {
                if (e.code == Code::TokenMismatch || e.code == Code::ContextChanged) throw;
                throw CoordinatorError{Code::Indeterminate};
            } catch (...) { throw CoordinatorError{Code::Indeterminate}; }
        }
        const auto oldCursor = cursorRaw(*this, slot);
        out.base = selected(call, slot, oldCursor, validate);
        capacity(contextRoot, contextRoot / "operations", requestRaw.size());
        call.check(); guard.verify();
        require(fs::create_directory(dir), Code::IoFailed);
        guard.add(dir);
        bool published = false;
        writeFile(dir / "request.json", requestRaw, guard, false, Code::IoFailed, published);
        call.check();
        auto capture = [&](CloudSide side) {
            CloudSnapshot snapshot;
            try { snapshot = transport->readSnapshot(side, path); }
            catch (...) { snapshot.state = CloudReadState::Failed; snapshot.error = CloudReadError::Io; }
            call.check();
            return snapshot;
        };
        const auto local = capture(CloudSide::Local);
        const auto cloud = capture(CloudSide::Cloud);
        out.local = observe(call, slot, local, validate);
        out.cloud = observe(call, slot, cloud, validate);
        out.code = Code::Complete;
        out.ancestor = CloudAncestorSelection::NotRequested;
        std::optional<CloudPreservedRecordRef> desired;
        const bool localPresent = out.local.state == CloudReadState::Present;
        const bool cloudPresent = out.cloud.state == CloudReadState::Present;
        const bool invalid = (localPresent && out.local.validity != CloudSaveValidity::ValidCurrentPolicy) ||
            (cloudPresent && out.cloud.validity != CloudSaveValidity::ValidCurrentPolicy);
        if (!isKnown(out.local) || !isKnown(out.cloud)) {
            out.code = Code::InspectionIncomplete;
            out.comparison = CloudComparison::InspectionIncomplete;
        } else if (localPresent && cloudPresent) {
            const uint64_t payloads = local.bytes.size() + cloud.bytes.size() +
                (out.base ? openRecord(call, slot, *out.base).localBytes.size() : 0);
            byteCapacity(contextRoot, payloads + 3 * manifestLimit);
            CloudConflictStore store(contextRoot / "records");
            std::optional<ConflictRecordRef> base;
            if (out.base) base = rawRef(*out.base);
            const auto saved = store.preserve({binding.scopeId, slot, binding.policyEpoch}, local, cloud, base);
            if (saved.candidateRef) out.candidateRecord = publicRef(*saved.candidateRef);
            call.check();
            if (saved.code == ConflictStoreCode::Preserved && saved.record) {
                out.preservation = CloudPreservation::Complete;
                out.record = publicRef(saved.record->ref);
                switch (saved.record->kind) {
                case ConflictRecordKind::EqualObserved: out.comparison = CloudComparison::EqualObserved; break;
                case ConflictRecordKind::LocalChanged: out.comparison = CloudComparison::LocalChanged; break;
                case ConflictRecordKind::CloudChanged: out.comparison = CloudComparison::CloudChanged; break;
                case ConflictRecordKind::Conflict: out.comparison = CloudComparison::Conflict; break;
                case ConflictRecordKind::DivergedWithoutBase: out.comparison = CloudComparison::DivergedWithoutBase; break;
                }
                if (!invalid && out.comparison == CloudComparison::EqualObserved) desired = out.record;
            } else if (saved.code == ConflictStoreCode::Indeterminate) {
                out.preservation = CloudPreservation::Indeterminate; out.code = Code::PreservationIndeterminate;
            } else {
                out.preservation = CloudPreservation::Failed;
                out.code = saved.code == ConflictStoreCode::CapacityExceeded ? Code::CapacityExceeded : Code::PreservationFailed;
            }
        } else if (localPresent || cloudPresent) {
            out.comparison = CloudComparison::OneSideMissing;
            const auto& bytes = localPresent ? local.bytes : cloud.bytes;
            byteCapacity(contextRoot, bytes.size() + manifestLimit);
            bool saved = false;
            writeFile(dir / "observed.bin", bytes, guard, false, Code::PreservationFailed, saved);
            call.check();
            out.preservation = CloudPreservation::Complete;
        } else out.comparison = CloudComparison::BothMissing;
        if (invalid && out.code == Code::Complete) {
            out.code = Code::InvalidSave; out.comparison = CloudComparison::InvalidSave;
        }
        call.check();
        const Json validation{{"schema", schema}, {"policy", policy}};
        const Json oldSha = oldCursor ? Json(digest(*call.crypto, *oldCursor)) : Json(nullptr);
        const auto receipt = Json{{"schema_version", 1}, {"context", context(*this, slot)}, {"token", token},
            {"validation", validation}, {"code", static_cast<int>(out.code)},
            {"comparison", static_cast<int>(out.comparison)}, {"preservation", static_cast<int>(out.preservation)},
            {"local", sideJson(out.local)}, {"cloud", sideJson(out.cloud)},
            {"record", optionalRef(out.record)}, {"base", optionalRef(out.base)},
            {"candidate_record", optionalRef(out.candidateRecord)}, {"old_cursor", oldSha},
            {"desired", optionalRef(desired)}}.dump();
        require(receipt.size() <= manifestLimit, Code::InvalidInput);
        byteCapacity(contextRoot, receipt.size() + (desired ? manifestLimit : 0));
        const CloudPreparationRef reference{token, digest(*call.crypto, receipt)};
        bool receiptPublished = false;
        try {
            writeFile(dir / "receipt.json", receipt, guard, false,
                out.record ? Code::RecordPreservedReceiptFailed : Code::IoFailed, receiptPublished);
            out.preparation = reference;
            require(digest(*call.crypto, readBytes(dir / "receipt.json", manifestLimit)) == reference.receiptSha256);
            call.check();
        } catch (...) {
            if (receiptPublished) { out.preparation = reference; out.code = Code::Indeterminate; return out; }
            throw;
        }
        if (desired) {
            const auto raw = cursorBytes(*this, slot, reference, *desired, validation);
            out.candidateCursorSha256 = digest(*call.crypto, raw);
            bool cursorPublished = false;
            try {
                call.check();
                require(cursorRaw(*this, slot) == oldCursor, Code::ContextChanged);
                writeFile(cursorPath(*this, slot), raw, guard, true, Code::IoFailed, cursorPublished);
                // This digest is deliberately after publication and actual reopening.
                require(digest(*call.crypto, *cursorRaw(*this, slot)) == out.candidateCursorSha256);
                (void)selected(call, slot, cursorRaw(*this, slot), validate);
                call.check();
                out.ancestor = CloudAncestorSelection::Selected;
            } catch (...) {
                out.ancestor = cursorPublished ? CloudAncestorSelection::Indeterminate : CloudAncestorSelection::NotSelected;
                if (cursorPublished) out.code = Code::Indeterminate;
                else if (!bound) out.code = Code::ContextChanged;
            }
        } else {
            (void)openPreparation(call, reference, validate);
        }
        guard.verify(); call.check();
    } catch (const CoordinatorError& e) { out.code = e.code; }
      catch (...) { out.code = Code::IoFailed; }
    return out;
}
CloudPrepareResult CloudCoordinatorState::reopen(const CloudPreparationRef& expected, const Validate& validate) {
    CloudPrepareResult out;
    try {
        Call call(*this); call.check();
        auto opened = openPreparation(call, expected, validate);
        recovered(call, opened, validate);
        return opened.result;
    } catch (const CoordinatorError& e) { out.code = e.code; }
      catch (...) { out.code = Code::InvalidInput; }
    return out;
}
CloudPreparationList CloudCoordinatorState::list(int slot, const Validate& validate) {
    CloudPreparationList out;
    try {
        Call call(*this); call.check();
        require(slot >= -2 && slot <= 99, Code::InvalidInput);
        DirectoryGuard guard(contextRoot / "operations");
        byteCapacity(contextRoot, 0);
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(contextRoot / "operations")) {
            require(++count <= recordLimit, Code::CapacityExceeded);
            const auto token = utf8Path(entry.path().filename());
            if (!lowerHex(token, 32) || !fs::is_directory(statusOf(entry.path()))) { ++out.invalidOperations; continue; }
            if (!fs::exists(statusOf(entry.path() / "request.json")) || !fs::exists(statusOf(entry.path() / "receipt.json"))) {
                ++out.incompleteOperations; continue;
            }
            try {
                const auto raw = readBytes(entry.path() / "receipt.json", manifestLimit);
                const CloudPreparationRef ref{token, digest(*call.crypto, raw)};
                const auto opened = openPreparation(call, ref, validate);
                if (opened.result.slot == slot) out.preparations.push_back(ref);
            } catch (const CoordinatorError& e) {
                if (e.code == Code::ContextChanged) throw;
                ++out.invalidOperations;
            } catch (...) { ++out.invalidOperations; }
        }
        std::sort(out.preparations.begin(), out.preparations.end(),
            [](const auto& a, const auto& b) { return a.token < b.token; });
        guard.verify(); call.check();
        out.code = Code::Complete;
    } catch (const CoordinatorError& e) { out.code = e.code; }
      catch (...) { out.code = Code::IoFailed; }
    return out;
}
CloudHistoryExportResult CloudCoordinatorState::exportHistory(const CloudHistorySelection& selection,
    const std::string& token, const Validate& validate) {
    CloudHistoryExportResult out;
    out.token = token;
    bool payloadPublished = false, receiptPublished = false;
    try {
        choiceContext(*this, selection);
        Call call(*this); call.check();
        require(lowerHex(token, 32), Code::InvalidInput);
        const auto opened = openPreparation(call, selection.preparation, validate);
        const auto bytes = variantBytes(opened, selection.variant);
        require(validBytes(call, opened.result.slot, bytes, validate), Code::InvalidSave);
        const auto sha = digest(*call.crypto, bytes);
        const auto dir = contextRoot / "exports" / token;
        const auto requestObject = Json{{"schema_version", 1}, {"purpose", "export"},
            {"context", context(*this, opened.result.slot)}, {"preparation", prepJson(selection.preparation)},
            {"variant", static_cast<int>(selection.variant)}};
        const auto requestRaw = requestObject.dump();
        const auto receiptRaw = Json{{"schema_version", 1}, {"request_sha256", digest(*call.crypto, requestRaw)},
            {"size", static_cast<uint64_t>(bytes.size())}, {"sha256", sha}}.dump();
        const auto receiptSha = digest(*call.crypto, receiptRaw);
        auto verifyOutput = [&] {
            DirectoryGuard guard(dir);
            require(readBytes(dir / "request.json", manifestLimit) == requestRaw);
            require(readBytes(dir / "save.bin", payloadLimit) == bytes);
            const auto raw = readBytes(dir / "receipt.json", manifestLimit);
            require(strictJson(raw) == strictJson(receiptRaw) && raw == receiptRaw);
            require(digest(*call.crypto, raw) == receiptSha);
            closure(dir, {"request.json", "save.bin", "receipt.json"});
            // Retained source references are rechecked after every writer callback.
            const auto final = openPreparation(call, selection.preparation, validate);
            require(variantBytes(final, selection.variant) == bytes);
            choiceContext(*this, selection); guard.verify(); call.check();
        };
        if (fs::exists(statusOf(dir))) {
            try {
                DirectoryGuard existing(dir);
                require(strictJson(readBytes(dir / "request.json", manifestLimit)) == requestObject, Code::TokenMismatch);
                verifyOutput();
            } catch (const CoordinatorError& e) {
                if (e.code == Code::TokenMismatch || e.code == Code::ContextChanged) throw;
                throw CoordinatorError{Code::Indeterminate};
            } catch (...) { throw CoordinatorError{Code::Indeterminate}; }
            out.code = Code::Replayed;
        } else {
            DirectoryGuard guard(contextRoot);
            capacity(contextRoot, contextRoot / "exports", requestRaw.size() + bytes.size() + receiptRaw.size());
            call.check(); guard.verify();
            require(fs::create_directory(dir), Code::IoFailed);
            guard.add(dir);
            bool requestPublished = false;
            writeFile(dir / "request.json", requestRaw, guard, false, Code::IoFailed, requestPublished);
            call.check();
            writeFile(dir / "save.bin", bytes, guard, false, Code::IoFailed, payloadPublished);
            call.check();
            writeFile(dir / "receipt.json", receiptRaw, guard, false, Code::Indeterminate, receiptPublished);
            verifyOutput();
            out.code = Code::Complete;
        }
        out.path = utf8Path(dir / "save.bin"); out.byteCount = bytes.size(); out.sha256 = sha; out.receiptSha256 = receiptSha;
    } catch (const CoordinatorError& e) { out.code = payloadPublished || receiptPublished ? Code::Indeterminate : e.code; }
      catch (...) { out.code = payloadPublished || receiptPublished ? Code::Indeterminate : Code::InvalidInput; }
    return out;
}
CloudPublicationCheck CloudCoordinatorState::check(const CloudHistorySelection& selection, CloudSide destination,
    ICloudSaveSnapshotTransport* transport, const std::function<std::string(int)>& path, const Validate& validate) {
    CloudPublicationCheck out;
    try {
        choiceContext(*this, selection);
        Call call(*this); call.check();
        require(destination == CloudSide::Local || destination == CloudSide::Cloud, Code::InvalidInput);
        require(transport != nullptr, Code::UnsupportedSnapshot);
        const auto opened = openPreparation(call, selection.preparation, validate);
        const auto bytes = variantBytes(opened, selection.variant);
        require(validBytes(call, opened.result.slot, bytes, validate), Code::InvalidSave);
        const auto name = path(opened.result.slot);
        auto capture = [&](CloudSide side) {
            CloudSnapshot value;
            try { value = transport->readSnapshot(side, name); }
            catch (...) { value.state = CloudReadState::Failed; value.error = CloudReadError::Io; }
            call.check();
            return value;
        };
        const auto local = capture(CloudSide::Local), cloud = capture(CloudSide::Cloud);
        out.currentLocal = observe(call, opened.result.slot, local, validate);
        out.currentCloud = observe(call, opened.result.slot, cloud, validate);
        if (!isKnown(out.currentLocal) || !isKnown(out.currentCloud)) out.code = Code::InspectionIncomplete;
        else if (sideJson(out.currentLocal) != sideJson(opened.result.local) ||
                 sideJson(out.currentCloud) != sideJson(opened.result.cloud)) out.code = Code::StaleObservation;
        else out.code = Code::UnsupportedConditionalWrite;
        // There is intentionally no legacy transfer or destination-write call.
        call.check();
    } catch (const CoordinatorError& e) { out.code = e.code; }
      catch (...) { out.code = Code::InvalidInput; }
    return out;
}
} // namespace Caesura::detail
