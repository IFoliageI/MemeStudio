#pragma once
#include <string>
#include <string_view>

namespace meme::base {

// Strict UTF-8 -> UTF-16 conversion for paths, configs, and identifiers.
// Throws meme::Error on malformed UTF-8 (never returns fake sentinel text).
std::wstring utf8ToWideStrict(std::string_view value);

// Lossy UTF-8 -> UTF-16 conversion for external subprocess logs and untrusted text.
// Replaces invalid byte sequences with U+FFFD instead of discarding the entire string.
std::wstring utf8ToWideLossy(std::string_view value);

// Strict UTF-16 -> UTF-8 conversion.
std::string wideToUtf8(std::wstring_view value);

// Converts Windows Active Code Page (ACP) error strings (such as std::filesystem_error::what())
// safely into valid UTF-8 so localized OS error messages are never lost.
std::string systemNarrowToUtf8(std::string_view narrowText);

std::string trimAscii(std::string_view text);

} // namespace meme::base
