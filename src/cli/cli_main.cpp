#include "core/config_store.hpp"
#include "core/plugin_host.hpp"
#include "core/replace_engine.hpp"
#include "media/wic_codec.hpp"
#include "net/openai_client.hpp"
#include "base/app_paths.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"

#include <windows.h>
#include <objbase.h>
#include <iostream>
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
        "MemeStudio-cli 2.0.0 — 命令行表情角色替换与插件调度工具\n\n"
        "用法：\n"
        "  MemeStudio-cli run        [--config <file.json|.ini>] [--source <path>] [--character <path>]\n"
        "                            [--output <path>] [--prompt <text>] [--style <path>]\n"
        "                            [--base-url <url>] [--api-key <key>] [--model <id>]\n"
        "                            [--count <1..10>] [--stride <n>] [--max-frames <n>]\n"
        "                            [--stream] [--no-resume] [--plugin <id>] [--plugin-dir <dir>]\n"
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
            std::cout << "MemeStudio-cli 2.0.0\n";
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
            if (target.empty()) throw meme::Error("inspect 需要指定图像路径（--source <path>）");
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
                else throw meme::Error("未知的命令行选项：" + a);
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
                else throw meme::Error("未知的命令行选项：" + a);
            }
            if (pluginId.empty() || commandId.empty()) {
                throw meme::Error("plugin-cmd 必须同时指定 --plugin <id> 与 --command <cmd>");
            }

            meme::net::WinHttpPool pool;
            auto pdir = pluginDir.empty() ? meme::base::defaultPluginsDir() : pluginDir;
            auto discovered = meme::core::discoverPluginsInDirectory(pdir);
            std::vector<std::string> enabled{pluginId};
            meme::core::PluginHost host(pool, discovered, enabled);
            auto res = host.invokeCommand(pluginId, commandId, nlohmann::json::parse(argsJsonStr), nullptr, cancelToken);
            std::cout << res.dump(2) << "\n";
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
                else throw meme::Error("未知的命令行参数：" + a);
            }

            nlohmann::json doc = loadOptionalConfigFile(cfgFile);
            auto baseDir = cfgFile.empty() ? std::filesystem::path{} : cfgFile.parent_path();
            auto taskCfg = meme::core::parseReplaceTaskConfig(doc, baseDir);

            // Apply explicit CLI overrides relative to current working directory
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

            // Guard against accidentally overwriting the config file itself
            if (!cfgFile.empty() && meme::base::isSameFilePath(cfgFile, taskCfg.outputPath)) {
                throw meme::Error("输出文件路径不能覆盖任务配置文件");
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
            throw meme::Error("未知的子命令：" + subcmd + "（使用 --help 查看帮助）");
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
