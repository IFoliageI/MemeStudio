#include "uikit/controls.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"
#include <algorithm>
#include <charconv>

namespace meme::uikit {

std::string getTextUtf8(HWND control) {
    if (!control) return {};
    int len = GetWindowTextLengthW(control);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(control, buf.data(), len + 1);
    if (copied <= 0) return {};
    buf.resize(static_cast<size_t>(copied));
    return base::wideToUtf8(buf);
}

void setTextUtf8(HWND control, std::string_view text) {
    if (!control) return;
    std::wstring w = base::utf8ToWideLossy(text);
    SetWindowTextW(control, w.c_str());
}

bool isChecked(HWND button) noexcept {
    return button && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setChecked(HWND button, bool checked) noexcept {
    if (button) {
        SendMessageW(button, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

int parseBoundedInt(HWND editControl, std::string_view fieldNameUtf8, int minVal, int maxVal) {
    std::string raw = base::trimAscii(getTextUtf8(editControl));
    if (raw.empty()) {
        throw Error("请填写「" + std::string(fieldNameUtf8) + "」");
    }
    int val = 0;
    auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), val);
    if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
        throw Error("「" + std::string(fieldNameUtf8) + "」必须是整数");
    }
    if (val < minVal || val > maxVal) {
        throw Error("「" + std::string(fieldNameUtf8) + "」必须在 " +
                    std::to_string(minVal) + " 到 " + std::to_string(maxVal) + " 之间");
    }
    return val;
}

void sizeAndCenterWindow(HWND window, HWND owner, int clientWidthPx, int clientHeightPx) noexcept {
    if (!window) return;
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    UINT dpi = GetDpiForWindow(window);
    if (dpi < 72) dpi = 96;

    RECT desired{0, 0, clientWidthPx, clientHeightPx};
    AdjustWindowRectExForDpi(&desired, style, FALSE, exStyle, dpi);
    int winW = desired.right - desired.left;
    int winH = desired.bottom - desired.top;

    HMONITOR mon = MonitorFromWindow(owner && IsWindow(owner) ? owner : window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(MONITORINFO)};
    RECT workArea{0, 0, 1280, 800};
    if (GetMonitorInfoW(mon, &mi)) {
        workArea = mi.rcWork;
    }

    int maxW = std::max(320, static_cast<int>(workArea.right - workArea.left - 24));
    int maxH = std::max(240, static_cast<int>(workArea.bottom - workArea.top - 24));
    winW = std::min(winW, maxW);
    winH = std::min(winH, maxH);

    int posX = workArea.left + (workArea.right - workArea.left - winW) / 2;
    int posY = workArea.top + (workArea.bottom - workArea.top - winH) / 2;

    if (owner && IsWindow(owner) && IsWindowVisible(owner) && !IsIconic(owner)) {
        RECT ownerRc{};
        if (GetWindowRect(owner, &ownerRc)) {
            posX = ownerRc.left + ((ownerRc.right - ownerRc.left) - winW) / 2;
            posY = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - winH) / 2;
        }
    }

    // Strictly clamp to monitor workArea (includes workArea.left/top when taskbar is on left/top!)
    posX = std::clamp<int>(posX, workArea.left, std::max<int>(workArea.left, workArea.right - winW));
    posY = std::clamp<int>(posY, workArea.top, std::max<int>(workArea.top, workArea.bottom - winH));

    SetWindowPos(window, nullptr, posX, posY, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
}

void showErrorBox(HWND owner, std::string_view titleUtf8, std::string_view messageUtf8) noexcept {
    std::wstring wTitle = base::utf8ToWideLossy(titleUtf8);
    std::wstring wMsg = base::utf8ToWideLossy(messageUtf8);
    MessageBoxW(owner, wMsg.c_str(), wTitle.c_str(), MB_OK | MB_ICONERROR);
}

} // namespace meme::uikit
