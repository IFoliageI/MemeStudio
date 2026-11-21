#include "base/app_paths.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <mutex>

namespace meme::base {

namespace {
std::mutex g_rootMutex;
fs::path g_overrideRoot;
} // namespace

void setAppDataRootOverride(const fs::path& customRoot) {
    std::lock_guard lock(g_rootMutex);
    g_overrideRoot = customRoot;
}

fs::path appDataRoot() {
    {
        std::lock_guard lock(g_rootMutex);
        if (!g_overrideRoot.empty()) {
            return g_overrideRoot;
        }
    }
    PWSTR directory = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory);
    if (FAILED(hr) || !directory) {
        if (directory) CoTaskMemFree(directory);
        checkHr(hr, "获取 LocalAppData 目录");
    }
    fs::path root = fs::path(directory) / L"MemeStudio";
    CoTaskMemFree(directory);
    return root;
}

fs::path configDir() {
    return appDataRoot() / L"config";
}

fs::path settingsFilePath() {
    return configDir() / L"settings.json";
}

fs::path logsDir() {
    return appDataRoot() / L"logs";
}

fs::path jobCacheRootDir() {
    return appDataRoot() / L"cache" / L"jobs";
}

fs::path jobCacheDir(std::string_view jobSignatureHex) {
    if (jobSignatureHex.empty()) {
        throw Error("任务缓存签名不能为空");
    }
    std::string prefix(jobSignatureHex.substr(0, std::min<size_t>(32, jobSignatureHex.size())));
    return jobCacheRootDir() / utf8ToWideStrict(prefix);
}

bool isValidIdentifier(std::string_view id) {
    return !id.empty() && id.size() <= 80 &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
           }) &&
           id != "." && id != "..";
}

fs::path pluginDataDir(std::string_view pluginId) {
    if (!isValidIdentifier(pluginId)) {
        throw Error("无效的插件标识符：" + std::string(pluginId));
    }
    fs::path dir = appDataRoot() / L"plugins" / utf8ToWideStrict(pluginId);
    fs::create_directories(dir);
    return dir;
}

fs::path executableFilePath() {
    std::wstring buf(32768, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        throw Error("无法获取主程序路径");
    }
    buf.resize(len);
    return fs::path(buf);
}

fs::path defaultPluginsDir() {
    return executableFilePath().parent_path() / L"plugins";
}

} // namespace meme::base
