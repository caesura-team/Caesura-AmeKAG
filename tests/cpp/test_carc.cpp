// test_carc.cpp - CARC format, CryptoEngine, and archive I/O tests
#include "doctest.h"
#include "archive/CARCFormat.h"
#include "archive/CryptoEngine.h"
#include "archive/CARCWriter.h"
#include "archive/CARCReader.h"
#include "archive/CRLManager.h"
#include "TestPaths.h"
#include "PublisherArchives.h"
#include <cstring>
#include <cstdio>
#include <vector>
#include <fstream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>

using namespace Caesura::carc;

TEST_CASE("CARCFormat::constants") {
    CHECK(CARC_MAGIC == 0x43524143);
    CHECK(CARC_VERSION == 1);
    CHECK(sizeof(CARCHeader) == 64);
    CHECK(AES_KEY_SIZE == 32);
    CHECK(AES_NONCE_SIZE == 12);
    CHECK(AES_TAG_SIZE == 16);
}

TEST_CASE("CryptoEngine::AES-256-GCM round-trip") {
    uint8_t key[AES_KEY_SIZE], nonce[AES_NONCE_SIZE], tag[AES_TAG_SIZE];
    for (int i = 0; i < AES_KEY_SIZE; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < AES_NONCE_SIZE; i++) nonce[i] = (uint8_t)(i + 0x42);
    auto ct = CryptoEngine::encrypt((const uint8_t*)"Hello CARC!", 11, key, nonce, tag);
    REQUIRE_FALSE(ct.empty());
    auto pt = CryptoEngine::decrypt(ct.data(), ct.size(), key, nonce, tag);
    REQUIRE_FALSE(pt.empty());
    CHECK(memcmp(pt.data(), "Hello CARC!", 11) == 0);
}

TEST_CASE("CryptoEngine::bad key fails decrypt") {
    uint8_t key[AES_KEY_SIZE], bad[AES_KEY_SIZE];
    uint8_t nonce[AES_NONCE_SIZE], tag[AES_TAG_SIZE];
    for (int i = 0; i < AES_KEY_SIZE; i++) { key[i] = (uint8_t)i; bad[i] = (uint8_t)(i ^ 0xFF); }
    for (int i = 0; i < AES_NONCE_SIZE; i++) nonce[i] = (uint8_t)i;
    auto ct = CryptoEngine::encrypt((const uint8_t*)"test", 4, key, nonce, tag);
    auto pt = CryptoEngine::decrypt(ct.data(), ct.size(), bad, nonce, tag);
    CHECK(pt.empty());
}

TEST_CASE("CryptoEngine::generateKey entropy") {
    uint8_t k1[AES_KEY_SIZE], k2[AES_KEY_SIZE];
    CryptoEngine::generateKey(k1);
    CryptoEngine::generateKey(k2);
    CHECK(memcmp(k1, k2, AES_KEY_SIZE) != 0);
}

TEST_CASE("CryptoEngine::generateNonce entropy") {
    uint8_t n1[AES_NONCE_SIZE], n2[AES_NONCE_SIZE];
    CryptoEngine::generateNonce(n1);
    CryptoEngine::generateNonce(n2);
    CHECK(memcmp(n1, n2, AES_NONCE_SIZE) != 0);
}

TEST_CASE("CryptoEngine::keypair generation") {
    uint8_t pub[PUBLICKEY_SIZE], priv[64];
    CryptoEngine::generateKeyPair(pub, priv);
    bool ok = false;
    for (int i = 0; i < PUBLICKEY_SIZE; i++) if (pub[i] != 0) ok = true;
    CHECK(ok);
}

TEST_CASE("CryptoEngine::generateKeyPair with sign/verify round-trip") {
    uint8_t pub[PUBLICKEY_SIZE], priv[64];
    CryptoEngine::generateKeyPair(pub, priv);

    const char* msg = "test message for ed25519";
    size_t len = strlen(msg);

    uint8_t sig[SIGNATURE_SIZE];
    REQUIRE(CryptoEngine::sign((const uint8_t*)msg, len, priv, sig));

    bool ok = CryptoEngine::verify((const uint8_t*)msg, len, pub, sig);
    CHECK(ok);
}

TEST_CASE("CryptoEngine::SHA-256") {
    uint8_t hash[PATH_HASH_SIZE];
    // SHA-256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
    CryptoEngine::sha256((const uint8_t*)"hello", 5, hash);
    uint8_t expected[PATH_HASH_SIZE] = {
        0x2c,0xf2,0x4d,0xba,0x5f,0xb0,0xa3,0x0e,
        0x26,0xe8,0x3b,0x2a,0xc5,0xb9,0xe2,0x9e,
        0x1b,0x16,0x1e,0x5c,0x1f,0xa7,0x42,0x5e,
        0x73,0x04,0x33,0x62,0x93,0x8b,0x98,0x24
    };
    CHECK(memcmp(hash, expected, PATH_HASH_SIZE) == 0);
}

TEST_CASE("CRLManager::lifecycle") {
    CRLManager crl;
    CHECK_FALSE(crl.isRevoked("x"));
    CHECK(crl.revokedCount() == 0);
    CHECK(crl.mode() == CRLMode::Hybrid);
    crl.addRevoked("fp");
    CHECK(crl.isRevoked("fp"));
    CHECK(crl.revokedCount() == 1);
    crl.clear();
    CHECK(crl.revokedCount() == 0);
}

TEST_CASE("CARC container: write then read") {
    namespace fs = std::filesystem;
    fs::remove("test_carc.carc");
    {
        CARCWriter w;
        CHECK(w.create("test_carc.carc"));
        CHECK(w.addFile("data.txt", (const uint8_t*)"payload", 7));
        CHECK(w.finalize());
    }
    CHECK(fs::exists("test_carc.carc"));
    CHECK(fs::file_size("test_carc.carc") > 0);
    {
        CARCReader r;
        REQUIRE(r.open("test_carc.carc"));
        CHECK(r.numFiles() == 1);
        CHECK(r.hasFile("data.txt"));
        auto data = r.readFile("data.txt");
        REQUIRE_FALSE(data.empty());
        CHECK(data.size() == 7);
        CHECK(memcmp(data.data(), "payload", 7) == 0);
    }
    fs::remove("test_carc.carc");
}

TEST_CASE("CARC filesystem paths preserve UTF-8 archives and keys") {
    namespace fs = std::filesystem;
    Caesura::TestPaths::ScopedTempDir temp("carc_unicode");
    const auto utf8Path = [&](const char8_t* name) {
        const auto path = (temp.path() / fs::path(name)).u8string();
        return std::string(reinterpret_cast<const char*>(path.data()), path.size());
    };
    const auto archive = utf8Path(u8"剧情 𠮷.carc");
    const auto privateKey = utf8Path(u8"签名 𠮷.key");
    const auto publicKey = utf8Path(u8"发布 𠮷.pub");
    const std::vector<uint8_t> payload{0, 1, 42, 255};

    CARCWriter writer;
    REQUIRE(writer.create(archive, privateKey, publicKey));
    REQUIRE(writer.addFile("story.bin", payload.data(), payload.size()));
    REQUIRE(writer.finalize());
    CHECK(fs::is_regular_file(temp.path() / fs::path(u8"剧情 𠮷.carc")));
    CHECK(fs::is_regular_file(temp.path() / fs::path(u8"签名 𠮷.key")));
    CHECK(fs::is_regular_file(temp.path() / fs::path(u8"发布 𠮷.pub")));

    CARCReader reader;
    REQUIRE(reader.open(archive, publicKey));
    CHECK(reader.readFile("story.bin") == payload);

    uint8_t key[64];
    uint8_t signature[SIGNATURE_SIZE];
    ArchivePublicKey publisher{};
    REQUIRE(CryptoEngine::readPrivateKey(privateKey, key));
    REQUIRE(CryptoEngine::readPublicKey(publicKey, publisher.data()));
    REQUIRE(CryptoEngine::sign(payload.data(), payload.size(), key, signature));
    CHECK(CryptoEngine::verify(payload.data(), payload.size(), publisher.data(), signature));

    publisher[0] ^= 1;
    CHECK_FALSE(reader.open(archive, publisher));
    CHECK_FALSE(reader.isOpen());
    REQUIRE(reader.open(archive, publicKey));
    CHECK_FALSE(reader.open(archive + '\0' + ".ignored", publicKey));
    CHECK_FALSE(reader.isOpen());

#ifdef _WIN32
    // The historical Windows narrow spelling still works when the active
    // code page can represent the name losslessly.
    const auto nativeName = temp.path() / fs::path(u8"résumé.carc");
    std::string legacyName;
    try {
        const auto narrow = nativeName.string();
        if (fs::path(narrow) == nativeName) legacyName = narrow;
    } catch (const std::system_error&) {
        // This code page cannot represent the filename.
    }
    if (!legacyName.empty()) {
        fs::copy_file(temp.path() / fs::path(u8"剧情 𠮷.carc"), nativeName);
        REQUIRE(reader.open(legacyName, publicKey));
        CHECK(reader.readFile("story.bin") == payload);
    }
#endif
}

// =============================================================================
// Expanded tests: edge cases & robustness
// =============================================================================

TEST_CASE("CARCReader::open truncated file returns false") {
    namespace fs = std::filesystem;
    const char* path = "test_truncated.carc";
    fs::remove(path);

    // Write less than a full CARC header (64 bytes)
    {
        std::ofstream out(path, std::ios::binary);
        const char garbage[] = "not a real CARC file";
        out.write(garbage, sizeof(garbage) - 1);
    }

    CARCReader r;
    CHECK_FALSE(r.open(path));
    CHECK_FALSE(r.isOpen());

    fs::remove(path);
}

TEST_CASE("CARCReader::open corrupt magic bytes returns false") {
    namespace fs = std::filesystem;
    const char* path = "test_badmagic.carc";
    fs::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        std::vector<uint8_t> garbage(64, 0x00);
        out.write(reinterpret_cast<const char*>(garbage.data()), garbage.size());
    }

    CARCReader r;
    CHECK_FALSE(r.open(path));

    fs::remove(path);
}

TEST_CASE("CARCReader rejects signed index smaller than authentication tag") {
    namespace fs = std::filesystem;
    Caesura::TestPaths::ScopedTempDir temp("carc_short_index");
    const fs::path archivePath = temp.path() / "short-index.carc";
    const fs::path privateKeyPath = temp.path() / "short-index.key";
    const fs::path publicKeyPath = temp.path() / "short-index.pub";

    {
        CARCWriter writer;
        REQUIRE(writer.create(archivePath.string(), privateKeyPath.string(),
                              publicKeyPath.string()));
        REQUIRE(writer.addFile("data.txt", reinterpret_cast<const uint8_t*>("payload"), 7));
        REQUIRE(writer.finalize());
    }

    std::ifstream input(archivePath, std::ios::binary | std::ios::ate);
    REQUIRE(input.is_open());
    const auto byteCount = input.tellg();
    REQUIRE(byteCount > static_cast<std::streamoff>(sizeof(CARCHeader) +
                                                    SIGNATURE_SIZE + PUBLICKEY_SIZE));
    std::vector<uint8_t> bytes(static_cast<size_t>(byteCount));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), byteCount);
    input.close();

    CARCHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.indexSize = AES_TAG_SIZE - 1;
    std::memcpy(bytes.data(), &header, sizeof(header));

    uint8_t privateKey[64] = {};
    REQUIRE(CryptoEngine::readPrivateKey(privateKeyPath.string(), privateKey));
    uint8_t signature[SIGNATURE_SIZE] = {};
    const uint64_t signedSize = header.indexOffset + header.indexSize;
    REQUIRE(signedSize < bytes.size() - SIGNATURE_SIZE - PUBLICKEY_SIZE);
    REQUIRE(CryptoEngine::sign(bytes.data(), static_cast<size_t>(signedSize),
                               privateKey, signature));
    const size_t signatureOffset = bytes.size() - SIGNATURE_SIZE - PUBLICKEY_SIZE;
    std::memcpy(bytes.data() + signatureOffset, signature, SIGNATURE_SIZE);

    std::ofstream output(archivePath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();

    CARCReader reader;
    bool opened = false;
    CHECK_NOTHROW(opened = reader.open(archivePath.string()));
    CHECK_FALSE(opened);
}

TEST_CASE("CryptoEngine::encrypt with wrong key length returns empty") {
    CryptoEngine ce;
    uint8_t key[16] = {};
    uint8_t nonce[12] = {};
    uint8_t tag[16] = {};

    auto ct = ce.encrypt(
        (const uint8_t*)"test", 4,
        key, 16,
        nonce, 12,
        tag, 16);
    CHECK(ct.empty());
}

TEST_CASE("CryptoEngine::decrypt with wrong nonce length returns empty") {
    CryptoEngine ce;
    uint8_t key[32] = {};
    uint8_t nonce[8] = {};
    uint8_t tag[16] = {};

    uint8_t okNonce[12] = {}; uint8_t okTag[16] = {};
    auto ct = ce.encrypt((const uint8_t*)"test", 4, key, 32, okNonce, 12, okTag, 16);
    REQUIRE_FALSE(ct.empty());

    auto pt = ce.decrypt(ct.data(), ct.size(), key, 32, nonce, 8, okTag, 16);
    CHECK(pt.empty());
}

TEST_CASE("CryptoEngine::encrypt then decrypt empty data") {
    uint8_t key[32], nonce[12], tag[16];
    CryptoEngine::generateKey(key);
    CryptoEngine::generateNonce(nonce);

    auto ct = CryptoEngine::encrypt((const uint8_t*)"", 0, key, nonce, tag);
    // Encrypting 0 bytes must not crash. The output may be empty or
    // contain only the GCM tag — either is valid, but the call must succeed.

    auto pt = CryptoEngine::decrypt(ct.data(), ct.size(), key, nonce, tag);
    CHECK(pt.empty());
}

TEST_CASE("CARC container: concurrent open on same archive") {
    namespace fs = std::filesystem;
    const char* path = "test_concurrent.carc";
    fs::remove(path);

    {
        CARCWriter w;
        REQUIRE(w.create(path));
        REQUIRE(w.addFile("shared.txt", (const uint8_t*)"concurrent data", 15));
        REQUIRE(w.finalize());
    }

    // Diagnostic: does the file exist after writer goes out of scope?
    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 0);

    // Two threads each open their own reader on the same archive file.
    // Verifies that concurrent open() is thread-safe (no crash / no race).
    std::atomic<bool> ok1{false}, ok2{false};
    std::thread t1([&]() {
        CARCReader r;
        ok1.store(r.open(path) && r.numFiles() == 1 && r.hasFile("shared.txt"));
    });
    std::thread t2([&]() {
        CARCReader r;
        ok2.store(r.open(path) && r.numFiles() == 1 && r.hasFile("shared.txt"));
    });
    t1.join();
    t2.join();

    CHECK(ok1.load());
    CHECK(ok2.load());
    fs::remove(path);
}

// =============================================================================
// Completion tests: previously untested methods
// =============================================================================

TEST_CASE("CARCReader::readFileByHash returns correct data") {
    namespace fs = std::filesystem;
    const char* path = "test_byhash.carc";
    fs::remove(path);

    {
        CARCWriter w;
        REQUIRE(w.create(path));
        const char* name = "hash_test.txt";
        REQUIRE(w.addFile(name, (const uint8_t*)"by hash", 7));
        REQUIRE(w.finalize());
    }

    CARCReader r;
    REQUIRE(r.open(path));

    // Compute the path hash for "hash_test.txt"
    uint8_t ph[PATH_HASH_SIZE];
    CARCReader::hashPath("hash_test.txt", ph);

    CHECK(r.hasFileByHash(ph));

    auto data = r.readFileByHash(ph);
    REQUIRE_FALSE(data.empty());
    CHECK(data.size() == 7);
    CHECK(memcmp(data.data(), "by hash", 7) == 0);

    r.close();
    fs::remove(path);
}

TEST_CASE("CARCReader::readFile on nonexistent file returns empty") {
    namespace fs = std::filesystem;
    const char* path = "test_nofile.carc";
    fs::remove(path);

    {
        CARCWriter w;
        REQUIRE(w.create(path));
        REQUIRE(w.addFile("only.txt", (const uint8_t*)"hi", 2));
        REQUIRE(w.finalize());
    }

    CARCReader r;
    REQUIRE(r.open(path));
    CHECK_FALSE(r.hasFile("nonexistent.txt"));
    auto data = r.readFile("nonexistent.txt");
    CHECK(data.empty());

    r.close();
    fs::remove(path);
}

TEST_CASE("CARCReader::version and publicKey accessors") {
    namespace fs = std::filesystem;
    const char* path = "test_accessors.carc";
    fs::remove(path);

    {
        CARCWriter w;
        REQUIRE(w.create(path));
        REQUIRE(w.addFile("f.txt", (const uint8_t*)"x", 1));
        REQUIRE(w.finalize());
    }

    CARCReader r;
    REQUIRE(r.open(path));
    CHECK(r.version() == CARC_VERSION);
    CHECK(r.hasPublicKey());
    CHECK(r.publicKey() != nullptr);
    // fileList should contain one entry (hex hash string)
    CHECK(r.fileList().size() == 1);
    CHECK_FALSE(r.fileList()[0].empty());

    r.close();
    fs::remove(path);
}

TEST_CASE("CryptoEngine::writePublicKey + readPublicKey round-trip") {
    namespace fs = std::filesystem;
    const char* keyPath = "test_key.pub";
    fs::remove(keyPath);

    uint8_t pub[PUBLICKEY_SIZE], priv[64];
    CryptoEngine::generateKeyPair(pub, priv);

    CHECK(CryptoEngine::writePublicKey(keyPath, pub));
    CHECK(fs::exists(keyPath));

    uint8_t loaded[PUBLICKEY_SIZE] = {};
    CHECK(CryptoEngine::readPublicKey(keyPath, loaded));
    CHECK(memcmp(loaded, pub, PUBLICKEY_SIZE) == 0);

    fs::remove(keyPath);
}

TEST_CASE("CryptoEngine::writePrivateKey + readPrivateKey round-trip") {
    namespace fs = std::filesystem;
    const char* keyPath = "test_key.priv";
    fs::remove(keyPath);

    uint8_t pub[PUBLICKEY_SIZE], priv[64];
    CryptoEngine::generateKeyPair(pub, priv);

    CHECK(CryptoEngine::writePrivateKey(keyPath, priv));
    CHECK(fs::exists(keyPath));

    uint8_t loaded[64] = {};
    CHECK(CryptoEngine::readPrivateKey(keyPath, loaded));
    CHECK(memcmp(loaded, priv, 64) == 0);

    // Verify the loaded key still works for signing
    uint8_t sig[SIGNATURE_SIZE];
    REQUIRE(CryptoEngine::sign((const uint8_t*)"test", 4, loaded, sig));
    CHECK(CryptoEngine::verify((const uint8_t*)"test", 4, pub, sig));

    fs::remove(keyPath);
}

TEST_CASE("CARC container: create with keys + open with public key") {
    namespace fs = std::filesystem;
    const char* arcPath = "test_keyed.carc";
    const char* pubPath = "test_keyed.pub";
    const char* privPath = "test_keyed.priv";
    fs::remove(arcPath); fs::remove(pubPath); fs::remove(privPath);

    // Create archive with explicit key pair
    {
        CARCWriter w;
        REQUIRE(w.create(arcPath, privPath, pubPath));
        REQUIRE(w.addFile("secret.txt", (const uint8_t*)"classified", 10));
        REQUIRE(w.finalize());
    }
    CHECK(fs::exists(pubPath));
    CHECK(fs::exists(privPath));

    // Open using the public key file
    CARCReader r;
    REQUIRE(r.open(arcPath, pubPath));
    CHECK(r.numFiles() == 1);
    CHECK(r.hasFile("secret.txt"));

    auto data = r.readFile("secret.txt");
    REQUIRE_FALSE(data.empty());
    CHECK(data.size() == 10);
    CHECK(memcmp(data.data(), "classified", 10) == 0);

    r.close();
    // Opening without the public key should also work (trailing key in file)
    CARCReader r2;
    CHECK(r2.open(arcPath));
    CHECK(r2.numFiles() == 1);

    r2.close();
    fs::remove(arcPath); fs::remove(pubPath); fs::remove(privPath);
}

// =============================================================================
// Final completion: previously exempted methods (index, chain trust, setters)
// =============================================================================

TEST_CASE("CARCReader::index returns non-empty map after open") {
    namespace fs = std::filesystem;
    const char* path = "test_index.carc";
    fs::remove(path);

    {
        CARCWriter w;
        REQUIRE(w.create(path));
        REQUIRE(w.addFile("a.txt", (const uint8_t*)"aa", 2));
        REQUIRE(w.addFile("b.txt", (const uint8_t*)"bb", 2));
        REQUIRE(w.finalize());
    }

    CARCReader r;
    REQUIRE(r.open(path));
    CHECK(r.numFiles() == 2);

    const auto& idx = r.index();
    CHECK(idx.size() == 2);  // two entries in the hash map

    // Each entry should have non-zero offsets and key material
    for (const auto& [hash, info] : idx) {
        CHECK_FALSE(hash.empty());
        CHECK(info.compressedSize > 0);
        CHECK(info.originalSize > 0);
    }

    r.close();
    fs::remove(path);
}

TEST_CASE("CARCReader::setCRLManager and setRootPublicKey do not crash") {
    namespace fs = std::filesystem;
    const char* path = "test_setters.carc";
    fs::remove(path);

    {
        CARCWriter w;
        REQUIRE(w.create(path));
        REQUIRE(w.addFile("f.txt", (const uint8_t*)"x", 1));
        REQUIRE(w.finalize());
    }

    CARCReader r;
    REQUIRE(r.open(path));

    // Set root public key (32 bytes of any data)
    uint8_t rootKey[PUBLICKEY_SIZE] = {}, unused[64] = {};
    CryptoEngine::generateKeyPair(rootKey, unused);
    r.setRootPublicKey(rootKey);

    // Set CRL manager and test hybrid mode revocation
    CRLManager crl;
    r.setCRLManager(&crl);

    // verifyChainTrust with no root key should skip signature verification
    // but still fail because cert doesn't exist
    uint8_t dummyPub[PUBLICKEY_SIZE] = {};
    CHECK_FALSE(r.verifyChainTrust(dummyPub, "nonexistent_cert.json"));

    // nullptr childPublicKey returns false
    CHECK_FALSE(r.verifyChainTrust(nullptr, "cert.json"));

    // Empty certificate path returns false
    CHECK_FALSE(r.verifyChainTrust(dummyPub, ""));

    r.close();
    fs::remove(path);
}

TEST_CASE("CARCReader::verifyChainTrust happy path with root signature") {
    namespace fs = std::filesystem;
    const char* parentPath = "test_chain_parent.carc";
    fs::remove(parentPath);

    // Step A: generate root key pair (signs certificates)
    uint8_t rootPub[PUBLICKEY_SIZE], rootPriv[64];
    CryptoEngine::generateKeyPair(rootPub, rootPriv);

    // Step B: generate child key pair (simulates a child CARC's public key)
    uint8_t childPub[PUBLICKEY_SIZE], childPriv[64];
    CryptoEngine::generateKeyPair(childPub, childPriv);

    // Step C: compute SHA-256 fingerprint of child public key
    std::string childFingerprint = CRLManager::computeFingerprint(childPub, PUBLICKEY_SIZE);

    // Step D: build certificate JSON and sign with root key
    // The payload is everything before "signature" + "\n}"
    std::string certPayload =
        "{\n"
        "  \"child_pubkey_hash\": \"" + childFingerprint + "\",\n"
        "  \"expiry\": 9999999999,\n"
        "  \"permissions\": 1\n"
        "}";

    uint8_t certSig[SIGNATURE_SIZE];
    REQUIRE(CryptoEngine::sign(
        reinterpret_cast<const uint8_t*>(certPayload.data()), certPayload.size(),
        rootPriv, certSig));

    // Hex-encode the signature (128 hex chars)
    std::ostringstream sigHex;
    sigHex << std::hex << std::setfill('0');
    for (int i = 0; i < SIGNATURE_SIZE; ++i)
        sigHex << std::setw(2) << static_cast<int>(certSig[i]);

    // Step E: construct full certificate JSON with signature
    std::string certJson =
        "{\n"
        "  \"child_pubkey_hash\": \"" + childFingerprint + "\",\n"
        "  \"expiry\": 9999999999,\n"
        "  \"permissions\": 1,\n"
        "  \"signature\": \"" + sigHex.str() + "\"\n"
        "}";

    // Step F: create parent CARC containing the certificate as a file
    {
        CARCWriter w;
        REQUIRE(w.create(parentPath));
        REQUIRE(w.addFile("cert.json",
            reinterpret_cast<const uint8_t*>(certJson.data()), certJson.size()));
        REQUIRE(w.finalize());
    }

    // Step G: open parent, set root key, verify chain trust
    CARCReader parent;
    REQUIRE(parent.open(parentPath));
    parent.setRootPublicKey(rootPub);

    CHECK(parent.verifyChainTrust(childPub, "cert.json"));

    parent.close();
    fs::remove(parentPath);
}

TEST_CASE("CARCReader::verifyChainTrust fails with expired certificate") {
    namespace fs = std::filesystem;
    const char* path = "test_chain_expired.carc";
    fs::remove(path);

    uint8_t rootPub[PUBLICKEY_SIZE], rootPriv[64];
    CryptoEngine::generateKeyPair(rootPub, rootPriv);

    uint8_t childPub[PUBLICKEY_SIZE], childPriv[64];
    CryptoEngine::generateKeyPair(childPub, childPriv);
    std::string childFp = CRLManager::computeFingerprint(childPub, PUBLICKEY_SIZE);

    // Build certificate with expired timestamp (Jan 1 2020)
    std::string certPayload =
        "{\n"
        "  \"child_pubkey_hash\": \"" + childFp + "\",\n"
        "  \"expiry\": 1577836800,\n"
        "  \"permissions\": 1\n"
        "}";

    uint8_t sig[SIGNATURE_SIZE];
    REQUIRE(CryptoEngine::sign(
        reinterpret_cast<const uint8_t*>(certPayload.data()), certPayload.size(),
        rootPriv, sig));

    std::ostringstream sh;
    sh << std::hex << std::setfill('0');
    for (int i = 0; i < SIGNATURE_SIZE; ++i) sh << std::setw(2) << static_cast<int>(sig[i]);

    std::string certJson =
        "{\n"
        "  \"child_pubkey_hash\": \"" + childFp + "\",\n"
        "  \"expiry\": 1577836800,\n"
        "  \"permissions\": 1,\n"
        "  \"signature\": \"" + sh.str() + "\"\n"
        "}";

    {
        CARCWriter w;
        REQUIRE(w.create(path));
        REQUIRE(w.addFile("cert.json",
            reinterpret_cast<const uint8_t*>(certJson.data()), certJson.size()));
        REQUIRE(w.finalize());
    }

    CARCReader r;
    REQUIRE(r.open(path));
    r.setRootPublicKey(rootPub);

    // Certificate expired in 2020 -- chain trust should fail
    CHECK_FALSE(r.verifyChainTrust(childPub, "cert.json"));

    r.close();
    fs::remove(path);
}

TEST_CASE("CARCReader::verifyChainTrust fails with hash mismatch") {
    namespace fs = std::filesystem;
    const char* path = "test_chain_hash.carc";
    fs::remove(path);

    uint8_t rootPub[PUBLICKEY_SIZE], rootPriv[64];
    CryptoEngine::generateKeyPair(rootPub, rootPriv);

    uint8_t childPub[PUBLICKEY_SIZE], childPriv[64];
    CryptoEngine::generateKeyPair(childPub, childPriv);
    std::string childFp = CRLManager::computeFingerprint(childPub, PUBLICKEY_SIZE);

    // Certificate claims a DIFFERENT child pubkey hash
    uint8_t otherPub[PUBLICKEY_SIZE], otherPriv[64];
    CryptoEngine::generateKeyPair(otherPub, otherPriv);
    std::string otherFp = CRLManager::computeFingerprint(otherPub, PUBLICKEY_SIZE);

    std::string certPayload =
        "{\n"
        "  \"child_pubkey_hash\": \"" + otherFp + "\",\n"
        "  \"expiry\": 9999999999,\n"
        "  \"permissions\": 1\n"
        "}";

    uint8_t sig[SIGNATURE_SIZE];
    REQUIRE(CryptoEngine::sign(
        reinterpret_cast<const uint8_t*>(certPayload.data()), certPayload.size(),
        rootPriv, sig));

    std::ostringstream sh;
    sh << std::hex << std::setfill('0');
    for (int i = 0; i < SIGNATURE_SIZE; ++i) sh << std::setw(2) << static_cast<int>(sig[i]);

    std::string certJson =
        "{\n"
        "  \"child_pubkey_hash\": \"" + otherFp + "\",\n"
        "  \"expiry\": 9999999999,\n"
        "  \"permissions\": 1,\n"
        "  \"signature\": \"" + sh.str() + "\"\n"
        "}";

    {
        CARCWriter w;
        REQUIRE(w.create(path));
        REQUIRE(w.addFile("cert.json",
            reinterpret_cast<const uint8_t*>(certJson.data()), certJson.size()));
        REQUIRE(w.finalize());
    }

    CARCReader r;
    REQUIRE(r.open(path));
    r.setRootPublicKey(rootPub);

    // Hash in cert doesn't match child's actual pubkey
    CHECK_FALSE(r.verifyChainTrust(childPub, "cert.json"));

    r.close();
    fs::remove(path);
}

TEST_CASE("U8: reader pins the host publisher and retains path-key compatibility") {
    Caesura::Test::PublisherArchives files;
    CARCReader concrete;
    IArchiveReader& reader = concrete;
    REQUIRE(reader.open(files.trusted.string(), files.trustedKey));
    const auto payload = reader.readFile("u8_publisher_payload.txt");
    CHECK(std::string(payload.begin(), payload.end()) == "publisher A");
    CHECK_FALSE(reader.open(files.attacker.string(), files.trustedKey));
    CHECK_FALSE(reader.isOpen());
    CHECK(reader.numFiles() == 0);
    CHECK_FALSE(reader.hasFile("u8_publisher_payload.txt"));
    CHECK(reader.readFile("u8_publisher_payload.txt").empty());

    CHECK(reader.open(files.trusted.string(), files.trustedKeyPath.string()));
    CHECK_FALSE(reader.open(files.attacker.string(), files.trustedKeyPath.string()));
    CHECK_FALSE(reader.open(files.trusted.string(), (files.temp.path() / "missing.pub").string()));
    const auto badKey = files.temp.path() / "short.pub";
    { std::ofstream stream(badKey, std::ios::binary); stream << "short"; }
    CHECK_FALSE(reader.open(files.trusted.string(), badKey.string()));
    CHECK_FALSE(reader.isOpen());
}

TEST_CASE("U8: embedded and sidecar keys cannot grant publisher trust") {
    Caesura::Test::PublisherArchives files;
    const auto package = files.mount("patch.carc", false);
    REQUIRE(CryptoEngine::writePublicKey((files.root / "patch.pub").string(),
                                       files.attackerKey.data()));
    CARCReader reader;
    REQUIRE(reader.open(package.string())); // valid attacker self-signature
    CHECK_FALSE(reader.open(package.string(), files.trustedKey));
    // Even forging the trailing declaration of publisher A does not repair B's signature.
    Caesura::Test::PublisherArchives::replaceEmbeddedKey(package, files.trustedKey);
    CHECK_FALSE(reader.open(package.string(), files.trustedKey));
    CHECK_FALSE(reader.isOpen());
}

TEST_CASE("U8: pinned bytes remain authoritative when a declared or on-disk key changes") {
    Caesura::Test::PublisherArchives files;
    const auto package = files.mount("base.carc", true);
    Caesura::Test::PublisherArchives::replaceEmbeddedKey(package, files.attackerKey);
    REQUIRE(CryptoEngine::writePublicKey(files.trustedKeyPath.string(), files.attackerKey.data()));
    CARCReader reader;
    REQUIRE(reader.open(package.string(), files.trustedKey));
    const auto payload = reader.readFile("u8_publisher_payload.txt");
    CHECK(std::string(payload.begin(), payload.end()) == "publisher A");
    CHECK_FALSE(reader.open(package.string()));
    CHECK_FALSE(reader.open(package.string(), files.trustedKeyPath.string()));
}
