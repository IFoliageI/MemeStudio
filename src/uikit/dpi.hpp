#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace meme::uikit {

class DpiContext {
    UINT dpi_ = 96;
    HFONT normalFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT boldFont_ = nullptr;
    HFONT titleFont_ = nullptr;

    void destroyFonts() noexcept;
    void createFonts() noexcept;
public:
    DpiContext() { createFonts(); }
    ~DpiContext() { destroyFonts(); }
    DpiContext(const DpiContext&) = delete;
    DpiContext& operator=(const DpiContext&) = delete;

    void updateForWindow(HWND hwnd) noexcept;
    void updateForDpi(UINT newDpi) noexcept;

    UINT dpi() const noexcept { return dpi_; }
    int px(int dip) const noexcept { return MulDiv(dip, static_cast<int>(dpi_), 96); }
    int toDip(int pixels) const noexcept { return MulDiv(pixels, 96, static_cast<int>(dpi_)); }
    int vscrollWidthPx() const noexcept;

    HFONT normalFont() const noexcept { return normalFont_; }
    HFONT smallFont() const noexcept { return smallFont_; }
    HFONT boldFont() const noexcept { return boldFont_; }
    HFONT titleFont() const noexcept { return titleFont_; }

    void applyNormalFontToChildren(HWND parent) const noexcept;
};

// Flicker-free atomic multi-window repositioning batch using BeginDeferWindowPos / DeferWindowPos.
class DeferBatch {
    HDWP hdwp_ = nullptr;
public:
    explicit DeferBatch(int estimatedCount = 24);
    ~DeferBatch();
    DeferBatch(const DeferBatch&) = delete;
    DeferBatch& operator=(const DeferBatch&) = delete;

    void move(HWND child, int x, int y, int width, int height) noexcept;
};

} // namespace meme::uikit
