# MemeStudio

面向 Windows 10 / 11 的轻量原生 AI 表情包创作与修图工作台（C++20 / PerMonitorV2 高分屏 / 前后端彻底分离架构）。

## 三大内置核心创作模式

1. **表情与动图角色替换（主工作台）**  
   导入单张或多张源表情图片 / 整个文件夹（支持 PNG、JPEG、WebP、BMP、GIF 动图），配合目标人物参考图（支持框选裁剪）与最多 8 张画风参考图，完整保留原表情动作、神态、构图与台词并替换为新角色。左栏预览框下方提供 `<- [ 1 ] / N ->` 翻页与输入序号回车瞬时跳转预览。
2. **提示词生图（顶部「提示词生图…」）**  
   输入自然语言描述（可搭配内置灵感预设与最多 8 张画风参考图），直接从零绘制全新表情贴纸或角色插画，支持生成后一键送入修图工作台继续微调。
3. **局部精修与提示词修图（顶部「精修 / 修图…」）**  
   支持对成品或任意本地静态图 / GIF 动图进行多轮连续迭代精修，内置两种模式：
   - **提示词整图修图**：无需画框，直接用中文自然语言增删配饰（如戴墨镜）、调整表情张力、替换纯白贴纸背景或清理画面文字；
   - **局部框选精修**：在画布上左键拖拽框选需要重绘的局部矩形（如修手部手指、去局部杂线），框外原始像素 **100% 无损保留**。

## 技术与体验亮点

- **GIF 动图跨帧连贯与防闪烁编码**：采用首尾与中点双向关键帧调度稳定跨帧角色特征；导出阶段构建跨帧共享主调色板与单一透明色槽，消除静态背景色彩跳动与透明残影。
- **前后端彻底解耦（WinUI 3 就绪）**：完整业务状态机独立封装于 `StudioSession`，并导出跨语言纯 C ABI 动态库 `MemeStudioEngine.dll`（支持直出 GPU 预乘 `PARGB32` 像素缓冲），可随时无损接入 WinUI 3、Qt 或 WebView2 前端。
- **按需扩展插件生态（`MemeStudioPlugins`）**：多源 × 多角色矩阵批处理、Waifu2x 超分降噪、接口参数兼容检测、多渠道负载均衡、本地 `sd.cpp` GGUF 推理与 DeepSeek Harness 命名管道桥接均以独立插件按需挂载。

## 快速上手

1. 启动 `MemeStudio.exe`，在右栏填写兼容 OpenAI Images API 的 **接口地址 (Base URL)**、**API 密钥** 与 **模型名称**。
2. **角色替换**：在左栏拖入源表情（或整个文件夹）与目标人物参考图，点击 **开始生成**。
3. **直接画图 / 后期修图**：点击顶部 **提示词生图…** 或 **精修 / 修图…** 打开对应创作窗口即可使用。
4. **选装扩展插件**：将 `MemeStudioPlugins` 编译产出的插件目录放入同级 `plugins/` 文件夹，点击右上角 **插件管理…** 勾选启用。

## 编译、测试与打包

环境要求：Windows 10 (1903+) 或 Windows 11、Visual Studio 2022+（含 C++ 桌面开发与 CMake 组件）、Node.js 18+（仅运行命令行 E2E 测试需要）。

```powershell
# 编译 x64 Release 并运行全套单元与 E2E 测试
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Architecture x64 -Configuration Release -Test

# 交叉编译 ARM64 Release
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Architecture arm64 -Configuration Release

# 生成纯净发行压缩包（输出至 out/packages/MemeStudio-Windows-x64.zip）
powershell -ExecutionPolicy Bypass -File .\package.ps1 -Architecture x64
```

## 文档导航

- [命令行工具与配置文件指南 (`docs/CLI.md`)](docs/CLI.md)
- [前后端解耦与 WinUI 3 升级指南 (`docs/FRONTEND_ARCHITECTURE.md`)](docs/FRONTEND_ARCHITECTURE.md)
- [C ABI v2 插件 SDK 开发规范 (`docs/PLUGIN_SDK.md`)](docs/PLUGIN_SDK.md)
- [第三方开源声明 (`THIRD_PARTY.md`)](THIRD_PARTY.md)
