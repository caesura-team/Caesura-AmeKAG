// CARCReader implementation
// Phase 9: Added verifyChainTrust() for multi-CARC chain validation (spec [10.2.63])
#include "CARCReader.h"
#include "ArchivePath.h"
#include "CryptoEngine.h"
#include <zstd.h>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <limits>
#include <new>

namespace Caesura::carc {

namespace {
constexpr uint64_t kCarcTrailerSize = SIGNATURE_SIZE + PUBLICKEY_SIZE;
constexpr uint64_t kMaxCarcIndexSize = 64ull * 1024ull * 1024ull;
constexpr uint64_t kMaxCarcEntrySize = 512ull * 1024ull * 1024ull;
constexpr uint64_t kMaxCarcSignedSize = 1024ull * 1024ull * 1024ull;

bool checkedAdd(uint64_t left, uint64_t right, uint64_t& result) {
    if (left > std::numeric_limits<uint64_t>::max() - right) return false;
    result = left + right;
    return true;
}

// P2: one helper for every binary stream read: fail-closed on short reads
// (a truncated file must never leave partially-initialized fields behind).
bool checkedRead(std::istream& s, void* dst, std::streamsize size) {
    if (size < 0) return false;
    s.read(static_cast<char*>(dst), size);
    return static_cast<bool>(s);
}
}

// ==========================================================================
// open -- load and verify a CARC file
// ==========================================================================
bool CARCReader::open(const std::string& path, const std::string& pubKeyPath)
try
{
    if (pubKeyPath.empty()) return openWithExpectedKey(path, nullptr);
    ArchivePublicKey expectedKey{};
    if (!CryptoEngine::readPublicKey(pubKeyPath, expectedKey.data())) {
        close();
        return false;
    }
    return open(path, expectedKey);
}
catch (const std::bad_alloc&) {
    close();
    return false;
}

bool CARCReader::open(const std::string& path, const ArchivePublicKey& expectedKey)
{
    static_assert(std::tuple_size<ArchivePublicKey>::value == PUBLICKEY_SIZE);
    return openWithExpectedKey(path, expectedKey.data());
}

bool CARCReader::openWithExpectedKey(const std::string& path, const uint8_t* expectedKey)
try
{
    close();

    m_stream.open(pathFromUtf8(path), std::ios::binary);
    if (!m_stream.is_open()) return false;

    // 1. Read header
    m_stream.read(reinterpret_cast<char*>(&m_header), sizeof(CARCHeader));
    if (!m_stream || m_header.magic != CARC_MAGIC) {
        close();
        return false;
    }
    if (m_header.version != CARC_VERSION) {
        close();
        return false;
    }

    m_stream.seekg(0, std::ios::end);
    const std::streampos endPosition = m_stream.tellg();
    if (endPosition < 0) {
        close();
        return false;
    }
    const uint64_t fileSize = static_cast<uint64_t>(endPosition);
    uint64_t contentEnd = 0;
    uint64_t indexEnd = 0;
    uint64_t expectedPlainIndexSize = 0;
    uint64_t expectedEncryptedIndexSize = 0;
    const bool headerValid =
        fileSize >= sizeof(CARCHeader) + kCarcTrailerSize &&
        m_header.contentOffset == sizeof(CARCHeader) &&
        m_header.indexSize >= AES_TAG_SIZE &&
        m_header.indexSize <= kMaxCarcIndexSize &&
        checkedAdd(m_header.contentOffset, m_header.contentSize, contentEnd) &&
        contentEnd == m_header.indexOffset &&
        checkedAdd(m_header.indexOffset, m_header.indexSize, indexEnd) &&
        indexEnd <= kMaxCarcSignedSize &&
        indexEnd == fileSize - kCarcTrailerSize &&
        m_header.numFiles <= (kMaxCarcIndexSize - sizeof(uint32_t)) / sizeof(FileEntry) &&
        checkedAdd(sizeof(uint32_t),
                   static_cast<uint64_t>(m_header.numFiles) * sizeof(FileEntry),
                   expectedPlainIndexSize) &&
        checkedAdd(expectedPlainIndexSize, AES_TAG_SIZE, expectedEncryptedIndexSize) &&
        m_header.indexSize == expectedEncryptedIndexSize;
    if (!headerValid) {
        close();
        return false;
    }

    // 2. Select the verification key. Host bytes are copied once and never
    // replaced by archive metadata or a neighboring .pub file.
    if (expectedKey) {
        memcpy(m_publicKey, expectedKey, PUBLICKEY_SIZE);
    } else if (!readTrailingPublicKey()) {
        close();
        return false;
    }
    m_hasPublicKey = true;

    // 3. Verify signature
    if (!verifySignature()) {
        close();
        return false;
    }

    // 4. Decrypt index block
    std::vector<uint8_t> indexData;
    if (!decryptIndex(indexData)) {
        close();
        return false;
    }

    // 5. Parse index entries
    uint32_t count = 0;
    if (indexData.size() < sizeof(uint32_t)) {
        close();
        return false;
    }
    memcpy(&count, indexData.data(), sizeof(uint32_t));
    if (count != m_header.numFiles) {
        close();
        return false;
    }

    size_t expectedSize = sizeof(uint32_t) + static_cast<size_t>(count) * sizeof(FileEntry);
    if (indexData.size() != expectedSize) {
        close();
        return false;
    }

    // Use memcpy for alignment-safe reads (FileEntry contains uint64_t fields
    // but indexData may be only 4-byte aligned on the heap)
    const uint8_t* entryPtr = indexData.data() + sizeof(uint32_t);
    for (uint32_t i = 0; i < count; ++i) {
        FileEntry e;
        memcpy(&e, entryPtr + i * sizeof(FileEntry), sizeof(FileEntry));
        if (e.offset > m_header.contentSize ||
            e.compressedSize > m_header.contentSize - e.offset ||
            e.compressedSize > kMaxCarcEntrySize ||
            e.originalSize > kMaxCarcEntrySize) {
            close();
            return false;
        }
        std::string hexHash = pathHashToHex(e.pathHash);

        CarcFileInfo info;
        info.offset = e.offset;
        info.compressedSize = e.compressedSize;
        info.originalSize = e.originalSize;
        memcpy(info.aesKey, e.aesKey, AES_KEY_SIZE);
        memcpy(info.nonce, e.nonce, AES_NONCE_SIZE);
        memcpy(info.tag, e.tag, AES_TAG_SIZE);
        m_index[hexHash] = info;
        m_fileList.push_back(hexHash);
    }

    return true;
}
catch (const std::bad_alloc&) {
    // An unsuccessful open must not expose a partial index or an open stream.
    // Avoid allocating a diagnostic while the caller is already out of memory.
    close();
    return false;
}

void CARCReader::close()
{
    m_stream.close();
    m_index.clear();
    m_fileList.clear();
    m_hasPublicKey = false;
}

// ==========================================================================
// readFile -- decrypt + decompress one file by relative path
// ==========================================================================
std::vector<uint8_t> CARCReader::readFile(const std::string& relativePath)
{
    uint8_t hash[PATH_HASH_SIZE];
    CryptoEngine::sha256(reinterpret_cast<const uint8_t*>(relativePath.data()),
                         relativePath.size(), hash);
    return readFileByHash(hash);
}

std::vector<uint8_t> CARCReader::readFileByHash(const uint8_t pathHash[PATH_HASH_SIZE])
try
{
    if (!m_stream.is_open()) return {};

    std::string hexHash = pathHashToHex(pathHash);
    auto it = m_index.find(hexHash);
    if (it == m_index.end()) return {};

    const CarcFileInfo& info = it->second;

    // Read encrypted + compressed data from content block.
    // Mutex guards the shared ifstream across concurrent JobSystem workers.
    if (info.compressedSize > kMaxCarcEntrySize || info.originalSize > kMaxCarcEntrySize ||
        info.offset > m_header.contentSize ||
        info.compressedSize > m_header.contentSize - info.offset) {
        return {};
    }
    std::vector<uint8_t> encrypted(static_cast<size_t>(info.compressedSize));
    bool readSucceeded = false;
    {
        std::lock_guard<std::mutex> lock(m_streamMutex);
        m_stream.clear();
        m_stream.seekg(m_header.contentOffset + info.offset, std::ios::beg);
        if (!m_stream) return {};
        m_stream.read(reinterpret_cast<char*>(encrypted.data()),
                      static_cast<std::streamsize>(info.compressedSize));
        readSucceeded = static_cast<bool>(m_stream);
    }
    if (!readSucceeded) return {};

    // AES-GCM decrypt
    std::vector<uint8_t> compressed = CryptoEngine::decrypt(
        encrypted.data(), encrypted.size(), info.aesKey, info.nonce, info.tag);
    if (compressed.empty()) return {};

    if (compressed.size() == info.originalSize && info.compressedSize == info.originalSize) {
        return compressed;
    }
    size_t destLen = static_cast<size_t>(info.originalSize);
    std::vector<uint8_t> decompressed(destLen);
    size_t result = ZSTD_decompress(decompressed.data(), destLen,
                                    compressed.data(), compressed.size());
    if (ZSTD_isError(result)) {
        if (compressed.size() == info.originalSize) {
            return compressed;
        }
        return {};
    }
    if (result != destLen) return {};
    return decompressed;
}
catch (const std::bad_alloc&) {
    // Temporary buffers/stream locks unwind before returning. The verified
    // index remains usable, and the next read resets the stream position/state.
    return {};
}

// ==========================================================================
// hasFile -- existence check (no decrypt/decompress)
// ==========================================================================
bool CARCReader::hasFile(const std::string& relativePath) const
{
    uint8_t hash[PATH_HASH_SIZE];
    hashPath(relativePath, hash);
    return hasFileByHash(hash);
}

bool CARCReader::hasFileByHash(const uint8_t pathHash[PATH_HASH_SIZE]) const
{
    return m_index.find(pathHashToHex(pathHash)) != m_index.end();
}

// ==========================================================================
// hashPath -- SHA-256 of a path string
// ==========================================================================
void CARCReader::hashPath(const std::string& path, uint8_t out[PATH_HASH_SIZE])
{
    CryptoEngine::sha256(reinterpret_cast<const uint8_t*>(path.data()),
                         path.size(), out);
}

// ==========================================================================
// verifySignature -- Ed25519 verify over header + content + index blocks
// ==========================================================================
bool CARCReader::verifySignature()
{
    if (!m_hasPublicKey) return false;

    m_stream.seekg(-static_cast<int64_t>(PUBLICKEY_SIZE + SIGNATURE_SIZE), std::ios::end);
    if (!m_stream) return false;

    uint8_t signature[SIGNATURE_SIZE];
    m_stream.read(reinterpret_cast<char*>(signature), SIGNATURE_SIZE);
    if (!m_stream) return false;

    // Data covered: header + content + index
    uint64_t dataSize = 0;
    if (!checkedAdd(m_header.indexOffset, m_header.indexSize, dataSize) ||
        dataSize > kMaxCarcSignedSize || dataSize > std::numeric_limits<size_t>::max()) {
        return false;
    }
    m_stream.seekg(0, std::ios::beg);
    std::vector<uint8_t> signedData(static_cast<size_t>(dataSize));
    m_stream.read(reinterpret_cast<char*>(signedData.data()),
                  static_cast<std::streamsize>(dataSize));
    if (!m_stream) return false;

    return CryptoEngine::verify(signedData.data(), signedData.size(),
                                m_publicKey, signature);
}

// ==========================================================================
// decryptIndex -- read and decrypt the index block
// ==========================================================================
bool CARCReader::decryptIndex(std::vector<uint8_t>& outIndexData)
{
    if (m_header.indexSize < AES_TAG_SIZE || m_header.indexSize > kMaxCarcIndexSize) {
        return false;
    }
    m_stream.seekg(m_header.indexOffset, std::ios::beg);
    if (!m_stream) return false;

    uint8_t indexKey[AES_KEY_SIZE];
    uint8_t h[PATH_HASH_SIZE];
    CryptoEngine::sha256(m_publicKey, PUBLICKEY_SIZE, h);
    memcpy(indexKey, h, AES_KEY_SIZE);

    // Index nonce derivation MUST mirror CARCWriter (review A-5):
    // [version(4)] + [first 8 bytes of sha256(public key)] -- see writer.
    uint8_t indexNonce[AES_NONCE_SIZE] = {};
    memcpy(indexNonce, &m_header.version, sizeof(m_header.version));
    static_assert(PATH_HASH_SIZE >= AES_NONCE_SIZE - 4u, "hash too small");
    memcpy(indexNonce + 4, h, AES_NONCE_SIZE - 4u);

    uint8_t indexTag[AES_TAG_SIZE];
    uint64_t encryptedDataSize = m_header.indexSize - AES_TAG_SIZE;
    m_stream.seekg(m_header.indexOffset + encryptedDataSize, std::ios::beg);
    m_stream.read(reinterpret_cast<char*>(indexTag), AES_TAG_SIZE);

    m_stream.seekg(m_header.indexOffset, std::ios::beg);
    std::vector<uint8_t> encryptedIndex(static_cast<size_t>(encryptedDataSize));
    m_stream.read(reinterpret_cast<char*>(encryptedIndex.data()),
                  static_cast<std::streamsize>(encryptedDataSize));
    if (!m_stream) return false;

    outIndexData = CryptoEngine::decrypt(
        encryptedIndex.data(), encryptedIndex.size(),
        indexKey, indexNonce, indexTag);
    return !outIndexData.empty();
}

// ==========================================================================
// readTrailingPublicKey
// ==========================================================================
bool CARCReader::readTrailingPublicKey()
{
    m_stream.seekg(-static_cast<int64_t>(PUBLICKEY_SIZE), std::ios::end);
    if (!m_stream) return false;

    m_stream.read(reinterpret_cast<char*>(m_publicKey), PUBLICKEY_SIZE);
    return m_stream.gcount() == PUBLICKEY_SIZE;
}

// ==========================================================================
// pathHashToHex
// ==========================================================================
std::string CARCReader::pathHashToHex(const uint8_t hash[PATH_HASH_SIZE])
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < PATH_HASH_SIZE; ++i)
        oss << std::setw(2) << static_cast<int>(hash[i]);
    return oss.str();
}

// ==========================================================================
//  Phase 9: Chain Trust (spec [10.2.63])
// ==========================================================================

// ==========================================================================
//  setRootPublicKey
// ==========================================================================
void CARCReader::setRootPublicKey(const uint8_t key[PUBLICKEY_SIZE])
{
    memcpy(m_rootPublicKey, key, PUBLICKEY_SIZE);
    m_hasRootKey = true;
}

// ==========================================================================
//  parseCertificate -- parse JSON certificate from raw data
//  Expected format:
//  {
//    "child_pubkey_hash": "hex_sha256...",
//    "expiry": 1717632000,
//    "permissions": 1,
//    "signature": "hex_ed25519..."
//  }
// ==========================================================================
bool CARCReader::parseCertificate(const std::string& json, CarcCertificate& cert) const
{
    if (json.empty()) return false;

    // Simple JSON extraction helpers (inline -- no external lib)
    auto findString = [](const std::string& s, const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        size_t pos = s.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        while (pos < s.size() && s[pos] != ':') pos++;
        pos++; // skip ':'
        while (pos < s.size() && s[pos] != '"') pos++;
        if (pos >= s.size()) return "";
        pos++; // skip opening quote
        size_t end = pos;
        while (end < s.size() && s[end] != '"') {
            if (s[end] == '\\') end++; // skip escaped
            end++;
        }
        std::string raw = s.substr(pos, end - pos);
        // Unescape simple escapes
        std::string result;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                char c = raw[++i];
                if (c == '"') result += '"';
                else if (c == '\\') result += '\\';
                else if (c == 'n') result += '\n';
                else { result += '\\'; result += c; }
            } else {
                result += raw[i];
            }
        }
        return result;
    };

    auto findInt = [](const std::string& s, const std::string& key) -> int64_t {
        std::string search = "\"" + key + "\"";
        size_t pos = s.find(search);
        if (pos == std::string::npos) return 0;
        pos += search.size();
        while (pos < s.size() && s[pos] != ':') pos++;
        pos++; // skip ':'
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) pos++;
        std::string num;
        while (pos < s.size() && (isdigit(s[pos]) || s[pos] == '-')) {
            num += s[pos];
            pos++;
        }
        if (num.empty()) return 0;
        try {
            return std::stoll(num);
        } catch (const std::exception&) {
            return 0;  // malformed cert payload: treat field as absent
        }
    };

    cert.childPubKeyHash = findString(json, "child_pubkey_hash");
    cert.expiry          = findInt(json, "expiry");
    cert.permissions     = static_cast<uint32_t>(findInt(json, "permissions"));
    cert.signature       = findString(json, "signature");

    return !cert.childPubKeyHash.empty() && !cert.signature.empty();
}

// ==========================================================================
//  verifyChainTrust -- full 7-step chain verification per spec [10.2.63]
// ==========================================================================
bool CARCReader::verifyChainTrust(const uint8_t* childPublicKey,
                                   const std::string& certificatePath)
{
    if (!childPublicKey || certificatePath.empty()) return false;

    // Step 1: Compute SHA-256 hash of child's trailing 32B public key
    std::string childPubKeyHash = CRLManager::computeFingerprint(childPublicKey, PUBLICKEY_SIZE);

    // Step 2: Read certificate JSON from this (parent) CARC at certificatePath
    std::vector<uint8_t> certData = readFile(certificatePath);
    if (certData.empty()) {
        // Cert not found in archive -- chain trust fails
        return false;
    }
    std::string certJson(reinterpret_cast<const char*>(certData.data()), certData.size());

    // Step 3 & 3a: Parse certificate + verify signature with root Ed25519 public key
    CarcCertificate cert;
    if (!parseCertificate(certJson, cert)) return false;

    // Verify certificate signature if root key is available
    if (m_hasRootKey) {
        // Build the signed payload: everything except the signature field
        size_t sigPos = certJson.find("\"signature\"");
        if (sigPos == std::string::npos) return false;

        // Extract payload: trim trailing signature field + comma
        size_t payloadEnd = sigPos;
        while (payloadEnd > 0 && certJson[payloadEnd] != ',' && certJson[payloadEnd] != '{') payloadEnd--;
        std::string payload = certJson.substr(0, payloadEnd);
        while (!payload.empty() && (payload.back() == ' ' || payload.back() == '\t' ||
               payload.back() == '\n' || payload.back() == '\r' || payload.back() == ',')) {
            payload.pop_back();
        }
        payload += "\n}";

        // Decode signature hex -> binary
        const std::string& sigHex = cert.signature;
        if (sigHex.size() != 128) return false; // Ed25519 sig = 64 bytes = 128 hex chars

        uint8_t signature[64];
        for (size_t i = 0; i < 64; ++i) {
            char hex[3] = { sigHex[i * 2], sigHex[i * 2 + 1], 0 };
            signature[i] = static_cast<uint8_t>(strtoul(hex, nullptr, 16));
        }

        if (!CryptoEngine::verify(
                reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size(),
                m_rootPublicKey,
                signature)) {
            return false; // Signature invalid
        }
    }

    // Step 4: SHA-256 compare cert's pubkey hash vs child's trailing pubkey hash
    if (cert.childPubKeyHash != childPubKeyHash) {
        return false; // Hash mismatch -- child CARC not authorized by this cert
    }

    // Step 5: Verify child CARC signature with child pubkey
    // (This is done in child CARC's own open() call -- we have already
    // verified that the hash matches, so the child will verify itself)

    // Step 6: Check certificate expiry
    auto now = static_cast<int64_t>(std::time(nullptr));
    if (cert.expiry > 0 && now > cert.expiry) {
        return false; // Certificate expired
    }

    // Step 7: Check CRL revocation list
    if (m_crlManager) {
        // Check if the certificate's pubkey hash is in the revocation list
        if (!m_crlManager->verify(cert.childPubKeyHash, m_crlManager->mode())) {
            return false; // Certificate revoked
        }
    }

    return true;
}

} // namespace Caesura::carc
