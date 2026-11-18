#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace Caesura::carc_tool {

bool validMemberPath(const std::filesystem::path& path);
bool prepareOutputRoot(std::filesystem::path& root);
bool writeExtractedFile(const std::filesystem::path& root,
                        const std::filesystem::path& member,
                        std::span<const uint8_t> bytes);

} // namespace Caesura::carc_tool
