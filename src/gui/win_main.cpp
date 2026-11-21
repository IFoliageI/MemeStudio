#include "gui/studio_window.hpp"
#include "uikit/controls.hpp"
#include "base/error.hpp"
#include <objbase.h>

int wmain(int argc, wchar_t** argv) {
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int exitCode = 0;
    try {
        meme::checkHr(comHr, "初始化桌面 COM 环境");
        std::filesystem::path initialFile;
        if (argc >= 2 && argv[1] && argv[1][0] != L'-') {
            initialFile = argv[1];
        }
        exitCode = meme::gui::runStudioDesktopApp(GetModuleHandleW(nullptr), initialFile);
    } catch (const std::exception& e) {
        meme::uikit::showErrorBox(nullptr, "MemeStudio 启动异常", e.what());
        exitCode = 1;
    }
    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }
    return exitCode;
}
