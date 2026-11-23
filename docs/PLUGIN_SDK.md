# MemeStudio 插件 SDK 规范 (`MemeStudioPluginABI v2`)

`MemeStudio` 采用微内核插件架构：宿主核心不包含任何特定插件的业务逻辑或硬编码插件 ID，所有扩展能力均通过 `sdk/memestudio_plugin.h`（C ABI v2）动态装载。官方示例脚手架与全部 8 个独立功能插件位于独立仓库 **`MemeStudioPlugins`**。

## 1. 插件目录与清单 (`plugin.json`)

每个插件位于 `<MemeStudio.exe 所在目录>/plugins/<slug>/`，至少包含 `<slug>.dll` 与 `plugin.json`：

```json
{
  "api_version": 2,
  "id": "vendor.my-plugin",
  "name": "插件显示名称",
  "version": "1.0.0",
  "description": "插件功能简述",
  "library": "my-plugin.dll",
  "commands": [
    {
      "id": "open-panel",
      "title": "扩展面板…",
      "toolbar": true
    }
  ]
}
```
- 设置 `"toolbar": true` 的命令在启用插件后会自动渲染在主窗口右上角动态扩展栏中。

## 2. 实例生命周期与内存安全契约

插件必须导出 `extern "C" const MemeStudioPluginV2* __cdecl MemeStudioGetPluginV2(void)`：
1. **零全局状态实例隔离**：`create(host, &instance, &err)` 创建插件私有实例指针，后续所有回调均接收该 `void* instance`；卸载时仅当 `create` 返回 `0` 才会调用 `destroy(instance)`。
2. **统一跨边界内存分配**：插件向宿主返回任何动态字符串或字节块（`MemeStudioBytesV2`）时，必须通过 `host->alloc`（或 `sdk/memestudio_plugin_kit.hpp` 中的 `HostClient::writeOutBytes`）分配；宿主分配并返回给插件的数据必须通过 `host->free_bytes` / `host->free_images` 释放，严禁跨 DLL 直接混用 `delete` / `free`。
3. **强制 AppData 读写隔离**：插件严禁向自身安装目录写入配置、日志或缓存文件，必须调用 `host->get_data_dir` 获取 `%LOCALAPPDATA%\MemeStudio\plugins\<id>\` 专属沙箱目录，敏感凭据通过 `host->protect_secret` / `unprotect_secret`（DPAPI）加密存储。

## 3. 宿主通用服务表 (`MemeStudioHostV2`)

- **网络池与流式解析 (`http_send`)**：复用宿主 `WinHttpPool` 连接池、系统代理（本机回环自动直连）、取消令牌与 SSE 事件流解析。
- **图像与动画编解码 (`probe_media` / `decode_media` / `encode_media`)**：复用宿主 WIC 解码器、文件头极速探测与共享主调色板防闪烁 GIF 编码器。
- **工作台协同 (`get_workspace_state` / `set_session_endpoint_override` / `present_output_file` / `run_core_replace_job`)**：
  - `set_session_endpoint_override` 以非破坏性会话横幅形式接管当前连接，绝不覆盖用户保存的商业 API 密钥；
  - `run_core_replace_job` 允许批量或自动化插件直接驱动宿主核心角色替换管线。
- **高分屏面板容器 (`open_panel`)**：创建受宿主管理的 `PerMonitorV2` DPI 感知非模态工具窗口。

## 4. 四类标准扩展钩子 (`MemeStudioPluginV2`)

1. `invoke_command`：响应工具栏按钮、插件管理器或 `MemeStudio-cli plugin-cmd` 调用。
2. `transform_prompt`：在发送图像编辑请求前改写或增强提示词。
3. `process_frames_batch`：在 `"input"`（输入预处理）或 `"output"`（成品输出前）阶段对整组帧执行单次批处理（支持插件重分配放大后的帧像素缓冲区）。
4. `intercept_image_api`：透明拦截 `"edit"`、`"generate"`、`"models"` 请求（返回 `1` 表示放行交还宿主默认单端点客户端，返回 `0` 表示已由插件接管完成）。
