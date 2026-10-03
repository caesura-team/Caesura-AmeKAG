#include "CloudConflictStore.h"
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
using Code = ConflictStoreCode;
using Crypto = carc::ICryptoEngine;
constexpr uint64_t payloadLimit = 10u * 1024u * 1024u;
constexpr uint64_t manifestLimit = 16u * 1024u;
constexpr uint64_t recordLimit = 128;
constexpr uint64_t storageLimit = 256u * 1024u * 1024u;
constexpr size_t scanEntryLimit = 4096;
constexpr size_t scanDepthLimit = 16;

struct StoreError { Code code; };
void require(bool condition, Code code = Code::InvalidRecord) {
    if (!condition) throw StoreError{code};
}
bool lowerHex(const std::string& value, size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool validContext(const ConflictStoreContext& context) {
    return lowerHex(context.scopeId, 32) && context.slot >= -2 && context.slot <= 99 &&
           context.policyEpoch != 0;
}
bool sameContext(const ConflictStoreContext& a, const ConflictStoreContext& b) {
    return a.scopeId == b.scopeId && a.slot == b.slot && a.policyEpoch == b.policyEpoch;
}
bool validRef(const ConflictRecordRef& ref) {
    return lowerHex(ref.id, 32) && lowerHex(ref.manifestSha256, 64);
}
bool validLimits(const ConflictStoreLimits& limits) {
    return limits.maxRecords > 0 && limits.maxRecords <= recordLimit &&
           limits.maxStoredBytes > 0 && limits.maxStoredBytes <= storageLimit;
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
    if (snapshot.state == CloudReadState::Failed) throw StoreError{Code::IoFailed};
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
ConflictStoreContext parseContext(const Json& value) {
    require(keys(value, {"scope_id", "slot", "policy_epoch"}));
    ConflictStoreContext result;
    result.scopeId = stringValue(value.at("scope_id"));
    const auto& slot = value.at("slot");
    require(slot.is_number_integer());
    if (slot.is_number_unsigned()) {
        require(slot.get<uint64_t>() <= 99);
        result.slot = static_cast<int>(slot.get<uint64_t>());
    } else {
        const auto number = slot.get<int64_t>();
        require(number >= -2 && number <= 99);
        result.slot = static_cast<int>(number);
    }
    result.policyEpoch = unsignedValue(value.at("policy_epoch"));
    require(validContext(result));
    return result;
}
ConflictRecordRef parseRef(const Json& value) {
    require(keys(value, {"id", "manifest_sha256"}));
    ConflictRecordRef result{stringValue(value.at("id")), stringValue(value.at("manifest_sha256"))};
    require(validRef(result));
    return result;
}
ConflictRecordKind classify(const std::string& local, const std::string& cloud,
                            const std::optional<std::string>& base) {
    if (local == cloud) return ConflictRecordKind::EqualObserved;
    if (!base) return ConflictRecordKind::DivergedWithoutBase;
    if (local == *base) return ConflictRecordKind::CloudChanged;
    if (cloud == *base) return ConflictRecordKind::LocalChanged;
    return ConflictRecordKind::Conflict;
}
const char* kindName(ConflictRecordKind kind) {
    switch (kind) {
    case ConflictRecordKind::EqualObserved: return "equal_observed";
    case ConflictRecordKind::LocalChanged: return "local_changed";
    case ConflictRecordKind::CloudChanged: return "cloud_changed";
    case ConflictRecordKind::Conflict: return "conflict";
    case ConflictRecordKind::DivergedWithoutBase: return "diverged_without_base";
    }
    throw StoreError{Code::InvalidRecord};
}
void checkClosure(const fs::path& directory, bool base, bool manifest) {
    std::set<std::string> expected{"local.bin", "cloud.bin"};
    if (base) expected.insert("base.bin");
    if (manifest) expected.insert("manifest.json");
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        require(++count <= expected.size());
        require(expected.count(utf8Path(entry.path().filename())) == 1);
        (void)regularSize(entry.path());
    }
    require(count == expected.size());
}
std::string payloadFrom(const fs::path& directory, const char* role,
                        const Json& metadata, Crypto& crypto) {
    require(keys(metadata, {"state", "size", "sha256"}));
    require(stringValue(metadata.at("state")) == "present");
    const auto size = unsignedValue(metadata.at("size"));
    const auto sha = stringValue(metadata.at("sha256"));
    require(size <= payloadLimit && lowerHex(sha, 64));
    auto bytes = readBytes(directory / (std::string(role) + ".bin"), payloadLimit);
    require(bytes.size() == size && digest(crypto, bytes) == sha);
    return bytes;
}

// List may discover a reference, but it never selects/authorizes a base. All
// preserve/read callers supply the manifest digest from outside this function.
PreservedConflictRecord openRecord(const fs::path& root, const std::string& id,
                                  Crypto& crypto, DirectoryGuard& guard,
                                  const std::optional<std::string>& expectedSha) {
    const auto directory = root / id;
    const auto directoryStatus = statusOf(directory);
    if (!fs::exists(directoryStatus)) throw StoreError{Code::Missing};
    require(fs::is_directory(directoryStatus));
    DirectoryGuard recordGuard(directory);
    const auto manifestPath = directory / "manifest.json";
    if (!fs::exists(statusOf(manifestPath))) throw StoreError{Code::Incomplete};
    const auto raw = readBytes(manifestPath, manifestLimit);
    const auto sha = digest(crypto, raw);
    require(!expectedSha || sha == *expectedSha);
    ManifestSax sax;
    require(Json::sax_parse(raw, &sax));
    const auto manifest = Json::parse(raw);
    require(keys(manifest, {"schema_version", "id", "context", "kind", "envelope_validation",
                            "revision_kind", "base_ref", "payloads"}));
    require(unsignedValue(manifest.at("schema_version")) == 1 &&
            stringValue(manifest.at("id")) == id &&
            stringValue(manifest.at("envelope_validation")) == "NOT_CHECKED" &&
            stringValue(manifest.at("revision_kind")) == "none");
    PreservedConflictRecord record;
    record.ref = {id, sha};
    record.context = parseContext(manifest.at("context"));
    const auto& baseRef = manifest.at("base_ref");
    if (!baseRef.is_null()) {
        record.baseRef = parseRef(baseRef);
        require(record.baseRef->id != id);
    }
    const auto& payloads = manifest.at("payloads");
    require(record.baseRef ? keys(payloads, {"base", "local", "cloud"})
                           : keys(payloads, {"local", "cloud"}));
    checkClosure(directory, record.baseRef.has_value(), true);
    if (record.baseRef) record.baseBytes = payloadFrom(directory, "base", payloads.at("base"), crypto);
    record.localBytes = payloadFrom(directory, "local", payloads.at("local"), crypto);
    record.cloudBytes = payloadFrom(directory, "cloud", payloads.at("cloud"), crypto);
    record.kind = classify(record.localBytes, record.cloudBytes, record.baseBytes);
    require(stringValue(manifest.at("kind")) == kindName(record.kind));
    // Recheck the complete retained closure at the end, including the raw
    // manifest identity. No individual early read is treated as final proof.
    require(readBytes(manifestPath, manifestLimit) == raw);
    if (record.baseBytes) require(readBytes(directory / "base.bin", payloadLimit) == *record.baseBytes);
    require(readBytes(directory / "local.bin", payloadLimit) == record.localBytes);
    require(readBytes(directory / "cloud.bin", payloadLimit) == record.cloudBytes);
    checkClosure(directory, record.baseRef.has_value(), true);
    recordGuard.verify();
    guard.verify();
    return record;
}

struct Usage { uint64_t records = 0, bytes = 0; size_t entries = 0; };
void addUsage(const fs::path& path, size_t depth, Usage& usage) {
    require(depth <= scanDepthLimit && ++usage.entries <= scanEntryLimit, Code::CapacityExceeded);
    const auto status = statusOf(path);
    if (fs::is_directory(status)) {
        for (const auto& child : fs::directory_iterator(path)) addUsage(child.path(), depth + 1, usage);
    } else {
        const auto size = regularSize(path);
        require(size <= storageLimit - usage.bytes, Code::CapacityExceeded);
        usage.bytes += size;
    }
}
Usage usageOf(const fs::path& root, DirectoryGuard& guard) {
    Usage usage;
    // Every root entry consumes a record slot; unknown/incomplete entries and
    // their actual file bytes count too. Unsafe/unmeasurable entries fail closed.
    for (const auto& entry : fs::directory_iterator(root)) {
        require(++usage.records <= recordLimit, Code::CapacityExceeded);
        addUsage(entry.path(), 1, usage);
    }
    guard.verify();
    return usage;
}
Json payloadMetadata(const std::string& bytes, Crypto& crypto) {
    return Json{{"state", "present"}, {"size", static_cast<uint64_t>(bytes.size())},
                {"sha256", digest(crypto, bytes)}};
}
std::string manifestBytes(const PreservedConflictRecord& record, Crypto& crypto) {
    Json payloads{{"local", payloadMetadata(record.localBytes, crypto)},
                  {"cloud", payloadMetadata(record.cloudBytes, crypto)}};
    Json baseRef = nullptr;
    if (record.baseRef) {
        baseRef = Json{{"id", record.baseRef->id}, {"manifest_sha256", record.baseRef->manifestSha256}};
        payloads["base"] = payloadMetadata(*record.baseBytes, crypto);
    }
    Json manifest{{"schema_version", 1u}, {"id", record.ref.id},
        {"context", {{"scope_id", record.context.scopeId}, {"slot", record.context.slot},
                     {"policy_epoch", record.context.policyEpoch}}},
        {"kind", kindName(record.kind)}, {"envelope_validation", "NOT_CHECKED"},
        {"revision_kind", "none"}, {"base_ref", std::move(baseRef)}, {"payloads", std::move(payloads)}};
    auto bytes = manifest.dump();
    require(bytes.size() <= manifestLimit, Code::CapacityExceeded);
    return bytes;
}
Code inputState(const CloudSnapshot& snapshot) {
    require(snapshot.revisionKind == CloudRevisionKind::None && snapshot.revision.empty(), Code::InvalidInput);
    require(static_cast<int>(snapshot.error) >= static_cast<int>(CloudReadError::None) &&
            static_cast<int>(snapshot.error) <= static_cast<int>(CloudReadError::ChangedDuringRead),
            Code::InvalidInput);
    switch (snapshot.state) {
    case CloudReadState::Present:
        require(snapshot.error == CloudReadError::None, Code::InvalidInput);
        return Code::Complete;
    case CloudReadState::Missing:
    case CloudReadState::Unavailable:
    case CloudReadState::Failed:
    case CloudReadState::Invalid:
    case CloudReadState::Unsupported:
        require(snapshot.bytes.empty(), Code::InvalidInput);
        return Code::InspectionIncomplete;
    }
    throw StoreError{Code::InvalidInput};
}
class ActiveCall {
public:
    explicit ActiveCall(bool& active) : m_active(active) { active = true; }
    ~ActiveCall() { m_active = false; }
    ActiveCall(const ActiveCall&) = delete;
    ActiveCall& operator=(const ActiveCall&) = delete;
private:
    bool& m_active;
};
void writePayload(const fs::path& path, const std::string& bytes, DirectoryGuard& guard) {
    guard.verify();
    require(!fs::exists(statusOf(path)), Code::PublicationFailed);
    // AtomicSaveFile's existing narrow path convention is native, whereas the
    // typed reader explicitly consumes UTF-8. Reject a lossy native conversion.
    const auto native = path.string();
    require(fs::path(native) == path, Code::PublicationFailed);
    require(writeSaveFileAtomically(native, bytes), Code::PublicationFailed);
    guard.verify();
    require(readBytes(path, payloadLimit) == bytes, Code::PublicationFailed);
}
} // namespace

CloudConflictStore::CloudConflictStore(fs::path root, ConflictStoreLimits limits)
    : m_root(std::move(root)), m_limits(limits), m_ownerThread(std::this_thread::get_id()) {}
CloudConflictStore::~CloudConflictStore() = default;

ConflictStoreResult CloudConflictStore::preserve(
    const ConflictStoreContext& context, const CloudSnapshot& local, const CloudSnapshot& cloud,
    const std::optional<ConflictRecordRef>& expectedBase) {
    ConflictStoreResult result;
    result.code = Code::InvalidInput;
    if (std::this_thread::get_id() != m_ownerThread || m_active) return result;
    ActiveCall active(m_active);
    bool published = false;
    try {
        require(validContext(context) && validLimits(m_limits), Code::InvalidInput);
        const auto localState = inputState(local);
        const auto cloudState = inputState(cloud);
        if (localState != Code::Complete || cloudState != Code::Complete) {
            result.code = Code::InspectionIncomplete;
            return result;
        }
        require(local.bytes.size() <= payloadLimit && cloud.bytes.size() <= payloadLimit,
                Code::CapacityExceeded);
        auto* crypto = BackendRegistry::instance().getCryptoEngine();
        require(crypto != nullptr, Code::CryptoUnavailable);
        DirectoryGuard guard(m_root);
        PreservedConflictRecord record;
        record.context = context;
        record.localBytes = local.bytes;
        record.cloudBytes = cloud.bytes;
        if (expectedBase) {
            require(validRef(*expectedBase), Code::InvalidInput);
            // A bad explicit base is never silently converted to no-base mode.
            try {
                const auto base = openRecord(m_root, expectedBase->id, *crypto, guard,
                                             expectedBase->manifestSha256);
                require(sameContext(context, base.context) && base.kind == ConflictRecordKind::EqualObserved &&
                        base.localBytes == base.cloudBytes);
                record.baseBytes = base.localBytes;
                record.baseRef = *expectedBase;
            } catch (const StoreError&) { throw StoreError{Code::InvalidInput}; }
        }
        record.kind = classify(record.localBytes, record.cloudBytes, record.baseBytes);
        auto usage = usageOf(m_root, guard);
        require(usage.records < m_limits.maxRecords && usage.bytes <= m_limits.maxStoredBytes,
                Code::CapacityExceeded);
        std::string manifest;
        // Exclusive directory creation, not entropy alone, establishes ownership.
        for (size_t attempt = 0; attempt < 128; ++attempt) {
            std::array<uint8_t, 16> random{};
            crypto->generateNonce(random.data(), random.size());
            record.ref.id = hex(random.data(), random.size());
            manifest = manifestBytes(record, *crypto);
            const uint64_t required = static_cast<uint64_t>(record.localBytes.size()) +
                record.cloudBytes.size() + (record.baseBytes ? record.baseBytes->size() : 0) + manifest.size();
            require(required <= m_limits.maxStoredBytes - usage.bytes, Code::CapacityExceeded);
            guard.verify();
            std::error_code error;
            const bool created = fs::create_directory(m_root / record.ref.id, error);
            if (created) {
                result.operationId = record.ref.id;
                break;
            }
            require(!error || error == std::errc::file_exists, Code::IoFailed);
        }
        require(!result.operationId.empty(), Code::IoFailed);
        const auto directory = m_root / result.operationId;
        guard.add(directory);
        if (record.baseBytes) writePayload(directory / "base.bin", *record.baseBytes, guard);
        writePayload(directory / "local.bin", record.localBytes, guard);
        writePayload(directory / "cloud.bin", record.cloudBytes, guard);
        // Re-read every earlier role after the last write's callbacks have run.
        if (record.baseBytes) require(readBytes(directory / "base.bin", payloadLimit) == *record.baseBytes);
        require(readBytes(directory / "local.bin", payloadLimit) == record.localBytes);
        require(readBytes(directory / "cloud.bin", payloadLimit) == record.cloudBytes);
        checkClosure(directory, record.baseBytes.has_value(), false);
        if (record.baseRef) {
            const auto base = openRecord(m_root, record.baseRef->id, *crypto, guard,
                                        record.baseRef->manifestSha256);
            require(sameContext(context, base.context) && base.kind == ConflictRecordKind::EqualObserved &&
                    base.localBytes == *record.baseBytes && base.cloudBytes == *record.baseBytes);
        }
        require(BackendRegistry::instance().getCryptoEngine() == crypto, Code::CryptoUnavailable);
        record.ref.manifestSha256 = digest(*crypto, manifest);
        result.candidateRef = record.ref; // exact identity exists BEFORE publication
        guard.verify();
        const auto manifestPath = directory / "manifest.json";
        require(!fs::exists(statusOf(manifestPath)));
        const auto native = manifestPath.string();
        require(fs::path(native) == manifestPath);
        require(writeSaveFileAtomically(native, manifest), Code::PublicationFailed);
        published = true; // the only commit point; never delete or retry beyond it
        auto reopened = openRecord(m_root, record.ref.id, *crypto, guard, record.ref.manifestSha256);
        require(sameContext(context, reopened.context) && reopened.kind == record.kind &&
                reopened.baseBytes == record.baseBytes && reopened.localBytes == record.localBytes &&
                reopened.cloudBytes == record.cloudBytes);
        require(BackendRegistry::instance().getCryptoEngine() == crypto);
        guard.verify();
        result.record = std::move(reopened);
        result.code = Code::Preserved;
    } catch (const StoreError& error) {
        result.code = published ? Code::Indeterminate :
                      (!result.operationId.empty() ? Code::PublicationFailed : error.code);
        result.record.reset();
    } catch (...) {
        result.code = published ? Code::Indeterminate :
                      (!result.operationId.empty() ? Code::PublicationFailed : Code::IoFailed);
        result.record.reset();
    }
    return result;
}

ConflictStoreResult CloudConflictStore::readRecord(
    const ConflictStoreContext& context, const ConflictRecordRef& expectedRecord) const {
    ConflictStoreResult result;
    result.code = Code::InvalidInput;
    if (std::this_thread::get_id() != m_ownerThread || m_active) return result;
    ActiveCall active(m_active);
    try {
        require(validContext(context) && validRef(expectedRecord) && validLimits(m_limits), Code::InvalidInput);
        auto* crypto = BackendRegistry::instance().getCryptoEngine();
        require(crypto != nullptr, Code::CryptoUnavailable);
        DirectoryGuard guard(m_root);
        auto record = openRecord(m_root, expectedRecord.id, *crypto, guard, expectedRecord.manifestSha256);
        require(sameContext(context, record.context));
        require(BackendRegistry::instance().getCryptoEngine() == crypto);
        result.record = std::move(record);
        result.code = Code::Complete;
    } catch (const StoreError& error) {
        result.code = error.code;
    } catch (...) {
        result.code = Code::IoFailed;
    }
    return result;
}

ConflictStoreListResult CloudConflictStore::listRecords(const ConflictStoreContext& context) const {
    ConflictStoreListResult result;
    result.code = Code::InvalidInput;
    if (std::this_thread::get_id() != m_ownerThread || m_active) return result;
    ActiveCall active(m_active);
    try {
        require(validContext(context) && validLimits(m_limits), Code::InvalidInput);
        auto* crypto = BackendRegistry::instance().getCryptoEngine();
        require(crypto != nullptr, Code::CryptoUnavailable);
        DirectoryGuard guard(m_root);
        (void)usageOf(m_root, guard); // includes unknown/incomplete bytes, bounded scan
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(m_root)) {
            require(++count <= recordLimit, Code::CapacityExceeded);
            const auto id = utf8Path(entry.path().filename());
            if (!lowerHex(id, 32) || !fs::is_directory(statusOf(entry.path()))) {
                ++result.invalidRecords;
                continue;
            }
            try {
                const auto record = openRecord(m_root, id, *crypto, guard, std::nullopt);
                if (sameContext(context, record.context)) result.completeRecords.push_back(record.ref);
            } catch (const StoreError& error) {
                if (error.code == Code::Incomplete) ++result.incompleteRecords;
                else if (error.code == Code::IoFailed) throw;
                else ++result.invalidRecords;
            }
        }
        std::sort(result.completeRecords.begin(), result.completeRecords.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        require(BackendRegistry::instance().getCryptoEngine() == crypto);
        guard.verify();
        result.code = result.invalidRecords ? Code::InvalidRecord : Code::Complete;
    } catch (const StoreError& error) {
        result.code = error.code;
    } catch (...) {
        result.code = Code::IoFailed;
    }
    return result;
}
} // namespace Caesura::detail
