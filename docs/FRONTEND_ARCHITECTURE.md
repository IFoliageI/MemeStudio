# 前后端解耦与 WinUI 3 / 多前端升级指南

`MemeStudio` 采用前后端彻底解耦架构：三大核心创作引擎（**角色替换**、**提示词生图**、**局部精修与提示词修图**）、多源素材队列（`<- [ 1 ] / N ->`）、WIC 防闪烁编解码器、WinHTTP 连接池、DPAPI 凭据存储与插件宿主均封装在无界面依赖的 `meme::core::StudioSession` 中，并通过 **`MemeStudioEngine.dll`（`sdk/memestudio_engine.h`）** 暴露跨语言纯 C ABI。

## 1. 模块分层边界

- **`meme_core`（无头静态内核）**：包含 `StudioSession`、`ReplaceEngine`、`DrawEngine`、`RetouchEngine`、`WicCodec`（内置直出预乘 `PARGB32` 像素转换）、`WinHttpPool` 与 `PluginHost`，完全不依赖 `user32` / `gdi32` / `comctl32`。
- **`MemeStudioEngine.dll`（跨语言 C ABI 引擎库）**：屏蔽 C++ STL 内存布局差异，供 **WinUI 3（C# .NET `LibraryImport` 或 C++/WinRT）**、**Tauri（Rust FFI）** 或 **Qt 6** 直接调用。
- **薄视图层（可随时替换）**：当前内置的极简 Win32 高分屏界面（`src/gui/`）仅负责渲染控件与转发用户输入。

## 2. 接入 WinUI 3（Windows App SDK）的标准流程

1. **会话与异步事件订阅**：调用 `meme_session_create(onEvent, userData, &session, &err)` 创建会话；在 `onEvent` 中通过 `DispatcherQueue.TryEnqueue` 将 `progress`、`job_succeeded`、`job_failed`、`models_fetched`、`endpoint_override_changed`、`present_output`、`log` 等 JSON 事件切回 UI 线程刷新 ViewModel。
2. **三大内置工作流一键触发**：
   - 角色替换（支持单图与多源队列）：`meme_session_start_replace`
   - 提示词生图：`meme_session_start_draw`
   - 提示词修图 / 局部框选精修：`meme_session_start_retouch`
3. **零编码损耗 60fps 预览**：调用 `meme_session_read_frame_pargb32` 直接将预乘 `BGRA8`（`PARGB32`）像素帧写入 WinUI 3 `WriteableBitmap.PixelBuffer` 或 Win2D `CanvasBitmap`。
4. **插件面板双轨兼容**：
   - 通过 `WinRT.Interop.WindowNative.GetWindowHandle(window)` 传入主窗口 `HWND`，现有插件弹窗首日即可作为受控子窗口正常弹出；
   - 发送标准命令 `MEMESTUDIO_CMD_UI_SCHEMA` (`"get-ui-schema"`) 获取插件声明式表单描述，即可由 WinUI 3 宿主自动渲染原生 Fluent XAML 控件。
