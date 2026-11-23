#include "gui/retouch_dialog.hpp"
#include "media/wic_codec.hpp"
#include "uikit/controls.hpp"
#include "uikit/dpi.hpp"
#include "uikit/file_dialogs.hpp"
#include "uikit/image_canvas.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"

#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace meme::gui {

namespace {

constexpr UINT WM_RETOUCH_UI_EVENT = WM_APP + 32;

struct QuickRetouchTemplate {
    const wchar_t* title;
    const char* promptUtf8;
};

constexpr QuickRetouchTemplate kRetouchTemplates[] = {
    {L"— 插入常用修图指令模板 —", ""},
    {L"修正手部手指与肢体穿插错误",
     "修复角色手部手指数量、手掌连接角度与手臂遮挡关系，消除多余肢体或畸变，其余画面细节与构图保持不变。"},
    {L"清理边缘杂线、重影与噪点色块",
     "清理人物头发、服装边缘附近的杂乱线条、残留色块与伪影，使轮廓干净利落，保留原有画风与文字。"},
    {L"给角色戴上一副酷炫黑墨镜",
     "自然地为画面中的角色戴上一副贴合脸型朝向的黑色反光墨镜，保持发型、头饰、服装与动作完全不变。"},
    {L"将背景替换为干净纯白贴纸底",
     "将人物以外的杂乱背景替换为干净均匀的纯白贴纸背景，主体边缘留出自然轮廓，保留主要道具与原有文字。"},
    {L"增强表情张力与喜剧感染力",
     "强化角色眉眼、嘴形与面部神态的情绪表现力，使表情更加生动传神，同时严格保留人物身份特征与服装头饰。"},
    {L"去除画面内所有文字与气泡框",
     "干净移除画面中所有的文字、字幕、气泡框与水印，并根据周围环境自然补全被文字遮挡的人物或背景细节。"}
};

enum RetouchWorkbenchId : int {
    IdPickSrcBtn       = 4201,
    IdUseResultBaseBtn = 4202,
    IdResetFullBtn     = 4203,
    IdModeCombo        = 4204,
    IdTemplateCombo    = 4205,
    IdPromptEdit       = 4206,
    IdAddStyleBtn      = 4207,
    IdRemStyleBtn      = 4208,
    IdStyleList        = 4209,
    IdKeepTextCheck    = 4210,
    IdCountEdit        = 4211,
    IdOutPathEdit      = 4212,
    IdBrowseOutBtn     = 4213,
    IdStartRetouchBtn  = 4214,
    IdCancelRetouchBtn = 4215,
    IdCloseRetouchBtn  = 4216
};

struct RetouchUiEvent {
    enum class Kind { Progress, Succeeded, Failed } kind = Kind::Progress;
    core::ReplaceProgress progress;
    std::vector<std::filesystem::path> outputs;
    std::string error;
};

std::filesystem::path suggestRetouchedFilePath(const std::filesystem::path& src) {
    if (src.empty()) return {};
    std::wstring ext = src.extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    std::wstring outExt = (ext == L".gif") ? L".gif" : L".png";
    std::wstring stem = src.stem().wstring();
    std::filesystem::path cand = src.parent_path() / (stem + L"-retouched" + outExt);
    int suffix = 2;
    while (base::isSameFilePath(cand, src) || std::filesystem::exists(cand)) {
        cand = src.parent_path() / (stem + L"-retouched-" + std::to_wstring(suffix++) + outExt);
    }
    return cand;
}

struct RetouchWorkbenchState {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    core::StudioSession* session = nullptr;
    uikit::DpiContext dpi;
    uikit::ImageCanvas canvas;

    std::filesystem::path sourcePath;
    std::filesystem::path lastFinishedOutput;
    std::vector<std::filesystem::path> stylePaths;
    media::Animation sourceAnim;
    std::function<void()> onSyncControlsBeforeRun;

    HWND srcTitleLabel = nullptr, useResultBaseBtn = nullptr, resetRegionBtn = nullptr;
    HWND modeCombo = nullptr, coordHintLabel = nullptr;
    HWND templateCombo = nullptr, promptEdit = nullptr;
    HWND lblStyle = nullptr, styleList = nullptr;
    HWND keepTextCheck = nullptr, countEdit = nullptr, outPathEdit = nullptr;
    HWND statusLabel = nullptr, startBtn = nullptr, cancelBtn = nullptr;

    net::CancelToken cancelToken;
    std::thread worker;
    bool busy = false;
    std::mutex evMutex;
    std::deque<RetouchUiEvent> evQueue;

    ~RetouchWorkbenchState() {
        cancelToken.stop();
        if (worker.joinable()) worker.join();
    }

    bool isRegionMode() const noexcept {
        return SendMessageW(modeCombo, CB_GETCURSEL, 0, 0) == 1;
    }

    void pushEvent(RetouchUiEvent ev) {
        {
            std::lock_guard lk(evMutex);
            evQueue.push_back(std::move(ev));
        }
        if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_RETOUCH_UI_EVENT, 0, 0);
    }

    void setBusy(bool b) {
        busy = b;
        EnableWindow(startBtn, b ? FALSE : TRUE);
        EnableWindow(cancelBtn, b ? TRUE : FALSE);
        EnableWindow(useResultBaseBtn, (!b && !lastFinishedOutput.empty()) ? TRUE : FALSE);
    }

    void refreshModeAndCoordHint() {
        bool regionMode = isRegionMode();
        EnableWindow(resetRegionBtn, regionMode ? TRUE : FALSE);
        if (!regionMode) {
            canvas.setCropRect({0.0, 0.0, 1.0, 1.0});
            uikit::setTextUtf8(coordHintLabel, "当前为「提示词整图修图」：直接输入自然语言修改要求即可作用于全图（支持 GIF 动图）");
        } else {
            auto r = canvas.cropRect();
            char buf[180]{};
            if (r.isFullImage()) {
                std::snprintf(buf, sizeof(buf), "当前为「局部框选精修」：请在上方画布左键拖拽框选需重绘的局部矩形，框外像素将 100%% 无损保留");
            } else {
                std::snprintf(buf, sizeof(buf), "局部重绘选区：X=%.2f, Y=%.2f, 宽=%.2f, 高=%.2f（框外原始像素严格锁死不变）",
                              r.x, r.y, r.width, r.height);
            }
            uikit::setTextUtf8(coordHintLabel, buf);
        }
    }

    void loadBaseImage(const std::filesystem::path& path) {
        if (path.empty() || !std::filesystem::is_regular_file(path)) {
            uikit::setTextUtf8(srcTitleLabel, "底图素材：（尚未选择图片）");
            canvas.clearImage();
            return;
        }
        try {
            sourceAnim = media::decodeAnimation(path);
            sourcePath = path;
            std::string tag = base::wideToUtf8(path.filename().wstring());
            if (sourceAnim.animated) tag += " (" + std::to_string(sourceAnim.frames.size()) + " 帧 GIF)";
            uikit::setTextUtf8(srcTitleLabel, "底图素材：" + tag);
            canvas.setCropRect({0.0, 0.0, 1.0, 1.0});
            canvas.setImage(&sourceAnim.frames.front().image);
            uikit::setTextUtf8(outPathEdit, base::wideToUtf8(suggestRetouchedFilePath(path).wstring()));
            refreshModeAndCoordHint();
        } catch (const std::exception& e) {
            uikit::showErrorBox(hwnd, "载入修图底图失败", e.what());
        }
    }

    void createUi() {
        dpi.updateForWindow(hwnd);
        const auto& d = dpi;

        auto lbl = [&](const wchar_t* text, int x, int y, int w, int h = 20) {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                d.px(x), d.px(y), d.px(w), d.px(h), hwnd, nullptr, instance, nullptr);
        };
        auto btn = [&](const wchar_t* text, int id, int x, int y, int w, int h = 28, DWORD extra = 0) {
            return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra,
                d.px(x), d.px(y), d.px(w), d.px(h), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };

        // Top Mode Bar
        lbl(L"修图模式：", 16, 15, 76, 22);
        modeCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            d.px(92), d.px(11), d.px(396), d.px(140), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdModeCombo)), instance, nullptr);
        SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"提示词整图修图（按自然语言增删配饰 / 改神态 / 换背景 / 修细节）"));
        SendMessageW(modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"局部框选精修（左键拖拽绿框局部重绘，框外原始像素无损保留）"));
        SendMessageW(modeCombo, CB_SETCURSEL, 0, 0);

        // Left: Base Preview + Iterative Handoff
        srcTitleLabel    = lbl(L"底图素材：（尚未选择图片）", 16, 48, 220, 22);
        btn(L"换一张图…", IdPickSrcBtn, 240, 44, 84, 26);
        resetRegionBtn   = btn(L"重置选区", IdResetFullBtn, 328, 44, 72, 26);
        useResultBaseBtn = btn(L"以此成品继续修", IdUseResultBaseBtn, 404, 44, 112, 26);
        EnableWindow(useResultBaseBtn, FALSE);

        canvas.create(instance, hwnd, "点击「换一张图…」或直接拖入待修图的图片 / GIF\n滚轮缩放 · 右键拖拽平移 · 局部精修模式下左键拖拽框选", true);
        SetWindowPos(canvas.hwnd(), nullptr, d.px(16), d.px(76), d.px(500), d.px(420), SWP_NOZORDER | SWP_NOACTIVATE);
        canvas.setOnCropChanged([this](const media::NormalizedRect& r) {
            if (!r.isFullImage() && !isRegionMode()) {
                // Automatically switch into RegionRedraw mode when user drags a sub-region box on the canvas!
                SendMessageW(modeCombo, CB_SETCURSEL, 1, 0);
            }
            refreshModeAndCoordHint();
        });
        canvas.setOnFileDropped([this](const std::filesystem::path& p) { loadBaseImage(p); });

        coordHintLabel = lbl(L"", 16, 504, 500, 22);

        // Right: Templates + Prompt + Styles + Options + Save & Run
        lbl(L"修图提示词指令（支持中文自然语言）：", 532, 15, 320, 20);
        templateCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            d.px(532), d.px(40), d.px(332), d.px(220), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdTemplateCombo)), instance, nullptr);
        for (const auto& tpl : kRetouchTemplates) {
            SendMessageW(templateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tpl.title));
        }
        SendMessageW(templateCombo, CB_SETCURSEL, 0, 0);

        promptEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL,
            d.px(532), d.px(74), d.px(332), d.px(172), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdPromptEdit)), instance, nullptr);

        lblStyle = lbl(L"附加画风参考 (0/8，可选)：", 532, 258, 200, 20);
        btn(L"添加…", IdAddStyleBtn, 744, 254, 58, 26);
        btn(L"移除",   IdRemStyleBtn, 808, 254, 56, 26);
        styleList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
            d.px(532), d.px(284), d.px(332), d.px(78), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdStyleList)), instance, nullptr);

        keepTextCheck = btn(L"保留原图已有文字与台词", IdKeepTextCheck, 532, 372, 210, 24, BS_AUTOCHECKBOX);
        uikit::setChecked(keepTextCheck, true);

        lbl(L"份数：", 754, 374, 48, 20);
        countEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            d.px(802), d.px(370), d.px(62), d.px(26), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdCountEdit)), instance, nullptr);

        lbl(L"修图结果保存路径：", 532, 408, 220, 20);
        outPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            d.px(532), d.px(430), d.px(288), d.px(26), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdOutPathEdit)), instance, nullptr);
        btn(L"…", IdBrowseOutBtn, 826, 430, 38, 26);

        statusLabel = lbl(L"就绪", 532, 466, 332, 22);

        startBtn  = btn(L"开始修图", IdStartRetouchBtn, 582, 496, 118, 34, BS_DEFPUSHBUTTON);
        cancelBtn = btn(L"取消",     IdCancelRetouchBtn, 706, 496, 74, 34);
        btn(L"关闭", IdCloseRetouchBtn, 786, 496, 78, 34);
        EnableWindow(cancelBtn, FALSE);

        d.applyNormalFontToChildren(hwnd);

        // Smart default base image: explicit preferred -> latest main result -> current main source
        std::filesystem::path initImg = sourcePath;
        if (initImg.empty()) {
            auto outs = session->resultPaths();
            if (!outs.empty()) initImg = outs.front();
            else if (auto curSrc = session->currentSourceEntry()) initImg = curSrc->path;
        }
        loadBaseImage(initImg);
        refreshModeAndCoordHint();
    }

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<RetouchWorkbenchState*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            st->createUi();
            return 0;
        }
        if (msg == WM_COMMAND) {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IdModeCombo && code == CBN_SELCHANGE) {
                st->refreshModeAndCoordHint();
                return 0;
            }
            if (id == IdTemplateCombo && code == CBN_SELCHANGE) {
                LRESULT sel = SendMessageW(st->templateCombo, CB_GETCURSEL, 0, 0);
                if (sel > 0 && static_cast<size_t>(sel) < std::size(kRetouchTemplates)) {
                    std::string cur = base::trimAscii(uikit::getTextUtf8(st->promptEdit));
                    if (!cur.empty()) cur += "\r\n";
                    cur += kRetouchTemplates[sel].promptUtf8;
                    uikit::setTextUtf8(st->promptEdit, cur);
                    SendMessageW(st->templateCombo, CB_SETCURSEL, 0, 0);
                }
                return 0;
            }
            if (id == IdPickSrcBtn && !st->busy) {
                if (auto p = uikit::pickOpenImageFile(w, L"选择需要修图的图片或 GIF 动画")) {
                    st->loadBaseImage(*p);
                }
                return 0;
            }
            if (id == IdUseResultBaseBtn && !st->busy && !st->lastFinishedOutput.empty()) {
                st->loadBaseImage(st->lastFinishedOutput);
                uikit::setTextUtf8(st->statusLabel, "已将上一轮修图成品载入为新底图，可继续叠加修图");
                return 0;
            }
            if (id == IdResetFullBtn && !st->busy) {
                st->canvas.setCropRect({0.0, 0.0, 1.0, 1.0});
                st->refreshModeAndCoordHint();
                return 0;
            }
            if (id == IdAddStyleBtn && !st->busy) {
                for (const auto& p : uikit::pickOpenMultipleImageFiles(w, L"选择附加画风参考图片（最多 8 张）")) {
                    if (st->stylePaths.size() >= 8) break;
                    if (std::none_of(st->stylePaths.begin(), st->stylePaths.end(), [&](const auto& ex) { return base::isSameFilePath(ex, p); })) {
                        st->stylePaths.push_back(p);
                        std::wstring wn = p.filename().wstring();
                        SendMessageW(st->styleList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wn.c_str()));
                    }
                }
                uikit::setTextUtf8(st->lblStyle, "附加画风参考 (" + std::to_string(st->stylePaths.size()) + "/8，可选)：");
                return 0;
            }
            if (id == IdRemStyleBtn && !st->busy) {
                LRESULT sel = SendMessageW(st->styleList, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && static_cast<size_t>(sel) < st->stylePaths.size()) {
                    st->stylePaths.erase(st->stylePaths.begin() + sel);
                    SendMessageW(st->styleList, LB_DELETESTRING, static_cast<WPARAM>(sel), 0);
                    uikit::setTextUtf8(st->lblStyle, "附加画风参考 (" + std::to_string(st->stylePaths.size()) + "/8，可选)：");
                }
                return 0;
            }
            if (id == IdBrowseOutBtn && !st->busy) {
                std::filesystem::path cur = base::utf8ToWideLossy(uikit::getTextUtf8(st->outPathEdit));
                if (auto p = uikit::pickSaveOutputFile(w, cur, st->sourceAnim.animated)) {
                    uikit::setTextUtf8(st->outPathEdit, base::wideToUtf8(p->wstring()));
                }
                return 0;
            }
            if (id == IdStartRetouchBtn && !st->busy) {
                try {
                    if (st->onSyncControlsBeforeRun) st->onSyncControlsBeforeRun();
                    auto appSt = st->session->settingsSnapshot();

                    core::RetouchTaskConfig task;
                    task.provider = st->session->effectiveProvider();
                    task.sourcePath = st->sourcePath;
                    task.outputPath = base::utf8ToWideStrict(base::trimAscii(uikit::getTextUtf8(st->outPathEdit)));
                    task.mode = st->isRegionMode() ? core::RetouchMode::RegionRedraw : core::RetouchMode::PromptEdit;
                    task.region = st->canvas.cropRect();
                    task.stylePaths = st->stylePaths;
                    task.instruction = base::trimAscii(uikit::getTextUtf8(st->promptEdit));
                    task.copyCount = uikit::parseBoundedInt(st->countEdit, "生成份数", 1, 10);
                    task.frameStride = appSt.frameStride;
                    task.maxFrames = appSt.maxFrames;
                    task.temporalConsistency = appSt.temporalConsistency;
                    task.preserveText = uikit::isChecked(st->keepTextCheck);

                    if (task.sourcePath.empty()) throw Error("请先选择待修图的底图素材");
                    if (task.instruction.empty()) throw Error("请输入修图提示词指令");
                    if (task.outputPath.empty()) throw Error("请指定修图结果保存路径");

                    if (st->worker.joinable()) st->worker.join();
                    st->cancelToken.reset();
                    st->setBusy(true);
                    uikit::setTextUtf8(st->statusLabel, "正在按指令执行修图…");

                    st->worker = std::thread([st, task = std::move(task)]() {
                        HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        try {
                            auto outs = st->session->runRetouchSync(task, st->cancelToken, [st](const core::ReplaceProgress& prog) {
                                RetouchUiEvent ev;
                                ev.kind = RetouchUiEvent::Kind::Progress;
                                ev.progress = prog;
                                st->pushEvent(std::move(ev));
                            });
                            RetouchUiEvent done;
                            done.kind = RetouchUiEvent::Kind::Succeeded;
                            done.outputs = std::move(outs);
                            st->pushEvent(std::move(done));
                        } catch (const std::exception& e) {
                            RetouchUiEvent fail;
                            fail.kind = RetouchUiEvent::Kind::Failed;
                            fail.error = e.what();
                            st->pushEvent(std::move(fail));
                        }
                        if (SUCCEEDED(comHr)) CoUninitialize();
                    });
                } catch (const std::exception& e) {
                    uikit::showErrorBox(w, "局部精修与提示词修图", e.what());
                }
                return 0;
            }
            if (id == IdCancelRetouchBtn && st->busy) {
                st->cancelToken.stop();
                uikit::setTextUtf8(st->statusLabel, "正在取消修图任务…");
                return 0;
            }
            if (id == IdCloseRetouchBtn || id == IDCANCEL) {
                if (!st->busy) DestroyWindow(w);
                return 0;
            }
        }
        if (msg == WM_RETOUCH_UI_EVENT) {
            std::deque<RetouchUiEvent> batch;
            {
                std::lock_guard lk(st->evMutex);
                batch.swap(st->evQueue);
            }
            for (auto& ev : batch) {
                if (ev.kind == RetouchUiEvent::Kind::Progress) {
                    uikit::setTextUtf8(st->statusLabel, ev.progress.messageUtf8);
                    if (ev.progress.previewImage && !ev.progress.previewImage->empty()) {
                        st->canvas.setImage(ev.progress.previewImage.get());
                    }
                } else if (ev.kind == RetouchUiEvent::Kind::Succeeded) {
                    if (st->worker.joinable()) st->worker.join();
                    if (!ev.outputs.empty()) {
                        st->lastFinishedOutput = ev.outputs.front();
                        try {
                            auto previewAnim = media::decodeAnimation(st->lastFinishedOutput);
                            if (!previewAnim.frames.empty()) {
                                st->canvas.setImage(&previewAnim.frames.front().image);
                            }
                        } catch (...) {}
                    }
                    st->setBusy(false);
                    uikit::setTextUtf8(st->statusLabel, "修图完成！已同步至主窗口（可点「以此成品继续修」叠加修改）");
                } else {
                    if (st->worker.joinable()) st->worker.join();
                    st->setBusy(false);
                    uikit::setTextUtf8(st->statusLabel, "修图中断：" + ev.error);
                    if (ev.error.find("取消") == std::string::npos) {
                        uikit::showErrorBox(w, "修图失败", ev.error);
                    }
                }
            }
            return 0;
        }
        if (msg == WM_CLOSE) {
            if (!st->busy) DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, msg, wp, lp);
    }

    static void MEMESTUDIO_CALL onDestroy(void* ctx) {
        delete static_cast<RetouchWorkbenchState*>(ctx);
    }
};

} // namespace

void openRetouchWorkbench(
    HINSTANCE instance,
    HWND owner,
    uikit::PanelManager& panelManager,
    core::StudioSession& session,
    const std::filesystem::path& preferredSource,
    std::function<void()> onSyncControlsBeforeRun) {
    // Close existing retouch panel if an explicit new source image was pushed from Prompt Draw
    if (!preferredSource.empty()) {
        panelManager.closePanel("builtin-retouch");
    }

    auto* st = new RetouchWorkbenchState();
    st->instance = instance;
    st->session = &session;
    st->sourcePath = preferredSource;
    st->onSyncControlsBeforeRun = std::move(onSyncControlsBeforeRun);

    panelManager.openOrActivatePluginPanel(
        "builtin-retouch",
        owner,
        880,
        544,
        "局部精修与提示词修图",
        RetouchWorkbenchState::wndProc,
        st,
        RetouchWorkbenchState::onDestroy);
}

} // namespace meme::gui
