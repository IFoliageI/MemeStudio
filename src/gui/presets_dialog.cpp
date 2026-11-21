#include "gui/presets_dialog.hpp"
#include "gui/presets.hpp"
#include "uikit/controls.hpp"
#include "base/utf8.hpp"
#include <vector>

namespace meme::gui {

namespace {

constexpr const wchar_t* kPresetPopupClass = L"MemeStudioPresetPickerV2";

enum PresetControlId : int {
    IdCategoryCombo = 2101,
    IdTitleList     = 2102,
    IdPreviewEdit   = 2103,
    IdBtnAppend     = 2104,
    IdBtnReplace    = 2105,
    IdBtnClose      = 2106
};

struct PresetPickerState {
    uikit::DpiContext dpi;
    HWND hwnd = nullptr;
    HWND categoryCombo = nullptr;
    HWND titleList = nullptr;
    HWND previewBox = nullptr;
    std::vector<std::string> categories;
    std::vector<size_t> filteredIndices;
    std::function<void(const std::string&, PresetApplyMode)> onApply;

    void populateCategories() {
        categories.clear();
        for (const auto& item : kInstructionPresets) {
            std::string cat(item.category);
            if (std::find(categories.begin(), categories.end(), cat) == categories.end()) {
                categories.push_back(cat);
                std::wstring wcat = base::utf8ToWideStrict(cat);
                SendMessageW(categoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wcat.c_str()));
            }
        }
        SendMessageW(categoryCombo, CB_SETCURSEL, 0, 0);
        refreshTitles();
    }

    void refreshTitles() {
        SendMessageW(titleList, LB_RESETCONTENT, 0, 0);
        filteredIndices.clear();
        LRESULT selCat = SendMessageW(categoryCombo, CB_GETCURSEL, 0, 0);
        if (selCat < 0 || static_cast<size_t>(selCat) >= categories.size()) return;

        const std::string& activeCat = categories[static_cast<size_t>(selCat)];
        for (size_t i = 0; i < std::size(kInstructionPresets); ++i) {
            if (activeCat == kInstructionPresets[i].category) {
                filteredIndices.push_back(i);
                std::wstring wtitle = base::utf8ToWideStrict(kInstructionPresets[i].title);
                SendMessageW(titleList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wtitle.c_str()));
            }
        }
        if (!filteredIndices.empty()) {
            SendMessageW(titleList, LB_SETCURSEL, 0, 0);
        }
        refreshPreview();
    }

    void refreshPreview() {
        LRESULT sel = SendMessageW(titleList, LB_GETCURSEL, 0, 0);
        if (sel < 0 || static_cast<size_t>(sel) >= filteredIndices.size()) {
            uikit::setTextUtf8(previewBox, "");
            return;
        }
        const auto& preset = kInstructionPresets[filteredIndices[static_cast<size_t>(sel)]];
        uikit::setTextUtf8(previewBox, preset.instruction);
    }

    void commitSelection(PresetApplyMode mode) {
        LRESULT sel = SendMessageW(titleList, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && static_cast<size_t>(sel) < filteredIndices.size() && onApply) {
            const auto& preset = kInstructionPresets[filteredIndices[static_cast<size_t>(sel)]];
            onApply(preset.instruction, mode);
        }
        DestroyWindow(hwnd);
    }

    static LRESULT CALLBACK wndProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(window, msg, wp, lp);
        }
        auto* st = reinterpret_cast<PresetPickerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!st) return DefWindowProcW(window, msg, wp, lp);

        switch (msg) {
            case WM_CREATE: {
                st->hwnd = window;
                st->dpi.updateForWindow(window);
                HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
                const auto& d = st->dpi;

                CreateWindowExW(0, L"STATIC", L"预设分类：", WS_CHILD | WS_VISIBLE,
                                d.px(16), d.px(16), d.px(90), d.px(22), window, nullptr, inst, nullptr);
                st->categoryCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                d.px(100), d.px(13), d.px(220), d.px(220), window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdCategoryCombo)), inst, nullptr);

                st->titleList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                                d.px(16), d.px(48), d.px(200), d.px(260), window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdTitleList)), inst, nullptr);

                st->previewBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                d.px(228), d.px(48), d.px(316), d.px(260), window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdPreviewEdit)), inst, nullptr);

                CreateWindowExW(0, L"BUTTON", L"追加到附加要求", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                d.px(228), d.px(322), d.px(120), d.px(32), window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdBtnAppend)), inst, nullptr);
                CreateWindowExW(0, L"BUTTON", L"替换当前要求", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                d.px(356), d.px(322), d.px(106), d.px(32), window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdBtnReplace)), inst, nullptr);
                CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                d.px(470), d.px(322), d.px(74), d.px(32), window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdBtnClose)), inst, nullptr);

                d.applyNormalFontToChildren(window);
                st->populateCategories();
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(wp);
                int code = HIWORD(wp);
                if (id == IdCategoryCombo && code == CBN_SELCHANGE) {
                    st->refreshTitles();
                    return 0;
                }
                if (id == IdTitleList && code == LBN_SELCHANGE) {
                    st->refreshPreview();
                    return 0;
                }
                if (id == IdTitleList && code == LBN_DBLCLK) {
                    st->commitSelection(PresetApplyMode::Append);
                    return 0;
                }
                if (id == IdBtnAppend) {
                    st->commitSelection(PresetApplyMode::Append);
                    return 0;
                }
                if (id == IdBtnReplace) {
                    st->commitSelection(PresetApplyMode::Replace);
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

void openPresetsPicker(
    HINSTANCE instance,
    HWND owner,
    std::function<void(const std::string& text, PresetApplyMode mode)> onApply) {
    WNDCLASSEXW existing{sizeof(WNDCLASSEXW)};
    if (!GetClassInfoExW(instance, kPresetPopupClass, &existing)) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = &PresetPickerState::wndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kPresetPopupClass;
        RegisterClassExW(&wc);
    }

    auto* st = new PresetPickerState();
    st->onApply = std::move(onApply);
    UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    if (dpi < 72) dpi = 96;
    int cw = MulDiv(560, static_cast<int>(dpi), 96);
    int ch = MulDiv(370, static_cast<int>(dpi), 96);

    HWND win = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kPresetPopupClass,
        L"重绘要求预设库",
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
