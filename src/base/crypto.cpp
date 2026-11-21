#include "base/crypto.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <iomanip>
#include <limits>
#include <sstream>

namespace meme::base {

void SecureString::scrub() noexcept {
    if (!storage_.empty()) {
        SecureZeroMemory(storage_.data(), storage_.size());
        storage_.clear();
    }
}

SecureString& SecureString::operator=(const SecureString& other) {
    if (this != &other) {
        scrub();
        storage_ = other.storage_;
    }
    return *this;
}

SecureString::SecureString(SecureString&& other) noexcept : storage_(std::move(other.storage_)) {
    other.scrub();
}

SecureString& SecureString::operator=(SecureString&& other) noexcept {
    if (this != &other) {
        scrub();
        storage_ = std::move(other.storage_);
        other.scrub();
    }
    return *this;
}

std::string sha256Hex(std::span<const uint8_t> data) {
    if (data.size() > static_cast<size_t>(std::numeric_limits<ULONG>::max())) {
        throw Error("SHA-256 输入超出最大字节限制");
    }
    uint8_t digest[32]{};
    // Win10+ pseudo-handle avoids repeated BCryptOpenAlgorithmProvider / Close overhead.
    NTSTATUS status = BCryptHash(
        BCRYPT_SHA256_ALG_HANDLE,
        nullptr,
        0,
        const_cast<PUCHAR>(data.empty() ? reinterpret_cast<const uint8_t*>("") : data.data()),
        static_cast<ULONG>(data.size()),
        digest,
        sizeof(digest));
    if (status < 0) {
        throw Error("计算 SHA-256 摘要失败");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '\0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    return out;
}

std::string sha256Hex(std::string_view text) {
    return sha256Hex(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

std::string base64Encode(std::span<const uint8_t> data) {
    if (data.empty()) return {};
    if (data.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        throw Error("Base64 编码输入过大");
    }
    DWORD size = 0;
    if (!CryptBinaryToStringA(
            data.data(), static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size) || size == 0) {
        throw Error("Base64 长度计算失败");
    }
    std::string result(size, '\0');
    if (!CryptBinaryToStringA(
            data.data(), static_cast<DWORD>(data.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &size)) {
        throw Error("Base64 编码失败");
    }
    result.resize(size);
    while (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

Bytes base64Decode(std::string_view input) {
    // Zero-copy strip of optional data:*;base64, prefix
    if (input.starts_with("data:")) {
        auto comma = input.find(',');
        if (comma == std::string_view::npos) {
            throw Error("Data URL 格式无效（缺少逗号分隔符）");
        }
        input.remove_prefix(comma + 1);
    }
    input = std::string_view(input.data(), input.size());
    while (!input.empty() && (input.front() == ' ' || input.front() == '\r' || input.front() == '\n' || input.front() == '\t')) {
        input.remove_prefix(1);
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\r' || input.back() == '\n' || input.back() == '\t')) {
        input.remove_suffix(1);
    }
    if (input.empty()) return {};
    if (input.size() > 180ull * 1024 * 1024) {
        throw Error("Base64 数据超出 180 MB 限制");
    }
    DWORD size = 0;
    if (!CryptStringToBinaryA(
            input.data(), static_cast<DWORD>(input.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &size, nullptr, nullptr)) {
        throw Error("无效的 Base64 数据");
    }
    Bytes out(size);
    if (!CryptStringToBinaryA(
            input.data(), static_cast<DWORD>(input.size()),
            CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, out.data(), &size, nullptr, nullptr)) {
        throw Error("Base64 解码失败");
    }
    out.resize(size);
    return out;
}

std::string dpapiProtect(std::string_view plainUtf8, std::wstring_view description) {
    if (plainUtf8.empty()) return {};
    std::wstring desc(description);
    DATA_BLOB input{
        static_cast<DWORD>(plainUtf8.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(plainUtf8.data()))
    };
    DATA_BLOB output{};
    if (!CryptProtectData(&input, desc.c_str(), nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw Error("Windows DPAPI 加密失败");
    }
    Bytes cipher(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return base64Encode(cipher);
}

SecureString dpapiUnprotect(std::string_view cipherBase64) {
    if (cipherBase64.empty()) return {};
    Bytes cipher = base64Decode(cipherBase64);
    if (cipher.empty()) return {};
    DATA_BLOB input{static_cast<DWORD>(cipher.size()), cipher.data()};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw Error("无法解密已保存的密钥（当前 Windows 用户不匹配或数据已损坏）");
    }
    std::string temp(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    // Validate UTF-8 integrity without creating un-scrubbed copies
    (void)utf8ToWideStrict(temp);
    return SecureString(std::move(temp));
}

SecureString readEnvSecret(std::string_view varNameUtf8) {
    if (varNameUtf8.empty()) return {};
    std::wstring wideName = utf8ToWideStrict(varNameUtf8);
    DWORD required = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (required == 0) return {};
    std::wstring wideVal(required, L'\0');
    DWORD written = GetEnvironmentVariableW(wideName.c_str(), wideVal.data(), required);
    if (written == 0 || written >= required) {
        SecureZeroMemory(wideVal.data(), wideVal.size() * sizeof(wchar_t));
        return {};
    }
    wideVal.resize(written);
    std::string utf8Val = wideToUtf8(wideVal);
    SecureZeroMemory(wideVal.data(), wideVal.size() * sizeof(wchar_t));
    return SecureString(std::move(utf8Val));
}

std::string generateRandomHex(size_t byteCount) {
    if (byteCount == 0 || byteCount > 1024) {
        throw Error("随机 ID 长度超出允许范围");
    }
    Bytes buf(byteCount);
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw Error("系统随机数生成失败");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(byteCount * 2, '\0');
    for (size_t i = 0; i < byteCount; ++i) {
        out[i * 2]     = kHex[(buf[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[buf[i] & 0x0F];
    }
    return out;
}

} // namespace meme::base
