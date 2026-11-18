// ICryptoEngine — pure virtual interface for cryptographic operations
// Concrete: CryptoEngine. Does NOT expose CARCFormat.h constants.
// All fixed-size arrays are replaced with pointer+length to keep the
// interface self-contained and reusable outside the archive module.
#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace Caesura::carc {

class ICryptoEngine {
public:
    virtual ~ICryptoEngine() = default;

    // AES-256-GCM
    virtual std::vector<uint8_t> encrypt(
        const uint8_t* plaintext, size_t plaintextLen,
        const uint8_t* key, size_t keyLen,
        uint8_t* nonce, size_t nonceLen,
        uint8_t* tag, size_t tagLen) = 0;

    virtual std::vector<uint8_t> decrypt(
        const uint8_t* ciphertext, size_t ciphertextLen,
        const uint8_t* key, size_t keyLen,
        const uint8_t* nonce, size_t nonceLen,
        const uint8_t* tag, size_t tagLen) = 0;

    // SHA-256
    virtual void sha256(const uint8_t* data, size_t len, uint8_t* hash, size_t hashLen) = 0;

    // Ed25519
    virtual bool sign(const uint8_t* data, size_t len,
                      const uint8_t* privateKey, size_t privateKeyLen,
                      uint8_t* signature, size_t signatureLen) = 0;

    virtual bool verify(const uint8_t* data, size_t len,
                        const uint8_t* publicKey, size_t publicKeyLen,
                        const uint8_t* signature, size_t signatureLen) = 0;

    // Random generation
    virtual void generateKey(uint8_t* key, size_t keyLen) = 0;
    virtual void generateNonce(uint8_t* nonce, size_t nonceLen) = 0;

    // Key pair
    virtual void generateKeyPair(uint8_t* publicKey, size_t publicKeyLen,
                                 uint8_t* privateKey, size_t privateKeyLen) = 0;

    // Key file I/O. Pass filesystem paths in UTF-8.
    virtual bool readPublicKey(const std::string& path, uint8_t* key, size_t keyLen) = 0;
    virtual bool readPrivateKey(const std::string& path, uint8_t* key, size_t keyLen) = 0;
    virtual bool writePublicKey(const std::string& path, const uint8_t* key, size_t keyLen) = 0;
    virtual bool writePrivateKey(const std::string& path, const uint8_t* key, size_t keyLen) = 0;
};

} // namespace Caesura::carc
