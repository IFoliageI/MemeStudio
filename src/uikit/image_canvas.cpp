#include "uikit/image_canvas.hpp"
#include "base/utf8.hpp"
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace meme::uikit {

namespace {

constexpr const wchar_t* kCanvasClassName = L"MemeStudioCanvasV2";

void ensureCanvasClassRegistered(HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(WNDCLASSEXW);
    if (GetClassInfoExW(instance, kCanvasClassName, &existing)) {
        return;
    }
    // Distinct zero-initialized WNDCLASSEXW with nullptr background brush (prevents flicker & dangling HBRUSH!)
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kCanvasClassName;
    wc.lpfnWndProc = DefWindowProcW;
    RegisterClassExW(&wc);
}

HBRUSH createCheckerboardPatternBrush(int cellPx) {
    int tileSize = std::max(8, cellPx * 2);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tileSize;
    bmi.bmiHeader.biHeight = -tileSize; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        return CreateSolidBrush(RGB(244, 245, 247));
    }

    auto* px = static_cast<uint32_t*>(bits);
    constexpr uint32_t kLight = 0xFFF7F8FAu;
    constexpr uint32_t kDark  = 0xFFE8EBF0u;
    int half = tileSize / 2;
    for (int y = 0; y < tileSize; ++y) {
        for (int x = 0; x < tileSize; ++x) {
            bool odd = ((x >= half) ^ (y >= half));
            px[y * tileSize + x] = odd ? kDark : kLight;
        }
    }

    HBRUSH brush = CreatePatternBrush(dib);
    DeleteObject(dib);
    return brush;
}

} // namespace

struct ImageCanvas::Impl {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    std::wstring emptyHint;
    bool cropEnabled = false;

    // Cached pre-multiplied PARGB32 bitmap (zero per-WM_PAINT pixel loops!)
    HBITMAP cachedBitmap = nullptr;
    uint32_t imgWidth = 0;
    uint32_t imgHeight = 0;

    HBRUSH checkerBrush = nullptr;
    HFONT hintFont = nullptr;

    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;

    bool panning = false;
    POINT panStartMouse{};
    double panStartX = 0.0;
    double panStartY = 0.0;

    bool draggingCrop = false;
    POINT cropStartMouse{};
    media::NormalizedRect crop{0.0, 0.0, 1.0, 1.0};
    RECT drawnImageRect{};

    std::function<void(const media::NormalizedRect&)> onCropChanged;
    std::function<void(const fs::path&)> onFileDropped;

    ~Impl() {
        releaseCachedBitmap();
        if (checkerBrush) DeleteObject(checkerBrush);
        if (hintFont) DeleteObject(hintFont);
    }

    void releaseCachedBitmap() noexcept {
        if (cachedBitmap) {
            DeleteObject(cachedBitmap);
            cachedBitmap = nullptr;
        }
        imgWidth = 0;
        imgHeight = 0;
    }

    void rebuildDpiResources(UINT nextDpi) noexcept {
        dpi = std::clamp<UINT>(nextDpi, 72u, 480u);
        if (checkerBrush) DeleteObject(checkerBrush);
        checkerBrush = createCheckerboardPatternBrush(MulDiv(12, static_cast<int>(dpi), 96));

        if (hintFont) DeleteObject(hintFont);
        hintFont = CreateFontW(
            -MulDiv(13, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    RECT computeTargetRect(const RECT& client) noexcept {
        int cw = std::max<int>(1, client.right - client.left - 8);
        int ch = std::max<int>(1, client.bottom - client.top - 8);
        if (!cachedBitmap || imgWidth == 0 || imgHeight == 0) {
            drawnImageRect = {0, 0, 0, 0};
            return drawnImageRect;
        }
        double baseScale = std::min(
            static_cast<double>(cw) / imgWidth,
            static_cast<double>(ch) / imgHeight);
        double effScale = baseScale * zoom;
        int dw = std::max(1, static_cast<int>(std::lround(imgWidth * effScale)));
        int dh = std::max(1, static_cast<int>(std::lround(imgHeight * effScale)));
        int dx = client.left + (client.right - client.left - dw) / 2 + static_cast<int>(std::lround(panX));
        int dy = client.top + (client.bottom - client.top - dh) / 2 + static_cast<int>(std::lround(panY));
        drawnImageRect = {dx, dy, dx + dw, dy + dh};
        return drawnImageRect;
    }

    void paint(HDC paintDc, const RECT& client) {
        int w = std::max<int>(1, client.right - client.left);
        int h = std::max<int>(1, client.bottom - client.top);

        HDC memDc = CreateCompatibleDC(paintDc);
        HBITMAP backBuf = CreateCompatibleBitmap(paintDc, w, h);
        auto oldBmp = SelectObject(memDc, backBuf);

        RECT localRc{0, 0, w, h};
        // 1 single FillRect call fills the entire checkerboard!
        FillRect(memDc, &localRc, checkerBrush);

        if (cachedBitmap && imgWidth > 0 && imgHeight > 0) {
            RECT target = computeTargetRect(localRc);
            HDC imgDc = CreateCompatibleDC(memDc);
            auto oldSrc = SelectObject(imgDc, cachedBitmap);

            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            SetStretchBltMode(memDc, HALFTONE);
            GdiAlphaBlend(
                memDc,
                target.left, target.top, target.right - target.left, target.bottom - target.top,
                imgDc,
                0, 0, static_cast<int>(imgWidth), static_cast<int>(imgHeight),
                blend);

            SelectObject(imgDc, oldSrc);
            DeleteDC(imgDc);

            if (cropEnabled) {
                int iw = target.right - target.left;
                int ih = target.bottom - target.top;
                RECT cropRc{
                    target.left + static_cast<int>(std::lround(crop.x * iw)),
                    target.top + static_cast<int>(std::lround(crop.y * ih)),
                    target.left + static_cast<int>(std::lround((crop.x + crop.width) * iw)),
                    target.top + static_cast<int>(std::lround((crop.y + crop.height) * ih))
                };
                HPEN pen = CreatePen(PS_SOLID, std::max(2, MulDiv(2, static_cast<int>(dpi), 96)), RGB(16, 163, 127));
                auto oldPen = SelectObject(memDc, pen);
                auto oldBrush = SelectObject(memDc, GetStockObject(NULL_BRUSH));
                Rectangle(memDc, cropRc.left, cropRc.top, cropRc.right, cropRc.bottom);
                SelectObject(memDc, oldBrush);
                SelectObject(memDc, oldPen);
                DeleteObject(pen);
            }
        } else if (!emptyHint.empty()) {
            SetBkMode(memDc, TRANSPARENT);
            SetTextColor(memDc, RGB(110, 118, 129));
            auto oldFont = SelectObject(memDc, hintFont);
            RECT textRc = localRc;
            InflateRect(&textRc, -12, -12);
            DrawTextW(memDc, emptyHint.c_str(), -1, &textRc, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(memDc, oldFont);
        }

        // Crisp 1px border
        HBRUSH borderBrush = CreateSolidBrush(RGB(208, 215, 222));
        FrameRect(memDc, &localRc, borderBrush);
        DeleteObject(borderBrush);

        BitBlt(paintDc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(backBuf);
        DeleteDC(memDc);
    }

    void updateCropFromDrag(POINT pt) {
        int iw = drawnImageRect.right - drawnImageRect.left;
        int ih = drawnImageRect.bottom - drawnImageRect.top;
        if (iw <= 2 || ih <= 2) return;

        auto normX = [&](int px) { return std::clamp(double(px - drawnImageRect.left) / iw, 0.0, 1.0); };
        auto normY = [&](int py) { return std::clamp(double(py - drawnImageRect.top) / ih, 0.0, 1.0); };

        double x0 = normX(cropStartMouse.x);
        double y0 = normY(cropStartMouse.y);
        double x1 = normX(pt.x);
        double y1 = normY(pt.y);

        double left = std::min(x0, x1);
        double top = std::min(y0, y1);
        double w = std::max(0.02, std::abs(x1 - x0));
        double h = std::max(0.02, std::abs(y1 - y0));
        if (left + w > 1.0) w = 1.0 - left;
        if (top + h > 1.0) h = 1.0 - top;

        crop = {left, top, w, h};
        InvalidateRect(hwnd, nullptr, FALSE);
        if (onCropChanged) onCropChanged(crop);
    }

    static LRESULT CALLBACK wndProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) return DefWindowProcW(window, msg, wp, lp);

        switch (msg) {
            case WM_ERASEBKGND:
                return 1; // 100% double-buffered in WM_PAINT — zero background erase flicker!
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC dc = BeginPaint(window, &ps);
                RECT rc{};
                GetClientRect(window, &rc);
                self->paint(dc, rc);
                EndPaint(window, &ps);
                return 0;
            }
            case WM_LBUTTONDBLCLK: {
                self->zoom = 1.0;
                self->panX = 0.0;
                self->panY = 0.0;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                SetFocus(window);
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                SetCapture(window);
                if (self->cropEnabled && self->cachedBitmap && !(GetKeyState(VK_SPACE) & 0x8000)) {
                    self->draggingCrop = true;
                    self->cropStartMouse = pt;
                } else {
                    self->panning = true;
                    self->panStartMouse = pt;
                    self->panStartX = self->panX;
                    self->panStartY = self->panY;
                }
                return 0;
            }
            case WM_MOUSEMOVE: {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (self->draggingCrop) {
                    self->updateCropFromDrag(pt);
                    return 0;
                }
                if (self->panning) {
                    self->panX = self->panStartX + (pt.x - self->panStartMouse.x);
                    self->panY = self->panStartY + (pt.y - self->panStartMouse.y);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                break;
            }
            case WM_LBUTTONUP: {
                if (self->draggingCrop || self->panning) {
                    self->draggingCrop = false;
                    self->panning = false;
                    ReleaseCapture();
                }
                return 0;
            }
            case WM_RBUTTONDOWN: {
                if (self->cachedBitmap) {
                    self->panning = true;
                    self->panStartMouse = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    self->panStartX = self->panX;
                    self->panStartY = self->panY;
                    SetCapture(window);
                }
                return 0;
            }
            case WM_RBUTTONUP: {
                if (self->panning) {
                    self->panning = false;
                    ReleaseCapture();
                }
                return 0;
            }
            case WM_MOUSEWHEEL: {
                if (!self->cachedBitmap) break;
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                double factor = std::pow(1.15, static_cast<double>(delta) / WHEEL_DELTA);
                self->zoom = std::clamp(self->zoom * factor, 0.2, 16.0);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            case WM_DROPFILES: {
                HDROP drop = reinterpret_cast<HDROP>(wp);
                UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                if (count > 0 && self->onFileDropped) {
                    UINT len = DragQueryFileW(drop, 0, nullptr, 0);
                    std::wstring buf(len + 1, L'\0');
                    DragQueryFileW(drop, 0, buf.data(), len + 1);
                    buf.resize(len);
                    self->onFileDropped(fs::path(buf));
                }
                DragFinish(drop);
                return 0;
            }
        }
        return DefWindowProcW(window, msg, wp, lp);
    }
};

ImageCanvas::ImageCanvas() : impl_(std::make_unique<Impl>()) {}
ImageCanvas::~ImageCanvas() = default;

HWND ImageCanvas::create(HINSTANCE instance, HWND parent, std::string emptyHintUtf8, bool enableCropBox) {
    ensureCanvasClassRegistered(instance);
    impl_->emptyHint = base::utf8ToWideLossy(emptyHintUtf8);
    impl_->cropEnabled = enableCropBox;
    UINT parentDpi = parent ? GetDpiForWindow(parent) : 96;
    impl_->rebuildDpiResources(parentDpi >= 72 ? parentDpi : 96);

    HWND h = CreateWindowExW(
        0, kCanvasClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 100, 100, parent, nullptr, instance, nullptr);
    impl_->hwnd = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_.get()));
    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Impl::wndProc));
    DragAcceptFiles(h, TRUE);
    return h;
}

HWND ImageCanvas::hwnd() const noexcept { return impl_->hwnd; }

void ImageCanvas::setImage(const media::Image* image) {
    impl_->releaseCachedBitmap();
    if (!image || image->empty()) {
        if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(image->width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(image->height); // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dstBits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dstBits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (!dib || !dstBits) {
        if (dib) DeleteObject(dib);
        return;
    }

    // One-time pre-multiplied alpha conversion into cached DIBSection!
    const uint8_t* src = image->pixels.data();
    auto* dst = static_cast<uint8_t*>(dstBits);
    size_t pixelCount = static_cast<size_t>(image->width) * image->height;
    for (size_t i = 0; i < pixelCount; ++i) {
        uint32_t a = src[i * 4 + 3];
        dst[i * 4 + 0] = static_cast<uint8_t>((src[i * 4 + 0] * a + 127) / 255);
        dst[i * 4 + 1] = static_cast<uint8_t>((src[i * 4 + 1] * a + 127) / 255);
        dst[i * 4 + 2] = static_cast<uint8_t>((src[i * 4 + 2] * a + 127) / 255);
        dst[i * 4 + 3] = static_cast<uint8_t>(a);
    }

    impl_->cachedBitmap = dib;
    impl_->imgWidth = image->width;
    impl_->imgHeight = image->height;
    if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void ImageCanvas::clearImage() {
    setImage(nullptr);
}

void ImageCanvas::resetViewport() noexcept {
    impl_->zoom = 1.0;
    impl_->panX = 0.0;
    impl_->panY = 0.0;
    if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void ImageCanvas::setCropRect(const media::NormalizedRect& rect) noexcept {
    impl_->crop = rect;
    if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

media::NormalizedRect ImageCanvas::cropRect() const noexcept {
    return impl_->crop;
}

void ImageCanvas::setOnCropChanged(std::function<void(const media::NormalizedRect&)> cb) {
    impl_->onCropChanged = std::move(cb);
}

void ImageCanvas::setOnFileDropped(std::function<void(const fs::path&)> cb) {
    impl_->onFileDropped = std::move(cb);
}

void ImageCanvas::notifyDpiChanged(UINT newDpi) noexcept {
    impl_->rebuildDpiResources(newDpi);
    if (impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

} // namespace meme::uikit
