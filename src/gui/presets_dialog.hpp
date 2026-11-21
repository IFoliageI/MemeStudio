#pragma once
#include "uikit/dpi.hpp"
#include <functional>
#include <string>

namespace meme::gui {

enum class PresetApplyMode {
    Append,
    Replace
};

void openPresetsPicker(
    HINSTANCE instance,
    HWND owner,
    std::function<void(const std::string& text, PresetApplyMode mode)> onApply);

} // namespace meme::gui
