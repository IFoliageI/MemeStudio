#pragma once
#include "base/crypto.hpp"
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace meme::base {

namespace fs = std::filesystem;

Bytes readFileBytes(const fs::path& path, size_t maxBytes = 128ull * 1024 * 1024);
std::string readFileTailUtf8(const fs::path& path, size_t maxTailBytes = 24 * 1024);

// Writes to <path>.<random>.part and atomically replaces <path>.
// Automatically removes the .part file if writing, flushing, or renaming fails.
void writeFileAtomic(const fs::path& path, std::span<const uint8_t> data);
void writeTextAtomic(const fs::path& path, std::string_view utf8Text);

// Canonical case-insensitive & symlink/hardlink-aware path equivalence check on Windows.
bool isSameFilePath(const fs::path& a, const fs::path& b);
std::wstring canonicalPathKey(const fs::path& path);

} // namespace meme::base
