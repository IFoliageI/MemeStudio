#pragma once
#include "media/image_types.hpp"

namespace meme::media {

Image resizeImage(const Image& src, uint32_t targetWidth, uint32_t targetHeight);
Image limitImageEdge(const Image& src, uint32_t maxEdge = 2048);
Image cropImage(const Image& src, const NormalizedRect& crop);
Image preserveRegionOutside(const Image& original, const Image& edited, const NormalizedRect& editableRegion);

} // namespace meme::media
