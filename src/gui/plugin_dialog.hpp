#pragma once
#include "uikit/dpi.hpp"
#include "core/config_store.hpp"
#include "core/plugin_host.hpp"
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace meme::gui {

void openPluginManagerDialog(
    HINSTANCE instance,
    HWND owner,
    std::span<const core::PluginManifest> discovered,
    const std::vector<std::string>& currentEnabled,
    core::SignaturePolicy currentPolicy,
    std::function<bool(const std::vector<std::string>& nextEnabled, core::SignaturePolicy nextPolicy, std::string& outErr)> onApplySettings,
    std::function<void(const std::string& pluginId, const std::string& commandId)> onRunCommand);

} // namespace meme::gui
