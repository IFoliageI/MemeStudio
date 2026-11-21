#pragma once
#include "core/config_store.hpp"
#include "core/plugin_host.hpp"
#include "media/image_types.hpp"
#include "net/openai_client.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace meme::core {

namespace fs = std::filesystem;

struct ReplaceProgress {
    int completedFrames = 0;
    int totalFrames = 0;
    int currentCopyIndex = 0;
    int totalCopies = 1;
    int activeFrameIndex = -1;
    int partialPreviewIndex = -1;
    std::string messageUtf8;
    std::shared_ptr<media::Image> previewImage;
};

using ReplaceProgressFn = std::function<void(const ReplaceProgress&)>;

std::vector<fs::path> resolveCopyOutputPaths(const fs::path& baseOutput, int copyCount);

std::vector<fs::path> runCharacterReplaceJob(
    const ReplaceTaskConfig& job,
    net::OpenAiImageClient& apiClient,
    const PluginHost* plugins,
    net::CancelToken& cancel,
    const ReplaceProgressFn& onProgress = {});

} // namespace meme::core
