# MemeStudio-cli 命令行与配置文件指南

`MemeStudio-cli.exe` 是无图形界面依赖的原生命令行工具，完整内置了**角色替换 (`run`)**、**提示词生图 (`draw`)**、**提示词修图与局部精修 (`retouch`)**、**媒体检查 (`inspect`)**、**模型查询 (`models`)** 与 **独立插件调用 (`plugin-cmd`)** 六大子命令。

## 子命令详解

### 1. 表情与动图角色替换 (`run`)
```powershell
MemeStudio-cli run --config job.json
MemeStudio-cli run --source in.gif --character ref.png --output out.gif --prompt "保留原文字" --stream
```

### 2. 提示词生图 (`draw`)
无参考图时自动走 `/v1/images/generations` JSON 接口；附带 `--style` 画风参考图时自动走 `/v1/images/edits` 多图参考接口：
```powershell
MemeStudio-cli draw --prompt "Q版白底开心挥手贴纸" --output sticker.png --size 1024x1024
MemeStudio-cli draw --prompt "拿着咖啡杯叹气" --style style1.png --count 2 --output coffee.png
```

### 3. 提示词整图修图与局部框选精修 (`retouch`)
支持静态图与 GIF 动图修图，自动拒绝覆盖原图：
```powershell
# 模式一：提示词整图修图（--mode prompt，默认）
MemeStudio-cli retouch --source meme.png --output meme_glasses.png --prompt "给角色戴上一副黑色墨镜"

# 模式二：局部框选精修（--mode region，严格锁死 x,y,w,h 归一化矩形以外的所有原始像素）
MemeStudio-cli retouch --source meme.png --output meme_hand_fixed.png --region 0.55,0.40,0.35,0.45 --prompt "修复手部手指结构"
```

### 4. 检查媒体元数据 (`inspect`)
仅读取文件头宽高、帧数、各帧延迟与循环次数（不解码像素缓冲区）：
```powershell
MemeStudio-cli inspect --source .\anim.gif
```

### 5. 查询可用模型 (`models`)
```powershell
MemeStudio-cli models --base-url https://api.openai.com/v1
```

### 6. 调用独立扩展插件命令 (`plugin-cmd`)
通过无头方式直接驱动 `MemeStudioPlugins` 中的任意扩展插件（如 $X\times Y$ 批量矩阵、参数兼容检测、多渠道对比等）：
```powershell
MemeStudio-cli plugin-cmd --plugin memestudio.batch-convert --command run-batch --args "{\"sources\":[\"./src_dir\"],\"characters\":[\"./char_dir\"],\"output_directory\":\"./out_dir\"}"
```

## 通用命令行选项

- `--config <path>`：加载 `.json`（支持 `//` 注释）或 `.ini` 配置文件。
- `--base-url <url>` / `--api-key <key>` / `--model <id>`：覆盖接口参数（未传 `--api-key` 时默认读取环境变量 `OPENAI_API_KEY`）。
- `--style <path>`：附加画风参考图（可重复传入，最多 8 张）。
- `--quality <auto|low|medium|high>` / `--size <auto|WxH>` / `--count <1..10>`。
- `--stream` / `--no-stream`：开启或关闭流式草图传输。
- `--no-preserve-text`：不要求保留原图已有文字。
- `--plugin <id>` / `--plugin-dir <dir>`：加载指定扩展插件（如 `memestudio.waifu2x` 或 `memestudio.api-load-balancing`）参与本次执行。

## 配置文件字段参考（JSON / INI 通用）

| 字段名 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `base_url` | string | `"https://api.openai.com/v1"` | 接口基础地址（非本机回环地址强制要求 HTTPS） |
| `api_key` / `api_key_env` | string | `""` / `"OPENAI_API_KEY"` | 明文密钥或读取密钥的环境变量名 |
| `model` | string | `"gpt-image-1"` | 模型名称 |
| `source` / `character` / `output` | string | — | 源表情、人物参考与输出路径 |
| `crop` | object | `{x:0, y:0, width:1, height:1}` | 人物参考归一化裁剪框（INI 中为 `[crop]` 节） |
| `styles` | string[] | `[]` | 最多 8 张画风参考图路径 |
| `instruction` | string | `""` | 创作或修图提示词 |
| `stride` / `max_frames` | int | `1` / `0` | GIF 抽帧步长 / 最大采样帧数（`0` 为不限） |
| `count` | int | `1` | 生成份数（`1..10`） |
| `consistency` / `preserve_text` / `resume` | bool | `true` | 动图跨帧一致 / 保留原图文字 / 动图中断续传 |
| `plugins` | string[] | `[]` | 启用参与管线的插件 ID 列表 |
