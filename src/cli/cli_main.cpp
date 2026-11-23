#include "core/config_store.hpp"
#include "core/draw_engine.hpp"
#include "core/plugin_host.hpp"
#include "core/replace_engine.hpp"
#include "core/retouch_engine.hpp"
#include "media/wic_codec.hpp"
#include "net/openai_client.hpp"
#include "base/app_paths.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"

#include <windows.h>
#include <objbase.h>
#include <charconv>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

meme::net::CancelToken* g_cliCancel = nullptr;

BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        if (g_cliCancel) {
            g_cliCancel->stop();
        }
        return TRUE;
    }
    return FALSE;
}

void printHelp() {
    std::cout <<
        "MemeStudio-cli 2.1.0 — 表情角色替换、提示词生图与精修修图命令行工具\n\n"
        "用法：\n"
        "  MemeStudio-cli run        [--config <file.json|.ini>] [--source <path>] [--character <path>]\n"
        "                            [--output <path>] [--prompt <text>] [--style <path>]\n"
        "                            [--base-url <url>] [--api-key <key>] [--model <id>]\n"
        "                            [--count <1..10>] [--stride <n>] [--max-frames <n>]\n"
        "                            [--stream] [--no-resume] [--plugin <id>] [--plugin-dir <dir>]\n"
        "  MemeStudio-cli draw       --prompt <text> --output <path.png> [--style <path>] [--count <1..10>]\n"
        "                            [--quality <auto|low|medium|high>] [--size <auto|WxH>] [--stream]\n"
        "                            [--config <file>] [--base-url <url>] [--api-key <key>] [--model <id>]\n"
        "  MemeStudio-cli retouch    --source <path> --output <path> --prompt <text>\n"
        "                            [--mode <prompt|region>] [--region <x,y,w,h>] [--style <path>]\n"
        "                            [--count <1..10>] [--no-preserve-text] [--stream]\n"
        "                            [--config <file>] [--base-url <url>] [--api-key <key>] [--model <id>]\n"
        "  MemeStudio-cli inspect    --source <path> | <path>\n"
        "  MemeStudio-cli models     [--config <file>] [--base-url <url>] [--api-key <key>]\n"
        "                            [--plugin <id>] [--plugin-dir <dir>]\n"
        "  MemeStudio-cli plugin-cmd --plugin <id> --command <cmd> [--args <json>] [--plugin-dir <dir>]\n"
        "  MemeStudio-cli --help | --version\n";
}

nlohmann::json loadOptionalConfigFile(const std::filesystem::path& configPath) {
    if (configPath.empty()) return nlohmann::json::object();
    auto rawBytes = meme::base::readFileBytes(configPath, 2 * 1024 * 1024);
    std::string ext = meme::base::wideToUtf8(configPath.extension().wstring());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool isIni = (ext == ".ini");
    return meme::core::parseConfigDocument(
        std::string_view(reinterpret_cast<const char*>(rawBytes.data()), rawBytes.size()),
        isIni);
}

meme::media::NormalizedRect parseRegionCsv(std::string_view spec) {
    double vals[4]{};
    std::stringstream ss{std::string(spec)};
    std::string token;
    int idx = 0;
    while (std::getline(ss, token, ',')) {
        if (idx >= 4) throw meme::Error("--region 格式必须为 x,y,width,height（归一化范围 0..1）");
        std::string clean = meme::base::trimAscii(token);
        auto [p, ec] = std::from_chars(clean.data(), clean.data() + clean.size(), vals[idx]);
        if (ec != std::errc{} || p != clean.data() + clean.size()) {
            throw meme::Error("--region 包含无效数字：" + clean);
        }
        ++idx;
    }
    if (idx != 4) throw meme::Error("--region 格式必须包含 4 个数值：x,y,width,height");
    meme::media::NormalizedRect r{vals[0], vals[1], vals[2], vals[3]};
    r.validate("局部选区 (--region)");
    return r;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    meme::net::CancelToken cancelToken;
    g_cliCancel = &cancelToken;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    int exitCode = 0;
    try {
        meme::checkHr(comHr, "初始化命令行 COM 环境");

        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc));
        for (int i = 1; i < argc; ++i) {
            args.push_back(meme::base::wideToUtf8(argv[i]));
        }

        if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
            printHelp();
            if (SUCCEEDED(comHr)) CoUninitialize();
            return 0;
        }
        if (args[0] == "--version" || args[0] == "-v") {
            std::cout << "MemeStudio-cli 2.1.0\n";
            if (SUCCEEDED(comHr)) CoUninitialize();
            return 0;
        }

        std::string subcmd = args[0];

        if (subcmd == "inspect") {
            std::string target;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--source" && i + 1 < args.size()) {
                    target = args[++i];
                } else if (!args[i].starts_with("--")) {
                    target = args[i];
                }
            }
            if (target.empty()) throw meme::Error("inspect 请通过 --source <路径> 指定图像文件");
            auto meta = meme::media::probeMediaMetadata(meme::base::utf8ToWideStrict(target));
            nlohmann::json out{
                {"width", meta.width},
                {"height", meta.height},
                {"frames", meta.frameCount},
                {"animated", meta.animated},
                {"delays", meta.frameDelaysMs},
                {"loop", meta.loopCount.has_value() ? nlohmann::json(*meta.loopCount) : nlohmann::json(nullptr)}
            };
            std::cout << out.dump(2) << "\n";
        } else if (subcmd == "models") {
            std::filesystem::path cfgFile;
            std::filesystem::path pluginDir;
            std::string baseUrlOverride;
            std::string apiKeyOverride;
            std::vector<std::string> pluginIds;

            for (size_t i = 1; i < args.size(); ++i) {
                const auto& a = args[i];
                if (a == "--config" && i + 1 < args.size()) cfgFile = meme::base::utf8ToWideStrict(args[++i]);
                else if (a == "--base-url" && i + 1 < args.size()) baseUrlOverride = args[++i];
                else if (a == "--api-key" && i + 1 < args.size()) apiKeyOverride = args[++i];
                else if (a == "--plugin" && i + 1 < args.size()) pluginIds.push_back(args[++i]);
                else if (a == "--plugin-dir" && i + 1 < args.size()) pluginDir = meme::base::utf8ToWideStrict(args[++i]);
                else throw meme::Error("未识别的选项：" + a);
            }

            nlohmann::json doc = loadOptionalConfigFile(cfgFile);
            auto taskCfg = meme::core::parseReplaceTaskConfig(doc, cfgFile.empty() ? std::filesystem::path{} : cfgFile.parent_path());
            if (!baseUrlOverride.empty()) taskCfg.provider.baseUrl = baseUrlOverride;
            if (!apiKeyOverride.empty()) taskCfg.provider.apiKey = apiKeyOverride;
            for (const auto& pid : pluginIds) taskCfg.enabledPlugins.push_back(pid);
            if (!pluginDir.empty()) taskCfg.pluginDir = pluginDir;

            meme::net::WinHttpPool pool;
            std::unique_ptr<meme::core::PluginHost> plugins;
            if (!taskCfg.enabledPlugins.empty()) {
                auto pdir = taskCfg.pluginDir.empty() ? meme::base::defaultPluginsDir() : taskCfg.pluginDir;
                auto discovered = meme::core::discoverPluginsInDirectory(pdir);
                plugins = std::make_unique<meme::core::PluginHost>(pool, discovered, taskCfg.enabledPlugins);
            }

            meme::net::OpenAiImageClient client(pool, [&](
                std::string_view op, const meme::net::ProviderConfig& p, const nlohmann::json& m,
                std::span<const meme::net::UploadPart> u, meme::net::CancelToken& c,
                const meme::net::ImagePreviewFn& prev, meme::base::Bytes& r) {
                return plugins && plugins->tryInterceptApi(op, p, m, u, c, prev, r);
            });
            auto models = client.listModels(taskCfg.provider, cancelToken);
            std::cout << nlohmann::json{{"models", models}}.dump(2) << "\n";
        } else if (subcmd == "plugin-cmd") {
            std::string pluginId;
            std::string commandId;
            std::string argsJsonStr = "{}";
            std::filesystem::path pluginDir;

            for (size_t i = 1; i < args.size(); ++i) {
                const auto& a = args[i];
                if (a == "--plugin" && i + 1 < args.size()) pluginId = args[++i];
                else if (a == "--command" && i + 1 < args.size()) commandId = args[++i];
                else if (a == "--args" && i + 1 < args.size()) argsJsonStr = args[++i];
                else if (a == "--plugin-dir" && i + 1 < args.size()) pluginDir = meme::base::utf8ToWideStrict(args[++i]);
                else throw meme::Error("未识别的选项：" + a);
            }
            if (pluginId.empty() || commandId.empty()) {
                throw meme::Error("plugin-cmd 请同时指定 --plugin <插件ID> 与 --command <命令名>");
            }

            meme::net::WinHttpPool pool;
            auto pdir = pluginDir.empty() ? meme::base::defaultPluginsDir() : pluginDir;
            auto discovered = meme::core::discoverPluginsInDirectory(pdir);
            std::vector<std::string> enabled{pluginId};
            meme::core::PluginHost host(pool, discovered, enabled);
            auto res = host.invokeCommand(pluginId, commandId, nlohmann::json::parse(argsJsonStr), nullptr, cancelToken);
            std::cout << res.dump(2) << "\n";
        } else if (subcmd == "draw") {
            std::filesystem::path cfgFile;
            std::filesystem::path pluginDir;
            nlohmann::json over = nlohmann::json::object();
            std::string prompt;
            std::filesystem::path outPath;
            std::vector<std::filesystem::path> styles;
            std::vector<std::string> pluginIds;
            int count = 1;

            for (size_t i = 1; i < args.size(); ++i) {
                const auto& a = args[i];
                if (a == "--config" && i + 1 < args.size()) cfgFile = meme::base::utf8ToWideStrict(args[++i]);
                else if ((a == "--prompt" || a == "--instruction") && i + 1 < args.size()) prompt = args[++i];
                else if (a == "--output" && i + 1 < args.size()) outPath = meme::base::utf8ToWideStrict(args[++i]);
                else if (a == "--style" && i + 1 < args.size()) styles.push_back(meme::base::utf8ToWideStrict(args[++i]));
                else if (a == "--count" && i + 1 < args.size()) count = std::stoi(args[++i]);
                else if (a == "--base-url" && i + 1 < args.size()) over["base_url"] = args[++i];
                else if (a == "--api-key" && i + 1 < args.size()) over["api_key"] = args[++i];
                else if (a == "--model" && i + 1 < args.size()) over["model"] = args[++i];
                else if (a == "--quality" && i + 1 < args.size()) over["quality"] = args[++i];
                else if (a == "--size" && i + 1 < args.size()) over["size"] = args[++i];
                else if (a == "--stream") over["stream"] = true;
                else if (a == "--no-stream") over["stream"] = false;
                else if (a == "--plugin" && i + 1 < args.size()) pluginIds.push_back(args[++i]);
                else if (a == "--plugin-dir" && i + 1 < args.size()) pluginDir = meme::base::utf8ToWideStrict(args[++i]);
                else throw meme::Error("未识别的 draw 选项：" + a);
            }

            nlohmann::json doc = loadOptionalConfigFile(cfgFile);
            for (const auto& [k, v] : over.items()) doc[k] = v;
            auto baseCfg = meme::core::parseReplaceTaskConfig(doc, cfgFile.empty() ? std::filesystem::path{} : cfgFile.parent_path());

            meme::core::PromptDrawTaskConfig drawCfg;
            drawCfg.provider = baseCfg.provider;
            drawCfg.prompt = prompt.empty() ? baseCfg.instruction : prompt;
            drawCfg.outputPath = outPath.empty() ? baseCfg.outputPath : outPath;
            drawCfg.stylePaths = styles.empty() ? baseCfg.stylePaths : styles;
            drawCfg.copyCount = count;

            for (const auto& pid : pluginIds) baseCfg.enabledPlugins.push_back(pid);
            meme::net::WinHttpPool pool;
            std::unique_ptr<meme::core::PluginHost> plugins;
            if (!baseCfg.enabledPlugins.empty()) {
                auto pdir = pluginDir.empty() ? (baseCfg.pluginDir.empty() ? meme::base::defaultPluginsDir() : baseCfg.pluginDir) : pluginDir;
                plugins = std::make_unique<meme::core::PluginHost>(pool, meme::core::discoverPluginsInDirectory(pdir), baseCfg.enabledPlugins);
            }
            meme::net::OpenAiImageClient client(pool, [&](
                std::string_view op, const meme::net::ProviderConfig& p, const nlohmann::json& m,
                std::span<const meme::net::UploadPart> u, meme::net::CancelToken& c,
                const meme::net::ImagePreviewFn& prev, meme::base::Bytes& r) {
                return plugins && plugins->tryInterceptApi(op, p, m, u, c, prev, r);
            });

            auto outs = meme::core::runPromptDrawJob(drawCfg, client, plugins.get(), cancelToken);
            nlohmann::json outArr = nlohmann::json::array();
            for (const auto& op : outs) outArr.push_back(meme::base::wideToUtf8(op.wstring()));
            std::cout << nlohmann::json{{"ok", true}, {"outputs", outArr}}.dump(2) << "\n";
        } else if (subcmd == "retouch") {
            std::filesystem::path cfgFile;
            std::filesystem::path pluginDir;
            nlohmann::json over = nlohmann::json::object();
            std::filesystem::path srcPath, outPath;
            std::string prompt;
            std::string modeStr = "prompt";
            std::optional<meme::media::NormalizedRect> explicitRegion;
            std::vector<std::filesystem::path> styles;
            std::vector<std::string> pluginIds;
            int count = 1;
            bool preserveText = true;

            for (size_t i = 1; i < args.size(); ++i) {
                const auto& a = args[i];
                if (a == "--config" && i + 1 < args.size()) cfgFile = meme::base::utf8ToWideStrict(args[++i]);
                else if (a == "--source" && i + 1 < args.size()) srcPath = meme::base::utf8ToWideStrict(args[++i]);
                else if (a == "--output" && i + 1 < args.size()) outPath = meme::base::utf8ToWideStrict(args[++i]);
                else if ((a == "--prompt" || a == "--instruction") && i + 1 < args.size()) prompt = args[++i];
                else if (a == "--mode" && i + 1 < args.size()) modeStr = args[++i];
                else if (a == "--region" && i + 1 < args.size()) {
                    explicitRegion = parseRegionCsv(args[++i]);
                    modeStr = "region";
                }
                else if (a == "--style" && i + 1 < args.size()) styles.push_back(meme::base::utf8ToWideStrict(args[++i]));
                else if (a == "--count" && i + 1 < args.size()) count = std::stoi(args[++i]);
                else if (a == "--no-preserve-text") preserveText = false;
                else if (a == "--base-url" && i + 1 < args.size()) over["base_url"] = args[++i];
                else if (a == "--api-key" && i + 1 < args.size()) over["api_key"] = args[++i];
                else if (a == "--model" && i + 1 < args.size()) over["model"] = args[++i];
                else if (a == "--quality" && i + 1 < args.size()) over["quality"] = args[++i];
                else if (a == "--size" && i + 1 < args.size()) over["size"] = args[++i];
                else if (a == "--stream") over["stream"] = true;
                else if (a == "--no-stream") over["stream"] = false;
                else if (a == "--plugin" && i + 1 < args.size()) pluginIds.push_back(args[++i]);
                else if (a == "--plugin-dir" && i + 1 < args.size()) pluginDir = meme::base::utf8ToWideStrict(args[++i]);
                else throw meme::Error("未识别的 retouch 选项：" + a);
            }

            nlohmann::json doc = loadOptionalConfigFile(cfgFile);
            for (const auto& [k, v] : over.items()) doc[k] = v;
            auto baseCfg = meme::core::parseReplaceTaskConfig(doc, cfgFile.empty() ? std::filesystem::path{} : cfgFile.parent_path());

            meme::core::RetouchTaskConfig retCfg;
            retCfg.provider = baseCfg.provider;
            retCfg.sourcePath = srcPath.empty() ? baseCfg.sourcePath : srcPath;
            retCfg.outputPath = outPath.empty() ? baseCfg.outputPath : outPath;
            retCfg.instruction = prompt.empty() ? baseCfg.instruction : prompt;
            retCfg.mode = (modeStr == "region") ? meme::core::RetouchMode::RegionRedraw : meme::core::RetouchMode::PromptEdit;
            if (explicitRegion.has_value()) retCfg.region = *explicitRegion;
            retCfg.stylePaths = styles.empty() ? baseCfg.stylePaths : styles;
            retCfg.copyCount = count;
            retCfg.preserveText = preserveText;

            for (const auto& pid : pluginIds) baseCfg.enabledPlugins.push_back(pid);
            meme::net::WinHttpPool pool;
            std::unique_ptr<meme::core::PluginHost> plugins;
            if (!baseCfg.enabledPlugins.empty()) {
                auto pdir = pluginDir.empty() ? (baseCfg.pluginDir.empty() ? meme::base::defaultPluginsDir() : baseCfg.pluginDir) : pluginDir;
                plugins = std::make_unique<meme::core::PluginHost>(pool, meme::core::discoverPluginsInDirectory(pdir), baseCfg.enabledPlugins);
            }
            meme::net::OpenAiImageClient client(pool, [&](
                std::string_view op, const meme::net::ProviderConfig& p, const nlohmann::json& m,
                std::span<const meme::net::UploadPart> u, meme::net::CancelToken& c,
                const meme::net::ImagePreviewFn& prev, meme::base::Bytes& r) {
                return plugins && plugins->tryInterceptApi(op, p, m, u, c, prev, r);
            });

            auto outs = meme::core::runImageRetouchJob(retCfg, client, plugins.get(), cancelToken);
            nlohmann::json outArr = nlohmann::json::array();
            for (const auto& op : outs) outArr.push_back(meme::base::wideToUtf8(op.wstring()));
            std::cout << nlohmann::json{{"ok", true}, {"outputs", outArr}}.dump(2) << "\n";
        } else if (subcmd == "run") {
            std::filesystem::path cfgFile;
            nlohmann::json cliOverrides = nlohmann::json::object();
            std::vector<std::string> styleOverrides;
            std::vector<std::string> pluginOverrides;

            for (size_t i = 1; i < args.size(); ++i) {
                const auto& a = args[i];
                if (a == "--config" && i + 1 < args.size()) cfgFile = meme::base::utf8ToWideStrict(args[++i]);
                else if (a == "--source" && i + 1 < args.size()) cliOverrides["source"] = args[++i];
                else if (a == "--character" && i + 1 < args.size()) cliOverrides["character"] = args[++i];
                else if (a == "--output" && i + 1 < args.size()) cliOverrides["output"] = args[++i];
                else if ((a == "--prompt" || a == "--instruction") && i + 1 < args.size()) cliOverrides["instruction"] = args[++i];
                else if (a == "--style" && i + 1 < args.size()) styleOverrides.push_back(args[++i]);
                else if (a == "--base-url" && i + 1 < args.size()) cliOverrides["base_url"] = args[++i];
                else if (a == "--api-key" && i + 1 < args.size()) cliOverrides["api_key"] = args[++i];
                else if (a == "--model" && i + 1 < args.size()) cliOverrides["model"] = args[++i];
                else if (a == "--quality" && i + 1 < args.size()) cliOverrides["quality"] = args[++i];
                else if (a == "--size" && i + 1 < args.size()) cliOverrides["size"] = args[++i];
                else if (a == "--format" && i + 1 < args.size()) cliOverrides["output_format"] = args[++i];
                else if (a == "--compression" && i + 1 < args.size()) cliOverrides["output_compression"] = std::stoi(args[++i]);
                else if (a == "--image-field" && i + 1 < args.size()) cliOverrides["image_field"] = args[++i];
                else if (a == "--count" && i + 1 < args.size()) cliOverrides["count"] = std::stoi(args[++i]);
                else if (a == "--stride" && i + 1 < args.size()) cliOverrides["stride"] = std::stoi(args[++i]);
                else if (a == "--max-frames" && i + 1 < args.size()) cliOverrides["max_frames"] = std::stoi(args[++i]);
                else if (a == "--timeout" && i + 1 < args.size()) cliOverrides["timeout_seconds"] = std::stoi(args[++i]);
                else if (a == "--stream") cliOverrides["stream"] = true;
                else if (a == "--no-stream") cliOverrides["stream"] = false;
                else if (a == "--no-resume") cliOverrides["resume"] = false;
                else if (a == "--no-consistency") cliOverrides["consistency"] = false;
                else if (a == "--no-preserve-text") cliOverrides["preserve_text"] = false;
                else if ((a == "--plugin" || a == "--load") && i + 1 < args.size()) pluginOverrides.push_back(args[++i]);
                else if (a == "--plugin-dir" && i + 1 < args.size()) cliOverrides["plugin_directory"] = args[++i];
                else throw meme::Error("未识别的选项：" + a);
            }

            nlohmann::json doc = loadOptionalConfigFile(cfgFile);
            auto baseDir = cfgFile.empty() ? std::filesystem::path{} : cfgFile.parent_path();
            auto taskCfg = meme::core::parseReplaceTaskConfig(doc, baseDir);

            if (!cliOverrides.empty()) {
                nlohmann::json merged = doc;
                for (const auto& [k, v] : cliOverrides.items()) merged[k] = v;
                auto overCfg = meme::core::parseReplaceTaskConfig(merged);
                if (cliOverrides.contains("source")) taskCfg.sourcePath = overCfg.sourcePath;
                if (cliOverrides.contains("character")) taskCfg.characterPath = overCfg.characterPath;
                if (cliOverrides.contains("output")) taskCfg.outputPath = overCfg.outputPath;
                if (cliOverrides.contains("plugin_directory")) taskCfg.pluginDir = overCfg.pluginDir;
                if (cliOverrides.contains("instruction")) taskCfg.instruction = overCfg.instruction;
                if (cliOverrides.contains("base_url")) taskCfg.provider.baseUrl = overCfg.provider.baseUrl;
                if (cliOverrides.contains("api_key")) taskCfg.provider.apiKey = overCfg.provider.apiKey;
                if (cliOverrides.contains("model")) taskCfg.provider.model = overCfg.provider.model;
                if (cliOverrides.contains("quality")) taskCfg.provider.quality = overCfg.provider.quality;
                if (cliOverrides.contains("size")) taskCfg.provider.size = overCfg.provider.size;
                if (cliOverrides.contains("output_format")) taskCfg.provider.outputFormat = overCfg.provider.outputFormat;
                if (cliOverrides.contains("output_compression")) taskCfg.provider.compression = overCfg.provider.compression;
                if (cliOverrides.contains("image_field")) taskCfg.provider.imageField = overCfg.provider.imageField;
                if (cliOverrides.contains("count")) taskCfg.copyCount = overCfg.copyCount;
                if (cliOverrides.contains("stride")) taskCfg.frameStride = overCfg.frameStride;
                if (cliOverrides.contains("max_frames")) taskCfg.maxFrames = overCfg.maxFrames;
                if (cliOverrides.contains("timeout_seconds")) taskCfg.provider.timeoutSeconds = overCfg.provider.timeoutSeconds;
                if (cliOverrides.contains("stream")) taskCfg.provider.stream = overCfg.provider.stream;
                if (cliOverrides.contains("resume")) taskCfg.allowResume = overCfg.allowResume;
                if (cliOverrides.contains("consistency")) taskCfg.temporalConsistency = overCfg.temporalConsistency;
                if (cliOverrides.contains("preserve_text")) taskCfg.preserveText = overCfg.preserveText;
            }
            if (!styleOverrides.empty()) {
                taskCfg.stylePaths.clear();
                for (const auto& s : styleOverrides) taskCfg.stylePaths.push_back(meme::base::utf8ToWideStrict(s));
            }
            for (const auto& pid : pluginOverrides) {
                if (std::find(taskCfg.enabledPlugins.begin(), taskCfg.enabledPlugins.end(), pid) == taskCfg.enabledPlugins.end()) {
                    taskCfg.enabledPlugins.push_back(pid);
                }
            }

            if (!cfgFile.empty() && meme::base::isSameFilePath(cfgFile, taskCfg.outputPath)) {
                throw meme::Error("输出保存路径不能覆盖配置文件本身");
            }

            meme::net::WinHttpPool pool;
            std::unique_ptr<meme::core::PluginHost> plugins;
            if (!taskCfg.enabledPlugins.empty()) {
                auto pdir = taskCfg.pluginDir.empty() ? meme::base::defaultPluginsDir() : taskCfg.pluginDir;
                auto discovered = meme::core::discoverPluginsInDirectory(pdir);
                plugins = std::make_unique<meme::core::PluginHost>(pool, discovered, taskCfg.enabledPlugins);
                if (auto ov = plugins->sessionEndpointOverride()) {
                    taskCfg.provider.baseUrl = ov->baseUrl;
                    if (!ov->apiKey.empty()) taskCfg.provider.apiKey = ov->apiKey;
                }
            }

            meme::net::OpenAiImageClient client(pool, [&](
                std::string_view op, const meme::net::ProviderConfig& p, const nlohmann::json& m,
                std::span<const meme::net::UploadPart> u, meme::net::CancelToken& c,
                const meme::net::ImagePreviewFn& prev, meme::base::Bytes& r) {
                return plugins && plugins->tryInterceptApi(op, p, m, u, c, prev, r);
            });

            auto outputs = meme::core::runCharacterReplaceJob(taskCfg, client, plugins.get(), cancelToken);
            nlohmann::json outArr = nlohmann::json::array();
            for (const auto& op : outputs) outArr.push_back(meme::base::wideToUtf8(op.wstring()));
            std::cout << nlohmann::json{{"ok", true}, {"outputs", outArr}}.dump(2) << "\n";
        } else {
            throw meme::Error("未识别的子命令：" + subcmd + "（使用 --help 查看完整用法）");
        }
    } catch (const std::exception& e) {
        std::cerr << "[MemeStudio-cli 错误] " << e.what() << "\n";
        exitCode = 1;
    }

    g_cliCancel = nullptr;
    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }
    return exitCode;
}
