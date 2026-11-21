#include "base/utf8.hpp"
#include "base/error.hpp"
#include <windows.h>
#include <iomanip>
#include <limits>
#include <sstream>

namespace meme {

void checkHr(long hr, std::string_view operation) {
    if (hr < 0) {
        std::ostringstream out;
        out << operation << " 失败 (HRESULT 0x"
            << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
            << static_cast<uint32_t>(hr) << ")";
        throw Error(out.str());
    }
}

} // namespace meme

namespace meme::base {

std::wstring utf8ToWideStrict(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw Error("UTF-8 文本超出最大长度限制");
    }
    int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw Error("文本包含无效的 UTF-8 字节序列");
    }
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), count) <= 0) {
        throw Error("UTF-8 转 UTF-16 失败");
    }
    return result;
}

std::wstring utf8ToWideLossy(std::string_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        value = value.substr(0, static_cast<size_t>(std::numeric_limits<int>::max()));
    }
    int count = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        count = MultiByteToWideChar(
            CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0) return L"[无法解码的二进制输出]";
        std::wstring fallback(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), fallback.data(), count);
        return fallback;
    }
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string wideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw Error("UTF-16 文本超出最大长度限制");
    }
    int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw Error("文本包含无效的 UTF-16 代理对");
    }
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), count, nullptr, nullptr) <= 0) {
        throw Error("UTF-16 转 UTF-8 失败");
    }
    return result;
}

std::string systemNarrowToUtf8(std::string_view narrowText) {
    if (narrowText.empty()) return {};
    int utf8Check = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, narrowText.data(), static_cast<int>(narrowText.size()), nullptr, 0);
    if (utf8Check > 0) {
        return std::string(narrowText);
    }
    int wideLen = MultiByteToWideChar(
        CP_ACP, 0, narrowText.data(), static_cast<int>(narrowText.size()), nullptr, 0);
    if (wideLen <= 0) return "[未知系统错误]";
    std::wstring wideBuf(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, narrowText.data(), static_cast<int>(narrowText.size()), wideBuf.data(), wideLen);
    return wideToUtf8(wideBuf);
}

std::string trimAscii(std::string_view text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

} // namespace meme::base
