// CloudSaveProvider: opaque save bytes over the Steam I/O boundary.
#include "CloudSaveProvider.h"
#include "LocalFileSaveProvider.h"
#include "CloudSaveSnapshot.h"
#include "../steam/api/ISteamBackend.h"
#include "../debug/api/DebugLog.h"
#include <charconv>
#include <exception>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>
#include <algorithm>

namespace Caesura {
namespace {
constexpr int32_t kChunkSize = 256 * 1024;
constexpr int32_t kMaxChunkedSize = 64 * 1024 * 1024;
constexpr int32_t kMaxMetadataSize = 1024;

struct ChunkManifest {
    int32_t totalSize = 0;
    int32_t chunks = 0;
    std::string generation;  // Empty means the original total,count format.
};

bool decimal(std::string_view text, int32_t& value) {
    if (text.empty() || !std::all_of(text.begin(), text.end(),
            [](char c) { return c >= '0' && c <= '9'; })) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parseManifest(const std::string& bytes, ChunkManifest& manifest) {
    std::string_view text(bytes);
    const bool versioned = text.substr(0, 3) == "v2,";
    if (versioned) text.remove_prefix(3);
    const auto comma = text.find(',');
    if (comma == std::string_view::npos ||
        !decimal(text.substr(0, comma), manifest.totalSize)) return false;
    text.remove_prefix(comma + 1);
    const auto next = text.find(',');
    if (!decimal(text.substr(0, next), manifest.chunks)) return false;
    manifest.generation.clear();
    if (versioned) {
        if (next == std::string_view::npos) return false;
        const auto generation = text.substr(next + 1);
        if (generation.size() != 32 || !std::all_of(generation.begin(), generation.end(),
                [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            return false;
        manifest.generation.assign(generation.data(), generation.size());
    } else if (next != std::string_view::npos) {
        return false;
    }
    return manifest.totalSize > 0 && manifest.totalSize <= kMaxChunkedSize &&
           manifest.chunks == (manifest.totalSize + kChunkSize - 1) / kChunkSize;
}

bool readExact(ISteamBackend& steam, const std::string& name, int32_t size,
               std::string& bytes) {
    if (size <= 0 || size > kMaxChunkedSize || steam.cloudFileSize(name.c_str()) != size)
        return false;
    bytes.assign(static_cast<size_t>(size), '\0');
    return steam.cloudRead(name.c_str(), bytes.data(), size) == size;
}

bool readManifest(ISteamBackend& steam, const std::string& name, ChunkManifest& manifest) {
    const int32_t size = steam.cloudFileSize(name.c_str());
    if (size <= 0 || size > kMaxMetadataSize) return false;
    std::string bytes;
    return readExact(steam, name, size, bytes) && parseManifest(bytes, manifest);
}

std::string chunkName(const std::string& path, const ChunkManifest& manifest, int32_t index) {
    std::ostringstream name;
    name << path;
    if (!manifest.generation.empty()) name << ".g" << manifest.generation;
    name << ".chunk" << std::setfill('0') << std::setw(3) << index;
    return name.str();
}

bool deleteIfPresent(ISteamBackend& steam, const std::string& name) {
    return !steam.cloudFileExists(name.c_str()) || steam.cloudDelete(name.c_str());
}

bool cleanupChunks(ISteamBackend& steam, const std::string& path,
                   const ChunkManifest& manifest) {
    bool cleaned = true;
    for (int32_t index = 0; index < manifest.chunks; ++index) {
        if (!deleteIfPresent(steam, chunkName(path, manifest, index))) cleaned = false;
    }
    return cleaned;
}

bool selectGeneration(ISteamBackend& steam, const std::string& path,
                      const ChunkManifest& previous, ChunkManifest& next) {
    // 128 random bits, not a wall-clock timestamp. Collision checks also
    // protect deterministic random_device implementations and abandoned data.
    try {
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::ostringstream token;
            token << std::hex << std::setfill('0');
            for (int word = 0; word < 8; ++word)
                token << std::setw(4) << (random() & 0xffffu);
            next.generation = token.str();
            if (next.generation == previous.generation) continue;
            bool collision = false;
            // Check the entire valid generation namespace, including orphan
            // chunks beyond this new payload's final chunk.
            for (int32_t index = 0; index < kMaxChunkedSize / kChunkSize; ++index) {
                if (steam.cloudFileExists(chunkName(path, next, index).c_str())) {
                    collision = true;
                    break;
                }
            }
            if (!collision) return true;
        }
    } catch (const std::exception&) {
        // No trustworthy unique name means no remote writes are attempted.
    }
    return false;
}
} // namespace

CloudSaveProvider::CloudSaveProvider(ISteamBackend* steam) : m_steam(steam) {}

CloudSnapshot CloudSaveProvider::readSnapshot(CloudSide side, const std::string& slotPath) {
    if (side == CloudSide::Local) return detail::readLocalCloudSnapshot(slotPath);
    CloudSnapshot snapshot;
    auto fail = [&](CloudReadState state, CloudReadError error) {
        snapshot.state = state;
        snapshot.error = error;
        snapshot.bytes.clear();
        return snapshot;
    };
    const std::string path = cloudKey(slotPath);
    if (slotPath.find('\0') != std::string::npos || path.empty() || path == "." || path == "..")
        return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);
    if (!m_steam || !m_steam->isAvailable())
        return fail(CloudReadState::Unavailable, CloudReadError::BackendUnavailable);

    // Size/exists cannot distinguish remote absence from a failed SDK query.
    // Exact reads and repeat size observations detect changes, not atomicity.
    auto read = [&](const std::string& name, int32_t size, std::string& bytes,
                    bool payload) -> CloudReadError {
        if (m_steam->cloudFileSize(name.c_str()) != size)
            return CloudReadError::ChangedDuringRead;
        bytes.assign(static_cast<size_t>(size), '\0');
        const int32_t count = m_steam->cloudRead(name.c_str(), bytes.data(), size);
        if (payload && count > 0)
            snapshot.observedBytes += static_cast<uint64_t>((std::min)(count, size));
        if (count != size) return CloudReadError::Truncated;
        if (m_steam->cloudFileSize(name.c_str()) != size)
            return CloudReadError::ChangedDuringRead;
        return CloudReadError::None;
    };
    const std::string headName = path + ".meta";
    if (m_steam->cloudFileExists(headName.c_str())) {
        const int32_t headSize = m_steam->cloudFileSize(headName.c_str());
        if (headSize <= 0)
            return fail(CloudReadState::Failed, CloudReadError::IndeterminateSize);
        if (headSize > kMaxMetadataSize)
            return fail(CloudReadState::Invalid, CloudReadError::TooLarge);
        std::string head;
        auto error = read(headName, headSize, head, false);
        if (error != CloudReadError::None) return fail(CloudReadState::Failed, error);
        ChunkManifest manifest;
        if (!parseManifest(head, manifest))
            return fail(CloudReadState::Invalid, CloudReadError::MalformedMetadata);
        snapshot.bytes.reserve(static_cast<size_t>(manifest.totalSize));
        for (int32_t index = 0; index < manifest.chunks; ++index) {
            const int32_t expected = (std::min)(kChunkSize, manifest.totalSize - index * kChunkSize);
            std::string chunk;
            error = read(chunkName(path, manifest, index), expected, chunk, true);
            if (error != CloudReadError::None) return fail(CloudReadState::Failed, error);
            snapshot.bytes += chunk;
        }
        std::string finalHead;
        error = read(headName, headSize, finalHead, false);
        if (error != CloudReadError::None) return fail(CloudReadState::Failed, error);
        if (head != finalHead)
            return fail(CloudReadState::Failed, CloudReadError::ChangedDuringRead);
    } else {
        if (!m_steam->cloudFileExists(path.c_str()))
            return fail(CloudReadState::Unavailable, CloudReadError::IndeterminateMissing);
        const int32_t size = m_steam->cloudFileSize(path.c_str());
        if (size <= 0)
            return fail(CloudReadState::Failed, CloudReadError::IndeterminateSize);
        if (size > kMaxChunkedSize)
            return fail(CloudReadState::Invalid, CloudReadError::TooLarge);
        const auto error = read(path, size, snapshot.bytes, true);
        if (error != CloudReadError::None) return fail(CloudReadState::Failed, error);
        if (m_steam->cloudFileExists(headName.c_str()))
            return fail(CloudReadState::Failed, CloudReadError::ChangedDuringRead);
    }
    snapshot.state = CloudReadState::Present;
    return snapshot;
}

CloudConditionalWriteSupport CloudSaveProvider::conditionalWriteSupport(CloudSide) const {
    return CloudConditionalWriteSupport::Unsupported;
}

// Steam Remote Storage is flat: caller directory prefixes never enter keys.
std::string CloudSaveProvider::cloudKey(const std::string& slotPath) {
    const auto pos = slotPath.find_last_of("/\\");
    return pos == std::string::npos ? slotPath : slotPath.substr(pos + 1);
}

std::string CloudSaveProvider::readFile(const std::string& rawPath) {
    if (!m_steam) return {};
    const std::string path = cloudKey(rawPath);
    if (path.empty()) return {};
    if (m_steam->cloudFileExists((path + ".meta").c_str())) {
        ChunkManifest manifest;
        if (!readManifest(*m_steam, path + ".meta", manifest)) return {};
        std::string result;
        result.reserve(static_cast<size_t>(manifest.totalSize));
        for (int32_t index = 0; index < manifest.chunks; ++index) {
            const int32_t expected = std::min(kChunkSize,
                manifest.totalSize - index * kChunkSize);
            std::string chunk;
            if (!readExact(*m_steam, chunkName(path, manifest, index), expected, chunk)) return {};
            result += chunk;
        }
        return result;
    }
    std::string bytes;
    const int32_t size = m_steam->cloudFileSize(path.c_str());
    return readExact(*m_steam, path, size, bytes) ? bytes : std::string{};
}

bool CloudSaveProvider::writeFile(const std::string& rawPath, const std::string& content) {
    if (!m_steam || content.empty() || content.size() > static_cast<size_t>(kMaxChunkedSize))
        return false;
    const std::string path = cloudKey(rawPath);
    if (path.empty()) return false;
    const std::string head = path + ".meta";
    const bool hadManifest = m_steam->cloudFileExists(head.c_str());
    ChunkManifest previous;
    if (hadManifest && !readManifest(*m_steam, head, previous)) return false;
    const auto size = static_cast<int32_t>(content.size());
    if (!hadManifest && size <= kChunkSize)
        return m_steam->cloudWrite(path.c_str(), content.data(), size);

    ChunkManifest next{size, (size + kChunkSize - 1) / kChunkSize, {}};
    if (!selectGeneration(*m_steam, path, previous, next)) return false;
    int32_t attemptedChunks = 0;
    auto discardStaged = [&]() {
        ChunkManifest staged = next;
        staged.chunks = attemptedChunks;
        if (!cleanupChunks(*m_steam, path, staged))
            DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                       "[CloudSaveProvider] failed to clean an unpublished generation");
    };
    for (int32_t index = 0; index < next.chunks; ++index) {
        const int32_t offset = index * kChunkSize;
        const int32_t length = std::min(kChunkSize, size - offset);
        ++attemptedChunks;
        if (!m_steam->cloudWrite(chunkName(path, next, index).c_str(),
                                content.data() + offset, length)) {
            discardStaged();
            return false;
        }
    }
    // All chunks must exist with the exact supplied bytes before publication.
    for (int32_t index = 0; index < next.chunks; ++index) {
        const int32_t offset = index * kChunkSize;
        const int32_t length = std::min(kChunkSize, size - offset);
        std::string written;
        if (!readExact(*m_steam, chunkName(path, next, index), length, written) ||
            written.compare(0, written.size(), content, static_cast<size_t>(offset),
                            static_cast<size_t>(length)) != 0) {
            discardStaged();
            return false;
        }
    }
    const std::string metadata = "v2," + std::to_string(size) + "," +
        std::to_string(next.chunks) + "," + next.generation;
    // Sole publication call. This assumes the synchronous SDK boundary does
    // not change an existing object on rejection. It is not a crash-atomic or
    // concurrent-writer guarantee, and no in-memory rollback is attempted.
    if (!m_steam->cloudWrite(head.c_str(), metadata.data(), static_cast<int32_t>(metadata.size()))) {
        discardStaged();
        return false;
    }
    // Publication has succeeded. Cleanup failure must not turn that committed
    // operation into a false failure or destroy the newly selected generation.
    const bool chunksCleaned = !hadManifest || cleanupChunks(*m_steam, path, previous);
    const bool directCleaned = deleteIfPresent(*m_steam, path);
    if (!chunksCleaned || !directCleaned)
        DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                   "[CloudSaveProvider] published save retained obsolete cloud objects");
    return true;
}

bool CloudSaveProvider::deleteFile(const std::string& rawPath) {
    if (!m_steam) return false;
    const std::string path = cloudKey(rawPath);
    if (path.empty()) return false;
    const std::string head = path + ".meta";
    if (m_steam->cloudFileExists(head.c_str())) {
        ChunkManifest manifest;
        if (!readManifest(*m_steam, head, manifest)) {
            // Preserve corrupt-slot recovery without following any untrusted
            // references: only these two caller-derived keys may be removed.
            if (!deleteIfPresent(*m_steam, path)) return false;
            return m_steam->cloudDelete(head.c_str());
        }
        if (!cleanupChunks(*m_steam, path, manifest)) return false;
        if (!deleteIfPresent(*m_steam, path)) return false;
        return m_steam->cloudDelete(head.c_str());
    }
    return m_steam->cloudDelete(path.c_str());
}

std::vector<std::string> CloudSaveProvider::listFiles(const std::string&) {
    // Logical pattern enumeration is not implemented. In particular, never
    // expose generation/staging keys as logical save files. SaveManager probes
    // its bounded slot set through readFile instead.
    return {};
}

// push = LOCAL FILE -> CLOUD. One direction, no merge, no timestamp compare:
// the on-disk file wins and the cloud copy of that slot is replaced. Pushing a
// stale local save therefore overwrites a newer cloud save -- that is the
// CALLER's decision. Nothing in the engine calls this on its own (only the
// explicit Lua KAG.cloud_push binding), so there is no automatic path that can
// clobber cloud data behind the player's back.
bool CloudSaveProvider::pushToCloud(const std::string& slotPath) {
    if (!m_steam) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                   "[CloudSaveProvider] pushToCloud(%s) refused: no Steam backend "
                   "(Steamworks unavailable or not initialized)", slotPath.c_str());
        return false;
    }
    const auto content = readLocalFile(slotPath);
    return !content.empty() && writeCloudFile(slotPath, content);
}

std::string CloudSaveProvider::readLocalFile(const std::string& slotPath) {
    // Read the LOCAL file only. The previous revision fell back to reading the
    // CLOUD copy and writing it straight back, so a cloud->cloud no-op reported
    // success and "nothing local to push" was indistinguishable from a real
    // upload.
    std::ifstream in(slotPath, std::ios::binary);
    if (!in.is_open()) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                   "[CloudSaveProvider] pushToCloud(%s): no local file to push",
                   slotPath.c_str());
        return {};
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                   "[CloudSaveProvider] pushToCloud(%s): local file is empty; "
                   "refusing to replace the cloud copy with nothing",
                   slotPath.c_str());
        return {};
    }
    // Same ceiling readFile() enforces on the way back: never ship a payload
    // upstream that this engine would refuse to reassemble.
    if (size > kMaxChunkedSize) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                  "[CloudSaveProvider] pushToCloud(%s) refused: %zu bytes exceeds "
                  "the %d-byte chunked ceiling", slotPath.c_str(), static_cast<size_t>(size),
                  static_cast<int>(kMaxChunkedSize));
        return {};
    }
    std::string content(static_cast<size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    in.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!in) return {};
    return content;
}

// pull = CLOUD -> LOCAL FILE. One direction, no merge, no timestamp compare:
// the cloud copy wins and REPLACES the local file. This is the only call that
// can destroy an on-disk save the player made offline, so it is guarded three
// ways: an absent/empty cloud file aborts BEFORE the local file is touched; the
// write goes through LocalFileSaveProvider (temp file + atomic rename under the
// same 10 MiB ceiling as SaveManager), so a crash or an oversized cloud payload
// cannot truncate the existing save; and nothing in the engine invokes it
// automatically (only the explicit Lua KAG.cloud_pull binding).
bool CloudSaveProvider::pullFromCloud(const std::string& slotPath) {
    if (!m_steam) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                   "[CloudSaveProvider] pullFromCloud(%s) refused: no Steam backend "
                   "(Steamworks unavailable or not initialized)", slotPath.c_str());
        return false;
    }
    const std::string content = readCloudFile(slotPath);
    if (content.empty()) {
        DEBUG_WARN(SubSys::Storage, ErrCode::Storage_SaveReadFailed,
                   "[CloudSaveProvider] pullFromCloud(%s): cloud copy absent or "
                   "empty; local file left untouched", slotPath.c_str());
        return false;
    }
    return writeLocalFile(slotPath, content);
}

std::string CloudSaveProvider::readCloudFile(const std::string& slotPath) {
    return readFile(slotPath);
}

bool CloudSaveProvider::writeCloudFile(const std::string& slotPath, const std::string& bytes) {
    return writeFile(slotPath, bytes);
}

bool CloudSaveProvider::writeLocalFile(const std::string& slotPath, const std::string& content) {
    // Atomic + size-capped write (ST-3). A raw ofstream here would truncate the
    // player's local save the moment it opened the file.
    LocalFileSaveProvider local;
    if (!local.writeFile(slotPath, content)) {
        DEBUG_ERR(SubSys::Storage, ErrCode::Storage_SaveWriteFailed,
                  "[CloudSaveProvider] pullFromCloud(%s): local write rejected "
                  "(oversized payload or unwritable path); previous local save kept",
                  slotPath.c_str());
        return false;
    }
    return true;
}

} // namespace Caesura
