// DeltaCARC — differential CARC update: generate / apply / verify
//
// Format (v2): entries carry the CARC path-hash (hex) and, for
// Add/Replace, the plaintext file data. CARC indexes are path-hash
// addressed and plaintext paths are not recoverable, so hashes are the
// stable identifier. The whole delta body is AES-256-GCM encrypted;
// source/target SHAs are stored in the header.
#include "DeltaCARC.h"
#include "ArchivePath.h"
#include "CARCReader.h"
#include "CARCWriter.h"
#include "CryptoEngine.h"
#include "../debug/api/DebugLog.h"
#include <fstream>
#include <cstring>
#include <cstdio>
#include <unordered_set>
#include <unordered_map>

namespace Caesura::carc {

// ==========================================================================
// Helpers
// ==========================================================================

static bool computeSHA256(const std::string& path, uint8_t out[PATH_HASH_SIZE]) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    size_t size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(size);
    if (size > 0) {
        f.read(reinterpret_cast<char*>(data.data()), size);
        if (f.gcount() != static_cast<std::streamsize>(size)) return false; // short read
    }
    CryptoEngine::sha256(data.data(), size, out);
    return true;
}

static void writeU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((v >> 0) & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

static void writeU64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; i++) buf.push_back((v >> (i * 8)) & 0xFF);
}

static void writeBytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

// Read a little-endian u32 from buf at *p; advances *p.
static bool readU32(const uint8_t* p, const uint8_t* end, uint32_t& out) {
    if (static_cast<size_t>(end - p) < 4) return false;
    out = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    return true;
}

static bool readU64(const uint8_t* p, const uint8_t* end, uint64_t& out) {
    if (static_cast<size_t>(end - p) < 8) return false;
    out = 0;
    for (int i = 0; i < 8; i++) out |= ((uint64_t)p[i] << (i * 8));
    return true;
}

// Decode a 64-char hex SHA-256 into raw bytes.
static bool hexDecode(const std::string& hex, uint8_t out[PATH_HASH_SIZE]) {
    if (hex.size() != PATH_HASH_SIZE * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < PATH_HASH_SIZE; i++) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// ==========================================================================
// Generate delta
// ==========================================================================

bool DeltaCARC::generate(const std::string& oldPath,
                          const std::string& newPath,
                          const std::string& deltaPath) {
    // Open both CARCs
    CARCReader oldReader, newReader;
    if (!oldReader.open(oldPath)) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Cannot open old: %s", oldPath.c_str()); return false; }
    if (!newReader.open(newPath)) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Cannot open new: %s", newPath.c_str()); return false; }

    // Compute source/target SHAs
    uint8_t oldSHA[PATH_HASH_SIZE], newSHA[PATH_HASH_SIZE];
    if (!computeSHA256(oldPath, oldSHA)) return false;
    if (!computeSHA256(newPath, newSHA)) return false;

    const auto& oldIdx = oldReader.index();
    const auto& newIdx = newReader.index();

    std::vector<uint8_t> deltaBody;
    uint32_t entryCount = 0;

    // Files removed from old (present in old, absent in new)
    for (const auto& [hashHex, info] : oldIdx) {
        (void)info;
        if (newIdx.find(hashHex) == newIdx.end()) {
            deltaBody.push_back((uint8_t)DeltaFlag::Remove);
            writeU32(deltaBody, (uint32_t)hashHex.size());
            writeBytes(deltaBody, (const uint8_t*)hashHex.data(), hashHex.size());
            entryCount++;
        }
    }

    // Files added (absent in old) or replaced (different content).
    // Compressed size is a cheap first test; equal sizes fall through to
    // a plaintext comparison so content changes never slip through.
    // Note: readFileByHash returns {} for both empty content and failure,
    // so originalSize is used to tell them apart.
    for (const auto& [hashHex, info] : newIdx) {
        auto oldIt = oldIdx.find(hashHex);
        if (oldIt != oldIdx.end() && oldIt->second.compressedSize == info.compressedSize) {
            uint8_t hash[PATH_HASH_SIZE];
            if (!hexDecode(hashHex, hash)) return false;
            auto oldData = oldReader.readFileByHash(hash);
            auto newData = newReader.readFileByHash(hash);
            if ((!oldData.empty() || oldIt->second.originalSize == 0) &&
                (!newData.empty() || info.originalSize == 0) &&
                oldData == newData) {
                continue; // unchanged
            }
        }
        uint8_t hash[PATH_HASH_SIZE];
        if (!hexDecode(hashHex, hash)) {
            DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Bad hash in new index");
            return false;
        }
        auto data = newReader.readFileByHash(hash);
        if (data.empty() && info.originalSize != 0) {
            DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Cannot read new file: %s", hashHex.c_str());
            return false;
        }
        DeltaFlag flag = (oldIt == oldIdx.end()) ? DeltaFlag::Add : DeltaFlag::Replace;
        deltaBody.push_back((uint8_t)flag);
        writeU32(deltaBody, (uint32_t)hashHex.size());
        writeBytes(deltaBody, (const uint8_t*)hashHex.data(), hashHex.size());
        writeU64(deltaBody, data.size());
        if (!data.empty()) {
            writeBytes(deltaBody, data.data(), data.size());
        }
        entryCount++;
    }

    // Build delta header
    DeltaHeader hdr;
    hdr.magic = DELTA_MAGIC;
    hdr.version = DELTA_VERSION;
    memcpy(hdr.sourceSHA, oldSHA, PATH_HASH_SIZE);
    memcpy(hdr.targetSHA, newSHA, PATH_HASH_SIZE);
    hdr.entryCount = entryCount;
    memset(hdr.reserved, 0, sizeof(hdr.reserved));

    // Encrypt delta body. An empty body (no changes between archives) is
    // valid: the delta carries just the header.
    uint8_t key[AES_KEY_SIZE] = {}, nonce[AES_NONCE_SIZE] = {}, tag[AES_TAG_SIZE] = {};
    CryptoEngine::generateKey(key);
    CryptoEngine::generateNonce(nonce);
    std::vector<uint8_t> encrypted;
    if (!deltaBody.empty()) {
        encrypted = CryptoEngine::encrypt(deltaBody.data(), deltaBody.size(), key, nonce, tag);
        if (encrypted.empty()) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_CryptoFailed, "[DeltaCARC] Encryption failed"); return false; }
    }

    // Write delta file: [header][key][nonce][tag][encrypted_body]
    std::ofstream out(pathFromUtf8(deltaPath), std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(DeltaHeader));
    out.write(reinterpret_cast<const char*>(key), AES_KEY_SIZE);
    out.write(reinterpret_cast<const char*>(nonce), AES_NONCE_SIZE);
    out.write(reinterpret_cast<const char*>(tag), AES_TAG_SIZE);
    // Empty body (identical archives): skip the write to avoid the
    // nullptr+0 ofstream::write edge (strictly UB with a null data()).
    if (!encrypted.empty()) {
        out.write(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
    }

    printf("[DeltaCARC] Generated delta: %u entries, %zu → %zu bytes\n",
           entryCount, deltaBody.size(), encrypted.size());
    return true;
}

// ==========================================================================
// Apply delta
// ==========================================================================

bool DeltaCARC::apply(const std::string& sourcePath,
                       const std::string& deltaPath,
                       const std::string& outputPath) {
    // Read delta
    std::ifstream df(pathFromUtf8(deltaPath), std::ios::binary | std::ios::ate);
    if (!df) return false;
    size_t dfSize = df.tellg(); df.seekg(0);
    if (dfSize < sizeof(DeltaHeader) + AES_KEY_SIZE + AES_NONCE_SIZE + AES_TAG_SIZE) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Delta file too small");
        return false;
    }

    DeltaHeader hdr;
    df.read(reinterpret_cast<char*>(&hdr), sizeof(DeltaHeader));
    if (hdr.magic != DELTA_MAGIC) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Bad magic"); return false; }
    if (hdr.version != DELTA_VERSION) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Unsupported version %u", hdr.version); return false; }

    uint8_t key[AES_KEY_SIZE], nonce[AES_NONCE_SIZE], tag[AES_TAG_SIZE];
    df.read(reinterpret_cast<char*>(key), AES_KEY_SIZE);
    df.read(reinterpret_cast<char*>(nonce), AES_NONCE_SIZE);
    df.read(reinterpret_cast<char*>(tag), AES_TAG_SIZE);

    size_t encSize = dfSize - sizeof(DeltaHeader) - AES_KEY_SIZE - AES_NONCE_SIZE - AES_TAG_SIZE;
    std::vector<uint8_t> encrypted(encSize);
    if (encSize > 0) {
        df.read(reinterpret_cast<char*>(encrypted.data()), encSize);
    }
    df.close();

    // Verify source SHA
    uint8_t actualSHA[PATH_HASH_SIZE];
    if (!computeSHA256(sourcePath, actualSHA)) return false;

    // An empty body means the archives were identical (no changes).
    std::vector<uint8_t> deltaBody;
    if (encSize > 0) {
        deltaBody = CryptoEngine::decrypt(encrypted.data(), encSize, key, nonce, tag);
        if (deltaBody.empty()) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_CryptoFailed, "[DeltaCARC] Decrypt failed"); return false; }
    } else if (hdr.entryCount > 0) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Corrupt delta: empty body with %u entries", hdr.entryCount);
        return false;
    }
    if (memcmp(actualSHA, hdr.sourceSHA, PATH_HASH_SIZE) != 0) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Source SHA mismatch — delta not for this file");
        return false;
    }

    // Parse delta entries: removed hashes + replacement/added data
    std::unordered_set<std::string> removes;
    std::unordered_map<std::string, std::vector<uint8_t>> updates;
    uint32_t parsed = 0;
    if (!deltaBody.empty()) {
        const uint8_t* p = deltaBody.data();
        const uint8_t* end = p + deltaBody.size();
        while (p < end) {
            if (static_cast<size_t>(end - p) < 5) {
                DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Corrupt delta: truncated entry header");
                return false;
            }
            DeltaFlag flag = (DeltaFlag)*p++;
            uint32_t hashLen = 0;
            if (!readU32(p, end, hashLen)) return false;
            p += 4;
            if (hashLen > static_cast<size_t>(end - p)) {
                DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Corrupt delta: hashLen %u exceeds buffer", hashLen);
                return false;
            }
            std::string hashHex(reinterpret_cast<const char*>(p), hashLen);
            p += hashLen;

            if (flag == DeltaFlag::Remove) {
                removes.insert(hashHex);
                parsed++;
                continue;
            }
            if (flag != DeltaFlag::Add && flag != DeltaFlag::Replace) {
                DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Corrupt delta: unknown flag %u", (unsigned)flag);
                return false;
            }
            uint64_t dataLen = 0;
            if (!readU64(p, end, dataLen)) return false;
            p += 8;
            // Compare lengths as uint64 before any pointer arithmetic:
            // p + dataLen would wrap for dataLen >= 2^63 (UB / DoS).
            if (dataLen > static_cast<uint64_t>(end - p)) {
                DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Corrupt delta: data size %llu exceeds buffer",
                        (unsigned long long)dataLen);
                return false;
            }
            updates[hashHex].assign(p, p + dataLen);
            p += dataLen;
            parsed++;
        }
    }
    if (parsed != hdr.entryCount) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Corrupt delta: entry count mismatch (header %u, body %u)",
                hdr.entryCount, parsed);
        return false;
    }

    // Open source CARC and repack into the output
    CARCReader src;
    if (!src.open(sourcePath)) return false;

    // Snapshot the expected output index set before repack mutates `updates`.
    std::unordered_set<std::string> expectedIndex;
    for (const auto& [hashHex, info] : src.index()) {
        (void)info;
        if (removes.count(hashHex) == 0) expectedIndex.insert(hashHex);
    }
    for (const auto& [hashHex, data] : updates) {
        (void)data;
        expectedIndex.insert(hashHex);
    }

    CARCWriter writer;
    if (!writer.create(outputPath)) return false;

    auto addByHex = [&writer](const std::string& hashHex, const std::vector<uint8_t>& data) -> bool {
        uint8_t hash[PATH_HASH_SIZE];
        if (!hexDecode(hashHex, hash)) return false;
        return writer.addFileByHash(hash, data.data(), data.size());
    };

    for (const auto& [hashHex, info] : src.index()) {
        if (removes.count(hashHex) > 0) continue;
        auto it = updates.find(hashHex);
        if (it != updates.end()) {
            if (!addByHex(hashHex, it->second)) return false;
            updates.erase(it);
        } else {
            uint8_t hash[PATH_HASH_SIZE];
            if (!hexDecode(hashHex, hash)) return false;
            auto data = src.readFileByHash(hash);
            if (data.empty() && info.originalSize != 0) {
                DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Cannot read source file: %s", hashHex.c_str());
                return false;
            }
            if (!writer.addFileByHash(hash, data.data(), data.size())) return false;
        }
    }
    for (const auto& [hashHex, data] : updates) {
        if (!addByHex(hashHex, data)) return false;
    }

    if (!writer.finalize()) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_WriteFailed, "[DeltaCARC] Failed to finalize output CARC");
        return false;
    }

    // The output is freshly packed (new keys/nonces), so byte-exact SHA
    // equality with the original target is not achievable; the header SHA
    // is kept for provenance. Repack correctness is verified structurally:
    // the output must open with CARCReader and its index set must equal
    // (source index − removed) + updated/added hashes.
    CARCReader check;
    if (!check.open(outputPath)) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Output CARC failed verification");
        return false;
    }
    if (check.numFiles() != expectedIndex.size()) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Output index mismatch: %zu files, expected %zu",
                check.numFiles(), expectedIndex.size());
        return false;
    }
    for (const auto& [hashHex, info] : check.index()) {
        (void)info;
        if (expectedIndex.count(hashHex) == 0) {
            DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Output index mismatch: unexpected %s", hashHex.c_str());
            return false;
        }
    }

    printf("[DeltaCARC] Delta applied: %u entries → %s (%zu files)\n",
           hdr.entryCount, outputPath.c_str(), check.numFiles());
    return true;
}

// ==========================================================================
// Verify delta integrity
// ==========================================================================

bool DeltaCARC::verify(const std::string& deltaPath) {
    std::ifstream df(pathFromUtf8(deltaPath), std::ios::binary | std::ios::ate);
    if (!df) return false;
    size_t dfSize = df.tellg(); df.seekg(0);
    if (dfSize < sizeof(DeltaHeader) + AES_KEY_SIZE + AES_NONCE_SIZE + AES_TAG_SIZE) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Delta file too small");
        return false;
    }

    DeltaHeader hdr;
    df.read(reinterpret_cast<char*>(&hdr), sizeof(DeltaHeader));
    if (hdr.magic != DELTA_MAGIC) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Bad magic"); return false; }
    if (hdr.version != DELTA_VERSION) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Unsupported version"); return false; }

    uint8_t key[AES_KEY_SIZE], nonce[AES_NONCE_SIZE], tag[AES_TAG_SIZE];
    df.read(reinterpret_cast<char*>(key), AES_KEY_SIZE);
    df.read(reinterpret_cast<char*>(nonce), AES_NONCE_SIZE);
    df.read(reinterpret_cast<char*>(tag), AES_TAG_SIZE);

    size_t encSize = dfSize - sizeof(DeltaHeader) - AES_KEY_SIZE - AES_NONCE_SIZE - AES_TAG_SIZE;
    std::vector<uint8_t> encrypted(encSize);
    if (encSize > 0) {
        df.read(reinterpret_cast<char*>(encrypted.data()), encSize);
    }
    df.close();

    // An empty body means the archives were identical (no changes).
    std::vector<uint8_t> deltaBody;
    if (encSize > 0) {
        deltaBody = CryptoEngine::decrypt(encrypted.data(), encSize, key, nonce, tag);
        if (deltaBody.empty()) { DEBUG_ERR(SubSys::Archive, ErrCode::Archive_CryptoFailed, "[DeltaCARC] Decrypt failed"); return false; }
    }

    // Walk every entry with bounds checks — malformed bodies fail here.
    uint32_t parsed = 0;
    if (!deltaBody.empty()) {
        const uint8_t* p = deltaBody.data();
        const uint8_t* end = p + deltaBody.size();
        while (p < end) {
        if (static_cast<size_t>(end - p) < 5) return false;
        DeltaFlag flag = (DeltaFlag)*p++;
        uint32_t hashLen = 0;
        if (!readU32(p, end, hashLen)) return false;
        p += 4;
        if (hashLen > static_cast<size_t>(end - p)) return false;
        p += hashLen;
        if (flag == DeltaFlag::Add || flag == DeltaFlag::Replace) {
            uint64_t dataLen = 0;
            if (!readU64(p, end, dataLen)) return false;
            p += 8;
            // Compare lengths as uint64 before any pointer arithmetic:
            // p + dataLen would wrap for dataLen >= 2^63 (UB / DoS).
            if (dataLen > static_cast<uint64_t>(end - p)) return false;
            p += dataLen;
        } else if (flag != DeltaFlag::Remove) {
            return false; // unknown flag
        }
        parsed++;
        }
    }
    if (parsed != hdr.entryCount) {
        DEBUG_ERR(SubSys::Archive, ErrCode::Archive_ReadFailed, "[DeltaCARC] Entry count mismatch: header %u, body %u",
                hdr.entryCount, parsed);
        return false;
    }

    printf("[DeltaCARC] Delta verified: v%u, %u entries\n", hdr.version, parsed);
    return true;
}

} // namespace Caesura::carc
