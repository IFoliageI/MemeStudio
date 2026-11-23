#include "media/image_ops.hpp"
#include "media/wic_codec.hpp"
#include "base/error.hpp"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace meme::media {

using Microsoft::WRL::ComPtr;

bool NormalizedRect::isFullImage() const noexcept {
    return std::abs(x) <= kTolerance &&
           std::abs(y) <= kTolerance &&
           std::abs(width - 1.0) <= kTolerance &&
           std::abs(height - 1.0) <= kTolerance;
}

void NormalizedRect::validate(std::string_view fieldLabel) const {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
        x < -kTolerance || y < -kTolerance || width <= 0.0 || height <= 0.0 ||
        x + width > 1.0 + kTolerance || y + height > 1.0 + kTolerance) {
        throw Error(std::string(fieldLabel) + "超出有效归一化范围 [0, 1]");
    }
}

Image allocateImage(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
        static_cast<uint64_t>(width) * height > 32ull * 1024 * 1024) {
        throw Error("图像尺寸超出安全上限（最大 16384px / 3200 万像素）");
    }
    return Image{width, height, base::Bytes(static_cast<size_t>(width) * height * 4)};
}

Image resizeImage(const Image& src, uint32_t targetWidth, uint32_t targetHeight) {
    if (src.empty()) {
        throw Error("无法缩放空图像");
    }
    if (src.width == targetWidth && src.height == targetHeight) {
        return src;
    }
    // Single allocation — fixes the legacy double-128MB guard buffer waste!
    Image dst = allocateImage(targetWidth, targetHeight);

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (hr == CO_E_NOTINITIALIZED) {
        checkHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "初始化线程 COM");
        hr = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    checkHr(hr, "创建 WIC 缩放工厂");

    ComPtr<IWICBitmap> srcBmp;
    checkHr(factory->CreateBitmapFromMemory(
        src.width, src.height, GUID_WICPixelFormat32bppBGRA,
        src.stride(), static_cast<UINT>(src.pixels.size()),
        const_cast<BYTE*>(src.pixels.data()), &srcBmp), "创建源位图");

    ComPtr<IWICBitmapScaler> scaler;
    checkHr(factory->CreateBitmapScaler(&scaler), "创建图像缩放器");
    checkHr(scaler->Initialize(srcBmp.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant), "初始化图像缩放");

    ComPtr<IWICFormatConverter> converter;
    checkHr(factory->CreateFormatConverter(&converter), "创建像素格式转换器");
    checkHr(converter->Initialize(
        scaler.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "缩放像素转换");
    checkHr(converter->CopyPixels(
        nullptr, dst.stride(), static_cast<UINT>(dst.pixels.size()), dst.pixels.data()), "拷贝缩放像素");
    return dst;
}

Image limitImageEdge(const Image& src, uint32_t maxEdge) {
    if (src.empty()) {
        throw Error("无法限制空图像尺寸");
    }
    uint32_t longest = std::max(src.width, src.height);
    if (longest <= maxEdge || maxEdge == 0) {
        return src;
    }
    double scale = static_cast<double>(maxEdge) / static_cast<double>(longest);
    uint32_t newW = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(src.width * scale)));
    uint32_t newH = std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(src.height * scale)));
    return resizeImage(src, newW, newH);
}

Image cropImage(const Image& src, const NormalizedRect& crop) {
    if (src.empty()) {
        throw Error("无法裁剪空图像");
    }
    crop.validate("裁剪区域");
    double clampedX = std::clamp(crop.x, 0.0, 1.0);
    double clampedY = std::clamp(crop.y, 0.0, 1.0);
    uint32_t left = std::min<uint32_t>(
        src.width - 1,
        static_cast<uint32_t>(std::lround(clampedX * src.width)));
    uint32_t top = std::min<uint32_t>(
        src.height - 1,
        static_cast<uint32_t>(std::lround(clampedY * src.height)));
    uint32_t right = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::lround((clampedX + crop.width) * src.width)),
        left + 1, src.width);
    uint32_t bottom = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::lround((clampedY + crop.height) * src.height)),
        top + 1, src.height);

    uint32_t outW = right - left;
    uint32_t outH = bottom - top;
    Image out = allocateImage(outW, outH);
    for (uint32_t row = 0; row < outH; ++row) {
        std::memcpy(
            out.pixels.data() + static_cast<size_t>(row) * outW * 4,
            src.pixels.data() + (static_cast<size_t>(top + row) * src.width + left) * 4,
            static_cast<size_t>(outW) * 4);
    }
    return out;
}

Image preserveRegionOutside(const Image& original, const Image& edited, const NormalizedRect& editableRegion) {
    editableRegion.validate("重绘区域");
    if (original.empty() || edited.width != original.width || edited.height != original.height ||
        edited.pixels.size() != original.pixels.size()) {
        throw Error("重绘结果与原图尺寸不匹配");
    }
    if (editableRegion.isFullImage()) {
        return edited;
    }
    Image out = original;
    double cx = std::clamp(editableRegion.x, 0.0, 1.0);
    double cy = std::clamp(editableRegion.y, 0.0, 1.0);
    uint32_t left = std::min<uint32_t>(
        original.width - 1, static_cast<uint32_t>(std::lround(cx * original.width)));
    uint32_t top = std::min<uint32_t>(
        original.height - 1, static_cast<uint32_t>(std::lround(cy * original.height)));
    uint32_t right = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::lround((cx + editableRegion.width) * original.width)),
        left + 1, original.width);
    uint32_t bottom = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::lround((cy + editableRegion.height) * original.height)),
        top + 1, original.height);

    size_t rowBytes = static_cast<size_t>(right - left) * 4;
    for (uint32_t y = top; y < bottom; ++y) {
        size_t offset = (static_cast<size_t>(y) * original.width + left) * 4;
        std::memcpy(out.pixels.data() + offset, edited.pixels.data() + offset, rowBytes);
    }
    return out;
}

void premultiplyBgraToPargb32(const Image& src, uint8_t* dstBuffer, size_t dstStrideBytes) noexcept {
    if (src.empty() || !dstBuffer || dstStrideBytes < static_cast<size_t>(src.width) * 4) {
        return;
    }
    const uint8_t* sBase = src.pixels.data();
    size_t sStride = src.stride();
    for (uint32_t y = 0; y < src.height; ++y) {
        const uint8_t* sRow = sBase + static_cast<size_t>(y) * sStride;
        uint8_t* dRow = dstBuffer + static_cast<size_t>(y) * dstStrideBytes;
        for (uint32_t x = 0; x < src.width; ++x) {
            uint32_t a = sRow[x * 4 + 3];
            dRow[x * 4 + 0] = static_cast<uint8_t>((sRow[x * 4 + 0] * a + 127) / 255);
            dRow[x * 4 + 1] = static_cast<uint8_t>((sRow[x * 4 + 1] * a + 127) / 255);
            dRow[x * 4 + 2] = static_cast<uint8_t>((sRow[x * 4 + 2] * a + 127) / 255);
            dRow[x * 4 + 3] = static_cast<uint8_t>(a);
        }
    }
}

} // namespace meme::media
