#pragma once
#include "media/image_types.hpp"
#include "uikit/dpi.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace meme::uikit {

namespace fs = std::filesystem;

class ImageCanvas {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    ImageCanvas();
    ~ImageCanvas();
    ImageCanvas(const ImageCanvas&) = delete;
    ImageCanvas& operator=(const ImageCanvas&) = delete;

    HWND create(
        HINSTANCE instance,
        HWND parent,
        std::string emptyHintUtf8,
        bool enableCropBox = false);

    HWND hwnd() const noexcept;

    // Converts BGRA -> pre-multiplied PARGB32 ONCE and caches into a GDI DIBSection!
    void setImage(const media::Image* image);
    void clearImage();

    void resetViewport() noexcept;
    void setCropRect(const media::NormalizedRect& rect) noexcept;
    media::NormalizedRect cropRect() const noexcept;

    void setOnCropChanged(std::function<void(const media::NormalizedRect&)> cb);
    void setOnFileDropped(std::function<void(const fs::path&)> cb);
    void notifyDpiChanged(UINT newDpi) noexcept;
};

} // namespace meme::uikit
