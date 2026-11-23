# MemeStudio

面向 Windows 10/11 的轻量原生表情角色替换工作台与命令行工具（C++20 / Win32 PerMonitorV2）。

## 核心能力

- **表情与动图角色替换**：输入单张表情（PNG / JPEG / WebP / BMP）或 GIF 动画，结合人物参考图（支持可视框选裁剪）与最多 8 张附加画风参考图，保持原图动作、神态、构图与文字并替换为目标人物。
- **GIF 时序一致性与防闪烁导出**：采用首尾与中点双向关键帧调度提供跨帧参考；导出阶段构建跨帧共享主调色板与单一透明槽位，消除静态背景色彩跳动与透明残影。
- **PerMonitorV2 原生高分屏三栏界面**：内置预乘缓存双缓冲预览画布、动态 DPI 缩放、SSE 流式实时局部预览与多份生成版本对比。
- **多源素材队列与极速跳转预览**：源表情支持多选图片、直接选择或拖入整个文件夹，通过预览框下方的 `<- [ 1 ] / N ->` 切换按钮或直接输入序号回车即可瞬时跳转预览任意素材。
- **前后端彻底分离（WinUI 3 就绪）**：完整工作台状态机独立封装为 `StudioSession` 并同步产出纯 C ABI 动态库 `MemeStudioEngine.dll`（含直出 `PARGB32` 预乘帧缓冲接口），可无缝对接 WinUI 3（C# / C++/WinRT）或任意现代前端框架。
- **纯净微内核插件架构（C ABI v2）**：主程序保持单一核心职责，所有扩展工作流（局部精修、Waifu2x 超分、支持 $1\times Y$ 与 $X\times Y$ 多人物矩阵的批量队列、文生图、API 参数探测、多渠道负载均衡、本地 `sd.cpp` 推理、命名管道自动化）均作为真正独立的插件在独立仓库 **`MemeStudioPlugins`** 中维护与分发。

## 快速开始

### 1. 桌面界面 (`MemeStudio.exe`)
1. 启动 `MemeStudio.exe`，在左栏拖入或选择 **源表情素材** 与 **目标人物参考图**（可在人物预览框内左键拖拽裁剪区域）。
2. 在右栏填写兼容 OpenAI Images API 的 **接口地址 (Base URL)**、**API 密钥** 与 **模型名称**。
3. 可选添加画风参考图或点击 **预设…** 选择常用重绘指令，点击 **开始生成**。

### 2. 安装独立插件
将 `MemeStudioPlugins` 对应插件分支构建产出的 `plugins/<plugin-name>/` 文件夹放入 `MemeStudio.exe` 同级的 `plugins/` 目录，点击右上角 **插件管理…** 勾选并应用即可动态挂载扩展按钮或管线滤镜。

## 构建、测试与打包

环境要求：Windows 10 (1903+) 或 Windows 11、Visual Studio 2022+（含 C++ 桌面开发与 CMake 组件）、Node.js 18+（仅运行 E2E 测试需要）。

```powershell
# 编译 x64 Release 并运行全套单元与 CLI E2E 测试
.\build.ps1 -Architecture x64 -Configuration Release -Test

# 交叉编译 ARM64 Release
.\build.ps1 -Architecture arm64 -Configuration Release

# 生成纯净发行压缩包（输出至 out/packages/MemeStudio-Windows-x64.zip）
.\package.ps1 -Architecture x64
```

## 文档索引

- [命令行与配置文件参考 (`docs/CLI.md`)](docs/CLI.md)
- [C ABI v2 插件 SDK 开发规范 (`docs/PLUGIN_SDK.md`)](docs/PLUGIN_SDK.md)
- [前后端解耦与 WinUI 3 升级指南 (`docs/FRONTEND_ARCHITECTURE.md`)](docs/FRONTEND_ARCHITECTURE.md)
- [第三方开源声明 (`THIRD_PARTY.md`)](THIRD_PARTY.md)
