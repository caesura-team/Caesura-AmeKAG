// CARCWriter implementation
#include "CARCWriter.h"
#include "ArchivePath.h"
#include <zstd.h>
#include "CryptoEngine.h"
#include <cstring>
#include <algorithm>

namespace Caesura::carc {

bool CARCWriter::create(const std::string& outputPath,
                        const std::string& privateKeyPath,
                        const std::string& publicKeyPath)
{
    const auto nativePath = pathFromUtf8(outputPath);
    if (nativePath.empty()) return false;
    m_outputPath = outputPath;
    m_publicKeyPath = publicKeyPath;
    m_pendingFiles.clear();
    m_entries.clear();
    m_contentBlock.clear();

    CryptoEngine::generateKeyPair(m_publicKey, m_privateKey);

    if (!privateKeyPath.empty()) {
        if (!CryptoEngine::writePrivateKey(privateKeyPath, m_privateKey))
            return false;
    }
    if (!publicKeyPath.empty()) {
        if (!CryptoEngine::writePublicKey(publicKeyPath, m_publicKey))
            return false;
    }

    m_output.open(nativePath, std::ios::binary);
    return m_output.is_open();
}

bool CARCWriter::addFile(const std::string& relativePath,
                         const uint8_t* data, size_t size)
{
    if (!m_output.is_open()) return false;

    uint8_t pathHash[PATH_HASH_SIZE];
    CryptoEngine::sha256(reinterpret_cast<const uint8_t*>(relativePath.data()),
                         relativePath.size(), pathHash);

    // Deduplicate by path hash: adding an already-present relative path
    // updates the existing pending entry (last write wins), matching the
    // reader's hash-keyed index semantics. This keeps the index sized to the
    // number of unique paths rather than the number of addFile() calls.
    for (auto& pf : m_pendingFiles) {
        if (std::memcmp(pf.pathHash, pathHash, PATH_HASH_SIZE) == 0) {
            pf.relativePath = relativePath;
            pf.originalSize = size;
            if (size > 0) pf.data.assign(data, data + size);
            else pf.data.clear();
            return true;
        }
    }

    PendingFile pf;
    pf.relativePath = relativePath;
    pf.originalSize = size;
    if (size > 0) pf.data.assign(data, data + size);
    memcpy(pf.pathHash, pathHash, PATH_HASH_SIZE);
    m_pendingFiles.push_back(std::move(pf));
    return true;
}

bool CARCWriter::addFileByHash(const uint8_t pathHash[PATH_HASH_SIZE],
                               const uint8_t* data, size_t size)
{
    if (!m_output.is_open()) return false;

    // Deduplicate by path hash (same idempotent "update existing" semantics
    // as addFile): a path hash already pending gets its content replaced.
    for (auto& pf : m_pendingFiles) {
        if (std::memcmp(pf.pathHash, pathHash, PATH_HASH_SIZE) == 0) {
            pf.relativePath.clear();
            pf.originalSize = size;
            if (size > 0) pf.data.assign(data, data + size);
            else pf.data.clear();
            return true;
        }
    }

    PendingFile pf;
    pf.relativePath.clear();
    pf.originalSize = size;
    if (size > 0) pf.data.assign(data, data + size);
    memcpy(pf.pathHash, pathHash, PATH_HASH_SIZE);
    m_pendingFiles.push_back(std::move(pf));
    return true;
}

bool CARCWriter::finalize()
{
    if (!m_output.is_open()) return false;
    if (m_pendingFiles.empty()) {
        m_output.close();
        return false;
    }

    uint64_t contentPos = 0;

    for (auto& pf : m_pendingFiles) {
        FileEntry entry = {};
        memcpy(entry.pathHash, pf.pathHash, PATH_HASH_SIZE);
        entry.originalSize = pf.originalSize;

        CryptoEngine::generateKey(entry.aesKey);
        CryptoEngine::generateNonce(entry.nonce);

        size_t bound = ZSTD_compressBound(pf.originalSize);
        std::vector<uint8_t> compressed(bound);

        size_t actual = ZSTD_compress(compressed.data(), bound,
                                      pf.data.data(), pf.originalSize, ZSTD_CLEVEL_DEFAULT);
        if (ZSTD_isError(actual)) {
            compressed.assign(pf.data.begin(), pf.data.end());
            actual = pf.originalSize;
        }
        compressed.resize(actual);

        std::vector<uint8_t> encrypted = CryptoEngine::encrypt(
            compressed.data(), compressed.size(),
            entry.aesKey, entry.nonce, entry.tag);
        if (encrypted.empty()) return false;

        entry.compressedSize = encrypted.size();
        entry.offset = contentPos;

        m_contentBlock.insert(m_contentBlock.end(), encrypted.begin(), encrypted.end());
        contentPos += encrypted.size();
        m_entries.push_back(entry);
    }

    // Build index
    uint32_t count = static_cast<uint32_t>(m_entries.size());
    size_t indexPlainSize = sizeof(uint32_t) + count * sizeof(FileEntry);
    std::vector<uint8_t> indexPlain(indexPlainSize);
    memcpy(indexPlain.data(), &count, sizeof(uint32_t));
    memcpy(indexPlain.data() + sizeof(uint32_t), m_entries.data(),
           count * sizeof(FileEntry));

    uint8_t indexKey[AES_KEY_SIZE];
    uint8_t h[PATH_HASH_SIZE];
    CryptoEngine::sha256(m_publicKey, PUBLICKEY_SIZE, h);
    memcpy(indexKey, h, AES_KEY_SIZE);

    // Index nonce (review A-5): [version(4)] + [first 8 bytes of sha256(public key)].
    // Previously the nonce was version-on-zero-padding, so any two archives
    // signed with the same public key that had an identical index plaintext
    // would reuse the AES-GCM nonce+key pair (a confidentiality break on the
    // encryption). Binding the public-key hash into the nonce makes it unique
    // per signing key while staying deterministic per archive (reader derives
    // the same value from the embedded public key). 12 bytes = AES_NONCE_SIZE.
    uint8_t indexNonce[AES_NONCE_SIZE] = {};
    uint32_t version = CARC_VERSION;
    memcpy(indexNonce, &version, sizeof(version));
    static_assert(PATH_HASH_SIZE >= AES_NONCE_SIZE - 4u, "hash too small");
    memcpy(indexNonce + 4, h, AES_NONCE_SIZE - 4u);

    uint8_t indexTag[AES_TAG_SIZE];
    std::vector<uint8_t> encryptedIndex = CryptoEngine::encrypt(
        indexPlain.data(), indexPlain.size(), indexKey, indexNonce, indexTag);
    if (encryptedIndex.empty()) return false;

    uint64_t indexSize = encryptedIndex.size() + AES_TAG_SIZE;

    // Build header
    CARCHeader header = {};
    header.magic = CARC_MAGIC;
    header.version = CARC_VERSION;
    header.contentOffset = sizeof(CARCHeader);
    header.contentSize = m_contentBlock.size();
    header.indexOffset = header.contentOffset + header.contentSize;
    header.indexSize = indexSize;
    header.numFiles = count;

    // Write
    m_output.write(reinterpret_cast<const char*>(&header), sizeof(CARCHeader));
    m_output.write(reinterpret_cast<const char*>(m_contentBlock.data()),
                   m_contentBlock.size());
    m_output.write(reinterpret_cast<const char*>(encryptedIndex.data()),
                   encryptedIndex.size());
    m_output.write(reinterpret_cast<const char*>(indexTag), AES_TAG_SIZE);
    m_output.close();

    // Read back for signing
    const auto nativePath = pathFromUtf8(m_outputPath);
    std::ifstream inFile(nativePath, std::ios::binary);
    if (!inFile.is_open()) return false;

    uint64_t signedSize = sizeof(CARCHeader) + m_contentBlock.size() + indexSize;
    std::vector<uint8_t> signedData(static_cast<size_t>(signedSize));
    inFile.read(reinterpret_cast<char*>(signedData.data()), signedSize);
    if (!inFile) { inFile.close(); return false; }
    inFile.close();

    uint8_t signature[SIGNATURE_SIZE];
    memset(signature, 0, SIGNATURE_SIZE);
    if (!CryptoEngine::sign(signedData.data(), signedData.size(),
                           m_privateKey, signature)) {
        return false;
    }

    // Append signature + public key
    m_output.open(nativePath, std::ios::binary | std::ios::app);
    if (!m_output.is_open()) return false;
    m_output.write(reinterpret_cast<const char*>(signature), SIGNATURE_SIZE);
    m_output.write(reinterpret_cast<const char*>(m_publicKey), PUBLICKEY_SIZE);
    m_output.close();

    if (!m_publicKeyPath.empty()) {
        CryptoEngine::writePublicKey(m_publicKeyPath, m_publicKey);
    }

    return true;
}

} // namespace Caesura::carc
