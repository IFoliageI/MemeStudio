#pragma once
#include "uikit/dpi.hpp"
#include "../sdk/memestudio_plugin.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace meme::uikit {

// Instance-scoped modeless tool & plugin window manager (zero process-global inline vectors!).
class PanelManager {
    struct Entry;
    HINSTANCE instance_ = nullptr;
    std::vector<std::shared_ptr<Entry>> openPanels_;

    static LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
public:
    explicit PanelManager(HINSTANCE instance);
    ~PanelManager();
    PanelManager(const PanelManager&) = delete;
    PanelManager& operator=(const PanelManager&) = delete;

    // Compatible directly with core::HostUiCallbacks::onOpenPanel!
    int32_t openOrActivatePluginPanel(
        std::string_view panelKey,
        void* ownerHwnd,
        int32_t widthDip,
        int32_t heightDip,
        std::string_view titleUtf8,
        intptr_t(MEMESTUDIO_CALL* pluginWndProc)(void*, void*, uint32_t, uintptr_t, intptr_t),
        void* panelCtx,
        void(MEMESTUDIO_CALL* onDestroy)(void*));

    void closePanel(std::string_view panelKey) noexcept;
    void closeAll() noexcept;
    bool translateDialogNavigation(MSG& msg) const noexcept;
};

} // namespace meme::uikit
