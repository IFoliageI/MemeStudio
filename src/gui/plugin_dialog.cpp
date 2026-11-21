#include "gui/plugin_dialog.hpp"
#include "uikit/controls.hpp"
#include "uikit/dpi.hpp"
#include "base/utf8.hpp"
#include <commctrl.h>
#include <algorithm>

namespace meme::gui {

namespace {

constexpr const wchar_t* kPluginDlgClass = L"MemeStudioPluginManagerV2";

enum PluginMgrId : int {
    IdPluginList     = 2301,
    IdPluginDesc     = 2302,
    IdSigPolicyCombo = 2303,
    IdCmdCombo       = 2304,
    IdBtnRunCmd      = 2305,
    IdBtnApply       = 2306,
    IdBtnClose       = 2307
};

struct PluginMgrState {
    uikit::DpiContext dpi;
    HWND hwnd = nullptr;
    HWND listView = nullptr;
    HWND descBox = nullptr;
    HWND sigCombo = nullptr;
    HWND cmdCombo = nullptr;
    HWND btnRun = nullptr;

    std::vector<core::PluginManifest> plugins;
    std::vector<std::string> enabledIds;
    core::SignaturePolicy policy = core::SignaturePolicy::Disabled;

    std::function<bool(const std::vector<std::string>&, core::SignaturePolicy, std::string&)> onApply;
    std::function<void(const std::string&, const std::string&)> onRunCommand;

    int selectedPluginIndex() const noexcept {
        return ListView_GetNextItem(listView, -1, LVNI_SELECTED);
    }

    void populate() {
        ListView_DeleteAllItems(listView);
        for (size_t i = 0; i < plugins.size(); ++i) {
            const auto& p = plugins[i];
            std::wstring wname = base::utf8ToWideLossy(p.name);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = wname.data();
            ListView_InsertItem(listView, &item);

            std::wstring wid = base::utf8ToWideLossy(p.id);
            ListView_SetItemText(listView, static_cast<int>(i), 1, wid.data());
            std::wstring wver = base::utf8ToWideLossy(p.loadError.empty() ? p.version : ("不可用: " + p.loadError));
            ListView_SetItemText(listView, static_cast<int>(i), 2, wver.data());

            bool checked = std::find(enabledIds.begin(), enabledIds.end(), p.id) != enabledIds.end();
            ListView_SetCheckState(listView, static_cast<int>(i), checked ? TRUE : FALSE);
        }

        // Clear 3-option ComboBox instead of confusing tri-state checkbox!
        SendMessageW(sigCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"不验证插件数字签名（开发/本地自编译）"));
        SendMessageW(sigCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"要求插件具有受信任的有效数字签名"));
        SendMessageW(sigCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"要求插件签名者证书与主程序完全一致"));
        SendMessageW(sigCombo, CB_SETCURSEL, static_cast<WPARAM>(policy), 0);

        if (!plugins.empty()) {
            ListView_SetItemState(listView, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        refreshSelectionDetails();
    }

    void refreshSelectionDetails() {
        SendMessageW(cmdCombo, CB_RESETCONTENT, 0, 0);
        int idx = selectedPluginIndex();
        if (idx < 0 || static_cast<size_t>(idx) >= plugins.size()) {
            uikit::setTextUtf8(descBox, plugins.empty()
                ? "当前 plugins/ 目录未发现已安装的插件包。可将 MemeConvertStudio 编译产物解压至 plugins/ 目录。"
                : "请选择上方插件查看详情与可用命令。");
            EnableWindow(cmdCombo, FALSE);
            EnableWindow(btnRun, FALSE);
            return;
        }
        const auto& p = plugins[static_cast<size_t>(idx)];
        std::string info = p.name + " (" + p.id + " v" + p.version + ")\r\n" + p.description;
        if (!p.loadError.empty()) {
            info += "\r\n[加载错误] " + p.loadError;
        }
        uikit::setTextUtf8(descBox, info);

        for (const auto& c : p.commands) {
            std::wstring wt = base::utf8ToWideLossy(c.title + " (" + c.id + ")");
            SendMessageW(cmdCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wt.c_str()));
        }
        bool hasCmds = !p.commands.empty() && p.loadError.empty();
        if (hasCmds) SendMessageW(cmdCombo, CB_SETCURSEL, 0, 0);
        EnableWindow(cmdCombo, hasCmds ? TRUE : FALSE);
        EnableWindow(btnRun, hasCmds ? TRUE : FALSE);
    }

    bool tryApply(bool closeAfterSuccess) {
        std::vector<std::string> nextEnabled;
        for (size_t i = 0; i < plugins.size(); ++i) {
            if (ListView_GetCheckState(listView, static_cast<int>(i)) && plugins[i].loadError.empty()) {
                nextEnabled.push_back(plugins[i].id);
            }
        }
        LRESULT polSel = SendMessageW(sigCombo, CB_GETCURSEL, 0, 0);
        core::SignaturePolicy nextPol = core::SignaturePolicy::Disabled;
        if (polSel == 1) nextPol = core::SignaturePolicy::Trusted;
        else if (polSel == 2) nextPol = core::SignaturePolicy::SameSigner;

        std::string errMsg;
        // Validate & commit BEFORE closing window (fixes BUG-02!)
        if (onApply && !onApply(nextEnabled, nextPol, errMsg)) {
            uikit::showErrorBox(hwnd, "应用插件设置失败", errMsg);
            return false;
        }
        enabledIds = std::move(nextEnabled);
        policy = nextPol;
        if (closeAfterSuccess) {
            DestroyWindow(hwnd);
        }
        return true;
    }

    static LRESULT CALLBACK wndProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(window, msg, wp, lp);
        }
        auto* st = reinterpret_cast<PluginMgrState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!st) return DefWindowProcW(window, msg, wp, lp);

        switch (msg) {
            case WM_CREATE: {
                st->hwnd = window;
                st->dpi.updateForWindow(window);
                HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
                const auto& d = st->dpi;

                st->listView = CreateWindowExW(
                    WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                    d.px(16), d.px(16), d.px(608), d.px(200), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdPluginList)), inst, nullptr);
                ListView_SetExtendedListViewStyle(st->listView, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.pszText = const_cast<LPWSTR>(L"启用 / 名称"); col.cx = d.px(210);
                ListView_InsertColumn(st->listView, 0, &col);
                col.pszText = const_cast<LPWSTR>(L"插件标识符"); col.cx = d.px(220);
                ListView_InsertColumn(st->listView, 1, &col);
                col.pszText = const_cast<LPWSTR>(L"版本 / 状态"); col.cx = d.px(155);
                ListView_InsertColumn(st->listView, 2, &col);

                st->descBox = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                    d.px(16), d.px(226), d.px(608), d.px(72), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdPluginDesc)), inst, nullptr);

                CreateWindowExW(0, L"STATIC", L"插件签名策略：", WS_CHILD | WS_VISIBLE,
                    d.px(16), d.px(312), d.px(110), d.px(24), window, nullptr, inst, nullptr);
                st->sigCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                    d.px(126), d.px(308), d.px(498), d.px(160), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSigPolicyCombo)), inst, nullptr);

                CreateWindowExW(0, L"STATIC", L"插件操作命令：", WS_CHILD | WS_VISIBLE,
                    d.px(16), d.px(350), d.px(110), d.px(24), window, nullptr, inst, nullptr);
                st->cmdCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                    d.px(126), d.px(346), d.px(382), d.px(180), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdCmdCombo)), inst, nullptr);
                st->btnRun = CreateWindowExW(0, L"BUTTON", L"执行命令", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    d.px(516), d.px(345), d.px(108), d.px(28), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdBtnRunCmd)), inst, nullptr);

                CreateWindowExW(0, L"BUTTON", L"保存并应用", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    d.px(400), d.px(392), d.px(116), d.px(32), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdBtnApply)), inst, nullptr);
                CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    d.px(524), d.px(392), d.px(100), d.px(32), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdBtnClose)), inst, nullptr);

                d.applyNormalFontToChildren(window);
                st->populate();
                return 0;
            }
            case WM_NOTIFY: {
                auto* hdr = reinterpret_cast<NMHDR*>(lp);
                if (hdr && hdr->idFrom == IdPluginList && hdr->code == LVN_ITEMCHANGED) {
                    st->refreshSelectionDetails();
                }
                break;
            }
            case WM_COMMAND: {
                int id = LOWORD(wp);
                if (id == IdBtnApply) {
                    st->tryApply(true);
                    return 0;
                }
                if (id == IdBtnRunCmd) {
                    if (!st->tryApply(false)) return 0;
                    int pIdx = st->selectedPluginIndex();
                    LRESULT cIdx = SendMessageW(st->cmdCombo, CB_GETCURSEL, 0, 0);
                    if (pIdx >= 0 && static_cast<size_t>(pIdx) < st->plugins.size() && cIdx >= 0) {
                        const auto& p = st->plugins[static_cast<size_t>(pIdx)];
                        if (static_cast<size_t>(cIdx) < p.commands.size() && st->onRunCommand) {
                            st->onRunCommand(p.id, p.commands[static_cast<size_t>(cIdx)].id);
                        }
                    }
                    return 0;
                }
                if (id == IdBtnClose || id == IDCANCEL) {
                    DestroyWindow(window);
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
                DestroyWindow(window);
                return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                delete st;
                return 0;
        }
        return DefWindowProcW(window, msg, wp, lp);
    }
};

} // namespace

void openPluginManagerDialog(
    HINSTANCE instance,
    HWND owner,
    std::span<const core::PluginManifest> discovered,
    const std::vector<std::string>& currentEnabled,
    core::SignaturePolicy currentPolicy,
    std::function<bool(const std::vector<std::string>&, core::SignaturePolicy, std::string&)> onApplySettings,
    std::function<void(const std::string&, const std::string&)> onRunCommand) {
    WNDCLASSEXW existing{sizeof(WNDCLASSEXW)};
    if (!GetClassInfoExW(instance, kPluginDlgClass, &existing)) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = &PluginMgrState::wndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPluginDlgClass;
        RegisterClassExW(&wc);
    }

    auto* st = new PluginMgrState();
    st->plugins.assign(discovered.begin(), discovered.end());
    st->enabledIds = currentEnabled;
    st->policy = currentPolicy;
    st->onApply = std::move(onApplySettings);
    st->onRunCommand = std::move(onRunCommand);

    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    if (dpi < 72) dpi = 96;
    int cw = MulDiv(640, static_cast<int>(dpi), 96);
    int ch = MulDiv(440, static_cast<int>(dpi), 96);

    HWND win = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kPluginDlgClass,
        L"插件管理",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, cw, ch,
        owner, nullptr, instance, st);
    if (!win) {
        delete st;
        return;
    }
    uikit::sizeAndCenterWindow(win, owner, cw, ch);
    ShowWindow(win, SW_SHOW);
}

} // namespace meme::gui
