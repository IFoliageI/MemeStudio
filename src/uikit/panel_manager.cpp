#include "uikit/panel_manager.hpp"
#include "uikit/controls.hpp"
#include "base/utf8.hpp"
#include <algorithm>

namespace meme::uikit {

namespace {
constexpr const wchar_t* kPanelClassName = L"MemeStudioToolPanelV2";

void ensurePanelClassRegistered(HINSTANCE instance, WNDPROC proc) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(WNDCLASSEXW);
    if (GetClassInfoExW(instance, kPanelClassName, &existing)) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kPanelClassName;
    RegisterClassExW(&wc);
}
} // namespace

struct PanelManager::Entry {
    PanelManager* manager = nullptr;
    std::string key;
    HWND hwnd = nullptr;
    intptr_t(MEMESTUDIO_CALL* pluginProc)(void*, void*, uint32_t, uintptr_t, intptr_t) = nullptr;
    void* panelCtx = nullptr;
    void(MEMESTUDIO_CALL* onDestroy)(void*) = nullptr;

    ~Entry() {
        if (onDestroy) {
            onDestroy(panelCtx);
            onDestroy = nullptr;
            panelCtx = nullptr;
        }
    }
};

PanelManager::PanelManager(HINSTANCE instance) : instance_(instance) {
    ensurePanelClassRegistered(instance_, &PanelManager::hostProc);
}

PanelManager::~PanelManager() {
    closeAll();
}

LRESULT CALLBACK PanelManager::hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* entry = static_cast<Entry*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(entry));
        if (entry) entry->hwnd = hwnd;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* entry = reinterpret_cast<Entry*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!entry) return DefWindowProcW(hwnd, msg, wp, lp);

    if (msg == WM_DPICHANGED) {
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested) {
            SetWindowPos(
                hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    intptr_t res = 0;
    if (entry->pluginProc) {
        res = entry->pluginProc(entry->panelCtx, hwnd, msg, wp, lp);
    } else {
        res = DefWindowProcW(hwnd, msg, wp, lp);
    }

    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (entry->manager) {
            auto& list = entry->manager->openPanels_;
            std::erase_if(list, [entry](const auto& sp) { return sp.get() == entry; });
        }
    }
    return res;
}

int32_t PanelManager::openOrActivatePluginPanel(
    std::string_view panelKey,
    void* ownerHwnd,
    int32_t widthDip,
    int32_t heightDip,
    std::string_view titleUtf8,
    intptr_t(MEMESTUDIO_CALL* pluginWndProc)(void*, void*, uint32_t, uintptr_t, intptr_t),
    void* panelCtx,
    void(MEMESTUDIO_CALL* onDestroy)(void*)) {
    for (const auto& existing : openPanels_) {
        if (existing->key == panelKey && existing->hwnd && IsWindow(existing->hwnd)) {
            if (onDestroy && panelCtx) {
                onDestroy(panelCtx); // Clean up duplicate incoming context since existing panel is activated
            }
            ShowWindow(existing->hwnd, SW_RESTORE);
            SetForegroundWindow(existing->hwnd);
            return 0;
        }
    }

    HWND owner = static_cast<HWND>(ownerHwnd);
    UINT dpi = (owner && IsWindow(owner)) ? GetDpiForWindow(owner) : 96u;
    if (dpi < 72) dpi = 96;
    int clientW = MulDiv(std::max<int32_t>(280, widthDip), static_cast<int>(dpi), 96);
    int clientH = MulDiv(std::max<int32_t>(200, heightDip), static_cast<int>(dpi), 96);

    auto entry = std::make_shared<Entry>();
    entry->manager = this;
    entry->key.assign(panelKey);
    entry->pluginProc = pluginWndProc;
    entry->panelCtx = panelCtx;
    entry->onDestroy = onDestroy;

    openPanels_.push_back(entry);
    std::wstring wTitle = base::utf8ToWideLossy(titleUtf8);

    HWND created = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kPanelClassName,
        wTitle.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, clientW, clientH,
        owner, nullptr, instance_, entry.get());

    if (!created) {
        std::erase(openPanels_, entry);
        return -1;
    }

    sizeAndCenterWindow(created, owner, clientW, clientH);
    ShowWindow(created, SW_SHOW);
    UpdateWindow(created);
    return 0;
}

void PanelManager::closePanel(std::string_view panelKey) noexcept {
    for (const auto& item : openPanels_) {
        if (item->key == panelKey && item->hwnd && IsWindow(item->hwnd)) {
            DestroyWindow(item->hwnd);
            break;
        }
    }
}

void PanelManager::closeAll() noexcept {
    auto snapshot = openPanels_;
    for (const auto& item : snapshot) {
        if (item->hwnd && IsWindow(item->hwnd)) {
            DestroyWindow(item->hwnd);
        }
    }
    openPanels_.clear();
}

bool PanelManager::translateDialogNavigation(MSG& msg) const noexcept {
    HWND root = GetAncestor(msg.hwnd, GA_ROOT);
    if (!root) return false;
    for (const auto& item : openPanels_) {
        // Only intercept Tab navigation for panels we actually own (never blindly hijack all #32770!)
        if (item->hwnd == root && IsWindow(item->hwnd)) {
            // Preserve Enter key inside multiline ES_WANTRETURN edit controls!
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
                LONG style = GetWindowLongW(msg.hwnd, GWL_STYLE);
                if ((style & ES_MULTILINE) && (style & ES_WANTRETURN)) {
                    return false;
                }
            }
            return IsDialogMessageW(root, &msg) != FALSE;
        }
    }
    return false;
}

} // namespace meme::uikit
