#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>
#include <optional>
#include <vector>

namespace meme::uikit {

namespace fs = std::filesystem;

std::optional<fs::path> pickOpenImageFile(HWND owner, const wchar_t* title = L"选择图片或 GIF 动画");
std::vector<fs::path> pickOpenMultipleImageFiles(HWND owner, const wchar_t* title = L"选择画风参考图片");
std::optional<fs::path> pickSaveOutputFile(HWND owner, const fs::path& suggestedPath, bool animatedGif);
std::optional<fs::path> pickFolder(HWND owner, const wchar_t* title = L"选择文件夹");

} // namespace meme::uikit
