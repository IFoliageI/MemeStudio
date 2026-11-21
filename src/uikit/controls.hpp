#pragma once
#include "uikit/dpi.hpp"
#include <string>
#include <string_view>

namespace meme::uikit {

std::string getTextUtf8(HWND control);
void setTextUtf8(HWND control, std::string_view text);

bool isChecked(HWND button) noexcept;
void setChecked(HWND button, bool checked) noexcept;

int parseBoundedInt(HWND editControl, std::string_view fieldNameUtf8, int minVal, int maxVal);

// Resizes a top-level/popup window so its client area matches (clientWidthPx, clientHeightPx)
// clamped safely to the monitor's usable work area (never overflowing taskbars on small/HiDPI screens!),
// and centers it over `owner` or the active monitor work area.
void sizeAndCenterWindow(HWND window, HWND owner, int clientWidthPx, int clientHeightPx) noexcept;

void showErrorBox(HWND owner, std::string_view titleUtf8, std::string_view messageUtf8) noexcept;

} // namespace meme::uikit
