#pragma once
#include "media/image_types.hpp"
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace meme::core {

enum class RetouchMode {
    PromptEdit = 0,   // 提示词整图修图（按自然语言指令修改整幅画面/配饰/神态/背景/文字）
    RegionRedraw = 1  // 局部框选精修（仅重绘指定矩形区域，框外原始像素严格无损保留）
};

struct ReplacePromptSpec {
    size_t styleCount = 0;
    bool preserveText = true;
    bool animated = false;
    int frameIndex = 0;
    int totalFrames = 1;
    bool hasFirstFrameAnchor = false;
    std::span<const int> temporalNeighbors;
    std::string_view userInstruction;
};

struct RetouchPromptSpec {
    RetouchMode mode = RetouchMode::PromptEdit;
    media::NormalizedRect region{0.0, 0.0, 1.0, 1.0};
    size_t styleCount = 0;
    bool preserveText = true;
    bool animated = false;
    int frameIndex = 0;
    int totalFrames = 1;
    bool hasFirstFrameAnchor = false;
    std::span<const int> temporalNeighbors;
    std::string_view userInstruction;
};

// Computes bidirectional midpoint keyframe generation order [0, N-1, mid, ...]
// so every interpolated interior frame has temporal context from both sides.
std::vector<int> computeMidpointFrameSchedule(int totalFrames, bool enableConsistency);

std::string buildCharacterReplacePrompt(const ReplacePromptSpec& spec);
std::string buildPromptDrawPrompt(std::string_view userPrompt, size_t styleCount);
std::string buildImageRetouchPrompt(const RetouchPromptSpec& spec);

} // namespace meme::core
