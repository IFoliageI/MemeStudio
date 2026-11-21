#pragma once
#include <span>
#include <string>
#include <string_view>
#include "../../vendor/nlohmann/json.hpp"

namespace meme::net {

// Replaces every occurrence of non-empty secrets in plain text, then truncates cleanly
// on a valid UTF-8 codepoint boundary (never slicing multi-byte characters in half).
std::string redactAndTruncateUtf8(
    std::string_view text,
    std::span<const std::string> secrets,
    size_t maxCodepoints = 900);

// Recursively traverses a JSON value and redacts secrets ONLY inside string values,
// guaranteeing JSON keys and structural syntax are never corrupted even for short keys.
nlohmann::json redactJsonSecrets(
    const nlohmann::json& input,
    std::span<const std::string> secrets);

} // namespace meme::net
