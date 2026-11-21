# 第三方开源声明 (Third-Party Notices)

`MemeStudio` 主程序在编译期包含以下第三方开源组件：

- **nlohmann/json** (`vendor/nlohmann/json.hpp`)  
  上游项目：<https://github.com/nlohmann/json>  
  许可证：MIT License（完整版权声明与许可证文本见 `vendor/nlohmann/LICENSE.MIT`）。

---

主程序通过系统原生组件（Win32、Windows Imaging Component、WinHTTP、CNG / DPAPI、WinTrust）运行，不内置或分发任何第三方模型权重或外部推理二进制文件。独立扩展插件所涉及的外部开源项目声明详见独立插件仓库 `MemeConvertStudio/THIRD_PARTY.md`。
