#include "uikit/dpi.hpp"
#include <algorithm>

namespace meme::uikit {

namespace {
HFONT makeClearTypeFont(UINT dpi, int dipHeight, int weight) noexcept {
    int pxHeight = -MulDiv(dipHeight, static_cast<int>(dpi), 96);
    return CreateFontW(
        pxHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}
} // namespace

void DpiContext::destroyFonts() noexcept {
    if (normalFont_) { DeleteObject(normalFont_); normalFont_ = nullptr; }
    if (smallFont_)  { DeleteObject(smallFont_);  smallFont_ = nullptr; }
    if (boldFont_)   { DeleteObject(boldFont_);   boldFont_ = nullptr; }
    if (titleFont_)  { DeleteObject(titleFont_);  titleFont_ = nullptr; }
}

void DpiContext::createFonts() noexcept {
    normalFont_ = makeClearTypeFont(dpi_, 14, FW_NORMAL);
    smallFont_  = makeClearTypeFont(dpi_, 12, FW_NORMAL);
    boldFont_   = makeClearTypeFont(dpi_, 14, FW_SEMIBOLD);
    titleFont_  = makeClearTypeFont(dpi_, 18, FW_BOLD);
}

void DpiContext::updateForWindow(HWND hwnd) noexcept {
    if (!hwnd) return;
    UINT d = GetDpiForWindow(hwnd);
    if (d >= 72) {
        updateForDpi(d);
    }
}

void DpiContext::updateForDpi(UINT newDpi) noexcept {
    newDpi = std::clamp<UINT>(newDpi, 72u, 480u);
    if (newDpi == dpi_ && normalFont_) return;
    dpi_ = newDpi;
    destroyFonts();
    createFonts();
}

int DpiContext::vscrollWidthPx() const noexcept {
    return GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
}

void DpiContext::applyNormalFontToChildren(HWND parent) const noexcept {
    if (!parent || !normalFont_) return;
    EnumChildWindows(parent, [](HWND child, LPARAM lp) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(normalFont_));
}

DeferBatch::DeferBatch(int estimatedCount) {
    hdwp_ = BeginDeferWindowPos(std::max(1, estimatedCount));
}

DeferBatch::~DeferBatch() {
    if (hdwp_) {
        EndDeferWindowPos(hdwp_);
    }
}

void DeferBatch::move(HWND child, int x, int y, int width, int height) noexcept {
    if (!child) return;
    int w = std::max(0, width);
    int h = std::max(0, height);
    if (hdwp_) {
        hdwp_ = DeferWindowPos(
            hdwp_, child, nullptr, x, y, w, h,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    } else {
        SetWindowPos(child, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }
}

} // namespace meme::uikit
