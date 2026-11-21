#pragma once
#include <filesystem>
#include <string_view>

namespace meme::base {

namespace fs = std::filesystem;

// Returns %LOCALAPPDATA%\MemeStudio (or test sandbox override when set).
fs::path appDataRoot();
void setAppDataRootOverride(const fs::path& customRoot);

fs::path configDir();
fs::path settingsFilePath();
fs::path logsDir();
fs::path jobCacheRootDir();
fs::path jobCacheDir(std::string_view jobSignatureHex);
fs::path pluginDataDir(std::string_view pluginId);
fs::path executableFilePath();
fs::path defaultPluginsDir();

bool isValidIdentifier(std::string_view id);

} // namespace meme::base
