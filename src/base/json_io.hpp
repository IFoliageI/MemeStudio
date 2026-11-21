#pragma once
#include "base/fs_io.hpp"
#include "../../vendor/nlohmann/json.hpp"

namespace meme::base {

nlohmann::json readJsonFile(const fs::path& path, size_t maxBytes = 4ull * 1024 * 1024);
void writeJsonFile(const fs::path& path, const nlohmann::json& data);

} // namespace meme::base
