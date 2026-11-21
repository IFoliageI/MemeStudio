#include "base/fs_io.hpp"
#include "base/json_io.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"
#include <windows.h>
#include <cwctype>
#include <fstream>

namespace meme::base {

Bytes readFileBytes(const fs::path& path, size_t maxBytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw Error("无法打开文件：" + wideToUtf8(path.wstring()));
    }
    auto endPos = file.tellg();
    if (endPos < 0 || static_cast<uint64_t>(endPos) > maxBytes) {
        throw Error("文件大小超出限制：" + wideToUtf8(path.wstring()));
    }
    Bytes data(static_cast<size_t>(endPos));
    file.seekg(0, std::ios::beg);
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        throw Error("读取文件失败：" + wideToUtf8(path.wstring()));
    }
    return data;
}

std::string readFileTailUtf8(const fs::path& path, size_t maxTailBytes) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    auto total = file.tellg();
    if (total <= 0) return {};
    uint64_t len = static_cast<uint64_t>(total);
    uint64_t readLen = len > maxTailBytes ? maxTailBytes : len;
    file.seekg(static_cast<std::streamoff>(len - readLen), std::ios::beg);
    std::string raw(static_cast<size_t>(readLen), '\0');
    file.read(raw.data(), static_cast<std::streamsize>(readLen));
    raw.resize(static_cast<size_t>(file.gcount()));
    return wideToUtf8(utf8ToWideLossy(raw));
}

void writeFileAtomic(const fs::path& path, std::span<const uint8_t> data) {
    try {
        if (!path.parent_path().empty()) {
            fs::create_directories(path.parent_path());
        }
    } catch (const fs::filesystem_error& e) {
        throw Error("创建输出目录失败：" + systemNarrowToUtf8(e.what()));
    }

    fs::path tempPath = path;
    tempPath += L"." + utf8ToWideStrict(generateRandomHex(6)) + L".part";

    struct TempCleanupGuard {
        fs::path target;
        bool committed = false;
        ~TempCleanupGuard() {
            if (!committed) {
                std::error_code ec;
                fs::remove(target, ec);
            }
        }
    } guard{tempPath};

    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw Error("无法写入临时文件：" + wideToUtf8(path.wstring()));
        }
        if (!data.empty() && !out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
            throw Error("写入文件数据失败：" + wideToUtf8(path.wstring()));
        }
        out.flush();
        if (!out) {
            throw Error("刷新文件缓存失败：" + wideToUtf8(path.wstring()));
        }
    }

    if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        throw Error("原子替换目标文件失败（Windows 错误码 " + std::to_string(err) + "）：" + wideToUtf8(path.wstring()));
    }
    guard.committed = true;
}

void writeTextAtomic(const fs::path& path, std::string_view utf8Text) {
    writeFileAtomic(path, std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(utf8Text.data()), utf8Text.size()));
}

nlohmann::json readJsonFile(const fs::path& path, size_t maxBytes) {
    auto bytes = readFileBytes(path, maxBytes);
    try {
        return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, true, true /* allow comments */);
    } catch (const std::exception& e) {
        throw Error("解析 JSON 文件失败（" + wideToUtf8(path.wstring()) + "）：" + e.what());
    }
}

void writeJsonFile(const fs::path& path, const nlohmann::json& data) {
    writeTextAtomic(path, data.dump(2));
}

std::wstring canonicalPathKey(const fs::path& path) {
    std::error_code ec;
    fs::path norm = fs::weakly_canonical(path, ec);
    if (ec) {
        norm = fs::absolute(path, ec).lexically_normal();
    }
    std::wstring key = norm.wstring();
    for (auto& ch : key) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return key;
}

bool isSameFilePath(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ecA, ecB, ecEq;
    if (fs::exists(a, ecA) && fs::exists(b, ecB)) {
        if (fs::equivalent(a, b, ecEq) && !ecEq) return true;
    }
    return canonicalPathKey(a) == canonicalPathKey(b);
}

} // namespace meme::base
