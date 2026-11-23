#pragma once
#include "core/replace_engine.hpp"

namespace meme::core {

struct PromptDrawTaskConfig {
    net::ProviderConfig provider;
    std::string prompt;
    std::vector<fs::path> stylePaths;
    fs::path outputPath;
    int copyCount = 1;
};

std::vector<fs::path> runPromptDrawJob(
    const PromptDrawTaskConfig& job,
    net::OpenAiImageClient& apiClient,
    const PluginHost* plugins,
    net::CancelToken& cancel,
    const ReplaceProgressFn& onProgress = {});

} // namespace meme::core
