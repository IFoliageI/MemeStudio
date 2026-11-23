#pragma once
#include "media/image_types.hpp"

namespace meme::media {

Image resizeImage(const Image& src, uint32_t targetWidth, uint32_t targetHeight);
Image limitImageEdge(const Image& src, uint32_t maxEdge = 2048);
Image cropImage(const Image& src, const NormalizedRect& crop);
Image preserveRegionOutside(const Image& original, const Image& edited, const NormalizedRect& editableRegion);

// Converts straight-alpha BGRA8 into premultiplied PARGB32 (native GPU/UI surface format for WinUI 3, Win2D, GDI AlphaBlend, Qt, and WebCanvas).
void premultiplyBgraToPargb32(const Image& src, uint8_t* dstBuffer, size_t dstStrideBytes) noexcept;

} // namespace meme::media
