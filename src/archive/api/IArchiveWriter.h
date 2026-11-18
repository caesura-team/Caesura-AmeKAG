// IArchiveWriter — pure virtual interface for archive creation
// Concrete: CARCWriter. Pattern: module api/ directory.
#pragma once
#include <string>
#include <cstdint>

namespace Caesura::carc {

class IArchiveWriter {
public:
    virtual ~IArchiveWriter() = default;

    // Pass filesystem paths in UTF-8.
    virtual bool create(const std::string& outputPath,
                        const std::string& privateKeyPath = "",
                        const std::string& publicKeyPath = "") = 0;
    virtual bool addFile(const std::string& relativePath,
                         const uint8_t* data, size_t size) = 0;
    virtual bool finalize() = 0;
};

} // namespace Caesura::carc
