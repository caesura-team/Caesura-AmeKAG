// ===========================================================================
//  Caesura (AmeKAG) -- SaveManager.cpp
//  JSON save/load with schema versioning and migration chain.
//  Uses nlohmann/json v3.11.3 for robust structured serialization.
//  Save format: {"schema_version":1,"timestamp":12345,"scene":"...",
//                "token_index":5,"thumbnail":"...","engine_version":"1.0.0",
//                "data":{...}}
//  Encrypted format: "CAES" [12-byte nonce] [16-byte tag] [ciphertext...]
// ===========================================================================

#include "SaveManager.h"
#include "api/ISaveProvider.h"
#include "api/ICloudSaveTransport.h"
#include "CloudSaveProvider.h"
#include "HttpCloudSaveProvider.h"
#include "LocalFileSaveProvider.h"
#include "AtomicSaveFile.h"
#include "../di/BackendRegistry.h"
#include "../archive/api/ICryptoEngine.h"
#include "../debug/api/DebugLog.h"
#include <vector>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <algorithm>

#ifdef _WIN32
#include <direct.h>
#define mkdir_impl(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_impl(p) mkdir(p, 0755)
#endif

namespace Caesura {

namespace {

void secureErase(void* memory, size_t size) noexcept {
    auto* bytes = static_cast<volatile uint8_t*>(memory);
    while (size-- > 0) {
        *bytes++ = 0;
    }
}

} // namespace

// Engine version string (Spec U6: archive version management)
// Engine version recorded in save envelopes — derived from the CMake
// project version (CAESURA_VERSION compile definition) so it always matches
// the released binary.
#ifndef CAESURA_VERSION
#define CAESURA_VERSION "unknown"
#endif
const char* SaveManager::ENGINE_VERSION = CAESURA_VERSION;

SaveManager::SaveManager() = default;
SaveManager::~SaveManager() {
    secureErase(m_encryptKey, sizeof(m_encryptKey));
    m_keySet = false;
}

// ============================================================================
//  Pluggable save provider (SU-6)
// ============================================================================

void SaveManager::setSaveProvider(std::unique_ptr<ISaveProvider> provider) {
    m_saveProvider = std::move(provider);
    printf("[SaveManager] Custom save provider installed.\n");
}

void SaveManager::init(const std::string& saveDir) {
    m_saveDir = saveDir;

    if (!m_saveDir.empty() && m_saveDir.back() != '/' && m_saveDir.back() != '\\') {
        m_saveDir += '/';
    }

    mkdir_impl(m_saveDir.c_str());

    registerBuiltinMigrations();
    printf("[SaveManager] Initialized. Save dir: %s (schema v%d)\n",
           m_saveDir.c_str(), m_currentSchemaVersion);
}

// ============================================================================
//  Encryption (SU-2) -- AES-256-GCM via CryptoEngine
//  Encrypted save format: [4-byte "CAES"][12-byte nonce][16-byte tag][ciphertext]
// ============================================================================

// Cloud sync (C7): swap the save provider. WHERE THE BYTES LIVE differs per
// endpoint, and that difference is the whole story for "who wins" (t14):
//
//   ""                -> LocalFileSaveProvider. Disk is the only store.
//   "http(s)://..."   -> HttpCloudSaveProvider. read/write/delete/list still go
//                        to the LOCAL disk (it delegates to an internal
//                        LocalFileSaveProvider); only the explicit
//                        pushSlotToCloud/pullSlotFromCloud calls touch the
//                        remote. Disk stays the source of truth.
//   "steam"           -> CloudSaveProvider. Steam Remote Storage IS the store:
//                        save()/load()/listSaves()/slotExists() all read and
//                        write cloud files, and the local save_N.json files are
//                        NOT consulted (only pushSlotToCloud reads one, to
//                        upload it). This is by design -- Steam Cloud handles
//                        its own local caching -- but it means switching to the
//                        steam endpoint changes which store the player sees.
//
// There is no merge and no timestamp arbitration anywhere: whichever side a
// call names wins for that call. No code path syncs implicitly, so simply
// configuring an endpoint never moves or destroys a save.
bool SaveManager::configureCloudSync(const std::string& endpoint) {
    if (endpoint.empty()) {
        m_saveProvider = std::make_unique<LocalFileSaveProvider>();
        return true;
    }
    if (endpoint == "steam" || endpoint == "steam://" || endpoint == "steamcloud") {
        auto* steam = BackendRegistry::instance().getSteamBackend();
        if (!steam) {
            // Fail closed. A CloudSaveProvider over a null backend answers "" to
            // every readFile and false to every writeFile, so installing it here
            // would make save() fail and load()/listSaves() report the player's
            // existing saves as GONE -- a silent, total loss of visibility with
            // no diagnostic. Keep the current provider instead and say why.
            DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                      "[SaveManager] configureCloudSync(\"%s\") refused: no Steam "
                      "backend is registered. Keeping the current save provider; "
                      "installing Steam Cloud without a backend would hide every "
                      "existing save.", endpoint.c_str());
            printf("[SaveManager] Cloud sync NOT configured: Steam backend unavailable\n");
            return false;
        }
        m_saveProvider = std::make_unique<CloudSaveProvider>(steam);
        printf("[SaveManager] Cloud sync configured: Steam Remote Storage "
               "(cloud is the save store; local files are not read)\n");
        return true;
    }
    m_saveProvider = std::make_unique<HttpCloudSaveProvider>(endpoint);
    printf("[SaveManager] Cloud sync configured: %s (local disk stays the store; "
           "push/pull are explicit)\n", endpoint.c_str());
    return true;
}

// push/pull are EXPLICIT, caller-driven, one-directional transfers. They are
// never invoked by save()/load()/listSaves(), so configuring cloud sync alone
// never moves a byte: no automatic path can overwrite a player's save.
//
// A false return used to be ambiguous (audit t5): "no provider installed"
// (misconfiguration -- init() was never called), "the provider has no cloud
// end" (local-only, working as intended) and "the transfer failed" all looked
// identical. Each case now leaves a distinct diagnostic; readFile/writeFile can
// fall back to their own ifstream, but a cloud transfer has no meaningful
// fallback, so these still fail closed.
bool SaveManager::pushSlotToCloud(int slot) {
    if (!m_saveProvider) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                  "[SaveManager] pushSlotToCloud(%d) refused: no save provider "
                  "installed (call init() / configureCloudSync() first)", slot);
        return false;
    }
    if (!m_saveProvider->supportsCloudSync()) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Ok,
                   "[SaveManager] pushSlotToCloud(%d): local-only provider has no "
                   "cloud end; nothing to sync", slot);
        return false;
    }
    const std::string path = slotPath(slot);
    if (path.empty()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Ok,
                  "[SaveManager] pushSlotToCloud(%d) refused: slot out of range "
                  "[-2..99]", slot);
        return false;
    }
    auto* transport = dynamic_cast<ICloudSaveTransport*>(m_saveProvider.get());
    if (!transport) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                  "[SaveManager] Cloud provider does not support validated staged transfers");
        return false;
    }
    const auto bytes = transport->readLocalFile(path);
    if (loadContents(slot, decodeSaveBytes(bytes), nullptr).is_null()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                  "[SaveManager] Cloud push refused: local save fails current policy or validation");
        return false;
    }
    const bool ok = transport->writeCloudFile(path, bytes);
    if (!ok) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                   "[SaveManager] pushSlotToCloud(%d) failed (see provider "
                   "diagnostic above); transfer did not report success", slot);
    }
    return ok;
}

bool SaveManager::pullSlotFromCloud(int slot) {
    if (!m_saveProvider) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                  "[SaveManager] pullSlotFromCloud(%d) refused: no save provider "
                  "installed (call init() / configureCloudSync() first)", slot);
        return false;
    }
    if (!m_saveProvider->supportsCloudSync()) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Ok,
                   "[SaveManager] pullSlotFromCloud(%d): local-only provider has no "
                   "cloud end; nothing to sync", slot);
        return false;
    }
    const std::string path = slotPath(slot);
    if (path.empty()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Ok,
                  "[SaveManager] pullSlotFromCloud(%d) refused: slot out of range "
                  "[-2..99]", slot);
        return false;
    }
    auto* transport = dynamic_cast<ICloudSaveTransport*>(m_saveProvider.get());
    if (!transport) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                  "[SaveManager] Cloud provider does not support validated staged transfers");
        return false;
    }
    const auto bytes = transport->readCloudFile(path);
    if (loadContents(slot, decodeSaveBytes(bytes), nullptr).is_null()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                  "[SaveManager] Cloud pull refused: remote save fails current policy or validation");
        return false;
    }
    const bool ok = transport->writeLocalFile(path, bytes);
    if (!ok) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                   "[SaveManager] pullSlotFromCloud(%d) failed (see provider "
                   "diagnostic above); transfer did not report success", slot);
    }
    return ok;
}

void SaveManager::setEncryptionKey(const uint8_t key[32]) {
    std::memcpy(m_encryptKey, key, 32);
    m_keySet = true;
    printf("[SaveManager] Encryption key set (AES-256-GCM)\n");
}

void SaveManager::clearEncryptionKey() {
    secureErase(m_encryptKey, sizeof(m_encryptKey));
    m_keySet = false;
    printf("[SaveManager] Encryption key cleared\n");
}

// ============================================================================
//  File I/O (with optional AES-256-GCM encryption)
// ============================================================================

std::string SaveManager::slotPath(int slot) const {
    // System slots live in dedicated files OUTSIDE the 0..99 UI range:
    // quicksave (-1, F5/F6) and autosave (-2, engine timer) map to their own
    // files; listSaves() enumerates 0..99 only, so they never clutter the
    // save menu. Anything else negative/absurd fabricates no path at all.
    if (slot == -1) return m_saveDir + "save_quick.json";
    if (slot == -2) return m_saveDir + "save_auto.json";
    if (slot < 0 || slot > 99) return "";
    return m_saveDir + "save_" + std::to_string(slot) + ".json";
}

// Slot validity: the player-visible 0..99 plus the system slots -2/-1.
static bool validSlot(int slot) { return slot >= -2 && slot <= 99; }

static constexpr size_t MAX_SAVE_SIZE = 10 * 1024 * 1024;  // 10 MiB
static constexpr size_t ENCRYPT_HEADER_SIZE = 4 + 12 + 16;

static bool hasEncryptedMagic(const std::string& bytes) {
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "CAES", 4) == 0;
}

std::string SaveManager::readRawFile(const std::string& path) {
    if (m_saveProvider) {
        auto bytes = m_saveProvider->readFile(path);
        if (bytes.size() > MAX_SAVE_SIZE) return {};
        return bytes;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";

    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz <= 0 || static_cast<size_t>(sz) > MAX_SAVE_SIZE) return "";

    std::string content(static_cast<size_t>(sz), '\0');
    in.seekg(0, std::ios::beg);
    in.read(&content[0], sz);
    if (!in) return "";
    return content;
}

std::string SaveManager::readFile(const std::string& path) {
    return decodeSaveBytes(readRawFile(path));
}

std::string SaveManager::decodeSaveBytes(const std::string& content) {
    if (content.empty() || content.size() > MAX_SAVE_SIZE) return "";
    if (!hasEncryptedMagic(content)) {
        if (m_encryptionPolicy == SaveEncryptionPolicy::RequireEncrypted) {
            DEBUG_ERR(SubSys::Storage, ErrCode::Storage_CryptoFailed,
                      "[SaveManager] Plaintext save rejected by require-encrypted policy");
            return {};
        }
        if (m_keySet) {
            DEBUG_WARN(SubSys::Storage, ErrCode::Ok,
                       "[SaveManager] Reading unauthenticated plaintext under compatible policy");
        }
        return content;
    }
    // Once CAES is recognized, every failure terminates before any JSON parsing.
    if (!m_keySet || content.size() <= ENCRYPT_HEADER_SIZE) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_CryptoFailed,
                  "[SaveManager] Encrypted save requires a key and a complete CAES envelope");
        return {};
    }
    auto* crypto = BackendRegistry::instance().getCryptoEngine();
    if (!crypto) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_CryptoFailed, "[SaveManager] CryptoEngine not initialized");
        return {};
    }
    const auto* raw = reinterpret_cast<const uint8_t*>(content.data());
    const auto plain = crypto->decrypt(raw + ENCRYPT_HEADER_SIZE, content.size() - ENCRYPT_HEADER_SIZE,
                                       m_encryptKey, 32, raw + 4, 12, raw + 16, 16);
    if (plain.empty()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_CryptoFailed,
                  "[SaveManager] Decryption failed (wrong key or corrupted data)");
        return {};
    }
    return {reinterpret_cast<const char*>(plain.data()), plain.size()};
}

bool SaveManager::encodeSave(const std::string& content, std::string& bytes) {
    if (!m_keySet && m_encryptionPolicy == SaveEncryptionPolicy::RequireEncrypted) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_CryptoFailed,
                  "[SaveManager] Require-encrypted save refused: no encryption key is set");
        return false;
    }
    const size_t overhead = m_keySet ? ENCRYPT_HEADER_SIZE : 0;
    if (content.size() > MAX_SAVE_SIZE - overhead) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                  "[SaveManager] Save payload plus encryption header exceeds MAX_SAVE_SIZE (%zu)",
                  MAX_SAVE_SIZE);
        return false;
    }
    if (!m_keySet) {
        bytes = content;
        return true;
    }
    auto* crypto = BackendRegistry::instance().getCryptoEngine();
    if (!crypto) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_CryptoFailed, "[SaveManager] CryptoEngine not initialized");
        return false;
    }
    uint8_t nonce[12];
    uint8_t tag[16];
    crypto->generateNonce(nonce, sizeof(nonce));
    const auto cipher = crypto->encrypt(reinterpret_cast<const uint8_t*>(content.data()), content.size(),
                                         m_encryptKey, 32, nonce, sizeof(nonce), tag, sizeof(tag));
    if (cipher.empty() || cipher.size() > MAX_SAVE_SIZE - ENCRYPT_HEADER_SIZE) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_CryptoFailed, "[SaveManager] Encryption failed or output exceeds save limit");
        return false;
    }
    bytes.reserve(ENCRYPT_HEADER_SIZE + cipher.size());
    bytes.assign("CAES", 4);
    bytes.append(reinterpret_cast<const char*>(nonce), sizeof(nonce));
    bytes.append(reinterpret_cast<const char*>(tag), sizeof(tag));
    bytes.append(reinterpret_cast<const char*>(cipher.data()), cipher.size());
    return true;
}

bool SaveManager::writeFile(const std::string& path, const std::string& content) {
    std::string bytes;
    return encodeSave(content, bytes) && writeRawFile(path, bytes);
}

bool SaveManager::writeRawFile(const std::string& path, const std::string& dataToWrite) {
    if (m_saveProvider) return m_saveProvider->writeFile(path, dataToWrite);
    if (detail::writeSaveFileAtomically(path, dataToWrite)) return true;
    DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
              "[SaveManager] Atomic save publication failed: %s", path.c_str());
    return false;
}

// ============================================================================
//  Save (write structured JSON to disk)
// ============================================================================

bool SaveManager::save(int slot, const json& gameData,
                       const std::string& sceneName,
                       int tokenIndex,
                       const std::string& thumbnailPng) {
    // Slot bound: negative or absurd slots fabricate paths outside the save
    // dir; the UI only uses 0..99. Guard at the boundary.
    if (!validSlot(slot)) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Ok, "[SaveManager] Slot %d out of range [-2..99]", slot);
        return false;
    }
    // [R2-FIX] This is the canonical C++ JSON save path (Path A).
    // Legacy Lua serialization path (Path B, scripts/system.lua System.save/load)
    // was removed; all KAG [save]/[load] commands and scripts use the
    // KAG.save_game() / KAG.load_game() bindings which route here.

    if (m_saveDir.empty()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Ok, "[SaveManager] Not initialized; call init() first.");
        return false;
    }

    json envelope;
    envelope["schema_version"] = m_currentSchemaVersion;
    envelope["timestamp"]      = static_cast<uint64_t>(time(nullptr));
    envelope["scene"]          = sceneName;
    envelope["token_index"]    = tokenIndex;
    envelope["thumbnail"]      = thumbnailPng;
    envelope["engine_version"] = ENGINE_VERSION;
    envelope["data"]           = gameData;

    std::string path    = slotPath(slot);
    // Compact JSON (no indentation): saves run frequently (quicksave/auto)
    // and the envelope is re-read by listSaves -- ~40% smaller files.
    std::string jsonStr = envelope.dump();

    bool ok = writeFile(path, jsonStr);
    if (ok) {
        printf("[SaveManager] Saved slot %d (%s, token %d, %zu bytes)\n",
               slot, sceneName.c_str(), tokenIndex, jsonStr.size());
    }
    return ok;
}

// ============================================================================
//  Load (read JSON from disk, return structured data)
// ============================================================================

json SaveManager::load(int slot, SaveMeta* outMeta) {
    if (!validSlot(slot)) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Ok, "[SaveManager] Slot %d out of range [-2..99]", slot);
        return {};
    }
    // [R2-FIX] This is the canonical C++ JSON save path (Path A).
    // Legacy Lua serialization path (Path B, scripts/system.lua System.save/load)
    // was removed; all KAG [save]/[load] commands and scripts use the
    // KAG.save_game() / KAG.load_game() bindings which route here.

    return loadContents(slot, readFile(slotPath(slot)), outMeta);
}

json SaveManager::loadLegacyPlaintext(int slot, SaveMeta* outMeta) {
    if (!validSlot(slot) || m_saveDir.empty()) return {};
    const auto contents = readRawFile(slotPath(slot));
    if (hasEncryptedMagic(contents)) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_CryptoFailed,
                  "[SaveManager] Legacy plaintext import refuses CAES data; use authenticated load");
        return {};
    }
    // This explicit read never writes the original or changes encryption policy.
    return loadContents(slot, contents, outMeta);
}

json SaveManager::loadContents(int slot, const std::string& contents, SaveMeta* outMeta) {
    if (contents.empty()) return json();

    json envelope;
    try {
        envelope = json::parse(contents);
    } catch (const json::exception& e) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed, "[SaveManager] JSON parse error: %s", e.what());
        return json();
    }

    // A valid JSON value that is not an object ([] / "x" / 42) would make the
    // value() reads below throw type_error.306 across the Lua C boundary --
    // treat it as corrupt and fail gracefully instead of crashing.
    if (!envelope.is_object()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed, "[SaveManager] Save envelope is not a JSON object; treating as corrupt");
        return json();
    }

    // Field reads must be type-safe: value() throws type_error 302 when a key
    // exists with a wrong type (review ST-1) -- guard each read so a single
    // tampered/corrupt save degrades gracefully instead of crashing.
    auto safeStr = [&envelope](const char* key) -> std::string {
        const auto it = envelope.find(key);
        if (it == envelope.end() || !it->is_string()) return "";
        return it->get<std::string>();
    };
    auto safeUint = [&envelope](const char* key, uint64_t fallback) -> uint64_t {
        const auto it = envelope.find(key);
        if (it == envelope.end() || !it->is_number_unsigned()) return fallback;
        return it->get<uint64_t>();
    };
    auto safeInt = [&envelope](const char* key, int fallback) -> int {
        const auto it = envelope.find(key);
        if (it == envelope.end() || !it->is_number_integer()) return fallback;
        return it->get<int>();
    };
    uint64_t ts       = safeUint("timestamp", 0);
    std::string scene = safeStr("scene");
    int tokenIdx      = safeInt("token_index", 0);
    std::string thumb = safeStr("thumbnail");
    // Absence identifies legacy v1. An explicitly unsupported or malformed
    // schema cannot be interpreted as v1 or narrowed through an int overflow.
    const auto schema = envelope.find("schema_version");
    if (schema != envelope.end() &&
        (!schema->is_number_integer() || *schema < 1 || *schema > m_currentSchemaVersion)) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                  "[SaveManager] Unsupported or invalid save schema");
        return {};
    }
    int schemaVer = schema == envelope.end() ? 1 : schema->get<int>();
    std::string engineVer = safeStr("engine_version");

    // Handle schema migration on the "data" sub-object
    json data = envelope.value("data", json());
    if (data.is_null()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                  "[SaveManager] Save envelope has missing or null data");
        return {};
    }
    if (schemaVer < m_currentSchemaVersion) {
        try {
            data = migrate(data, schemaVer);
        } catch (...) {
            DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                      "[SaveManager] Save migration failed");
            return {};
        }
        if (data.is_null()) return {};
        schemaVer = m_currentSchemaVersion;
    }

    if (!engineVer.empty() && engineVer != ENGINE_VERSION) {
        printf("[SaveManager] Engine version mismatch: %s (engine %s) -- continue loading\n",
               engineVer.c_str(), ENGINE_VERSION);
    }

    printf("[SaveManager] Loaded slot %d (v%d, %s, token %d)\n",
           slot, schemaVer, scene.c_str(), tokenIdx);
    // Publish metadata only after the entire load succeeds. Empty objects and
    // arrays are valid data; only null represents a failed load.
    if (outMeta) {
        SaveMeta meta;
        meta.slot = slot;
        meta.timestamp = ts;
        meta.sceneName = std::move(scene);
        meta.tokenIndex = tokenIdx;
        meta.thumbnail = std::move(thumb);
        meta.schemaVersion = schemaVer;
        *outMeta = std::move(meta);
    }
    return data;
}

// ============================================================================
//  List / delete
// ============================================================================

std::vector<SaveMeta> SaveManager::listSaves() {
    std::vector<SaveMeta> result;
    if (m_saveDir.empty()) return result;

    // Scan the bounded slot range (0..99, same as the save/load guard):
    // legacy files beyond 99 would otherwise be enumerated here. NO
    // early break: a consecutive-empty optimization would hide sparse
    // slots (audit: a save at slot 99 vanished from the list when slots
    // 6..13 were empty). 100 stat calls are cheap for a low-frequency
    // list operation.
    for (int slot = 0; slot <= 99; slot++) {
        std::string contents = readFile(slotPath(slot));
        if (contents.empty()) {
            continue;
        }
        json envelope;
        try {
            envelope = json::parse(contents);
        } catch (const json::exception&) {
            continue;
        }
        if (!envelope.is_object()) continue;  // corrupt: not an object envelope

        SaveMeta meta;
        meta.slot          = slot;
        const auto si = envelope.find("timestamp");
        meta.timestamp     = (si != envelope.end() && si->is_number_unsigned()) ? si->get<uint64_t>() : 0;
        const auto sn = envelope.find("scene");
        meta.sceneName     = (sn != envelope.end() && sn->is_string()) ? sn->get<std::string>() : "";
        const auto st = envelope.find("thumbnail");
        meta.thumbnail     = (st != envelope.end() && st->is_string()) ? st->get<std::string>() : "";
        const auto sk = envelope.find("token_index");
        meta.tokenIndex    = (sk != envelope.end() && sk->is_number_integer()) ? sk->get<int>() : 0;
        const auto sv = envelope.find("schema_version");
        meta.schemaVersion = (sv != envelope.end() && sv->is_number_integer()) ? sv->get<int>() : 1;

        result.push_back(meta);
    }

    std::sort(result.begin(), result.end(),
              [](const SaveMeta& a, const SaveMeta& b) { return a.slot < b.slot; });

    printf("[SaveManager] Found %zu save(s)\n", result.size());
    return result;
}

bool SaveManager::slotExists(int slot) {
    if (!validSlot(slot)) return false;
    // Preserve the legacy compatible/no-key presence query even for opaque CAES bytes.
    // load()/listSaves() still reject those bytes until a key is supplied.
    if (!m_keySet && m_encryptionPolicy == SaveEncryptionPolicy::Compatible) {
        return !readRawFile(slotPath(slot)).empty();
    }
    return !readFile(slotPath(slot)).empty();
}

bool SaveManager::deleteSlot(int slot) {
    if (!validSlot(slot)) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Ok, "[SaveManager] Slot %d out of range [-2..99]", slot);
        return false;
    }
    std::string path = slotPath(slot);
    if (m_saveProvider) return m_saveProvider->deleteFile(path);
    if (remove(path.c_str()) == 0) {
        printf("[SaveManager] Deleted slot %d\n", slot);
        return true;
    }
    DEBUG_ERR(SubSys::Storage, ErrCode::Ok, "[SaveManager] Failed to delete slot %d", slot);
    return false;
}

// ============================================================================
//  Migration (with structured JSON)
// ============================================================================

void SaveManager::registerMigration(int fromVersion, int toVersion, MigrationFn fn) {
    if (toVersion <= fromVersion) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Ok, "[SaveManager] Rejected migration v%d -> v%d (must increase)", fromVersion, toVersion);
        return;
    }
    m_migrations[fromVersion] = {toVersion, fn};
    if (toVersion > m_currentSchemaVersion) {
        m_currentSchemaVersion = toVersion;
    }
    printf("[SaveManager] Registered migration: v%d -> v%d\n", fromVersion, toVersion);
}

json SaveManager::migrate(const json& data, int fromVersion) {
    if (fromVersion < 1 || fromVersion > m_currentSchemaVersion) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                  "[SaveManager] Unsupported migration source v%d", fromVersion);
        return {};
    }
    if (fromVersion == m_currentSchemaVersion) return data;
    if (!data.is_object()) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                  "[SaveManager] Migration input is not an object");
        return {};
    }
    json current = data;
    int ver = fromVersion;

    int steps = 0;
    while (ver < m_currentSchemaVersion && steps < 64) {
        auto it = m_migrations.find(ver);
        if (it == m_migrations.end()) {
            DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                      "[SaveManager] Missing migration from v%d to reach v%d",
                      ver, m_currentSchemaVersion);
            return {};
        }

        int nextVer = it->second.first;
        printf("[SaveManager] Applying migration v%d -> v%d\n", ver, nextVer);
        current = it->second.second(current);
        if (current.is_null()) {
            DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                      "[SaveManager] Migration returned null data at v%d", ver);
            return {};
        }
        ver = nextVer;
        steps++;
    }
    if (ver != m_currentSchemaVersion) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                  "[SaveManager] Migration chain did not reach v%d within 64 steps (stopped at v%d)",
                  m_currentSchemaVersion, ver);
        return {};
    }
    return current;
}

// ============================================================================
//  Built-in migration scripts
// ============================================================================

void SaveManager::registerBuiltinMigrations() {
    // [R2-FIX] Built-in schema migrations handle format evolution.
    // Current chain: v1->v2(add playtime)->v3(add minigame)->v4(add live2d)->v5(add editor).
    // New fields added in save.lua capture_state() should increment schema_version there.

    // v1 -> v2: Add playtime field
    registerMigration(1, 2, [](const json& data) -> json {
        json result = data;
        if (!result.contains("playtime")) result["playtime"] = 0;
        return result;
    });
    // v2 -> v3: Add MiniGame 3D state
    registerMigration(2, 3, [](const json& data) -> json {
        json result = data;
        if (!result.contains("minigame")) result["minigame"] = json::object();
        return result;
    });
    // v3 -> v4: Add Live2D state
    registerMigration(3, 4, [](const json& data) -> json {
        json result = data;
        if (!result.contains("live2d")) result["live2d"] = json::object();
        return result;
    });
    // v4 -> v5: Add Editor state
    registerMigration(4, 5, [](const json& data) -> json {
        json result = data;
        if (!result.contains("editor")) result["editor"] = json::object();
        return result;
    });
}


} // namespace Caesura
