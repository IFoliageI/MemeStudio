#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>

namespace meme::gui {

int runStudioDesktopApp(HINSTANCE instance, const std::filesystem::path& initialSource = {});

} // namespace meme::gui
