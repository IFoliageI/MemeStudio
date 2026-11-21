#pragma once
#include "media/image_types.hpp"
#include "net/openai_client.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "../../vendor/nlohmann/json.hpp"

namespace meme::core {

namespace fs = std::filesystem;

enum class SignaturePolicy {
    Disabled = 0,
    Trusted  = 1,
    SameSigner = 2
};

struct StudioSettings {
    net::ProviderConfig provider;
    bool rememberKey = false;
    int frameStride = 1;
    int maxFrames = 0; // Unified default 0 = unlimited in both GUI and CLI!
    bool temporalConsistency = true;
    bool preserveText = true;
    bool allowResume = true;
    int copyCount = 1;
    std::vector<std::string> enabledPlugins;
    SignaturePolicy signaturePolicy = SignaturePolicy::Disabled;
    int leftPaneDip = 250;
    int rightPaneDip = 336;
};

struct ReplaceTaskConfig {
    net::ProviderConfig provider;
    fs::path sourcePath;
    fs::path characterPath;
    media::NormalizedRect characterCrop;
    std::vector<fs::path> stylePaths;
    fs::path outputPath;
    std::string instruction;
    int frameStride = 1;
    int maxFrames = 0;
    int copyCount = 1;
    bool temporalConsistency = true;
    bool preserveText = true;
    bool allowResume = true;
    std::vector<std::string> enabledPlugins;
    fs::path pluginDir;
};

StudioSettings parseStudioSettingsJson(const nlohmann::json& data);
nlohmann::json serializeStudioSettingsJson(const StudioSettings& settings);

StudioSettings loadStudioSettings();
void saveStudioSettings(const StudioSettings& settings);

// Parses JSON, JSONC, or strict INI task configuration text without corrupting Windows backslashes
// or silently degrading invalid section types into strings.
nlohmann::json parseConfigDocument(std::string_view contentUtf8, bool isIniFormat);
ReplaceTaskConfig parseReplaceTaskConfig(const nlohmann::json& doc, const fs::path& baseDir = {});

} // namespace meme::core
