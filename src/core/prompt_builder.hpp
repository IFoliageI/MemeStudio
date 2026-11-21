#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace meme::core {

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

// Computes bidirectional midpoint keyframe generation order [0, N-1, mid, ...]
// so every interpolated interior frame has temporal context from both sides.
std::vector<int> computeMidpointFrameSchedule(int totalFrames, bool enableConsistency);

std::string buildCharacterReplacePrompt(const ReplacePromptSpec& spec);

} // namespace meme::core
