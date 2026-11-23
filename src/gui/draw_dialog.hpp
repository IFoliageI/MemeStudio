#pragma once
#include "core/studio_session.hpp"
#include "uikit/panel_manager.hpp"
#include <filesystem>
#include <functional>

namespace meme::gui {

void openPromptDrawWorkbench(
    HINSTANCE instance,
    HWND owner,
    uikit::PanelManager& panelManager,
    core::StudioSession& session,
    std::function<void(const std::filesystem::path& generatedFile)> onSendToRetouch,
    std::function<void()> onSyncControlsBeforeRun);

} // namespace meme::gui
