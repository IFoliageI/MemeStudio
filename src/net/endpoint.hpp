#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace meme::net {

enum class EndpointKind {
    ImageEdits,
    ImageGenerations,
    Models
};

struct CrackedUrl {
    std::wstring host;
    std::wstring pathAndQuery;
    uint16_t port = 443;
    bool secure = true;
    bool isLoopback = false;
};

bool isLoopbackHost(std::wstring_view host) noexcept;
CrackedUrl parseAndValidateUrl(std::string_view urlUtf8, bool allowLoopbackHttp = true);
std::string resolveOpenAiUrl(std::string_view baseUrlUtf8, EndpointKind kind);
void validateApiKeyFormat(std::string_view keyUtf8);

} // namespace meme::net
