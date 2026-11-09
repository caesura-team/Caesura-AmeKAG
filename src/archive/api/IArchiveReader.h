// IArchiveReader — pure virtual interface for archive reading
// Concrete: CARCReader. Pattern: module api/ directory.
#pragma once
#include <array>
#include <string>
#include <vector>
#include <cstdint>

namespace Caesura::carc {

// Raw Ed25519 verification key selected by the host.
using ArchivePublicKey = std::array<uint8_t, 32>;

class IArchiveReader {
public:
    virtual ~IArchiveReader() = default;

    // Pass filesystem paths in UTF-8. Validation, I/O and allocation failure
    // return false with closed state.
    virtual bool open(const std::string& path, const std::string& pubKeyPath = "") = 0;
    virtual bool open(const std::string& path, const ArchivePublicKey& expectedKey) = 0;
    virtual void close() = 0;
    // Failure, including allocation failure, returns empty; the reader can retry.
    virtual std::vector<uint8_t> readFile(const std::string& relativePath) = 0;
    virtual bool hasFile(const std::string& relativePath) const = 0;
    virtual size_t numFiles() const = 0;
    virtual bool isOpen() const = 0;
};

} // namespace Caesura::carc
