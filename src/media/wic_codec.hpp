#pragma once
#include "media/image_types.hpp"
#include <filesystem>
#include <optional>
#include <span>

namespace meme::media {

namespace fs = std::filesystem;

// Fast header-only inspection without decoding frame pixel buffers.
MediaMetadata probeMediaMetadata(const fs::path& path);

Animation decodeAnimation(const fs::path& path);
Image decodeFirstFrame(const fs::path& path);
Image decodeImageBytes(std::span<const uint8_t> encodedBytes);

base::Bytes encodePng(const Image& image);
void savePngFile(const fs::path& path, const Image& image);

// Anti-flicker GIF89a encoder with shared master palette quantization and atomic .part cleanup.
void saveGifFile(
    const fs::path& path,
    std::span<const Frame> frames,
    std::optional<uint16_t> loopCount = std::nullopt);

} // namespace meme::media
