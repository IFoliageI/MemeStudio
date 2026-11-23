# 前后端解耦与 WinUI 3 / 多前端升级指南

`MemeStudio` 采用彻底的前后端分层架构：所有图像/GIF 编解码、OpenAI 兼容网络池、DPAPI 加密配置、多源任务队列以及插件宿主均封装在完全无界面依赖的后端控制器 `meme::core::StudioSession` 中，并通过 **`MemeStudioEngine.dll`（`sdk/memestudio_engine.h`）** 暴露稳定的纯 C ABI。

## 1. 分层架构边界

- **`meme_core`（静态内核）**：包含 `StudioSession`、`ReplaceEngine`、`WicCodec`（内置直出预乘 `PARGB32` 像素转换）、`WinHttpPool` 与 `PluginHost`，不链接 `user32` / `gdi32` / `comctl32`。
- **`MemeStudioEngine.dll`（跨语言引擎动态库）**：提供 100% 纯 C 导出接口（`extern "C"`），屏蔽 C++ STL ABI 差异，供 **WinUI 3（C# .NET `LibraryImport` 或 C++/WinRT）**、**Tauri（Rust FFI）** 或 **Qt 6** 直接加载。
- **前端视图层（随时可替换）**：当前内置轻量化 Win32 `PerMonitorV2` 前端（`src/gui/`），仅负责控件渲染、输入采集与 `WM_SESSION_EVENT` 消息刷新。

## 2. 接入 WinUI 3（Windows App SDK）的标准方式

### 2.1 会话与实时事件流绑定
1. 启动时调用 `meme_session_create(onEventCallback, userData, &session, &err)`。
2. 在 `onEventCallback` 中通过 WinUI 3 `DispatcherQueue.TryEnqueue` 将后台发出的 JSON 事件（`progress`、`job_succeeded`、`job_failed`、`models_fetched`、`endpoint_override_changed`、`present_output`、`log`）切回 UI 线程更新 `ObservableObject` ViewModel。

### 2.2 零编码损耗的 60fps 图像与 GIF 预览
调用 `meme_session_read_frame_pargb32` 可直接将当前激活源表情（支持 `<- [ 1 ] / N ->` 多源队列切换）、人物参考图或生成结果的指定帧以 **预乘 `BGRA8`（`PARGB32`）** 写入 WinUI 3 `WriteableBitmap.PixelBuffer`（或 Win2D `CanvasBitmap`），无需经过磁盘文件或 PNG Base64 二次编解码。

### 2.3 独立插件在 WinUI 3 下的双轨兼容
1. **首日无缝兼容（`IWindowNative`）**：通过 `WinRT.Interop.WindowNative.GetWindowHandle(mainWindow)` 取得主窗口底层 `HWND`，传入 `meme_session_invoke_plugin(..., ownerHwnd, ...)`，所有现有插件弹窗即可直接作为受控子窗口弹出。
2. **原生 Fluent XAML 自动渲染（`MEMESTUDIO_CMD_UI_SCHEMA`）**：WinUI 3 宿主可先向插件发送标准命令 `"get-ui-schema"`；若插件返回声明式表单 JSON（如 `memestudio.batch-convert` 已内置），前端可直接使用原生 Fluent XAML 控件渲染深浅色自适应界面，并在提交时调用 `submit_command`。
