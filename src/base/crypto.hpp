#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace meme::base {

using Bytes = std::vector<uint8_t>;

// Heap-allocated secret string that overwrites its buffer via SecureZeroMemory before deallocation.
class SecureString {
    std::string storage_;
    void scrub() noexcept;
public:
    SecureString() = default;
    explicit SecureString(std::string_view text) : storage_(text) {}
    explicit SecureString(std::string&& text) noexcept : storage_(std::move(text)) {}
    ~SecureString() { scrub(); }

    SecureString(const SecureString& other) : storage_(other.storage_) {}
    SecureString& operator=(const SecureString& other);
    SecureString(SecureString&& other) noexcept;
    SecureString& operator=(SecureString&& other) noexcept;

    bool empty() const noexcept { return storage_.empty(); }
    size_t size() const noexcept { return storage_.size(); }
    std::string_view view() const noexcept { return storage_; }
    const char* c_str() const noexcept { return storage_.c_str(); }
    void clear() noexcept { scrub(); }
};

std::string sha256Hex(std::span<const uint8_t> data);
std::string sha256Hex(std::string_view text);

std::string base64Encode(std::span<const uint8_t> data);
Bytes base64Decode(std::string_view input);

std::string dpapiProtect(std::string_view plainUtf8, std::wstring_view description = L"MemeStudio Secret");
SecureString dpapiUnprotect(std::string_view cipherBase64);

SecureString readEnvSecret(std::string_view varNameUtf8);
std::string generateRandomHex(size_t byteCount = 16);

} // namespace meme::base
