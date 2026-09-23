// CANDIDATE ONLY: not compiled or executed. Test process; never installed.
#include "storage/CloudConflictStore.h"
#include "storage/CloudSaveSnapshot.h"
#include "storage/AtomicSaveFile.h"
#include "archive/CryptoEngine.h"
#include "di/BackendRegistry.h"
#include <nlohmann_json.hpp>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace Caesura;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Code = detail::ConflictStoreCode;
using Ref = detail::ConflictRecordRef;
constexpr int controlledExit = 73;
constexpr int windowsAbnormalExit = 74;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
unsigned long pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}
std::string utf8(const fs::path& path) {
    const auto raw = path.generic_u8string();
    return std::string(raw.begin(), raw.end());
}
std::string readBounded(const fs::path& path, size_t maximum) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(stream.good(), "fixture/report open failed");
    const auto size = stream.tellg();
    require(size >= 0 && static_cast<uint64_t>(size) <= maximum, "fixture/report size invalid");
    std::string bytes(static_cast<size_t>(size), '\0');
    stream.seekg(0);
    if (!bytes.empty()) stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "fixture/report read failed");
    return bytes;
}
void awaitOwnershipReceipt() {
    // The maintained launcher atomically publishes this live OS identity.
    // No fixture access or termination occurs until the launcher saw this PID.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto path = fs::path("owned-control") / "process.json";
        if (fs::is_regular_file(path)) {
            const auto identity = Json::parse(readBounded(path, 16u * 1024u));
            require(identity.at("pid").get<unsigned long>() == pid(), "ownership receipt PID differs");
            require(!identity.at("created").get<std::string>().empty(), "ownership receipt has no creation identity");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("live ownership receipt was not published before deadline");
}
std::string sha(const std::string& bytes) {
    std::array<uint8_t, 32> hash{};
    auto* crypto = BackendRegistry::instance().getCryptoEngine();
    require(crypto != nullptr, "crypto absent");
    crypto->sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), hash.data(), hash.size());
    std::string result;
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : hash) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
Json refJson(const Ref& ref) { return Json{{"id", ref.id}, {"manifest_sha256", ref.manifestSha256}}; }
Ref parseRef(const Json& value) {
    return {value.at("id").get<std::string>(), value.at("manifest_sha256").get<std::string>()};
}
Json bytesJson(const std::string& bytes) { return Json{{"size", bytes.size()}, {"sha256", sha(bytes)}}; }
void emit(Json event) {
    event["pid"] = pid();
    std::cout << "U26_B1B_EVENT " << event.dump() << std::endl;
    require(std::cout.good(), "event flush failed");
}
const char* codeName(Code code) {
    switch (code) {
    case Code::Preserved: return "Preserved";
    case Code::Complete: return "Complete";
    case Code::Missing: return "Missing";
    case Code::Incomplete: return "Incomplete";
    case Code::InvalidRecord: return "InvalidRecord";
    case Code::PublicationFailed: return "PublicationFailed";
    case Code::Indeterminate: return "Indeterminate";
    case Code::InvalidInput: return "InvalidInput";
    case Code::IoFailed: return "IoFailed";
    case Code::CapacityExceeded: return "CapacityExceeded";
    case Code::CryptoUnavailable: return "CryptoUnavailable";
    case Code::InspectionIncomplete: return "InspectionIncomplete";
    case Code::Unsupported: return "Unsupported";
    }
    return "Unknown";
}
const char* kindName(detail::ConflictRecordKind kind) {
    switch (kind) {
    case detail::ConflictRecordKind::EqualObserved: return "EqualObserved";
    case detail::ConflictRecordKind::LocalChanged: return "LocalChanged";
    case detail::ConflictRecordKind::CloudChanged: return "CloudChanged";
    case detail::ConflictRecordKind::Conflict: return "Conflict";
    case detail::ConflictRecordKind::DivergedWithoutBase: return "DivergedWithoutBase";
    }
    return "Unknown";
}
Json resultJson(const detail::ConflictStoreResult& result) {
    Json value{{"code", codeName(result.code)}, {"operation_id", result.operationId},
               {"candidate_ref", result.candidateRef ? refJson(*result.candidateRef) : Json(nullptr)},
               {"record", nullptr}};
    if (result.record) {
        const auto& record = *result.record;
        value["record"] = Json{{"ref", refJson(record.ref)}, {"kind", kindName(record.kind)},
            {"local", bytesJson(record.localBytes)}, {"cloud", bytesJson(record.cloudBytes)},
            {"base", record.baseBytes ? bytesJson(*record.baseBytes) : Json(nullptr)},
            {"base_ref", record.baseRef ? refJson(*record.baseRef) : Json(nullptr)}};
    }
    return value;
}
[[noreturn]] void abnormalExit() {
#ifdef _WIN32
    if (!TerminateProcess(GetCurrentProcess(), windowsAbnormalExit)) std::_Exit(91);
#else
    if (::kill(::getpid(), SIGKILL) != 0) std::_Exit(91);
#endif
    std::_Exit(92); // Never accepted as a successful abnormal-control outcome.
}
struct Registration {
    carc::CryptoEngine actual;
    carc::ICryptoEngine* previous = BackendRegistry::instance().getCryptoEngine();
    Registration() { BackendRegistry::instance().setCryptoEngine(&actual); }
    ~Registration() { BackendRegistry::instance().setCryptoEngine(previous); }
};
// Test-only backend fault. All ordinary calls delegate to the real CryptoEngine.
// The armed call verifies the actual published file BEFORE computing its real SHA.
struct PostCommitHashFailure final : carc::CryptoEngine {
    fs::path root;
    fs::path manifestPath;
    std::string preparedManifest;
    Ref preparedRef;
    size_t opened = 0;
    size_t armedCount = 0;
    size_t fired = 0;
    size_t delegatedArmedCalls = 0;
    bool armed = false;
    bool observedPublishedManifest = false;
    bool observedMatchingInput = false;
    bool realDigestMatchesPrepared = false;

    explicit PostCommitHashFailure(fs::path ownedRoot) : root(std::move(ownedRoot)) {}

    void sha256(const uint8_t* data, size_t length, uint8_t* hash, size_t hashLength) override {
        if (!armed) {
            carc::CryptoEngine::sha256(data, length, hash, hashLength);
            return;
        }
        require(fired == 0 && armedCount == 1, "postcommit fault must be armed exactly once");
        require(fs::is_regular_file(fs::symlink_status(manifestPath)),
                "postcommit fault reached before formal manifest exists");
        const auto published = readBounded(manifestPath, 16u * 1024u);
        require(published == preparedManifest, "published manifest differs from precommit bytes");
        observedPublishedManifest = true;
        require(data != nullptr && length == published.size() && hash != nullptr && hashLength == 32,
                "postcommit SHA arguments are not the expected manifest digest");
        require(std::string(reinterpret_cast<const char*>(data), length) == published,
                "first armed SHA input differs from the actual published manifest");
        observedMatchingInput = true;

        // Explicit qualified call, never a fake digest or recursive Registry call.
        carc::CryptoEngine::sha256(data, length, hash, hashLength);
        ++delegatedArmedCalls;
        std::string actual;
        constexpr char digits[] = "0123456789abcdef";
        for (size_t index = 0; index < hashLength; ++index) {
            actual += digits[hash[index] >> 4];
            actual += digits[hash[index] & 15];
        }
        require(actual == preparedRef.manifestSha256, "real delegated SHA differs from prepared reference");
        realDigestMatchesPrepared = true;
        armed = false;
        ++fired;
        throw std::runtime_error("U26_B1C_INJECTED_POSTCOMMIT_CRYPTO_EXCEPTION");
    }

    static bool checkpoint(detail::SaveWriteStage stage, const fs::path& temporary, void* context) {
        auto& self = *static_cast<PostCommitHashFailure*>(context);
        require(temporary.parent_path().parent_path() == self.root,
                "postcommit control writer escaped owned root");
        if (stage == detail::SaveWriteStage::CreateTemporary) ++self.opened;
        if (self.opened != 4 || stage != detail::SaveWriteStage::Replace) return true;
        require(!self.armed && self.armedCount == 0 && self.fired == 0,
                "postcommit control reached more than one manifest publication");
        const auto directory = temporary.parent_path();
        self.manifestPath = directory / "manifest.json";
        require(!fs::exists(self.manifestPath), "formal manifest already present before Replace");
        self.preparedManifest = readBounded(temporary, 16u * 1024u);
        const auto manifest = Json::parse(self.preparedManifest);
        require(manifest.at("id") == utf8(directory.filename()), "prepared manifest operation id differs");
        self.preparedRef = {manifest.at("id").get<std::string>(), sha(self.preparedManifest)};
        // sha above is still unarmed and delegates to the production backend.
        ++self.armedCount;
        self.armed = true;
        return true; // Let the unchanged writer actually publish the manifest.
    }
};

struct BorrowedCryptoRegistration {
    carc::ICryptoEngine* previous = BackendRegistry::instance().getCryptoEngine();
    explicit BorrowedCryptoRegistration(carc::ICryptoEngine& selected) {
        BackendRegistry::instance().setCryptoEngine(&selected);
    }
    ~BorrowedCryptoRegistration() { BackendRegistry::instance().setCryptoEngine(previous); }
    BorrowedCryptoRegistration(const BorrowedCryptoRegistration&) = delete;
    BorrowedCryptoRegistration& operator=(const BorrowedCryptoRegistration&) = delete;
};

struct StopBeforeManifest {
    fs::path root;
    std::string local, cloud, base;
    bool abnormal = false;
    size_t opened = 0;
    static bool checkpoint(detail::SaveWriteStage stage, const fs::path& temporary, void* context) {
        auto& self = *static_cast<StopBeforeManifest*>(context);
        require(temporary.parent_path().parent_path() == self.root, "writer escaped owned record root");
        if (stage == detail::SaveWriteStage::CreateTemporary) ++self.opened;
        if (self.opened != 4 || stage != detail::SaveWriteStage::Replace) return true;
        const auto directory = temporary.parent_path();
        require(!fs::exists(directory / "manifest.json"), "manifest already published at precommit boundary");
        require(readBounded(directory / "base.bin", 10u * 1024u * 1024u) == self.base, "base not fully published");
        require(readBounded(directory / "local.bin", 10u * 1024u * 1024u) == self.local, "local not fully published");
        require(readBounded(directory / "cloud.bin", 10u * 1024u * 1024u) == self.cloud, "cloud not fully published");
        const auto prepared = readBounded(temporary, 16u * 1024u);
        const auto manifest = Json::parse(prepared);
        require(manifest.at("id") == utf8(directory.filename()), "prepared manifest identity differs");
        const Ref candidate{manifest.at("id").get<std::string>(), sha(prepared)};
        emit(Json{{"event", "precommit_boundary"}, {"role", 4}, {"stage", "Replace"},
            {"candidate_ref", refJson(candidate)}, {"manifest_exists", false},
            {"payloads_match", true}, {"prepared_manifest_size", prepared.size()},
            {"termination", self.abnormal ? "self_abnormal" : "controlled_immediate_exit"}});
        if (self.abnormal) abnormalExit();
        std::_Exit(controlledExit); // Deliberately does not unwind C++ objects.
    }
};
CloudSnapshot fixtureSnapshot(const Json& request, const char* field) {
    const auto path = fs::u8path(request.at(field).get<std::string>());
    require(path.is_absolute(), "fixture path must be absolute");
    auto result = detail::readLocalCloudSnapshot(utf8(path));
    require(result.state == CloudReadState::Present && result.error == CloudReadError::None,
            "actual typed fixture reader did not return Present");
    return result;
}
int run(const Json& request) {
    require(request.at("schema_version") == 1, "unknown request schema");
    const auto root = fs::u8path(request.at("root").get<std::string>());
    require(root.is_absolute() && fs::is_directory(root), "existing absolute store root required");
    const auto mode = request.at("mode").get<std::string>();
    Registration crypto;
    const detail::ConflictStoreContext context{"0123456789abcdef0123456789abcdef", 3, 1};
    detail::CloudConflictStore store(root);
    if (mode == "read") {
        const auto read = store.readRecord(context, parseRef(request.at("ref")));
        const auto listed = store.listRecords(context);
        Json refs = Json::array();
        for (const auto& ref : listed.completeRecords) refs.push_back(refJson(ref));
        emit(Json{{"event", "readback"}, {"result", resultJson(read)},
            {"list", {{"code", codeName(listed.code)}, {"complete", std::move(refs)},
                      {"incomplete", listed.incompleteRecords}, {"invalid", listed.invalidRecords}}}});
        return 0; // Parent asserts precise product results, including negative controls.
    }
    auto local = fixtureSnapshot(request, "local");
    auto cloud = fixtureSnapshot(request, "cloud");
    if (mode == "seed") {
        Json checkpoints = Json::array();
        detail::ScopedSaveWriteTestHook trace({
            [](detail::SaveWriteStage stage, const fs::path& temporary, void* context) {
                static_cast<Json*>(context)->push_back(Json{{"stage", static_cast<int>(stage)},
                    {"temporary", utf8(temporary)}});
                return true;
            }, &checkpoints});
        const auto result = store.preserve(context, local, cloud);
        if (result.code != Code::Preserved || !result.record.has_value()) {
            Json diagnostic{{"code", codeName(result.code)}, {"checkpoints", checkpoints}};
#ifdef _WIN32
            diagnostic["last_win32_error"] = GetLastError();
#endif
            std::cerr << "U26_B1B_SEED_FAILURE " << diagnostic.dump() << std::endl;
        }
        require(result.code == Code::Preserved && result.record.has_value(), "seed did not preserve");
        emit(Json{{"event", "seed"}, {"result", resultJson(result)}});
        return 0;
    }
    require(mode == "write", "unknown mode");
    const auto base = parseRef(request.at("base_ref"));
    const auto boundary = request.at("boundary").get<std::string>();
    if (boundary == "after-hash-exception") {
        PostCommitHashFailure failure(root);
        BorrowedCryptoRegistration registration(failure);
        detail::ConflictStoreResult uncertain;
        {
            detail::ScopedSaveWriteTestHook hook({&PostCommitHashFailure::checkpoint, &failure});
            uncertain = store.preserve(context, local, cloud, base);
        }
        require(failure.opened == 4 && failure.armedCount == 1 && failure.fired == 1 &&
                failure.delegatedArmedCalls == 1 && !failure.armed,
                "postcommit backend exception was not injected exactly once at the intended boundary");
        require(failure.observedPublishedManifest && failure.observedMatchingInput &&
                failure.realDigestMatchesPrepared, "postcommit boundary or real SHA was not proven");
        require(uncertain.code == Code::Indeterminate && !uncertain.record && uncertain.candidateRef,
                "production preserve did not retain an Indeterminate reference");
        require(uncertain.operationId == failure.preparedRef.id &&
                uncertain.candidateRef->id == failure.preparedRef.id &&
                uncertain.candidateRef->manifestSha256 == failure.preparedRef.manifestSha256,
                "Indeterminate changed the prepublication candidate reference");
        require(readBounded(failure.manifestPath, 16u * 1024u) == failure.preparedManifest,
                "committed manifest was lost after Indeterminate");

        // Separate same-object guard recovery evidence; this is NOT a cold process.
        // Never issue a second preserve to 'recover' the uncertain operation.
        const auto sameProcess = store.readRecord(context, *uncertain.candidateRef);
        require(sameProcess.code == Code::Complete && sameProcess.record,
                "same store could not read after exception unwinding");
        require(failure.fired == 1 && failure.delegatedArmedCalls == 1 && !failure.armed,
                "once-only backend fault unexpectedly repeated during later reads");
        emit(Json{{"event", "indeterminate"}, {"result", resultJson(uncertain)},
            {"same_process_reopen", resultJson(sameProcess)},
            {"fault", {{"opened_roles", failure.opened}, {"armed_count", failure.armedCount},
                       {"fired", failure.fired}, {"delegated_armed_calls", failure.delegatedArmedCalls},
                       {"armed_after", failure.armed},
                       {"published_manifest_observed", failure.observedPublishedManifest},
                       {"input_equals_raw_manifest", failure.observedMatchingInput},
                       {"real_digest_matches_prepared", failure.realDigestMatchesPrepared},
                       {"prepared_ref", refJson(failure.preparedRef)}}},
            {"termination", "normal_return"}});
        return 0;
    }
    if (boundary == "before-controlled" || boundary == "before-abnormal") {
        const auto baseSnapshot = fixtureSnapshot(request, "base");
        StopBeforeManifest stop{root, local.bytes, cloud.bytes, baseSnapshot.bytes, boundary == "before-abnormal"};
        detail::ScopedSaveWriteTestHook hook({&StopBeforeManifest::checkpoint, &stop});
        (void)store.preserve(context, local, cloud, base);
        throw std::runtime_error("precommit termination boundary was not reached");
    }
    require(boundary == "after-normal" || boundary == "after-abnormal", "unknown termination boundary");
    const auto result = store.preserve(context, local, cloud, base);
    require(result.code == Code::Preserved && result.record.has_value(), "postcommit preservation failed");
    emit(Json{{"event", "committed"}, {"result", resultJson(result)},
              {"termination", boundary == "after-normal" ? "normal_return" : "self_abnormal"}});
    if (boundary == "after-abnormal") abnormalExit();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    try {
        require(argc == 2 && std::string(argv[1]) == "request.json", "expected ASCII request filename in owned cwd");
        awaitOwnershipReceipt();
        return run(Json::parse(readBounded(fs::path("request.json"), 32u * 1024u)));
    } catch (const std::exception& error) {
        std::cerr << "U26_B1B_ERROR " << error.what() << std::endl;
        return 90;
    }
}
