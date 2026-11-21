#include "net/redactor.hpp"

namespace meme::net {

namespace {

std::string replaceSecretsInString(std::string value, std::span<const std::string> secrets) {
    for (const auto& secret : secrets) {
        if (secret.empty()) continue;
        size_t pos = 0;
        while ((pos = value.find(secret, pos)) != std::string::npos) {
            value.replace(pos, secret.size(), "[REDACTED]");
            pos += 10;
        }
    }
    return value;
}

size_t utf8SafeTruncateOffset(std::string_view s, size_t maxCodepoints, bool* truncatedOut) {
    size_t i = 0;
    size_t cpCount = 0;
    while (i < s.size() && cpCount < maxCodepoints) {
        unsigned char lead = static_cast<unsigned char>(s[i]);
        size_t charLen = 1;
        if ((lead & 0x80u) == 0) {
            charLen = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            charLen = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            charLen = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            charLen = 4;
        }
        if (i + charLen > s.size()) {
            break; // Incomplete trailing multi-byte sequence
        }
        i += charLen;
        ++cpCount;
    }
    if (truncatedOut) {
        *truncatedOut = (i < s.size());
    }
    return i;
}

} // namespace

std::string redactAndTruncateUtf8(
    std::string_view text,
    std::span<const std::string> secrets,
    size_t maxCodepoints) {
    std::string clean = replaceSecretsInString(std::string(text), secrets);
    bool wasTruncated = false;
    size_t cut = utf8SafeTruncateOffset(clean, maxCodepoints, &wasTruncated);
    if (wasTruncated) {
        clean.resize(cut);
        clean += "…";
    }
    return clean;
}

nlohmann::json redactJsonSecrets(
    const nlohmann::json& input,
    std::span<const std::string> secrets) {
    if (input.is_string()) {
        return replaceSecretsInString(input.get<std::string>(), secrets);
    }
    if (input.is_array()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& elem : input) {
            arr.push_back(redactJsonSecrets(elem, secrets));
        }
        return arr;
    }
    if (input.is_object()) {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& [k, v] : input.items()) {
            obj[k] = redactJsonSecrets(v, secrets);
        }
        return obj;
    }
    return input;
}

} // namespace meme::net
