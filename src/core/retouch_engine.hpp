#pragma once
#include "core/prompt_builder.hpp"
#include "core/replace_engine.hpp"

namespace meme::core {

struct RetouchTaskConfig {
    net::ProviderConfig provider;
    fs::path sourcePath;
    fs::path outputPath;
    RetouchMode mode = RetouchMode::PromptEdit;
    media::NormalizedRect region{0.0, 0.0, 1.0, 1.0};
    std::vector<fs::path> stylePaths;
    std::string instruction;
    int copyCount = 1;
    int frameStride = 1;
    int maxFrames = 0;
    bool temporalConsistency = true;
    bool preserveText = true;
};

std::vector<fs::path> runImageRetouchJob(
    const RetouchTaskConfig& job,
    net::OpenAiImageClient& apiClient,
    const PluginHost* plugins,
    net::CancelToken& cancel,
    const ReplaceProgressFn& onProgress = {});

} // namespace meme::core
