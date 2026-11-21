#include "net/endpoint.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cwctype>

namespace meme::net {

bool isLoopbackHost(std::wstring_view host) noexcept {
    while (!host.empty() && host.back() == L'.') {
        host.remove_suffix(1);
    }
    if (host.empty()) return false;
    if (_wcsnicmp(host.data(), L"localhost", host.size()) == 0 && host.size() == 9) return true;
    if (host == L"::1" || host == L"[::1]") return true;
    // RFC 1122 127.0.0.0/8 loopback block check
    if (host.starts_with(L"127.")) {
        int dots = 0;
        int curOctet = -1;
        for (size_t i = 0; i < host.size(); ++i) {
            wchar_t c = host[i];
            if (c == L'.') {
                if (curOctet < 0 || curOctet > 255) return false;
                ++dots;
                curOctet = -1;
            } else if (c >= L'0' && c <= L'9') {
                curOctet = (curOctet < 0 ? 0 : curOctet * 10) + (c - L'0');
                if (curOctet > 255) return false;
            } else {
                return false;
            }
        }
        return dots == 3 && curOctet >= 0 && curOctet <= 255;
    }
    return false;
}

CrackedUrl parseAndValidateUrl(std::string_view urlUtf8, bool allowLoopbackHttp) {
    std::string trimmed = base::trimAscii(urlUtf8);
    if (trimmed.empty()) {
        throw Error("URL 不能为空");
    }
    if (trimmed.find_first_of("\r\n#") != std::string::npos) {
        throw Error("URL 不能包含控制字符或 # 片段");
    }
    std::wstring wideUrl = base::utf8ToWideStrict(trimmed);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(URL_COMPONENTS);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
        throw Error("无效的 HTTP/HTTPS URL 格式");
    }
    if (parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0) {
        throw Error("URL 不允许内嵌用户名或密码");
    }
    if (parts.dwHostNameLength == 0 || !parts.lpszHostName) {
        throw Error("URL 缺少主机名");
    }

    CrackedUrl out;
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.port = parts.nPort;
    out.secure = (parts.nScheme == INTERNET_SCHEME_HTTPS);
    out.isLoopback = isLoopbackHost(out.host);

    if (!out.secure) {
        if (parts.nScheme != INTERNET_SCHEME_HTTP || !allowLoopbackHttp || !out.isLoopback) {
            throw Error("远程接口与图像下载地址必须使用 HTTPS（仅本机回环地址允许 HTTP）");
        }
    }

    if (parts.dwUrlPathLength > 0 && parts.lpszUrlPath) {
        out.pathAndQuery.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    } else {
        out.pathAndQuery = L"/";
    }
    if (parts.dwExtraInfoLength > 0 && parts.lpszExtraInfo) {
        out.pathAndQuery.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    return out;
}

std::string resolveOpenAiUrl(std::string_view baseUrlUtf8, EndpointKind kind) {
    std::string trimmed = base::trimAscii(baseUrlUtf8);
    if (trimmed.empty()) {
        throw Error("请填写接口地址 (Base URL)");
    }
    if (trimmed.find_first_of("\r\n#") != std::string::npos) {
        throw Error("接口地址不能包含换行符或 # 片段");
    }

    std::string query;
    auto qPos = trimmed.find('?');
    if (qPos != std::string::npos) {
        query = trimmed.substr(qPos);
        trimmed.resize(qPos);
    }
    while (!trimmed.empty() && trimmed.back() == '/') {
        trimmed.pop_back();
    }

    auto stripSuffix = [&](std::string_view suffix) -> bool {
        if (trimmed.size() >= suffix.size() &&
            std::string_view(trimmed).substr(trimmed.size() - suffix.size()) == suffix) {
            trimmed.resize(trimmed.size() - suffix.size());
            return true;
        }
        return false;
    };

    // Normalize base prefix cleanly without string-chopping roundtrips
    if (!stripSuffix("/images/edits") && !stripSuffix("/images/generations")) {
        stripSuffix("/images");
        stripSuffix("/models");
    }

    std::string resolved = trimmed;
    switch (kind) {
        case EndpointKind::ImageEdits:       resolved += "/images/edits"; break;
        case EndpointKind::ImageGenerations: resolved += "/images/generations"; break;
        case EndpointKind::Models:           resolved += "/models"; break;
    }
    resolved += query;

    // Validate scheme & host security immediately
    (void)parseAndValidateUrl(resolved, true);
    return resolved;
}

void validateApiKeyFormat(std::string_view keyUtf8) {
    if (keyUtf8.empty()) {
        throw Error("请填写 API 密钥");
    }
    if (keyUtf8.size() > 4096) {
        throw Error("API 密钥长度超出限制");
    }
    for (unsigned char c : keyUtf8) {
        if (c < 33 || c > 126) {
            throw Error("API 密钥不能包含空格、控制字符或非 ASCII 字符");
        }
    }
}

} // namespace meme::net
