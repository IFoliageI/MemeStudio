#pragma once
#include "base/crypto.hpp"
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace meme::media {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    base::Bytes pixels; // BGRA8, row stride = width * 4

    bool empty() const noexcept {
        return width == 0 || height == 0 || pixels.size() != static_cast<size_t>(width) * height * 4;
    }
    uint32_t stride() const noexcept { return width * 4; }
};

struct Frame {
    Image image;
    uint32_t delayMs = 100;
};

struct Animation {
    std::vector<Frame> frames;
    // nullopt = absent NETSCAPE2.0 loop extension; 0 = infinite loop; 1..65535 = repeat count
    std::optional<uint16_t> loopCount;
    bool animated = false;
};

struct MediaMetadata {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameCount = 0;
    bool animated = false;
    std::optional<uint16_t> loopCount;
    std::vector<uint32_t> frameDelaysMs;
};

struct NormalizedRect {
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;

    static constexpr double kTolerance = 1e-6;

    bool isFullImage() const noexcept;
    void validate(std::string_view fieldLabel = "选区矩形") const;
};

Image allocateImage(uint32_t width, uint32_t height);

} // namespace meme::media
