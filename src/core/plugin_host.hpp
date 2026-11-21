#pragma once
#include "core/config_store.hpp"
#include "media/image_types.hpp"
#include "net/openai_client.hpp"
#include "../sdk/memestudio_plugin.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace meme::core {

namespace fs = std::filesystem;

struct PluginCommandManifest {
    std::string id;
    std::string title;
    bool showInToolbar = false;
};

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    fs::path directory;
    fs::path libraryPath;
    std::vector<PluginCommandManifest> commands;
    std::string loadError;
};

struct SessionEndpointOverride {
    std::string pluginId;
    std::string baseUrl;
    std::string apiKey;
    std::string model;
};

struct HostUiCallbacks {
    std::function<void(int level, std::string_view pluginId, std::string_view message)> onLog;
    std::function<nlohmann::json()> onGetWorkspaceState;
    std::function<void(const std::optional<SessionEndpointOverride>&)> onEndpointOverrideChanged;
    std::function<void(const fs::path&)> onPresentOutputFile;
    std::function<int32_t(
        std::string_view panelKey,
        void* ownerHwnd,
        int32_t widthDip,
        int32_t heightDip,
        std::string_view titleUtf8,
        intptr_t(MEMESTUDIO_CALL* wndProc)(void*, void*, uint32_t, uintptr_t, intptr_t),
        void* panelCtx,
        void(MEMESTUDIO_CALL* onDestroy)(void*))> onOpenPanel;
};

std::vector<PluginManifest> discoverPluginsInDirectory(const fs::path& rootDir);

class PluginHost {
    struct LoadedModule;
    net::WinHttpPool& httpPool_;
    HostUiCallbacks callbacks_;
    MemeStudioHostV2 hostTable_{};
    std::vector<std::unique_ptr<LoadedModule>> modules_;
    mutable std::mutex stateMutex_;
    std::optional<SessionEndpointOverride> endpointOverride_;

    static MemeStudioHostV2 makeHostTable(PluginHost* self);
public:
    PluginHost(
        net::WinHttpPool& httpPool,
        std::span<const PluginManifest> discovered,
        std::span<const std::string> enabledIds,
        SignaturePolicy sigPolicy = SignaturePolicy::Disabled,
        HostUiCallbacks callbacks = {});
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    size_t loadedCount() const noexcept { return modules_.size(); }
    bool isLoaded(std::string_view pluginId) const noexcept;
    nlohmann::json pipelineFingerprint() const;

    std::optional<SessionEndpointOverride> sessionEndpointOverride() const;
    void clearSessionEndpointOverride();

    // Prompt & batch frame hooks
    std::string transformPrompt(const nlohmann::json& context, std::string promptUtf8) const;
    void processFramesBatch(
        std::string_view stage,
        const nlohmann::json& context,
        std::span<media::Frame> frames,
        net::CancelToken& cancel) const;

    // Transport interceptor adapter compatible with OpenAiImageClient::ApiTransportInterceptor
    bool tryInterceptApi(
        std::string_view op,
        const net::ProviderConfig& provider,
        const nlohmann::json& requestMeta,
        std::span<const net::UploadPart> uploads,
        net::CancelToken& cancel,
        const net::ImagePreviewFn& onPreview,
        base::Bytes& outResponseBytes) const;

    // Explicit command invocation
    nlohmann::json invokeCommand(
        std::string_view pluginId,
        std::string_view commandId,
        const nlohmann::json& args,
        void* ownerHwnd,
        net::CancelToken& cancel) const;
};

} // namespace meme::core
