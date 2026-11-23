#include "core/studio_session.hpp"
#include "media/wic_codec.hpp"
#include "base/app_paths.hpp"
#include "base/crypto.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"

#include <objbase.h>
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>

namespace meme::core {

namespace {

bool hasSupportedMediaExtension(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    static const std::set<std::wstring> kExts = {
        L".png", L".gif", L".jpg", L".jpeg", L".webp", L".bmp"
    };
    return kExts.contains(ext);
}

std::wstring lowerWide(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

} // namespace

StudioSession::StudioSession(SessionEventCallback onEvent, HostOpenPanelFn onOpenPanel)
    : settings_(loadStudioSettings()),
      openPanelHook_(std::move(onOpenPanel)),
      eventSink_(std::move(onEvent)) {
    std::string ignoreErr;
    reloadPlugins(settings_.enabledPlugins, settings_.signaturePolicy, ignoreErr);
}

StudioSession::~StudioSession() {
    cancelToken_.stop();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    pluginHost_.reset();
}

void StudioSession::setEventCallback(SessionEventCallback cb) {
    std::lock_guard lock(stateMutex_);
    eventSink_ = std::move(cb);
}

void StudioSession::setOpenPanelHook(HostOpenPanelFn hook) {
    std::lock_guard lock(stateMutex_);
    openPanelHook_ = std::move(hook);
}

void StudioSession::emit(SessionEvent ev) const {
    SessionEventCallback cb;
    {
        std::lock_guard lock(stateMutex_);
        cb = eventSink_;
    }
    if (cb) {
        cb(std::move(ev));
    }
}

void StudioSession::decodeSelectedSourceLocked(size_t idx) {
    if (sources_.empty()) {
        currentSourceIdx_ = 0;
        currentSourceAnim_ = {};
        return;
    }
    currentSourceIdx_ = std::min(idx, sources_.size() - 1);
    currentSourceAnim_ = media::decodeAnimation(sources_[currentSourceIdx_].path);
}

size_t StudioSession::importSources(std::span<const fs::path> filesOrFolders, bool append) {
    std::vector<fs::path> expanded;
    for (const auto& item : filesOrFolders) {
        std::error_code ec;
        if (fs::is_directory(item, ec)) {
            std::vector<fs::path> children;
            for (const auto& entry : fs::directory_iterator(item, ec)) {
                if (entry.is_regular_file(ec) && hasSupportedMediaExtension(entry.path())) {
                    children.push_back(entry.path());
                }
            }
            std::sort(children.begin(), children.end(), [](const fs::path& a, const fs::path& b) {
                return lowerWide(a.filename().wstring()) < lowerWide(b.filename().wstring());
            });
            expanded.insert(expanded.end(), children.begin(), children.end());
        } else if (fs::is_regular_file(item, ec)) {
            expanded.push_back(item);
        }
    }

    if (expanded.empty()) {
        throw Error("未在所选路径中找到可读取的图片或 GIF 素材");
    }

    std::vector<SourceQueueEntry> nextQueue;
    std::unordered_set<std::wstring> seenKeys;

    {
        std::lock_guard lock(stateMutex_);
        if (append) {
            for (const auto& ex : sources_) {
                seenKeys.insert(base::canonicalPathKey(ex.path));
                nextQueue.push_back(ex);
            }
        }
    }

    size_t addedCount = 0;
    std::string firstProbeErr;
    for (const auto& candidate : expanded) {
        std::wstring key = base::canonicalPathKey(candidate);
        if (!seenKeys.insert(key).second) {
            continue;
        }
        try {
            auto meta = media::probeMediaMetadata(candidate);
            nextQueue.push_back(SourceQueueEntry{
                candidate,
                meta.width,
                meta.height,
                meta.frameCount,
                meta.animated
            });
            ++addedCount;
        } catch (const std::exception& e) {
            if (firstProbeErr.empty()) firstProbeErr = e.what();
        }
    }

    if (nextQueue.empty()) {
        throw Error(firstProbeErr.empty() ? "没有可用的有效图片素材" : ("读取图片失败：" + firstProbeErr));
    }

    {
        std::lock_guard lock(stateMutex_);
        bool wasEmpty = sources_.empty() || !append;
        sources_ = std::move(nextQueue);
        decodeSelectedSourceLocked(wasEmpty ? 0 : currentSourceIdx_);
    }
    return addedCount;
}

void StudioSession::clearSources() {
    std::lock_guard lock(stateMutex_);
    sources_.clear();
    currentSourceIdx_ = 0;
    currentSourceAnim_ = {};
}

size_t StudioSession::sourceCount() const {
    std::lock_guard lock(stateMutex_);
    return sources_.size();
}

size_t StudioSession::currentSourceIndex() const {
    std::lock_guard lock(stateMutex_);
    return currentSourceIdx_;
}

std::optional<SourceQueueEntry> StudioSession::currentSourceEntry() const {
    std::lock_guard lock(stateMutex_);
    if (sources_.empty() || currentSourceIdx_ >= sources_.size()) return std::nullopt;
    return sources_[currentSourceIdx_];
}

std::vector<SourceQueueEntry> StudioSession::listSources() const {
    std::lock_guard lock(stateMutex_);
    return sources_;
}

bool StudioSession::selectSourceIndex(size_t zeroBasedIndex) {
    std::lock_guard lock(stateMutex_);
    if (zeroBasedIndex >= sources_.size()) return false;
    if (zeroBasedIndex == currentSourceIdx_ && !currentSourceAnim_.frames.empty()) {
        return true;
    }
    decodeSelectedSourceLocked(zeroBasedIndex);
    return true;
}

bool StudioSession::stepSourceIndex(int delta) {
    std::lock_guard lock(stateMutex_);
    if (sources_.empty()) return false;
    int n = static_cast<int>(sources_.size());
    int next = (static_cast<int>(currentSourceIdx_) + (delta % n) + n) % n;
    decodeSelectedSourceLocked(static_cast<size_t>(next));
    return true;
}

media::Animation StudioSession::currentSourceAnimationSnapshot() const {
    std::lock_guard lock(stateMutex_);
    return currentSourceAnim_;
}

fs::path StudioSession::suggestedOutputTarget() const {
    std::lock_guard lock(stateMutex_);
    if (sources_.empty()) return {};
    if (sources_.size() == 1) {
        const auto& s = sources_.front();
        return s.path.parent_path() / (s.path.stem().wstring() + L"-edited" + (s.animated ? L".gif" : L".png"));
    }
    return sources_.front().path.parent_path() / L"MemeStudio-Output";
}

void StudioSession::loadCharacterFile(const fs::path& path) {
    media::Image decoded = media::decodeFirstFrame(path);
    std::lock_guard lock(stateMutex_);
    characterPath_ = path;
    characterImage_ = std::move(decoded);
    characterCrop_ = {0.0, 0.0, 1.0, 1.0};
}

fs::path StudioSession::characterPath() const {
    std::lock_guard lock(stateMutex_);
    return characterPath_;
}

media::Image StudioSession::characterImageSnapshot() const {
    std::lock_guard lock(stateMutex_);
    return characterImage_;
}

void StudioSession::setCharacterCrop(const media::NormalizedRect& crop) {
    crop.validate("人物参考裁剪框");
    std::lock_guard lock(stateMutex_);
    characterCrop_ = crop;
}

media::NormalizedRect StudioSession::characterCrop() const {
    std::lock_guard lock(stateMutex_);
    return characterCrop_;
}

size_t StudioSession::addStyleFiles(std::span<const fs::path> paths) {
    std::lock_guard lock(stateMutex_);
    size_t added = 0;
    for (const auto& p : paths) {
        if (stylePaths_.size() >= 8) break;
        bool dup = std::any_of(stylePaths_.begin(), stylePaths_.end(), [&](const fs::path& ex) {
            return base::isSameFilePath(ex, p);
        });
        if (!dup && fs::is_regular_file(p)) {
            stylePaths_.push_back(p);
            ++added;
        }
    }
    return added;
}

bool StudioSession::removeStyleAt(size_t index) {
    std::lock_guard lock(stateMutex_);
    if (index >= stylePaths_.size()) return false;
    stylePaths_.erase(stylePaths_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::vector<fs::path> StudioSession::stylePaths() const {
    std::lock_guard lock(stateMutex_);
    return stylePaths_;
}

void StudioSession::setInstruction(std::string text) {
    std::lock_guard lock(stateMutex_);
    instruction_ = std::move(text);
}

std::string StudioSession::instruction() const {
    std::lock_guard lock(stateMutex_);
    return instruction_;
}

void StudioSession::setResultFiles(std::vector<fs::path> paths) {
    media::Animation firstAnim;
    if (!paths.empty() && fs::is_regular_file(paths.front())) {
        try { firstAnim = media::decodeAnimation(paths.front()); } catch (...) {}
    }
    std::lock_guard lock(stateMutex_);
    resultPaths_ = std::move(paths);
    currentResultIdx_ = 0;
    currentResultAnim_ = std::move(firstAnim);
}

std::vector<fs::path> StudioSession::resultPaths() const {
    std::lock_guard lock(stateMutex_);
    return resultPaths_;
}

bool StudioSession::selectResultIndex(size_t index) {
    fs::path target;
    {
        std::lock_guard lock(stateMutex_);
        if (index >= resultPaths_.size()) return false;
        target = resultPaths_[index];
    }
    media::Animation decoded = media::decodeAnimation(target);
    std::lock_guard lock(stateMutex_);
    currentResultIdx_ = index;
    currentResultAnim_ = std::move(decoded);
    return true;
}

media::Animation StudioSession::currentResultAnimationSnapshot() const {
    std::lock_guard lock(stateMutex_);
    return currentResultAnim_;
}

StudioSettings StudioSession::settingsSnapshot() const {
    std::lock_guard lock(stateMutex_);
    return settings_;
}

void StudioSession::updateSettings(const StudioSettings& next, bool saveToDisk) {
    {
        std::lock_guard lock(stateMutex_);
        StudioSettings merged = next;
        // Preserve commercial BaseUrl/ApiKey when a non-destructive session endpoint override is active!
        if (activeOverride_.has_value()) {
            merged.provider.baseUrl = settings_.provider.baseUrl;
            merged.provider.apiKey  = settings_.provider.apiKey;
        }
        settings_ = std::move(merged);
    }
    if (saveToDisk) {
        saveStudioSettings(settingsSnapshot());
    }
}

net::ProviderConfig StudioSession::effectiveProvider() const {
    std::lock_guard lock(stateMutex_);
    net::ProviderConfig prov = settings_.provider;
    if (activeOverride_.has_value()) {
        prov.baseUrl = activeOverride_->baseUrl;
        if (!activeOverride_->apiKey.empty()) prov.apiKey = activeOverride_->apiKey;
        if (!activeOverride_->model.empty() && prov.model.empty()) prov.model = activeOverride_->model;
    }
    if (prov.apiKey.empty()) {
        auto envKey = base::readEnvSecret("OPENAI_API_KEY");
        prov.apiKey.assign(envKey.view());
    }
    return prov;
}

std::optional<SessionEndpointOverride> StudioSession::activeEndpointOverride() const {
    std::lock_guard lock(stateMutex_);
    return activeOverride_;
}

void StudioSession::clearEndpointOverride() {
    if (pluginHost_) {
        pluginHost_->clearSessionEndpointOverride();
    }
}

std::vector<PluginManifest> StudioSession::discoverAndListPlugins() {
    auto found = discoverPluginsInDirectory(base::defaultPluginsDir());
    std::lock_guard lock(stateMutex_);
    discoveredPlugins_ = found;
    return found;
}

bool StudioSession::reloadPlugins(
    const std::vector<std::string>& enabledIds,
    SignaturePolicy policy,
    std::string& outError) {
    try {
        auto found = discoverPluginsInDirectory(base::defaultPluginsDir());
        HostOpenPanelFn panelHook;
        {
            std::lock_guard lock(stateMutex_);
            panelHook = openPanelHook_;
        }

        HostUiCallbacks cbs;
        cbs.onLog = [this](int, std::string_view pid, std::string_view msg) {
            SessionEvent ev;
            ev.kind = SessionEvent::Kind::Log;
            ev.message = "[" + std::string(pid) + "] " + std::string(msg);
            emit(std::move(ev));
        };
        cbs.onGetWorkspaceState = [this]() -> nlohmann::json {
            auto curSrc = currentSourceEntry();
            auto outs = resultPaths();
            auto prov = effectiveProvider();
            return nlohmann::json{
                {"source_path", curSrc ? base::wideToUtf8(curSrc->path.wstring()) : ""},
                {"character_path", base::wideToUtf8(characterPath().wstring())},
                {"result_path", outs.empty() ? "" : base::wideToUtf8(outs.front().wstring())},
                {"instruction", instruction()},
                {"provider", {
                    {"base_url", prov.baseUrl},
                    {"api_key", prov.apiKey},
                    {"model", prov.model},
                    {"quality", prov.quality},
                    {"size", prov.size},
                    {"image_field", prov.imageField},
                    {"timeout_seconds", prov.timeoutSeconds}
                }}
            };
        };
        cbs.onEndpointOverrideChanged = [this](const std::optional<SessionEndpointOverride>& ov) {
            {
                std::lock_guard lock(stateMutex_);
                activeOverride_ = ov;
            }
            SessionEvent ev;
            ev.kind = SessionEvent::Kind::EndpointOverrideChanged;
            ev.endpointOverride = ov;
            emit(std::move(ev));
        };
        cbs.onPresentOutputFile = [this](const fs::path& outFile) {
            setResultFiles({outFile});
            SessionEvent ev;
            ev.kind = SessionEvent::Kind::PresentOutputFile;
            ev.presentedFile = outFile;
            emit(std::move(ev));
        };
        cbs.onOpenPanel = std::move(panelHook);

        auto nextHost = std::make_unique<PluginHost>(httpPool_, found, enabledIds, policy, std::move(cbs));
        auto nextOverride = nextHost->sessionEndpointOverride();

        {
            std::lock_guard lock(stateMutex_);
            discoveredPlugins_ = std::move(found);
            pluginHost_ = std::move(nextHost);
            activeOverride_ = nextOverride;
            settings_.enabledPlugins = enabledIds;
            settings_.signaturePolicy = policy;
        }
        saveStudioSettings(settingsSnapshot());
        return true;
    } catch (const std::exception& e) {
        outError = e.what();
        return false;
    }
}

bool StudioSession::isPluginLoaded(std::string_view pluginId) const {
    return pluginHost_ && pluginHost_->isLoaded(pluginId);
}

nlohmann::json StudioSession::activePipelineFiltersJson() const {
    return pluginHost_ ? pluginHost_->pipelineFingerprint() : nlohmann::json::array();
}

nlohmann::json StudioSession::invokePluginCommand(
    std::string_view pluginId,
    std::string_view commandId,
    const nlohmann::json& args,
    void* ownerHwnd) {
    if (!pluginHost_) throw Error("插件宿主未初始化");
    net::CancelToken cmdCancel;
    return pluginHost_->invokeCommand(pluginId, commandId, args, ownerHwnd, cmdCancel);
}

void StudioSession::startReplaceJob(const fs::path& outputTarget, std::string overrideInstruction) {
    if (isBusy()) throw Error("当前已有生成任务正在运行");

    std::vector<SourceQueueEntry> queueSnap;
    ReplaceTaskConfig baseTask;
    {
        std::lock_guard lock(stateMutex_);
        instruction_ = overrideInstruction;
        queueSnap = sources_;
        baseTask.characterPath = characterPath_;
        baseTask.characterCrop = characterCrop_;
        baseTask.stylePaths = stylePaths_;
        baseTask.instruction = instruction_;
        baseTask.copyCount = settings_.copyCount;
        baseTask.frameStride = settings_.frameStride;
        baseTask.maxFrames = settings_.maxFrames;
        baseTask.temporalConsistency = settings_.temporalConsistency;
        baseTask.preserveText = settings_.preserveText;
        baseTask.allowResume = settings_.allowResume;
    }
    baseTask.provider = effectiveProvider();

    if (queueSnap.empty()) throw Error("请先选择至少 1 张源表情素材或导入目录");
    if (baseTask.characterPath.empty()) throw Error("请先选择目标人物参考图");
    if (outputTarget.empty()) throw Error(queueSnap.size() > 1 ? "请指定多源批量输出目录" : "请指定结果保存路径");

    saveStudioSettings(settingsSnapshot());

    if (workerThread_.joinable()) workerThread_.join();
    cancelToken_.reset();
    workerBusy_.store(true, std::memory_order_release);

    PluginHost* activePlugins = pluginHost_.get();
    workerThread_ = std::thread([this, queueSnap = std::move(queueSnap), baseTask = std::move(baseTask), outputTarget, activePlugins]() {
        HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            net::OpenAiImageClient client(httpPool_, [activePlugins](
                std::string_view op, const net::ProviderConfig& prov, const nlohmann::json& meta,
                std::span<const net::UploadPart> ups, net::CancelToken& c,
                const net::ImagePreviewFn& prev, base::Bytes& resp) {
                return activePlugins && activePlugins->tryInterceptApi(op, prov, meta, ups, c, prev, resp);
            });

            std::vector<fs::path> allOutputs;
            if (queueSnap.size() == 1) {
                ReplaceTaskConfig single = baseTask;
                single.sourcePath = queueSnap.front().path;
                single.outputPath = outputTarget;
                allOutputs = runCharacterReplaceJob(single, client, activePlugins, cancelToken_, [this](const ReplaceProgress& prog) {
                    SessionEvent ev;
                    ev.kind = SessionEvent::Kind::Progress;
                    ev.progress = prog;
                    emit(std::move(ev));
                });
            } else {
                // Multi-source queue mode directly inside StudioSession!
                fs::create_directories(outputTarget);
                std::unordered_set<std::wstring> inputKeys;
                for (const auto& s : queueSnap) inputKeys.insert(base::canonicalPathKey(s.path));
                inputKeys.insert(base::canonicalPathKey(baseTask.characterPath));
                for (const auto& st : baseTask.stylePaths) inputKeys.insert(base::canonicalPathKey(st));

                std::unordered_set<std::wstring> usedOutputStems;
                int totalItems = static_cast<int>(queueSnap.size());
                for (int i = 0; i < totalItems; ++i) {
                    cancelToken_.verify();
                    const auto& srcItem = queueSnap[static_cast<size_t>(i)];
                    std::wstring stem = srcItem.path.stem().wstring();
                    if (stem.empty()) stem = L"meme";
                    std::wstring ext = srcItem.animated ? L".gif" : L".png";

                    std::wstring outName = stem + L"-edited";
                    if (!usedOutputStems.insert(lowerWide(outName)).second) {
                        std::string pUtf8 = base::wideToUtf8(srcItem.path.wstring());
                        std::string shortHash = base::sha256Hex(std::span<const uint8_t>(
                            reinterpret_cast<const uint8_t*>(pUtf8.data()), pUtf8.size())).substr(0, 6);
                        outName = stem + L"_" + base::utf8ToWideStrict(shortHash) + L"-edited";
                        usedOutputStems.insert(lowerWide(outName));
                    }

                    fs::path itemOutPath = outputTarget / (outName + ext);
                    if (inputKeys.contains(base::canonicalPathKey(itemOutPath))) {
                        throw Error("多源输出文件路径与输入素材冲突：" + base::wideToUtf8(itemOutPath.wstring()));
                    }

                    ReplaceTaskConfig itemTask = baseTask;
                    itemTask.sourcePath = srcItem.path;
                    itemTask.outputPath = itemOutPath;

                    std::string prefix = "[" + std::to_string(i + 1) + "/" + std::to_string(totalItems) + "] ";
                    auto subOutputs = runCharacterReplaceJob(itemTask, client, activePlugins, cancelToken_,
                        [this, i, totalItems, &prefix](const ReplaceProgress& subProg) {
                            SessionEvent ev;
                            ev.kind = SessionEvent::Kind::Progress;
                            ev.progress.completedFrames = i;
                            ev.progress.totalFrames = totalItems;
                            ev.progress.previewImage = subProg.previewImage;
                            ev.progress.messageUtf8 = prefix + subProg.messageUtf8;
                            emit(std::move(ev));
                        });
                    allOutputs.insert(allOutputs.end(), subOutputs.begin(), subOutputs.end());
                }
            }

            setResultFiles(allOutputs);
            workerBusy_.store(false, std::memory_order_release);

            SessionEvent doneEv;
            doneEv.kind = SessionEvent::Kind::JobSucceeded;
            doneEv.outputs = std::move(allOutputs);
            emit(std::move(doneEv));
        } catch (const std::exception& e) {
            workerBusy_.store(false, std::memory_order_release);
            SessionEvent failEv;
            failEv.kind = SessionEvent::Kind::JobFailed;
            failEv.message = e.what();
            emit(std::move(failEv));
        }
        if (SUCCEEDED(comHr)) CoUninitialize();
    });
}

void StudioSession::startFetchModels() {
    if (isBusy()) throw Error("当前已有后台任务正在运行");
    net::ProviderConfig prov = effectiveProvider();

    if (workerThread_.joinable()) workerThread_.join();
    cancelToken_.reset();
    workerBusy_.store(true, std::memory_order_release);

    PluginHost* activePlugins = pluginHost_.get();
    workerThread_ = std::thread([this, prov = std::move(prov), activePlugins]() {
        try {
            net::OpenAiImageClient client(httpPool_, [activePlugins](
                std::string_view op, const net::ProviderConfig& p, const nlohmann::json& m,
                std::span<const net::UploadPart> u, net::CancelToken& c,
                const net::ImagePreviewFn& prev, base::Bytes& r) {
                return activePlugins && activePlugins->tryInterceptApi(op, p, m, u, c, prev, r);
            });
            auto fetched = client.listModels(prov, cancelToken_);
            workerBusy_.store(false, std::memory_order_release);

            SessionEvent ev;
            ev.kind = SessionEvent::Kind::ModelsFetched;
            ev.models = std::move(fetched);
            emit(std::move(ev));
        } catch (const std::exception& e) {
            workerBusy_.store(false, std::memory_order_release);
            SessionEvent failEv;
            failEv.kind = SessionEvent::Kind::JobFailed;
            failEv.message = e.what();
            emit(std::move(failEv));
        }
    });
}

void StudioSession::cancelActiveJob() noexcept {
    cancelToken_.stop();
}

void StudioSession::waitForIdle() noexcept {
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

} // namespace meme::core
