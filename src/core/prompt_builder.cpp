#include "core/prompt_builder.hpp"
#include "base/utf8.hpp"
#include <cmath>
#include <utility>

namespace meme::core {

std::vector<int> computeMidpointFrameSchedule(int totalFrames, bool enableConsistency) {
    if (totalFrames <= 0) return {};
    std::vector<int> order;
    order.reserve(static_cast<size_t>(totalFrames));
    if (!enableConsistency || totalFrames <= 2) {
        for (int i = 0; i < totalFrames; ++i) {
            order.push_back(i);
        }
        return order;
    }

    order.push_back(0);
    order.push_back(totalFrames - 1);
    std::vector<std::pair<int, int>> queue{{0, totalFrames - 1}};
    for (size_t idx = 0; idx < queue.size(); ++idx) {
        auto [left, right] = queue[idx];
        if (right - left <= 1) continue;
        int mid = left + (right - left) / 2;
        order.push_back(mid);
        queue.emplace_back(left, mid);
        queue.emplace_back(mid, right);
    }
    return order;
}

std::string buildCharacterReplacePrompt(const ReplacePromptSpec& spec) {
    std::string prompt =
        "Edit image 1 (the source meme) by replacing its main character with the character shown in image 2 "
        "(the identity reference). Preserve image 1's exact pose, facial expression, gesture, camera angle, "
        "composition, props, and background. Adapt the character's face, hairstyle, outfit, colors, and "
        "accessories from image 2 naturally to the source pose without copying image 2's pose or background. ";

    constexpr size_t kStyleStartIdx = 3;
    if (spec.styleCount == 1) {
        prompt += "Image 3 is a STYLE reference only: adopt its linework, shading, and color treatment without "
                  "copying its character, pose, or scene. ";
    } else if (spec.styleCount > 1) {
        prompt += "Images 3 through " + std::to_string(kStyleStartIdx + spec.styleCount - 1) +
                  " are STYLE references only: adopt their linework, shading, and color treatment without "
                  "copying their characters, poses, or scenes. ";
    } else {
        prompt += "Match the visual style of image 1 unless specified otherwise below. ";
    }

    if (spec.preserveText) {
        prompt += "Keep all existing captions, text, and symbols from image 1 legible and in place. ";
    } else {
        prompt += "Remove existing captions and lettering cleanly. ";
    }

    if (spec.animated) {
        for (size_t i = 0; i < spec.temporalNeighbors.size(); ++i) {
            int nb = spec.temporalNeighbors[i];
            prompt += "Image " + std::to_string(kStyleStartIdx + spec.styleCount + i) +
                      " is the already redrawn animation frame " + std::to_string(nb + 1) +
                      (nb < spec.frameIndex ? " before" : " after") + " this frame (distance " +
                      std::to_string(std::abs(nb - spec.frameIndex)) + "). ";
        }
        if (!spec.temporalNeighbors.empty()) {
            prompt += "Use temporal reference frames only to keep character proportions, colors, and lighting "
                      "consistent across motion; follow image 1 strictly for the current frame's pose. ";
        }
        if (spec.hasFirstFrameAnchor) {
            prompt += "The final uploaded image is the redrawn frame 1 anchor for identity and palette stability; "
                      "do not copy its pose. ";
        }
        prompt += "Frame " + std::to_string(spec.frameIndex + 1) + " of " +
                  std::to_string(spec.totalFrames) + ". ";
    }

    prompt += "Output a single complete image matching image 1's aspect ratio with no extra panels or watermarks.";

    std::string trimmedUser = base::trimAscii(spec.userInstruction);
    if (!trimmedUser.empty()) {
        prompt += "\nUser instructions:\n" + trimmedUser;
    }
    return prompt;
}

} // namespace meme::core
