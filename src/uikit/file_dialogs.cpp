#include "uikit/file_dialogs.hpp"
#include "base/error.hpp"
#include <shobjidl.h>
#include <wrl/client.h>

namespace meme::uikit {

using Microsoft::WRL::ComPtr;

namespace {
constexpr COMDLG_FILTERSPEC kImageReadFilters[] = {
    {L"支持的图片格式 (*.png;*.gif;*.jpg;*.jpeg;*.webp;*.bmp)", L"*.png;*.gif;*.jpg;*.jpeg;*.webp;*.bmp"},
    {L"所有文件 (*.*)", L"*.*"}
};
} // namespace

std::optional<fs::path> pickOpenImageFile(HWND owner, const wchar_t* title) {
    ComPtr<IFileOpenDialog> dlg;
    checkHr(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)), "创建打开文件对话框");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
    dlg->SetFileTypes(static_cast<UINT>(std::size(kImageReadFilters)), kImageReadFilters);
    if (title) dlg->SetTitle(title);

    HRESULT hr = dlg->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    checkHr(hr, "显示打开文件对话框");

    ComPtr<IShellItem> item;
    checkHr(dlg->GetResult(&item), "读取所选文件");
    PWSTR raw = nullptr;
    checkHr(item->GetDisplayName(SIGDN_FILESYSPATH, &raw), "解析文件路径");
    fs::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::vector<fs::path> pickOpenMultipleImageFiles(HWND owner, const wchar_t* title) {
    ComPtr<IFileOpenDialog> dlg;
    checkHr(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)), "创建多选文件对话框");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT | FOS_NOCHANGEDIR);
    dlg->SetFileTypes(static_cast<UINT>(std::size(kImageReadFilters)), kImageReadFilters);
    if (title) dlg->SetTitle(title);

    HRESULT hr = dlg->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    checkHr(hr, "显示多选文件对话框");

    ComPtr<IShellItemArray> items;
    checkHr(dlg->GetResults(&items), "获取多选文件结果");
    DWORD count = 0;
    items->GetCount(&count);
    std::vector<fs::path> out;
    out.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                out.emplace_back(raw);
                CoTaskMemFree(raw);
            }
        }
    }
    return out;
}

std::optional<fs::path> pickSaveOutputFile(HWND owner, const fs::path& suggestedPath, bool animatedGif) {
    ComPtr<IFileSaveDialog> dlg;
    checkHr(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)), "创建保存文件对话框");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR);

    const COMDLG_FILTERSPEC gifFilter[] = {{L"GIF 动画 (*.gif)", L"*.gif"}};
    const COMDLG_FILTERSPEC pngFilter[] = {{L"PNG 图像 (*.png)", L"*.png"}};
    dlg->SetFileTypes(1, animatedGif ? gifFilter : pngFilter);
    dlg->SetDefaultExtension(animatedGif ? L"gif" : L"png");
    dlg->SetTitle(L"选择生成结果保存位置");

    if (!suggestedPath.empty()) {
        dlg->SetFileName(suggestedPath.filename().c_str());
        if (!suggestedPath.parent_path().empty() && fs::is_directory(suggestedPath.parent_path())) {
            ComPtr<IShellItem> folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(suggestedPath.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                dlg->SetFolder(folder.Get());
            }
        }
    }

    HRESULT hr = dlg->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    checkHr(hr, "显示保存文件对话框");

    ComPtr<IShellItem> item;
    checkHr(dlg->GetResult(&item), "获取保存路径");
    PWSTR raw = nullptr;
    checkHr(item->GetDisplayName(SIGDN_FILESYSPATH, &raw), "解析保存路径");
    fs::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::optional<fs::path> pickFolder(HWND owner, const wchar_t* title) {
    ComPtr<IFileOpenDialog> dlg;
    checkHr(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)), "创建选择文件夹对话框");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (title) dlg->SetTitle(title);

    HRESULT hr = dlg->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    checkHr(hr, "显示选择文件夹对话框");

    ComPtr<IShellItem> item;
    checkHr(dlg->GetResult(&item), "读取文件夹结果");
    PWSTR raw = nullptr;
    checkHr(item->GetDisplayName(SIGDN_FILESYSPATH, &raw), "解析文件夹路径");
    fs::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

} // namespace meme::uikit
