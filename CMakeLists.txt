cmake_minimum_required(VERSION 3.24)
project(MemeStudio VERSION 2.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "MemeStudio requires Windows 10 (1903+) or newer.")
endif()

option(MEMESTUDIO_BUILD_ENGINE_DLL "Build frontend-agnostic C ABI engine DLL (MemeStudioEngine.dll)" ON)
option(MEMESTUDIO_BUILD_GUI        "Build Win32 desktop application MemeStudio.exe" ON)
option(MEMESTUDIO_BUILD_CLI        "Build headless CLI tool MemeStudio-cli.exe" ON)
option(MEMESTUDIO_BUILD_TESTS      "Build unit and E2E test suites" ON)

add_library(meme_compiler_flags INTERFACE)
target_compile_features(meme_compiler_flags INTERFACE cxx_std_20)
target_compile_definitions(meme_compiler_flags INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(meme_compiler_flags INTERFACE /utf-8 /W4 /EHsc /permissive-)
endif()

# 1. Pure Headless Microkernel Core + Stateful StudioSession (zero GUI / dialog / test code!)
add_library(meme_core STATIC
    src/base/utf8.cpp
    src/base/crypto.cpp
    src/base/fs_io.cpp
    src/base/app_paths.cpp
    src/base/managed_process.cpp
    src/media/image_ops.cpp
    src/media/wic_codec.cpp
    src/net/endpoint.cpp
    src/net/redactor.cpp
    src/net/winhttp_pool.cpp
    src/net/openai_client.cpp
    src/core/config_store.cpp
    src/core/prompt_builder.cpp
    src/core/replace_engine.cpp
    src/core/plugin_host.cpp
    src/core/studio_session.cpp
)
target_include_directories(meme_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
target_include_directories(meme_core SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(meme_core
    PUBLIC meme_compiler_flags
    PRIVATE windowscodecs ole32 uuid propsys winhttp wintrust crypt32 bcrypt shlwapi advapi32 shell32)
if(MSVC)
    set_property(TARGET meme_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 2. Language-Neutral C ABI Backend Engine DLL (ready for WinUI 3 C#/.NET AOT, C++/WinRT, Tauri, Qt)
if(MEMESTUDIO_BUILD_ENGINE_DLL)
    add_library(MemeStudioEngine SHARED
        src/engine/engine_c_api.cpp)
    target_include_directories(MemeStudioEngine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
    target_include_directories(MemeStudioEngine SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
    target_link_libraries(MemeStudioEngine PRIVATE meme_core ole32)
    if(MSVC)
        set_property(TARGET MemeStudioEngine PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# 3. Pure Headless CLI (independent of GUI target, zero GDI/comctl32/comdlg32!)
if(MEMESTUDIO_BUILD_CLI)
    add_executable(MemeStudio-cli
        src/cli/cli_main.cpp)
    target_include_directories(MemeStudio-cli SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
    target_link_libraries(MemeStudio-cli PRIVATE meme_core ole32)
    if(MSVC)
        set_property(TARGET MemeStudio-cli PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_options(MemeStudio-cli PRIVATE
            /ENTRY:wmainCRTStartup
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/gui/app.manifest")
    endif()
endif()

# 4. PerMonitorV2 DPI-Aware Win32 UIKit + Thin Desktop Frontend (driving StudioSession)
if(MEMESTUDIO_BUILD_GUI)
    add_library(meme_uikit STATIC
        src/uikit/dpi.cpp
        src/uikit/controls.cpp
        src/uikit/file_dialogs.cpp
        src/uikit/image_canvas.cpp
        src/uikit/panel_manager.cpp)
    target_include_directories(meme_uikit PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
    target_link_libraries(meme_uikit
        PUBLIC meme_compiler_flags
        PRIVATE user32 gdi32 msimg32 comctl32 comdlg32 shell32 ole32 uuid)
    if(MSVC)
        set_property(TARGET meme_uikit PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    add_executable(MemeStudio WIN32
        src/gui/win_main.cpp
        src/gui/presets_dialog.cpp
        src/gui/plugin_dialog.cpp
        src/gui/studio_window.cpp)
    target_include_directories(MemeStudio SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
    target_link_libraries(MemeStudio PRIVATE meme_core meme_uikit user32 gdi32 comctl32 shell32 ole32)
    if(MSVC)
        set_property(TARGET MemeStudio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_options(MemeStudio PRIVATE
            /ENTRY:wmainCRTStartup
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/gui/app.manifest")
    endif()
endif()

# 5. Standalone Automated Test Targets
if(MEMESTUDIO_BUILD_TESTS)
    enable_testing()
    add_executable(MemeStudioTests
        tests/unit/unit_tests.cpp)
    target_include_directories(MemeStudioTests SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
    target_link_libraries(MemeStudioTests PRIVATE meme_core ole32)
    if(MEMESTUDIO_BUILD_ENGINE_DLL)
        target_link_libraries(MemeStudioTests PRIVATE MemeStudioEngine)
    endif()
    if(MSVC)
        set_property(TARGET MemeStudioTests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_options(MemeStudioTests PRIVATE /ENTRY:wmainCRTStartup)
    endif()
    add_test(NAME meme-core-unit-tests COMMAND MemeStudioTests)

    if(MEMESTUDIO_BUILD_CLI)
        add_test(NAME meme-cli-help-smoke COMMAND MemeStudio-cli --help)
        find_program(NODE_EXECUTABLE node)
        if(NODE_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/cli_e2e.mjs")
            add_test(
                NAME meme-cli-e2e
                COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/cli_e2e.mjs" "$<TARGET_FILE:MemeStudio-cli>")
        endif()
    endif()
endif()
