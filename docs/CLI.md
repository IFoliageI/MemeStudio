# MemeStudio-cli 命令行与配置文件参考

`MemeStudio-cli.exe` 是无图形界面依赖的原生命令行程序，共享与桌面端完全一致的核心替换引擎、WIC 编解码器与 C ABI v2 插件宿主。

## 子命令一览

### 1. 执行角色替换任务 (`run`)
```powershell
MemeStudio-cli run --config job.json
MemeStudio-cli run --source in.gif --character ref.png --output out.gif --prompt "保留原文字" --stream
```

常用命令行覆盖参数（优先级高于 `--config` 文件）：
- `--config <path>`：加载 `.json`（支持 `//` 注释）或 `.ini` 配置文件（文件内相对路径相对于配置文件所在目录解析）。
- `--source <path>` / `--character <path>` / `--output <path>`：源表情、人物参考图与输出路径（静态图输出 `.png`，动图输出 `.gif`）。
- `--style <path>`：附加画风参考图（可重复传入，最多 8 张）。
- `--prompt <text>`：附加重绘指令（UTF-8）。
- `--base-url <url>` / `--api-key <key>` / `--model <id>`：接口连接参数（未指定 `--api-key` 时默认读取环境变量 `OPENAI_API_KEY`）。
- `--quality <auto|low|medium|high>` / `--size <auto|WxH>` / `--format <png|jpeg|webp>` / `--compression <0..100>`。
- `--count <1..10>`：生成份数（大于 1 时自动输出 `stem_001.ext` .. `stem_NNN.ext`）。
- `--stride <1..600>` / `--max-frames <0..600>`：GIF 抽帧步长与最大帧数（`0` 表示不限制帧数）。
- `--stream` / `--no-stream`：开启或关闭 SSE 流式传输。
- `--no-resume`：禁用断点续传缓存（不写入任何中间帧缓存）。
- `--plugin <id>` / `--plugin-dir <dir>`：加载指定插件参与本次管线执行。

### 2. 检查媒体元数据 (`inspect`)
仅读取图像/GIF 文件头元数据（不展开像素帧）：
```powershell
MemeStudio-cli inspect --source .\anim.gif
```
输出标准 JSON：`{ "width", "height", "frames", "animated", "delays", "loop" }`。

### 3. 查询可用模型 (`models`)
```powershell
MemeStudio-cli models --base-url https://api.openai.com/v1
```

### 4. 调用独立插件命令 (`plugin-cmd`)
通过无头方式直接驱动 `MemeStudioPlugins` 中的任意独立插件命令（如批量队列、参数探测、渠道对比等）：
```powershell
MemeStudio-cli plugin-cmd --plugin memestudio.api-parameter-test --command plan --args "{}"
```

## 配置文件字段规范（JSON / INI 通用）

| 字段名 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `base_url` | string | `"https://api.openai.com/v1"` | 接口基础地址（非本机回环地址强制要求 HTTPS） |
| `api_key` | string | `""` | 明文密钥（建议改用 `api_key_env` 从环境变量读取） |
| `api_key_env` | string | `"OPENAI_API_KEY"` | 存放 API 密钥的环境变量名 |
| `model` | string | `"gpt-image-1"` | 模型标识符 |
| `source` / `character` / `output` | string | — | 输入与输出路径（严禁输出路径与任何输入或配置文件同名） |
| `crop` | object | `{x:0, y:0, width:1, height:1}` | 人物参考图归一化裁剪矩形（INI 中对应 `[crop]` 节） |
| `styles` | string[] | `[]` | 最多 8 张附加画风参考图路径 |
| `instruction` | string | `""` | 附加用户重绘指令 |
| `stride` / `max_frames` | int | `1` / `0` | GIF 抽帧步长 / 最大采样帧数（`0` 为不限） |
| `count` | int | `1` | 生成份数（`1..10`） |
| `consistency` / `preserve_text` / `resume` | bool | `true` | 跨帧角色一致 / 保留原图文字 / 异常断点续传 |
| `plugins` | string[] | `[]` | 需启用的插件 ID 列表 |
