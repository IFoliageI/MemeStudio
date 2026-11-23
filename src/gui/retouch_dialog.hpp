#pragma once
#include "core/studio_session.hpp"
#include "uikit/panel_manager.hpp"
#include <filesystem>
#include <functional>

namespace meme::gui {

void openRetouchWorkbench(
    HINSTANCE instance,
    HWND owner,
    uikit::PanelManager& panelManager,
    core::StudioSession& session,
    const std::filesystem::path& preferredSource = {},
    std::function<void()> onSyncControlsBeforeRun = {});

} // namespace meme::gui
