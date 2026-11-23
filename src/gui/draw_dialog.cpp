#include "gui/draw_dialog.hpp"
#include "gui/presets_dialog.hpp"
#include "media/wic_codec.hpp"
#include "uikit/controls.hpp"
#include "uikit/dpi.hpp"
#include "uikit/file_dialogs.hpp"
#include "uikit/image_canvas.hpp"
#include "base/app_paths.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"

#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace meme::gui {

namespace {

constexpr UINT WM_DRAW_UI_EVENT = WM_APP + 31;

enum DrawWorkbenchId : int {
    IdPromptEdit     = 4101,
    IdPresetsBtn     = 4102,
    IdAddStyleBtn    = 4103,
    IdRemStyleBtn    = 4104,
    IdStyleList      = 4105,
    IdQualityCombo   = 4106,
    IdSizeCombo      = 4107,
    IdCountEdit      = 4108,
    IdOutPathEdit    = 4109,
    IdBrowseOutBtn   = 4110,
    IdResultCombo    = 4111,
    IdSendRetouchBtn = 4112,
    IdOpenFileBtn    = 4113,
    IdStartDrawBtn   = 4114,
    IdCancelDrawBtn  = 4115,
    IdCloseDrawBtn   = 4116
};

struct DrawUiEvent {
    enum class Kind { Progress, Succeeded, Failed } kind = Kind::Progress;
    core::ReplaceProgress progress;
    std::vector<std::filesystem::path> outputs;
    std::string error;
};

struct DrawWorkbenchState {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    core::StudioSession* session = nullptr;
    uikit::DpiContext dpi;
    uikit::ImageCanvas previewCanvas;

    std::function<void(const std::filesystem::path&)> onSendToRetouch;
    std::function<void()> onSyncControlsBeforeRun;

    HWND promptEdit = nullptr, lblStyle = nullptr, styleList = nullptr;
    HWND qualCombo = nullptr, sizeCombo = nullptr, countEdit = nullptr;
    HWND outPathEdit = nullptr, resultCombo = nullptr, sendRetouchBtn = nullptr, openFileBtn = nullptr;
    HWND statusLabel = nullptr, startBtn = nullptr, cancelBtn = nullptr;

    std::vector<std::filesystem::path> stylePaths;
    std::vector<std::filesystem::path> generatedOutputs;
    media::Image currentPreviewImg;

    net::CancelToken cancelToken;
    std::thread worker;
    bool busy = false;
    std::mutex evMutex;
    std::deque<DrawUiEvent> evQueue;

    ~DrawWorkbenchState() {
        cancelToken.stop();
        if (worker.joinable()) worker.join();
    }

    void pushEvent(DrawUiEvent ev) {
        {
            std::lock_guard lk(evMutex);
            evQueue.push_back(std::move(ev));
        }
        if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_DRAW_UI_EVENT, 0, 0);
    }

    void setBusy(bool b) {
        busy = b;
        EnableWindow(startBtn, b ? FALSE : TRUE);
        EnableWindow(cancelBtn, b ? TRUE : FALSE);
    }

    void showOutputAtIndex(size_t idx) {
        if (idx >= generatedOutputs.size()) return;
        try {
            currentPreviewImg = media::decodeFirstFrame(generatedOutputs[idx]);
            previewCanvas.setImage(&currentPreviewImg);
        } catch (...) {}
    }

    void createUi() {
        dpi.updateForWindow(hwnd);
        const auto& d = dpi;
        auto st = session->settingsSnapshot();

        auto lbl = [&](const wchar_t* text, int x, int y, int w, int h = 20) {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                d.px(x), d.px(y), d.px(w), d.px(h), hwnd, nullptr, instance, nullptr);
        };
        auto btn = [&](const wchar_t* text, int id, int x, int y, int w, int h = 28, DWORD extra = 0) {
            return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra,
                d.px(x), d.px(y), d.px(w), d.px(h), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };

        // Left side: Prompt + Styles + Parameters + Output Path
        lbl(L"画面描述提示词（描述角色外观、动作、神态与背景氛围）：", 16, 16, 292, 20);
        btn(L"灵感预设…", IdPresetsBtn, 312, 12, 84, 26);
        promptEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL,
            d.px(16), d.px(42), d.px(380), d.px(200), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdPromptEdit)), instance, nullptr);

        lblStyle = lbl(L"画风参考图 (0/8，可选)：", 16, 254, 240, 20);
        btn(L"添加…", IdAddStyleBtn, 276, 250, 58, 26);
        btn(L"移除",   IdRemStyleBtn, 340, 250, 56, 26);
        styleList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
            d.px(16), d.px(280), d.px(380), d.px(88), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdStyleList)), instance, nullptr);

        lbl(L"生成画质：", 16, 380, 110, 20);
        qualCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
            d.px(16), d.px(402), d.px(114), d.px(160), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdQualityCombo)), instance, nullptr);
        for (const wchar_t* q : {L"auto", L"low", L"medium", L"high"}) {
            SendMessageW(qualCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(q));
        }
        SendMessageW(qualCombo, CB_SELECTSTRING, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(base::utf8ToWideLossy(st.provider.quality.empty() ? "auto" : st.provider.quality).c_str()));

        lbl(L"画面尺寸：", 142, 380, 140, 20);
        sizeCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
            d.px(142), d.px(402), d.px(154), d.px(160), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSizeCombo)), instance, nullptr);
        for (const wchar_t* sz : {L"auto", L"1024x1024", L"1536x1024", L"1024x1536"}) {
            SendMessageW(sizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sz));
        }
        uikit::setTextUtf8(sizeCombo, st.provider.size.empty() ? "auto" : st.provider.size);

        lbl(L"生成张数：", 308, 380, 88, 20);
        countEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            d.px(308), d.px(402), d.px(88), d.px(26), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdCountEdit)), instance, nullptr);

        lbl(L"保存文件路径 (.png)：", 16, 440, 220, 20);
        std::filesystem::path defaultOut = base::appDataRoot() / L"drawings" / L"prompt-drawn.png";
        if (auto curSrc = session->currentSourceEntry()) {
            defaultOut = curSrc->path.parent_path() / L"prompt-drawn.png";
        }
        outPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            d.px(16), d.px(462), d.px(336), d.px(26), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdOutPathEdit)), instance, nullptr);
        uikit::setTextUtf8(outPathEdit, base::wideToUtf8(defaultOut.wstring()));
        btn(L"…", IdBrowseOutBtn, 358, 462, 38, 26);

        // Right side: Live Zoomable Canvas + Handoff to Retouch
        lbl(L"绘制结果预览：", 414, 16, 120, 20);
        resultCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST,
            d.px(526), d.px(12), d.px(110), d.px(180), hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdResultCombo)), instance, nullptr);
        sendRetouchBtn = btn(L"送去精修 / 修图…", IdSendRetouchBtn, 642, 12, 126, 26);
        openFileBtn    = btn(L"打开", IdOpenFileBtn, 774, 12, 50, 26);
        EnableWindow(sendRetouchBtn, FALSE);
        EnableWindow(openFileBtn, FALSE);

        previewCanvas.create(instance, hwnd, "输入左侧画面描述后点击「开始生图」\n可拖入画风参考图 · 滚轮缩放 · 双击复位", false);
        SetWindowPos(previewCanvas.hwnd(), nullptr, d.px(414), d.px(42), d.px(410), d.px(410), SWP_NOZORDER | SWP_NOACTIVATE);
        previewCanvas.setOnFilesDropped([this](const std::vector<std::filesystem::path>& dropped) {
            for (const auto& p : dropped) {
                if (stylePaths.size() >= 8) break;
                if (std::filesystem::is_regular_file(p) &&
                    std::none_of(stylePaths.begin(), stylePaths.end(), [&](const auto& ex) { return base::isSameFilePath(ex, p); })) {
                    stylePaths.push_back(p);
                    std::wstring wn = p.filename().wstring();
                    SendMessageW(styleList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wn.c_str()));
                }
            }
            uikit::setTextUtf8(lblStyle, "画风参考图 (" + std::to_string(stylePaths.size()) + "/8，可选)：");
        });

        statusLabel = lbl(L"就绪（自动复用主窗口当前接口配置）", 414, 460, 410, 22);

        startBtn  = btn(L"开始生图", IdStartDrawBtn, 536, 494, 118, 34, BS_DEFPUSHBUTTON);
        cancelBtn = btn(L"取消",     IdCancelDrawBtn, 660, 494, 78, 34);
        btn(L"关闭", IdCloseDrawBtn, 744, 494, 80, 34);
        EnableWindow(cancelBtn, FALSE);

        d.applyNormalFontToChildren(hwnd);
    }

    static intptr_t MEMESTUDIO_CALL wndProc(void* ctx, void* rawHwnd, uint32_t msg, uintptr_t wp, intptr_t lp) {
        auto* st = static_cast<DrawWorkbenchState*>(ctx);
        HWND w = static_cast<HWND>(rawHwnd);
        if (msg == WM_CREATE) {
            st->hwnd = w;
            st->createUi();
            return 0;
        }
        if (msg == WM_COMMAND) {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == IdPresetsBtn && !st->busy) {
                openPresetsPicker(st->instance, w, [st](const std::string& text, PresetApplyMode mode) {
                    if (mode == PresetApplyMode::Replace) uikit::setTextUtf8(st->promptEdit, text);
                    else {
                        std::string cur = base::trimAscii(uikit::getTextUtf8(st->promptEdit));
                        if (!cur.empty()) cur += "\r\n";
                        cur += text;
                        uikit::setTextUtf8(st->promptEdit, cur);
                    }
                });
                return 0;
            }
            if (id == IdAddStyleBtn && !st->busy) {
                for (const auto& p : uikit::pickOpenMultipleImageFiles(w, L"选择画风参考图片（最多 8 张）")) {
                    if (st->stylePaths.size() >= 8) break;
                    if (std::none_of(st->stylePaths.begin(), st->stylePaths.end(), [&](const auto& ex) { return base::isSameFilePath(ex, p); })) {
                        st->stylePaths.push_back(p);
                        std::wstring wn = p.filename().wstring();
                        SendMessageW(st->styleList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wn.c_str()));
                    }
                }
                uikit::setTextUtf8(st->lblStyle, "画风参考图 (" + std::to_string(st->stylePaths.size()) + "/8，可选)：");
                return 0;
            }
            if (id == IdRemStyleBtn && !st->busy) {
                LRESULT sel = SendMessageW(st->styleList, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && static_cast<size_t>(sel) < st->stylePaths.size()) {
                    st->stylePaths.erase(st->stylePaths.begin() + sel);
                    SendMessageW(st->styleList, LB_DELETESTRING, static_cast<WPARAM>(sel), 0);
                    uikit::setTextUtf8(st->lblStyle, "画风参考图 (" + std::to_string(st->stylePaths.size()) + "/8，可选)：");
                }
                return 0;
            }
            if (id == IdBrowseOutBtn && !st->busy) {
                std::filesystem::path cur = base::utf8ToWideLossy(uikit::getTextUtf8(st->outPathEdit));
                if (auto p = uikit::pickSaveOutputFile(w, cur, false)) {
                    uikit::setTextUtf8(st->outPathEdit, base::wideToUtf8(p->wstring()));
                }
                return 0;
            }
            if (id == IdResultCombo && code == CBN_SELCHANGE) {
                LRESULT sel = SendMessageW(st->resultCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0) st->showOutputAtIndex(static_cast<size_t>(sel));
                return 0;
            }
            if (id == IdSendRetouchBtn && !st->generatedOutputs.empty()) {
                LRESULT sel = SendMessageW(st->resultCombo, CB_GETCURSEL, 0, 0);
                size_t idx = (sel >= 0 && static_cast<size_t>(sel) < st->generatedOutputs.size()) ? static_cast<size_t>(sel) : 0;
                if (st->onSendToRetouch) st->onSendToRetouch(st->generatedOutputs[idx]);
                return 0;
            }
            if (id == IdOpenFileBtn && !st->generatedOutputs.empty()) {
                LRESULT sel = SendMessageW(st->resultCombo, CB_GETCURSEL, 0, 0);
                size_t idx = (sel >= 0 && static_cast<size_t>(sel) < st->generatedOutputs.size()) ? static_cast<size_t>(sel) : 0;
                ShellExecuteW(w, L"open", st->generatedOutputs[idx].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (id == IdStartDrawBtn && !st->busy) {
                try {
                    if (st->onSyncControlsBeforeRun) st->onSyncControlsBeforeRun();
                    core::PromptDrawTaskConfig task;
                    task.provider = st->session->effectiveProvider();
                    task.provider.quality = uikit::getTextUtf8(st->qualCombo);
                    task.provider.size = base::trimAscii(uikit::getTextUtf8(st->sizeCombo));
                    task.prompt = base::trimAscii(uikit::getTextUtf8(st->promptEdit));
                    task.stylePaths = st->stylePaths;
                    task.outputPath = base::utf8ToWideStrict(base::trimAscii(uikit::getTextUtf8(st->outPathEdit)));
                    task.copyCount = uikit::parseBoundedInt(st->countEdit, "生成张数", 1, 10);

                    if (task.prompt.empty()) throw Error("请先输入画面描述提示词");
                    if (task.outputPath.empty()) throw Error("请指定图片保存路径");

                    if (st->worker.joinable()) st->worker.join();
                    st->cancelToken.reset();
                    st->setBusy(true);
                    uikit::setTextUtf8(st->statusLabel, "正在连接模型绘制图片…");

                    st->worker = std::thread([st, task = std::move(task)]() {
                        HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        try {
                            auto outs = st->session->runPromptDrawSync(task, st->cancelToken, [st](const core::ReplaceProgress& prog) {
                                DrawUiEvent ev;
                                ev.kind = DrawUiEvent::Kind::Progress;
                                ev.progress = prog;
                                st->pushEvent(std::move(ev));
                            });
                            DrawUiEvent done;
                            done.kind = DrawUiEvent::Kind::Succeeded;
                            done.outputs = std::move(outs);
                            st->pushEvent(std::move(done));
                        } catch (const std::exception& e) {
                            DrawUiEvent fail;
                            fail.kind = DrawUiEvent::Kind::Failed;
                            fail.error = e.what();
                            st->pushEvent(std::move(fail));
                        }
                        if (SUCCEEDED(comHr)) CoUninitialize();
                    });
                } catch (const std::exception& e) {
                    uikit::showErrorBox(w, "提示词生图", e.what());
                }
                return 0;
            }
            if (id == IdCancelDrawBtn && st->busy) {
                st->cancelToken.stop();
                uikit::setTextUtf8(st->statusLabel, "正在取消绘制任务…");
                return 0;
            }
            if (id == IdCloseDrawBtn || id == IDCANCEL) {
                if (!st->busy) DestroyWindow(w);
                return 0;
            }
        }
        if (msg == WM_DRAW_UI_EVENT) {
            std::deque<DrawUiEvent> batch;
            {
                std::lock_guard lk(st->evMutex);
                batch.swap(st->evQueue);
            }
            for (auto& ev : batch) {
                if (ev.kind == DrawUiEvent::Kind::Progress) {
                    uikit::setTextUtf8(st->statusLabel, ev.progress.messageUtf8);
                    if (ev.progress.previewImage && !ev.progress.previewImage->empty()) {
                        st->previewCanvas.setImage(ev.progress.previewImage.get());
                    }
                } else if (ev.kind == DrawUiEvent::Kind::Succeeded) {
                    if (st->worker.joinable()) st->worker.join();
                    st->setBusy(false);
                    st->generatedOutputs = std::move(ev.outputs);
                    SendMessageW(st->resultCombo, CB_RESETCONTENT, 0, 0);
                    for (size_t i = 0; i < st->generatedOutputs.size(); ++i) {
                        std::wstring wl = L"第 " + std::to_wstring(i + 1) + L" 张";
                        SendMessageW(st->resultCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wl.c_str()));
                    }
                    SendMessageW(st->resultCombo, CB_SETCURSEL, 0, 0);
                    ShowWindow(st->resultCombo, st->generatedOutputs.size() > 1 ? SW_SHOW : SW_HIDE);
                    EnableWindow(st->sendRetouchBtn, TRUE);
                    EnableWindow(st->openFileBtn, TRUE);
                    st->showOutputAtIndex(0);
                    uikit::setTextUtf8(st->statusLabel, "绘制完成，已同步回显至主窗口结果视图");
                } else {
                    if (st->worker.joinable()) st->worker.join();
                    st->setBusy(false);
                    uikit::setTextUtf8(st->statusLabel, "绘制中断：" + ev.error);
                    if (ev.error.find("取消") == std::string::npos) {
                        uikit::showErrorBox(w, "提示词生图失败", ev.error);
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
        delete static_cast<DrawWorkbenchState*>(ctx);
    }
};

} // namespace

void openPromptDrawWorkbench(
    HINSTANCE instance,
    HWND owner,
    uikit::PanelManager& panelManager,
    core::StudioSession& session,
    std::function<void(const std::filesystem::path&)> onSendToRetouch,
    std::function<void()> onSyncControlsBeforeRun) {
    auto* st = new DrawWorkbenchState();
    st->instance = instance;
    st->session = &session;
    st->onSendToRetouch = std::move(onSendToRetouch);
    st->onSyncControlsBeforeRun = std::move(onSyncControlsBeforeRun);

    panelManager.openOrActivatePluginPanel(
        "builtin-prompt-draw",
        owner,
        840,
        544,
        "提示词生图",
        DrawWorkbenchState::wndProc,
        st,
        DrawWorkbenchState::onDestroy);
}

} // namespace meme::gui
