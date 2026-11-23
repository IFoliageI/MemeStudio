#include "gui/studio_window.hpp"
#include "gui/plugin_dialog.hpp"
#include "gui/presets_dialog.hpp"
#include "core/studio_session.hpp"
#include "uikit/controls.hpp"
#include "uikit/dpi.hpp"
#include "uikit/file_dialogs.hpp"
#include "uikit/image_canvas.hpp"
#include "uikit/panel_manager.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <charconv>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace meme::gui {

namespace {

constexpr const wchar_t* kMainClass = L"MemeStudioMainWindowV2";
constexpr UINT WM_SESSION_EVENT = WM_APP + 10;
constexpr UINT_PTR kAnimTimerId = 1;

enum MainControlId : int {
    IdPickSourceFiles    = 1001,
    IdPickSourceFolder   = 1002,
    IdPrevSourceBtn      = 1003,
    IdSourceIndexEdit    = 1004,
    IdNextSourceBtn      = 1005,
    IdJumpSourceCommit   = 1006,

    IdPickCharacter      = 1010,
    IdResetCrop          = 1011,
    IdAddStyle           = 1012,
    IdRemoveStyle        = 1013,
    IdStyleList          = 1014,
    IdOpenPresets        = 1015,
    IdInstructionEdit    = 1016,

    IdViewModeCombo      = 1020,
    IdCopySelectCombo    = 1021,
    IdOpenOutputBtn      = 1022,
    IdOpenFolderBtn      = 1023,
    IdPlayPauseBtn       = 1024,
    IdFrameScrubber      = 1025,
    IdOutputEdit         = 1026,
    IdBrowseOutput       = 1027,
    IdStartGenerate      = 1028,
    IdCancelGenerate     = 1029,

    IdDisconnectOverride = 1040,
    IdBaseUrlEdit        = 1041,
    IdApiKeyEdit         = 1042,
    IdRememberKeyCheck   = 1043,
    IdModelCombo         = 1044,
    IdFetchModelsBtn     = 1045,
    IdQualityCombo       = 1046,
    IdSizeCombo          = 1047,
    IdOutputFormatCombo  = 1048,
    IdStreamCheck        = 1049,
    IdCopyCountEdit      = 1050,
    IdStrideEdit         = 1051,
    IdMaxFramesEdit      = 1052,
    IdConsistencyCheck   = 1053,
    IdPreserveTextCheck  = 1054,
    IdResumeCheck        = 1055,
    IdManagePluginsBtn   = 1060,

    IdPluginToolbarBase  = 1500
};

struct DynamicToolbarAction {
    std::string pluginId;
    std::string commandId;
    std::string title;
    HWND button = nullptr;
};

// Pure thin Win32 View driving the UI-independent core::StudioSession backend!
struct StudioAppWindow {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    uikit::DpiContext dpi;
    uikit::PanelManager panels;
    std::unique_ptr<core::StudioSession> session;

    std::vector<DynamicToolbarAction> toolbarActions;

    // Canvases & cached active animation copy for 60fps UI timer playback
    uikit::ImageCanvas sourceCanvas;
    uikit::ImageCanvas characterCanvas;
    uikit::ImageCanvas mainCanvas;

    media::Animation cachedSourceAnim;
    media::Image cachedCharacterImg;
    media::Animation cachedResultAnim;

    bool showingResultView = false;
    bool playingAnim = false;
    size_t currentFrameIdx = 0;
    bool updatingSourceNavControls = false;

    // Controls
    HWND headerTitle = nullptr;
    HWND managePluginsBtn = nullptr;

    // Left column (Source + <- [1]/N -> switcher + Character + Styles + Instruction)
    HWND lblSource = nullptr, pickSourceFilesBtn = nullptr, pickSourceDirBtn = nullptr;
    HWND prevSourceBtn = nullptr, sourceIndexEdit = nullptr, lblSourceSlashTotal = nullptr, nextSourceBtn = nullptr;
    HWND lblCharacter = nullptr, pickCharBtn = nullptr, resetCropBtn = nullptr;
    HWND lblStyles = nullptr, addStyleBtn = nullptr, removeStyleBtn = nullptr, styleList = nullptr;
    HWND lblInstruction = nullptr, presetsBtn = nullptr, instructionEdit = nullptr;

    // Center column
    HWND viewModeCombo = nullptr, copyCombo = nullptr, openOutBtn = nullptr, openDirBtn = nullptr;
    HWND playBtn = nullptr, scrubber = nullptr, frameInfoLabel = nullptr;
    HWND lblOutput = nullptr, outputEdit = nullptr, browseOutBtn = nullptr;
    HWND progressBar = nullptr, statusLabel = nullptr, startBtn = nullptr, cancelBtn = nullptr;

    // Right column
    HWND overrideBannerLabel = nullptr, disconnectOverrideBtn = nullptr;
    HWND lblBaseUrl = nullptr, baseUrlEdit = nullptr;
    HWND lblKey = nullptr, keyEdit = nullptr, rememberCheck = nullptr;
    HWND lblModel = nullptr, modelCombo = nullptr, fetchModelsBtn = nullptr;
    HWND lblQuality = nullptr, qualityCombo = nullptr, lblSize = nullptr, sizeCombo = nullptr;
    HWND lblFormat = nullptr, formatCombo = nullptr, streamCheck = nullptr;
    HWND lblCopies = nullptr, copyCountEdit = nullptr;
    HWND lblStride = nullptr, strideEdit = nullptr, lblMaxFrames = nullptr, maxFramesEdit = nullptr;
    HWND consistencyCheck = nullptr, preserveTextCheck = nullptr, resumeCheck = nullptr;
    HWND activeFiltersLabel = nullptr, logEdit = nullptr;

    std::mutex eventMutex;
    std::deque<core::SessionEvent> pendingEvents;
    bool closeAfterCancel = false;

    explicit StudioAppWindow(HINSTANCE inst)
        : instance(inst), panels(inst) {}

    ~StudioAppWindow() {
        stopPlaybackTimer();
        session.reset();
    }

    void enqueueSessionEvent(core::SessionEvent ev) {
        {
            std::lock_guard lock(eventMutex);
            pendingEvents.push_back(std::move(ev));
        }
        if (hwnd && IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_SESSION_EVENT, 0, 0);
        }
    }

    void appendLog(std::string_view lineUtf8) {
        if (!logEdit) return;
        std::string cur = uikit::getTextUtf8(logEdit);
        if (cur.size() > 16000) cur.erase(0, cur.size() - 12000);
        if (!cur.empty()) cur += "\r\n";
        cur += lineUtf8;
        uikit::setTextUtf8(logEdit, cur);
        SendMessageW(logEdit, EM_SETSEL, 0xFFFF, 0xFFFF);
        SendMessageW(logEdit, EM_SCROLLCARET, 0, 0);
    }

    const media::Animation& activeMainAnimation() const noexcept {
        return (showingResultView && !cachedResultAnim.frames.empty()) ? cachedResultAnim : cachedSourceAnim;
    }

    void stopPlaybackTimer() noexcept {
        if (hwnd && playingAnim) {
            KillTimer(hwnd, kAnimTimerId);
        }
        playingAnim = false;
        if (playBtn) uikit::setTextUtf8(playBtn, "▶ 播放");
    }

    void scheduleNextFrameTimer() noexcept {
        const auto& anim = activeMainAnimation();
        if (!hwnd || !anim.animated || anim.frames.size() <= 1) {
            stopPlaybackTimer();
            return;
        }
        playingAnim = true;
        if (playBtn) uikit::setTextUtf8(playBtn, "⏸ 暂停");
        uint32_t delay = std::clamp<uint32_t>(anim.frames[currentFrameIdx % anim.frames.size()].delayMs, 20u, 10000u);
        SetTimer(hwnd, kAnimTimerId, delay, nullptr);
    }

    void updateMainPreviewFrame() {
        const auto& anim = activeMainAnimation();
        if (anim.frames.empty()) {
            mainCanvas.clearImage();
            uikit::setTextUtf8(frameInfoLabel, "未加载素材");
            EnableWindow(playBtn, FALSE);
            EnableWindow(scrubber, FALSE);
            return;
        }
        if (currentFrameIdx >= anim.frames.size()) currentFrameIdx = 0;
        mainCanvas.setImage(&anim.frames[currentFrameIdx].image);

        SendMessageW(scrubber, TBM_SETRANGE, TRUE, MAKELPARAM(0, std::max<int>(0, int(anim.frames.size()) - 1)));
        SendMessageW(scrubber, TBM_SETPOS, TRUE, static_cast<LPARAM>(currentFrameIdx));

        std::string info = std::to_string(currentFrameIdx + 1) + " / " + std::to_string(anim.frames.size()) + " 帧";
        if (anim.animated) {
            info += " · " + std::to_string(anim.frames[currentFrameIdx].delayMs) + " ms";
        }
        uikit::setTextUtf8(frameInfoLabel, info);
        EnableWindow(playBtn, anim.animated ? TRUE : FALSE);
        EnableWindow(scrubber, anim.animated ? TRUE : FALSE);
    }

    // Updates <- [ 1 ] / N -> bar, source preview canvas, main preview canvas, and output label!
    void refreshSourceNavigationAndPreview(bool updateSuggestedOutput) {
        if (!session) return;
        size_t total = session->sourceCount();
        size_t curZeroIdx = session->currentSourceIndex();
        auto curEntry = session->currentSourceEntry();

        updatingSourceNavControls = true;
        if (total == 0) {
            uikit::setTextUtf8(lblSource, "源表情素材");
            uikit::setTextUtf8(sourceIndexEdit, "0");
            uikit::setTextUtf8(lblSourceSlashTotal, "/ 0");
            EnableWindow(prevSourceBtn, FALSE);
            EnableWindow(sourceIndexEdit, FALSE);
            EnableWindow(nextSourceBtn, FALSE);
            cachedSourceAnim = {};
            sourceCanvas.clearImage();
        } else {
            std::string fn = curEntry ? base::wideToUtf8(curEntry->path.filename().wstring()) : "";
            uikit::setTextUtf8(lblSource, "源表情 (" + fn + ")");
            uikit::setTextUtf8(sourceIndexEdit, std::to_string(curZeroIdx + 1));
            uikit::setTextUtf8(lblSourceSlashTotal, "/ " + std::to_string(total));
            bool multi = (total > 1);
            EnableWindow(prevSourceBtn, multi ? TRUE : FALSE);
            EnableWindow(sourceIndexEdit, multi ? TRUE : FALSE);
            EnableWindow(nextSourceBtn, multi ? TRUE : FALSE);

            cachedSourceAnim = session->currentSourceAnimationSnapshot();
            if (!cachedSourceAnim.frames.empty()) {
                sourceCanvas.setImage(&cachedSourceAnim.frames.front().image);
            } else {
                sourceCanvas.clearImage();
            }
        }
        updatingSourceNavControls = false;

        uikit::setTextUtf8(lblOutput, total > 1 ? "输出目录" : "保存路径");
        if (updateSuggestedOutput && total > 0) {
            uikit::setTextUtf8(outputEdit, base::wideToUtf8(session->suggestedOutputTarget().wstring()));
        }

        if (!showingResultView) {
            currentFrameIdx = 0;
            updateMainPreviewFrame();
            if (cachedSourceAnim.animated) scheduleNextFrameTimer();
            else stopPlaybackTimer();
        }
    }

    void importSourcePathsFromUi(const std::vector<std::filesystem::path>& paths, bool append = false) {
        if (paths.empty() || !session) return;
        try {
            size_t imported = session->importSources(paths, append);
            showingResultView = false;
            SendMessageW(viewModeCombo, CB_SETCURSEL, 0, 0);
            refreshSourceNavigationAndPreview(true);
            appendLog("已载入源表情队列：新增 " + std::to_string(imported) +
                      " 项（当前共 " + std::to_string(session->sourceCount()) + " 项）");
        } catch (const std::exception& e) {
            uikit::showErrorBox(hwnd, "载入源表情失败", e.what());
        }
    }

    void commitJumpSourceIndexFromEdit() {
        if (!session || updatingSourceNavControls) return;
        size_t total = session->sourceCount();
        if (total == 0) return;

        std::string raw = base::trimAscii(uikit::getTextUtf8(sourceIndexEdit));
        int parsedOneBased = 0;
        auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), parsedOneBased);
        if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
            // Restore valid current index display
            updatingSourceNavControls = true;
            uikit::setTextUtf8(sourceIndexEdit, std::to_string(session->currentSourceIndex() + 1));
            updatingSourceNavControls = false;
            return;
        }
        int clampedOneBased = std::clamp<int>(parsedOneBased, 1, static_cast<int>(total));
        try {
            session->selectSourceIndex(static_cast<size_t>(clampedOneBased - 1));
            showingResultView = false;
            SendMessageW(viewModeCombo, CB_SETCURSEL, 0, 0);
            refreshSourceNavigationAndPreview(total == 1);
        } catch (const std::exception& e) {
            uikit::showErrorBox(hwnd, "切换预览图片失败", e.what());
        }
    }

    void loadCharacterFromUi(const std::filesystem::path& path) {
        if (!session) return;
        try {
            session->loadCharacterFile(path);
            cachedCharacterImg = session->characterImageSnapshot();
            characterCanvas.setCropRect({0.0, 0.0, 1.0, 1.0});
            characterCanvas.setImage(&cachedCharacterImg);
            appendLog("已载入人物参考图：" + base::wideToUtf8(path.filename().wstring()));
        } catch (const std::exception& e) {
            uikit::showErrorBox(hwnd, "载入人物参考图失败", e.what());
        }
    }

    void syncControlsIntoSession(bool persist) {
        if (!session) return;
        auto st = session->settingsSnapshot();
        if (!session->activeEndpointOverride().has_value()) {
            st.provider.baseUrl = base::trimAscii(uikit::getTextUtf8(baseUrlEdit));
            st.provider.apiKey  = base::trimAscii(uikit::getTextUtf8(keyEdit));
        }
        st.rememberKey = uikit::isChecked(rememberCheck);
        st.provider.model = base::trimAscii(uikit::getTextUtf8(modelCombo));
        st.provider.quality = uikit::getTextUtf8(qualityCombo);
        st.provider.size = uikit::getTextUtf8(sizeCombo);
        std::string fmt = uikit::getTextUtf8(formatCombo);
        st.provider.outputFormat = (fmt == "默认 (不传参)") ? "" : fmt;
        st.provider.stream = uikit::isChecked(streamCheck);
        st.copyCount = uikit::parseBoundedInt(copyCountEdit, "生成份数", 1, 10);
        st.frameStride = uikit::parseBoundedInt(strideEdit, "抽帧步长", 1, 600);
        st.maxFrames = uikit::parseBoundedInt(maxFramesEdit, "最大帧数", 0, 600);
        st.temporalConsistency = uikit::isChecked(consistencyCheck);
        st.preserveText = uikit::isChecked(preserveTextCheck);
        st.allowResume = uikit::isChecked(resumeCheck);

        session->setCharacterCrop(characterCanvas.cropRect());
        session->setInstruction(uikit::getTextUtf8(instructionEdit));
        session->updateSettings(st, persist);
    }

    void refreshEndpointOverrideUi() {
        auto ov = session ? session->activeEndpointOverride() : std::nullopt;
        bool overridden = ov.has_value();
        bool busy = session && session->isBusy();
        if (overridden) {
            uikit::setTextUtf8(overrideBannerLabel, "⚡ 当前会话连接已由插件「" + ov->pluginId + "」接管");
        }
        ShowWindow(overrideBannerLabel, overridden ? SW_SHOW : SW_HIDE);
        ShowWindow(disconnectOverrideBtn, overridden ? SW_SHOW : SW_HIDE);
        EnableWindow(baseUrlEdit, (!overridden && !busy) ? TRUE : FALSE);
        EnableWindow(keyEdit, (!overridden && !busy) ? TRUE : FALSE);
        relayout();
    }

    void rebuildDynamicPluginToolbar() {
        for (auto& act : toolbarActions) {
            if (act.button && IsWindow(act.button)) DestroyWindow(act.button);
        }
        toolbarActions.clear();

        std::string filterSummary = "已挂载管线滤镜：无";
        if (session) {
            auto fp = session->activePipelineFiltersJson();
            if (!fp.empty()) {
                filterSummary = "已启用管线滤镜：";
                for (size_t i = 0; i < fp.size(); ++i) {
                    if (i > 0) filterSummary += "、";
                    filterSummary += fp[i].value("id", "");
                }
            }

            int nextId = IdPluginToolbarBase;
            for (const auto& m : session->discoverAndListPlugins()) {
                if (!session->isPluginLoaded(m.id)) continue;
                for (const auto& cmd : m.commands) {
                    if (!cmd.showInToolbar) continue;
                    std::wstring wtitle = base::utf8ToWideLossy(cmd.title);
                    HWND btn = CreateWindowExW(
                        0, L"BUTTON", wtitle.c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        0, 0, 10, 10, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(nextId++)),
                        instance, nullptr);
                    SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(dpi.normalFont()), TRUE);
                    toolbarActions.push_back(DynamicToolbarAction{m.id, cmd.id, cmd.title, btn});
                }
            }
        }
        if (activeFiltersLabel) uikit::setTextUtf8(activeFiltersLabel, filterSummary);
        relayout();
    }

    void setBusyUi(bool busy) {
        EnableWindow(startBtn, busy ? FALSE : TRUE);
        EnableWindow(cancelBtn, busy ? TRUE : FALSE);
        EnableWindow(fetchModelsBtn, busy ? FALSE : TRUE);
        EnableWindow(managePluginsBtn, busy ? FALSE : TRUE);
        refreshEndpointOverrideUi();
    }

    void populateResultsAndSwitchView(const std::vector<std::filesystem::path>& outs) {
        SendMessageW(copyCombo, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < outs.size(); ++i) {
            std::wstring label = std::to_wstring(i + 1) + L". " + outs[i].filename().wstring();
            SendMessageW(copyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (!outs.empty()) {
            SendMessageW(copyCombo, CB_SETCURSEL, 0, 0);
        }
        ShowWindow(copyCombo, outs.size() > 1 ? SW_SHOW : SW_HIDE);
        EnableWindow(openOutBtn, outs.empty() ? FALSE : TRUE);
        EnableWindow(openDirBtn, outs.empty() ? FALSE : TRUE);

        if (!outs.empty()) {
            cachedResultAnim = session->currentResultAnimationSnapshot();
            showingResultView = true;
            SendMessageW(viewModeCombo, CB_SETCURSEL, 1, 0);
            currentFrameIdx = 0;
            updateMainPreviewFrame();
            if (cachedResultAnim.animated) scheduleNextFrameTimer();
            else stopPlaybackTimer();
        }
    }

    void processQueuedSessionEvents() {
        std::deque<core::SessionEvent> batch;
        {
            std::lock_guard lock(eventMutex);
            batch.swap(pendingEvents);
        }
        using K = core::SessionEvent::Kind;
        for (auto& ev : batch) {
            switch (ev.kind) {
                case K::Progress: {
                    uikit::setTextUtf8(statusLabel, ev.progress.messageUtf8);
                    if (ev.progress.totalFrames > 0) {
                        int pct = (ev.progress.completedFrames * 100) / ev.progress.totalFrames;
                        SendMessageW(progressBar, PBM_SETPOS, std::clamp(pct, 0, 100), 0);
                    }
                    if (ev.progress.previewImage && !ev.progress.previewImage->empty()) {
                        mainCanvas.setImage(ev.progress.previewImage.get());
                    }
                    break;
                }
                case K::JobSucceeded: {
                    session->waitForIdle();
                    setBusyUi(false);
                    SendMessageW(progressBar, PBM_SETPOS, 100, 0);
                    populateResultsAndSwitchView(ev.outputs);
                    if (!ev.outputs.empty()) {
                        std::string msg = ev.outputs.size() == 1
                            ? ("生成完成：" + base::wideToUtf8(ev.outputs.front().wstring()))
                            : ("批量生成完成（共 " + std::to_string(ev.outputs.size()) + " 个结果文件）");
                        uikit::setTextUtf8(statusLabel, msg);
                        appendLog(msg);
                    }
                    if (closeAfterCancel) DestroyWindow(hwnd);
                    break;
                }
                case K::ModelsFetched: {
                    session->waitForIdle();
                    setBusyUi(false);
                    std::string previous = uikit::getTextUtf8(modelCombo);
                    SendMessageW(modelCombo, CB_RESETCONTENT, 0, 0);
                    for (const auto& m : ev.models) {
                        std::wstring wm = base::utf8ToWideStrict(m);
                        SendMessageW(modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wm.c_str()));
                    }
                    uikit::setTextUtf8(modelCombo, previous.empty() && !ev.models.empty() ? ev.models.front() : previous);
                    std::string msg = "已获取 " + std::to_string(ev.models.size()) + " 个可用模型";
                    uikit::setTextUtf8(statusLabel, msg);
                    appendLog(msg);
                    if (closeAfterCancel) DestroyWindow(hwnd);
                    break;
                }
                case K::JobFailed: {
                    session->waitForIdle();
                    setBusyUi(false);
                    std::string msg = "任务中断：" + ev.message;
                    uikit::setTextUtf8(statusLabel, msg);
                    appendLog(msg);
                    if (!closeAfterCancel && ev.message.find("取消") == std::string::npos) {
                        uikit::showErrorBox(hwnd, "MemeStudio", ev.message);
                    }
                    if (closeAfterCancel) DestroyWindow(hwnd);
                    break;
                }
                case K::EndpointOverrideChanged:
                    refreshEndpointOverrideUi();
                    break;
                case K::PresentOutputFile:
                    populateResultsAndSwitchView(session->resultPaths());
                    break;
                case K::Log:
                    appendLog(ev.message);
                    break;
            }
        }
    }

    void relayout() {
        if (!hwnd || !session) return;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int totalW = std::max<int>(dpi.px(980), rc.right - rc.left);
        int totalH = std::max<int>(dpi.px(700), rc.bottom - rc.top);

        const auto& d = dpi;
        auto st = session->settingsSnapshot();
        int pad = d.px(12);
        int topBarH = d.px(46);
        int contentTop = topBarH + d.px(6);
        int contentH = totalH - contentTop - pad;

        int leftW = d.px(st.leftPaneDip);
        int rightW = d.px(st.rightPaneDip);
        int centerX = pad + leftW + pad;
        int centerW = std::max(d.px(360), totalW - leftW - rightW - pad * 4);
        int rightX = centerX + centerW + pad;

        uikit::DeferBatch batch(64);

        // Top Header & Dynamic Plugin Toolbar
        batch.move(headerTitle, pad, d.px(11), d.px(180), d.px(26));
        int manageW = d.px(104);
        batch.move(managePluginsBtn, totalW - pad - manageW, d.px(9), manageW, d.px(30));

        int tbX = totalW - pad - manageW - d.px(8);
        for (auto it = toolbarActions.rbegin(); it != toolbarActions.rend(); ++it) {
            int btnW = d.px(114);
            tbX -= (btnW + d.px(6));
            if (tbX >= d.px(195)) {
                ShowWindow(it->button, SW_SHOW);
                batch.move(it->button, tbX, d.px(9), btnW, d.px(30));
            } else {
                ShowWindow(it->button, SW_HIDE);
            }
        }

        // Left Column: Source + <- [ 1 ] / N -> Nav Bar + Character + Styles + Instruction
        int ly = contentTop;
        batch.move(lblSource, pad, ly, leftW - d.px(142), d.px(22));
        batch.move(pickSourceFilesBtn, pad + leftW - d.px(138), ly - d.px(2), d.px(76), d.px(26));
        batch.move(pickSourceDirBtn,   pad + leftW - d.px(58),  ly - d.px(2), d.px(58), d.px(26));
        ly += d.px(26);
        int srcCanvasH = d.px(122);
        batch.move(sourceCanvas.hwnd(), pad, ly, leftW, srcCanvasH);
        ly += srcCanvasH + d.px(4);

        // Source Switcher Bar: [ <- ]  [  1  ] / N  [ -> ]
        int navBtnW = d.px(42);
        int navEditW = d.px(56);
        int navTotalW = d.px(58);
        int navGroupW = navBtnW + d.px(6) + navEditW + d.px(4) + navTotalW + d.px(6) + navBtnW;
        int navStartX = pad + std::max(0, (leftW - navGroupW) / 2);
        batch.move(prevSourceBtn,       navStartX, ly, navBtnW, d.px(24));
        batch.move(sourceIndexEdit,     navStartX + navBtnW + d.px(6), ly, navEditW, d.px(24));
        batch.move(lblSourceSlashTotal, navStartX + navBtnW + d.px(6) + navEditW + d.px(4), ly + d.px(3), navTotalW, d.px(20));
        batch.move(nextSourceBtn,       navStartX + navGroupW - navBtnW, ly, navBtnW, d.px(24));
        ly += d.px(32);

        batch.move(lblCharacter, pad, ly, d.px(110), d.px(22));
        batch.move(pickCharBtn,  pad + leftW - d.px(148), ly - d.px(2), d.px(70), d.px(26));
        batch.move(resetCropBtn, pad + leftW - d.px(74),  ly - d.px(2), d.px(74), d.px(26));
        ly += d.px(26);
        int charCanvasH = d.px(134);
        batch.move(characterCanvas.hwnd(), pad, ly, leftW, charCanvasH);
        ly += charCanvasH + d.px(8);

        batch.move(lblStyles, pad, ly, d.px(120), d.px(22));
        batch.move(addStyleBtn,    pad + leftW - d.px(116), ly - d.px(2), d.px(56), d.px(26));
        batch.move(removeStyleBtn, pad + leftW - d.px(56),  ly - d.px(2), d.px(56), d.px(26));
        ly += d.px(26);
        batch.move(styleList, pad, ly, leftW, d.px(64));
        ly += d.px(72);

        batch.move(lblInstruction, pad, ly, d.px(120), d.px(22));
        batch.move(presetsBtn, pad + leftW - d.px(72), ly - d.px(2), d.px(72), d.px(26));
        ly += d.px(26);
        int instH = std::max(d.px(60), contentTop + contentH - ly);
        batch.move(instructionEdit, pad, ly, leftW, instH);

        // Center Column
        int cy = contentTop;
        batch.move(viewModeCombo, centerX, cy, d.px(132), d.px(160));
        batch.move(copyCombo,     centerX + d.px(140), cy, d.px(148), d.px(220));
        batch.move(openDirBtn,    centerX + centerW - d.px(76),  cy, d.px(76), d.px(26));
        batch.move(openOutBtn,    centerX + centerW - d.px(158), cy, d.px(76), d.px(26));
        cy += d.px(32);

        int bottomPanelH = d.px(138);
        int mainCanvasH = std::max(d.px(220), contentH - d.px(32) - bottomPanelH);
        batch.move(mainCanvas.hwnd(), centerX, cy, centerW, mainCanvasH);
        cy += mainCanvasH + d.px(8);

        batch.move(playBtn,        centerX, cy, d.px(76), d.px(26));
        batch.move(scrubber,       centerX + d.px(82), cy, std::max(d.px(80), centerW - d.px(222)), d.px(26));
        batch.move(frameInfoLabel, centerX + centerW - d.px(134), cy + d.px(3), d.px(134), d.px(22));
        cy += d.px(34);

        batch.move(lblOutput,    centerX, cy + d.px(3), d.px(68), d.px(22));
        batch.move(outputEdit,   centerX + d.px(68), cy, centerW - d.px(112), d.px(26));
        batch.move(browseOutBtn, centerX + centerW - d.px(38), cy, d.px(38), d.px(26));
        cy += d.px(34);

        batch.move(progressBar, centerX, cy + d.px(4), centerW - d.px(196), d.px(24));
        batch.move(startBtn,    centerX + centerW - d.px(188), cy, d.px(112), d.px(32));
        batch.move(cancelBtn,   centerX + centerW - d.px(70),  cy, d.px(70),  d.px(32));
        cy += d.px(36);
        batch.move(statusLabel, centerX, cy, centerW, d.px(22));

        // Right Column
        int ry = contentTop;
        if (session->activeEndpointOverride().has_value()) {
            batch.move(overrideBannerLabel,   rightX, ry + d.px(3), rightW - d.px(82), d.px(24));
            batch.move(disconnectOverrideBtn, rightX + rightW - d.px(78), ry, d.px(78), d.px(26));
            ry += d.px(32);
        }

        batch.move(lblBaseUrl,  rightX, ry, rightW, d.px(20)); ry += d.px(20);
        batch.move(baseUrlEdit, rightX, ry, rightW, d.px(26)); ry += d.px(32);

        batch.move(lblKey,        rightX, ry, d.px(120), d.px(20));
        batch.move(rememberCheck, rightX + rightW - d.px(130), ry, d.px(130), d.px(20));
        ry += d.px(22);
        batch.move(keyEdit, rightX, ry, rightW, d.px(26)); ry += d.px(32);

        batch.move(lblModel,       rightX, ry, rightW, d.px(20)); ry += d.px(20);
        batch.move(modelCombo,     rightX, ry, rightW - d.px(78), d.px(220));
        batch.move(fetchModelsBtn, rightX + rightW - d.px(72), ry, d.px(72), d.px(26));
        ry += d.px(34);

        int halfW = (rightW - d.px(8)) / 2;
        batch.move(lblQuality,   rightX, ry, halfW, d.px(20));
        batch.move(lblSize,      rightX + halfW + d.px(8), ry, halfW, d.px(20));
        ry += d.px(20);
        batch.move(qualityCombo, rightX, ry, halfW, d.px(180));
        batch.move(sizeCombo,    rightX + halfW + d.px(8), ry, halfW, d.px(180));
        ry += d.px(32);

        batch.move(lblFormat,     rightX, ry, halfW, d.px(20));
        batch.move(lblCopies,     rightX + halfW + d.px(8), ry, halfW, d.px(20));
        ry += d.px(20);
        batch.move(formatCombo,   rightX, ry, halfW, d.px(160));
        batch.move(copyCountEdit, rightX + halfW + d.px(8), ry, halfW, d.px(26));
        ry += d.px(32);

        batch.move(streamCheck, rightX, ry, rightW, d.px(22));
        ry += d.px(28);

        batch.move(lblStride,     rightX, ry, halfW, d.px(20));
        batch.move(lblMaxFrames,  rightX + halfW + d.px(8), ry, halfW, d.px(20));
        ry += d.px(20);
        batch.move(strideEdit,    rightX, ry, halfW, d.px(26));
        batch.move(maxFramesEdit, rightX + halfW + d.px(8), ry, halfW, d.px(26));
        ry += d.px(32);

        batch.move(consistencyCheck,  rightX, ry, halfW, d.px(22));
        batch.move(preserveTextCheck, rightX + halfW + d.px(8), ry, halfW, d.px(22));
        ry += d.px(24);
        batch.move(resumeCheck, rightX, ry, rightW, d.px(22));
        ry += d.px(28);

        batch.move(activeFiltersLabel, rightX, ry, rightW, d.px(22));
        ry += d.px(24);
        int logH = std::max(d.px(64), contentTop + contentH - ry);
        batch.move(logEdit, rightX, ry, rightW, logH);
    }

    void createControls() {
        dpi.updateForWindow(hwnd);
        session = std::make_unique<core::StudioSession>(
            [this](core::SessionEvent ev) { enqueueSessionEvent(std::move(ev)); },
            [this](std::string_view key, void* owner, int32_t wDip, int32_t hDip, std::string_view title,
                   intptr_t(MEMESTUDIO_CALL* proc)(void*, void*, uint32_t, uintptr_t, intptr_t),
                   void* ctx, void(MEMESTUDIO_CALL* destroyFn)(void*)) {
                return panels.openOrActivatePluginPanel(key, owner ? owner : hwnd, wDip, hDip, title, proc, ctx, destroyFn);
            });
        auto settings = session->settingsSnapshot();

        auto mkLabel = [&](const wchar_t* text, DWORD extraStyle = SS_ENDELLIPSIS) {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | extraStyle,
                                   0, 0, 10, 10, hwnd, nullptr, instance, nullptr);
        };
        auto mkBtn = [&](const wchar_t* text, int id, DWORD extraStyle = 0) {
            return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
                                   0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };
        auto mkEdit = [&](int id, DWORD extraStyle = ES_AUTOHSCROLL) {
            return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
                                   0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };
        auto mkCombo = [&](int id, DWORD style) {
            return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | style,
                                   0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };

        headerTitle = mkLabel(L"MemeStudio");
        managePluginsBtn = mkBtn(L"插件管理…", IdManagePluginsBtn);

        // Source section + <- [ 1 ] / N -> navigation switcher
        lblSource = mkLabel(L"源表情素材");
        pickSourceFilesBtn = mkBtn(L"选择图片…", IdPickSourceFiles);
        pickSourceDirBtn   = mkBtn(L"目录…", IdPickSourceFolder);
        sourceCanvas.create(instance, hwnd, "拖入单张/多张图片或文件夹\n(支持 PNG / JPG / WebP / GIF)", false);
        sourceCanvas.setOnFilesDropped([this](const std::vector<std::filesystem::path>& dropped) {
            importSourcePathsFromUi(dropped, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        });

        prevSourceBtn       = mkBtn(L"<-", IdPrevSourceBtn);
        sourceIndexEdit     = mkEdit(IdSourceIndexEdit, ES_CENTER | ES_NUMBER | ES_AUTOHSCROLL);
        lblSourceSlashTotal = mkLabel(L"/ 0", SS_LEFT);
        nextSourceBtn       = mkBtn(L"->", IdNextSourceBtn);

        // Character section
        lblCharacter = mkLabel(L"目标人物参考");
        pickCharBtn  = mkBtn(L"选择…", IdPickCharacter);
        resetCropBtn = mkBtn(L"重置裁剪", IdResetCrop);
        characterCanvas.create(instance, hwnd, "拖入人物参考图\n(左键拖拽框选面部/全身)", true);
        characterCanvas.setOnFileDropped([this](const std::filesystem::path& p) { loadCharacterFromUi(p); });

        lblStyles      = mkLabel(L"附加画风参考");
        addStyleBtn    = mkBtn(L"添加", IdAddStyle);
        removeStyleBtn = mkBtn(L"移除", IdRemoveStyle);
        styleList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdStyleList)), instance, nullptr);

        lblInstruction  = mkLabel(L"附加重绘要求");
        presetsBtn      = mkBtn(L"预设…", IdOpenPresets);
        instructionEdit = mkEdit(IdInstructionEdit, ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);

        // Center column
        viewModeCombo = mkCombo(IdViewModeCombo, CBS_DROPDOWNLIST);
        SendMessageW(viewModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"视图：原表情素材"));
        SendMessageW(viewModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"视图：生成结果"));
        SendMessageW(viewModeCombo, CB_SETCURSEL, 0, 0);

        copyCombo = mkCombo(IdCopySelectCombo, CBS_DROPDOWNLIST);
        ShowWindow(copyCombo, SW_HIDE);
        openOutBtn = mkBtn(L"打开成品", IdOpenOutputBtn);
        openDirBtn = mkBtn(L"所在目录", IdOpenFolderBtn);
        EnableWindow(openOutBtn, FALSE);
        EnableWindow(openDirBtn, FALSE);

        mainCanvas.create(instance, hwnd, "拖入单张/多张图片或文件夹即可预览\n可用左侧 <- 1/N -> 切换或输入序号回车跳转", false);
        mainCanvas.setOnFilesDropped([this](const std::vector<std::filesystem::path>& dropped) {
            importSourcePathsFromUi(dropped, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        });

        playBtn = mkBtn(L"▶ 播放", IdPlayPauseBtn);
        scrubber = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
            0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdFrameScrubber)), instance, nullptr);
        frameInfoLabel = mkLabel(L"未加载素材");
        EnableWindow(playBtn, FALSE);
        EnableWindow(scrubber, FALSE);

        lblOutput    = mkLabel(L"保存路径");
        outputEdit   = mkEdit(IdOutputEdit);
        browseOutBtn = mkBtn(L"…", IdBrowseOutput);

        progressBar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            0, 0, 10, 10, hwnd, nullptr, instance, nullptr);
        startBtn    = mkBtn(L"开始生成", IdStartGenerate, BS_DEFPUSHBUTTON);
        cancelBtn   = mkBtn(L"取消", IdCancelGenerate);
        EnableWindow(cancelBtn, FALSE);
        statusLabel = mkLabel(L"就绪");

        // Right column
        overrideBannerLabel   = mkLabel(L"");
        disconnectOverrideBtn = mkBtn(L"断开接管", IdDisconnectOverride);
        ShowWindow(overrideBannerLabel, SW_HIDE);
        ShowWindow(disconnectOverrideBtn, SW_HIDE);

        lblBaseUrl  = mkLabel(L"接口地址 (Base URL)");
        baseUrlEdit = mkEdit(IdBaseUrlEdit);
        uikit::setTextUtf8(baseUrlEdit, settings.provider.baseUrl);

        lblKey        = mkLabel(L"API 密钥");
        rememberCheck = mkBtn(L"加密记住密钥", IdRememberKeyCheck, BS_AUTOCHECKBOX);
        uikit::setChecked(rememberCheck, settings.rememberKey);
        keyEdit       = mkEdit(IdApiKeyEdit, ES_PASSWORD | ES_AUTOHSCROLL);
        uikit::setTextUtf8(keyEdit, settings.provider.apiKey);

        lblModel   = mkLabel(L"模型名称");
        modelCombo = mkCombo(IdModelCombo, CBS_DROPDOWN | CBS_AUTOHSCROLL);
        for (const wchar_t* presetModel : {L"gpt-image-1", L"gpt-image-1-mini"}) {
            SendMessageW(modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(presetModel));
        }
        uikit::setTextUtf8(modelCombo, settings.provider.model);
        fetchModelsBtn = mkBtn(L"获取列表", IdFetchModelsBtn);

        lblQuality   = mkLabel(L"生成画质");
        qualityCombo = mkCombo(IdQualityCombo, CBS_DROPDOWNLIST);
        for (const wchar_t* q : {L"auto", L"low", L"medium", L"high"}) {
            SendMessageW(qualityCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(q));
        }
        SendMessageW(qualityCombo, CB_SELECTSTRING, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(base::utf8ToWideLossy(settings.provider.quality.empty() ? "auto" : settings.provider.quality).c_str()));

        lblSize   = mkLabel(L"图像尺寸");
        sizeCombo = mkCombo(IdSizeCombo, CBS_DROPDOWN);
        for (const wchar_t* sz : {L"auto", L"1024x1024", L"1536x1024", L"1024x1536"}) {
            SendMessageW(sizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sz));
        }
        uikit::setTextUtf8(sizeCombo, settings.provider.size.empty() ? "auto" : settings.provider.size);

        lblFormat   = mkLabel(L"输出格式");
        formatCombo = mkCombo(IdOutputFormatCombo, CBS_DROPDOWNLIST);
        for (const wchar_t* f : {L"默认 (不传参)", L"png", L"jpeg", L"webp"}) {
            SendMessageW(formatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f));
        }
        if (settings.provider.outputFormat.empty()) {
            SendMessageW(formatCombo, CB_SETCURSEL, 0, 0);
        } else {
            SendMessageW(formatCombo, CB_SELECTSTRING, static_cast<WPARAM>(-1),
                reinterpret_cast<LPARAM>(base::utf8ToWideLossy(settings.provider.outputFormat).c_str()));
        }

        lblCopies     = mkLabel(L"生成份数 (1-10)");
        copyCountEdit = mkEdit(IdCopyCountEdit, ES_NUMBER);
        uikit::setTextUtf8(copyCountEdit, std::to_string(settings.copyCount));

        streamCheck = mkBtn(L"启用 SSE 流式实时局部预览", IdStreamCheck, BS_AUTOCHECKBOX);
        uikit::setChecked(streamCheck, settings.provider.stream);

        lblStride     = mkLabel(L"GIF 抽帧步长");
        strideEdit    = mkEdit(IdStrideEdit, ES_NUMBER);
        uikit::setTextUtf8(strideEdit, std::to_string(settings.frameStride));

        lblMaxFrames  = mkLabel(L"GIF 最大帧数 (0=不限)");
        maxFramesEdit = mkEdit(IdMaxFramesEdit, ES_NUMBER);
        uikit::setTextUtf8(maxFramesEdit, std::to_string(settings.maxFrames));

        consistencyCheck  = mkBtn(L"跨帧角色一致", IdConsistencyCheck, BS_AUTOCHECKBOX);
        uikit::setChecked(consistencyCheck, settings.temporalConsistency);
        preserveTextCheck = mkBtn(L"保留原图文字", IdPreserveTextCheck, BS_AUTOCHECKBOX);
        uikit::setChecked(preserveTextCheck, settings.preserveText);
        resumeCheck       = mkBtn(L"启用异常中断断点续传", IdResumeCheck, BS_AUTOCHECKBOX);
        uikit::setChecked(resumeCheck, settings.allowResume);

        activeFiltersLabel = mkLabel(L"已挂载管线滤镜：无");
        logEdit = mkEdit(0, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);

        dpi.applyNormalFontToChildren(hwnd);
        SendMessageW(headerTitle, WM_SETFONT, reinterpret_cast<WPARAM>(dpi.titleFont()), TRUE);

        // Re-bind openPanel hook on initial reload so plugin popups attach to hwnd
        std::string reloadErr;
        if (!session->reloadPlugins(settings.enabledPlugins, settings.signaturePolicy, reloadErr) && !reloadErr.empty()) {
            appendLog("部分历史插件未自动加载：" + reloadErr);
        }
        refreshSourceNavigationAndPreview(false);
        refreshEndpointOverrideUi();
        rebuildDynamicPluginToolbar();
    }

    void handleCommand(int id, int notifyCode) {
        if (id >= IdPluginToolbarBase &&
            static_cast<size_t>(id - IdPluginToolbarBase) < toolbarActions.size()) {
            const auto& act = toolbarActions[static_cast<size_t>(id - IdPluginToolbarBase)];
            invokePluginCommandSafe(act.pluginId, act.commandId);
            return;
        }

        switch (id) {
            case IdPickSourceFiles: {
                auto picked = uikit::pickOpenMultipleImageFiles(hwnd, L"选择一张或多张源表情图片 / GIF（按住 Ctrl/Shift 可多选）");
                if (!picked.empty()) {
                    importSourcePathsFromUi(picked, false);
                }
                break;
            }
            case IdPickSourceFolder: {
                if (auto folder = uikit::pickFolder(hwnd, L"选择源表情所在文件夹")) {
                    importSourcePathsFromUi({*folder}, false);
                }
                break;
            }
            case IdPrevSourceBtn: {
                if (session && session->stepSourceIndex(-1)) {
                    showingResultView = false;
                    SendMessageW(viewModeCombo, CB_SETCURSEL, 0, 0);
                    refreshSourceNavigationAndPreview(session->sourceCount() == 1);
                }
                break;
            }
            case IdNextSourceBtn: {
                if (session && session->stepSourceIndex(+1)) {
                    showingResultView = false;
                    SendMessageW(viewModeCombo, CB_SETCURSEL, 0, 0);
                    refreshSourceNavigationAndPreview(session->sourceCount() == 1);
                }
                break;
            }
            case IdSourceIndexEdit: {
                if (notifyCode == EN_KILLFOCUS) {
                    commitJumpSourceIndexFromEdit();
                }
                break;
            }
            case IdJumpSourceCommit: {
                commitJumpSourceIndexFromEdit();
                SendMessageW(sourceIndexEdit, EM_SETSEL, 0, -1);
                break;
            }
            case IdPickCharacter: {
                if (auto p = uikit::pickOpenImageFile(hwnd, L"选择目标人物参考图片")) {
                    loadCharacterFromUi(*p);
                }
                break;
            }
            case IdResetCrop: {
                characterCanvas.setCropRect({0.0, 0.0, 1.0, 1.0});
                if (session) session->setCharacterCrop({0.0, 0.0, 1.0, 1.0});
                break;
            }
            case IdAddStyle: {
                auto picked = uikit::pickOpenMultipleImageFiles(hwnd);
                if (session) {
                    session->addStyleFiles(picked);
                    SendMessageW(styleList, LB_RESETCONTENT, 0, 0);
                    auto allStyles = session->stylePaths();
                    for (const auto& sp : allStyles) {
                        std::wstring wn = sp.filename().wstring();
                        SendMessageW(styleList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wn.c_str()));
                    }
                    uikit::setTextUtf8(lblStyles, "附加画风参考 (" + std::to_string(allStyles.size()) + "/8)");
                }
                break;
            }
            case IdRemoveStyle: {
                LRESULT sel = SendMessageW(styleList, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && session && session->removeStyleAt(static_cast<size_t>(sel))) {
                    SendMessageW(styleList, LB_DELETESTRING, static_cast<WPARAM>(sel), 0);
                    uikit::setTextUtf8(lblStyles, "附加画风参考 (" + std::to_string(session->stylePaths().size()) + "/8)");
                }
                break;
            }
            case IdOpenPresets: {
                openPresetsPicker(instance, hwnd, [this](const std::string& presetText, PresetApplyMode mode) {
                    if (mode == PresetApplyMode::Replace) {
                        uikit::setTextUtf8(instructionEdit, presetText);
                    } else {
                        std::string cur = base::trimAscii(uikit::getTextUtf8(instructionEdit));
                        if (!cur.empty()) cur += "\r\n";
                        cur += presetText;
                        uikit::setTextUtf8(instructionEdit, cur);
                    }
                });
                break;
            }
            case IdViewModeCombo: {
                if (notifyCode == CBN_SELCHANGE) {
                    showingResultView = (SendMessageW(viewModeCombo, CB_GETCURSEL, 0, 0) == 1);
                    currentFrameIdx = 0;
                    updateMainPreviewFrame();
                    if (activeMainAnimation().animated) scheduleNextFrameTimer();
                    else stopPlaybackTimer();
                }
                break;
            }
            case IdCopySelectCombo: {
                if (notifyCode == CBN_SELCHANGE && session) {
                    LRESULT sel = SendMessageW(copyCombo, CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && session->selectResultIndex(static_cast<size_t>(sel))) {
                        cachedResultAnim = session->currentResultAnimationSnapshot();
                        showingResultView = true;
                        SendMessageW(viewModeCombo, CB_SETCURSEL, 1, 0);
                        currentFrameIdx = 0;
                        updateMainPreviewFrame();
                        if (cachedResultAnim.animated) scheduleNextFrameTimer();
                        else stopPlaybackTimer();
                    }
                }
                break;
            }
            case IdOpenOutputBtn: {
                if (session) {
                    auto outs = session->resultPaths();
                    LRESULT sel = SendMessageW(copyCombo, CB_GETCURSEL, 0, 0);
                    size_t idx = (sel >= 0 && static_cast<size_t>(sel) < outs.size()) ? static_cast<size_t>(sel) : 0;
                    if (idx < outs.size()) {
                        ShellExecuteW(hwnd, L"open", outs[idx].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                break;
            }
            case IdOpenFolderBtn: {
                if (session) {
                    auto outs = session->resultPaths();
                    LRESULT sel = SendMessageW(copyCombo, CB_GETCURSEL, 0, 0);
                    size_t idx = (sel >= 0 && static_cast<size_t>(sel) < outs.size()) ? static_cast<size_t>(sel) : 0;
                    if (idx < outs.size()) {
                        std::wstring param = L"/select,\"" + outs[idx].wstring() + L"\"";
                        ShellExecuteW(hwnd, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                }
                break;
            }
            case IdPlayPauseBtn: {
                if (playingAnim) stopPlaybackTimer();
                else scheduleNextFrameTimer();
                break;
            }
            case IdBrowseOutput: {
                if (!session) break;
                if (session->sourceCount() > 1) {
                    if (auto dir = uikit::pickFolder(hwnd, L"选择多源批量输出目录")) {
                        uikit::setTextUtf8(outputEdit, base::wideToUtf8(dir->wstring()));
                    }
                } else {
                    std::filesystem::path curOut = base::utf8ToWideLossy(uikit::getTextUtf8(outputEdit));
                    if (auto p = uikit::pickSaveOutputFile(hwnd, curOut, cachedSourceAnim.animated)) {
                        uikit::setTextUtf8(outputEdit, base::wideToUtf8(p->wstring()));
                    }
                }
                break;
            }
            case IdStartGenerate: {
                if (!session || session->isBusy()) break;
                try {
                    syncControlsIntoSession(true);
                    std::filesystem::path outTarget = base::utf8ToWideStrict(base::trimAscii(uikit::getTextUtf8(outputEdit)));
                    session->startReplaceJob(outTarget, uikit::getTextUtf8(instructionEdit));
                    setBusyUi(true);
                    SendMessageW(progressBar, PBM_SETPOS, 0, 0);
                    uikit::setTextUtf8(statusLabel, "正在准备生成任务…");
                } catch (const std::exception& e) {
                    uikit::showErrorBox(hwnd, "无法启动生成任务", e.what());
                }
                break;
            }
            case IdCancelGenerate: {
                if (session && session->isBusy()) {
                    session->cancelActiveJob();
                    uikit::setTextUtf8(statusLabel, "正在停止当前任务…");
                }
                break;
            }
            case IdFetchModelsBtn: {
                if (!session || session->isBusy()) break;
                try {
                    syncControlsIntoSession(false);
                    session->startFetchModels();
                    setBusyUi(true);
                    uikit::setTextUtf8(statusLabel, "正在获取可用模型列表…");
                } catch (const std::exception& e) {
                    uikit::showErrorBox(hwnd, "获取模型失败", e.what());
                }
                break;
            }
            case IdDisconnectOverride:
                if (session) session->clearEndpointOverride();
                break;
            case IdManagePluginsBtn: {
                if (!session) break;
                auto discovered = session->discoverAndListPlugins();
                auto curSt = session->settingsSnapshot();
                openPluginManagerDialog(
                    instance,
                    hwnd,
                    discovered,
                    curSt.enabledPlugins,
                    curSt.signaturePolicy,
                    [this](const std::vector<std::string>& nextEn, core::SignaturePolicy pol, std::string& err) {
                        panels.closeAll();
                        if (!session->reloadPlugins(nextEn, pol, err)) return false;
                        refreshEndpointOverrideUi();
                        rebuildDynamicPluginToolbar();
                        return true;
                    },
                    [this](const std::string& pid, const std::string& cid) {
                        invokePluginCommandSafe(pid, cid);
                    });
                break;
            }
        }
    }

    void invokePluginCommandSafe(const std::string& pluginId, const std::string& commandId) {
        if (!session) return;
        try {
            syncControlsIntoSession(false);
            session->invokePluginCommand(pluginId, commandId, nlohmann::json::object(), hwnd);
        } catch (const std::exception& e) {
            uikit::showErrorBox(hwnd, "执行插件操作失败", e.what());
        }
    }

    static LRESULT CALLBACK wndProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* app = static_cast<StudioAppWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            if (app) app->hwnd = window;
            return DefWindowProcW(window, msg, wp, lp);
        }

        auto* app = reinterpret_cast<StudioAppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!app) return DefWindowProcW(window, msg, wp, lp);

        switch (msg) {
            case WM_CREATE:
                app->createControls();
                return 0;

            case WM_GETMINMAXINFO: {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                mmi->ptMinTrackSize.x = app->dpi.px(1020);
                mmi->ptMinTrackSize.y = app->dpi.px(720);
                return 0;
            }

            case WM_DPICHANGED: {
                UINT nextDpi = HIWORD(wp);
                app->dpi.updateForDpi(nextDpi);
                app->dpi.applyNormalFontToChildren(window);
                SendMessageW(app->headerTitle, WM_SETFONT, reinterpret_cast<WPARAM>(app->dpi.titleFont()), TRUE);
                app->sourceCanvas.notifyDpiChanged(nextDpi);
                app->characterCanvas.notifyDpiChanged(nextDpi);
                app->mainCanvas.notifyDpiChanged(nextDpi);

                const auto* suggested = reinterpret_cast<const RECT*>(lp);
                if (suggested) {
                    SetWindowPos(
                        window, nullptr,
                        suggested->left, suggested->top,
                        suggested->right - suggested->left,
                        suggested->bottom - suggested->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                app->relayout();
                return 0;
            }

            case WM_SIZE:
                if (wp != SIZE_MINIMIZED) app->relayout();
                return 0;

            case WM_HSCROLL: {
                if (reinterpret_cast<HWND>(lp) == app->scrubber) {
                    const auto& anim = app->activeMainAnimation();
                    if (!anim.frames.empty()) {
                        app->stopPlaybackTimer();
                        LRESULT pos = SendMessageW(app->scrubber, TBM_GETPOS, 0, 0);
                        app->currentFrameIdx = static_cast<size_t>(std::clamp<int>(
                            static_cast<int>(pos), 0, static_cast<int>(anim.frames.size()) - 1));
                        app->updateMainPreviewFrame();
                    }
                }
                return 0;
            }

            case WM_TIMER: {
                if (wp == kAnimTimerId) {
                    const auto& anim = app->activeMainAnimation();
                    if (!anim.animated || anim.frames.size() <= 1) {
                        app->stopPlaybackTimer();
                    } else {
                        app->currentFrameIdx = (app->currentFrameIdx + 1) % anim.frames.size();
                        app->updateMainPreviewFrame();
                        app->scheduleNextFrameTimer();
                    }
                }
                return 0;
            }

            case WM_COMMAND:
                app->handleCommand(LOWORD(wp), HIWORD(wp));
                return 0;

            case WM_SESSION_EVENT:
                app->processQueuedSessionEvents();
                return 0;

            case WM_CLOSE: {
                try { app->syncControlsIntoSession(true); } catch (...) {}
                if (app->session && app->session->isBusy()) {
                    int choice = MessageBoxW(
                        window,
                        L"当前有生成任务正在运行，是否立即取消任务并退出 MemeStudio？",
                        L"MemeStudio",
                        MB_YESNO | MB_ICONQUESTION);
                    if (choice != IDYES) return 0;
                    app->closeAfterCancel = true;
                    app->session->cancelActiveJob();
                    uikit::setTextUtf8(app->statusLabel, "正在停止后台任务并准备退出…");
                    EnableWindow(window, FALSE);
                    return 0;
                }
                DestroyWindow(window);
                return 0;
            }

            case WM_DESTROY:
                app->stopPlaybackTimer();
                app->panels.closeAll();
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(window, msg, wp, lp);
    }
};

} // namespace

int runStudioDesktopApp(HINSTANCE instance, const std::filesystem::path& initialSource) {
    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &StudioAppWindow::wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    StudioAppWindow app(instance);
    HWND win = CreateWindowExW(
        0,
        kMainClass,
        L"MemeStudio",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 1240, 820,
        nullptr, nullptr, instance, &app);
    if (!win) {
        return 1;
    }

    UINT dpi = GetDpiForWindow(win);
    uikit::sizeAndCenterWindow(
        win, nullptr,
        MulDiv(1220, static_cast<int>(dpi), 96),
        MulDiv(780, static_cast<int>(dpi), 96));
    ShowWindow(win, SW_SHOW);
    UpdateWindow(win);

    if (!initialSource.empty() && std::filesystem::exists(initialSource)) {
        app.importSourcePathsFromUi({initialSource}, false);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Pressing Enter inside the source jump index edit box jumps immediately to that image!
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == app.sourceIndexEdit) {
            SendMessageW(win, WM_COMMAND, MAKEWPARAM(IdJumpSourceCommit, 0), 0);
            continue;
        }
        if (!app.panels.translateDialogNavigation(msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

} // namespace meme::gui
