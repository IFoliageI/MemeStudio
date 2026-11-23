#pragma once
#include "core/config_store.hpp"
#include "core/plugin_host.hpp"
#include "core/replace_engine.hpp"
#include "media/image_types.hpp"
#include "net/winhttp_pool.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace meme::core {

namespace fs = std::filesystem;

struct SourceQueueEntry {
    fs::path path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameCount = 1;
    bool animated = false;
};

struct SessionEvent {
    enum class Kind {
        Progress,
        JobSucceeded,
        JobFailed,
        ModelsFetched,
        EndpointOverrideChanged,
        PresentOutputFile,
        Log
    } kind = Kind::Progress;

    ReplaceProgress progress;
    std::vector<fs::path> outputs;
    std::vector<std::string> models;
    std::optional<SessionEndpointOverride> endpointOverride;
    fs::path presentedFile;
    std::string message;
};

using SessionEventCallback = std::function<void(SessionEvent)>;
using HostOpenPanelFn = decltype(HostUiCallbacks{}.onOpenPanel);

// UI-framework-independent stateful application controller (ready for Win32, WinUI 3, Qt, or WebView2/Tauri).
class StudioSession {
    mutable std::mutex stateMutex_;
    net::WinHttpPool httpPool_;

    StudioSettings settings_;
    std::vector<PluginManifest> discoveredPlugins_;
    std::unique_ptr<PluginHost> pluginHost_;
    std::optional<SessionEndpointOverride> activeOverride_;

    // Multi-source input queue + currently previewed animation
    std::vector<SourceQueueEntry> sources_;
    size_t currentSourceIdx_ = 0;
    media::Animation currentSourceAnim_;

    // Character reference + styles + instruction + results
    fs::path characterPath_;
    media::Image characterImage_;
    media::NormalizedRect characterCrop_{0.0, 0.0, 1.0, 1.0};
    std::vector<fs::path> stylePaths_;
    std::string instruction_;

    std::vector<fs::path> resultPaths_;
    size_t currentResultIdx_ = 0;
    media::Animation currentResultAnim_;

    HostOpenPanelFn openPanelHook_;
    SessionEventCallback eventSink_;

    net::CancelToken cancelToken_;
    std::thread workerThread_;
    std::atomic<bool> workerBusy_{false};

    void emit(SessionEvent ev) const;
    void decodeSelectedSourceLocked(size_t idx);
public:
    explicit StudioSession(
        SessionEventCallback onEvent = {},
        HostOpenPanelFn onOpenPanel = {});
    ~StudioSession();
    StudioSession(const StudioSession&) = delete;
    StudioSession& operator=(const StudioSession&) = delete;

    void setEventCallback(SessionEventCallback cb);
    void setOpenPanelHook(HostOpenPanelFn hook);

    // ==================== Source Queue & Navigation (<- [1]/N ->) ====================
    // Accepts 1..N image files AND/OR directories, deduplicates, probes headers rapidly, and decodes the active item.
    size_t importSources(std::span<const fs::path> filesOrFolders, bool append = false);
    void clearSources();
    size_t sourceCount() const;
    size_t currentSourceIndex() const; // 0-based (display as index + 1)
    std::optional<SourceQueueEntry> currentSourceEntry() const;
    std::vector<SourceQueueEntry> listSources() const;

    bool selectSourceIndex(size_t zeroBasedIndex);
    bool stepSourceIndex(int delta); // Wraps cleanly across 0 .. N-1

    media::Animation currentSourceAnimationSnapshot() const;
    fs::path suggestedOutputTarget() const; // File path when N==1, output directory when N>1

    // ==================== Character, Styles, Instruction & Results ====================
    void loadCharacterFile(const fs::path& path);
    fs::path characterPath() const;
    media::Image characterImageSnapshot() const;
    void setCharacterCrop(const media::NormalizedRect& crop);
    media::NormalizedRect characterCrop() const;

    size_t addStyleFiles(std::span<const fs::path> paths);
    bool removeStyleAt(size_t index);
    std::vector<fs::path> stylePaths() const;

    void setInstruction(std::string text);
    std::string instruction() const;

    void setResultFiles(std::vector<fs::path> paths);
    std::vector<fs::path> resultPaths() const;
    bool selectResultIndex(size_t index);
    media::Animation currentResultAnimationSnapshot() const;

    // ==================== Settings, Endpoint Override & Plugins ====================
    StudioSettings settingsSnapshot() const;
    void updateSettings(const StudioSettings& next, bool saveToDisk = true);
    net::ProviderConfig effectiveProvider() const;
    std::optional<SessionEndpointOverride> activeEndpointOverride() const;
    void clearEndpointOverride();

    std::vector<PluginManifest> discoverAndListPlugins();
    bool reloadPlugins(const std::vector<std::string>& enabledIds, SignaturePolicy policy, std::string& outError);
    bool isPluginLoaded(std::string_view pluginId) const;
    nlohmann::json activePipelineFiltersJson() const;
    nlohmann::json invokePluginCommand(
        std::string_view pluginId,
        std::string_view commandId,
        const nlohmann::json& args = nlohmann::json::object(),
        void* ownerHwnd = nullptr);

    // ==================== Background Worker Orchestration ====================
    bool isBusy() const noexcept { return workerBusy_.load(std::memory_order_acquire); }
    void startReplaceJob(const fs::path& outputTarget, std::string overrideInstruction);
    void startFetchModels();
    void cancelActiveJob() noexcept;
    void waitForIdle() noexcept;
};

} // namespace meme::core
