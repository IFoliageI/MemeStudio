#include "core/config_store.hpp"
#include "base/app_paths.hpp"
#include "base/crypto.hpp"
#include "base/error.hpp"
#include "base/json_io.hpp"
#include "base/utf8.hpp"
#include <algorithm>
#include <charconv>
#include <sstream>

namespace meme::core {

namespace {

int parseBoundedInt(const nlohmann::json& j, std::string_view key, int defaultVal, int minVal, int maxVal) {
    if (!j.contains(key) || j[key].is_null()) return defaultVal;
    const auto& v = j[key];
    int parsed = defaultVal;
    if (v.is_number_integer()) {
        parsed = v.get<int>();
    } else if (v.is_string()) {
        std::string s = base::trimAscii(v.get<std::string>());
        if (s.empty() || s == "不发送" || s == "默认 (不传参)") return defaultVal;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), parsed);
        if (ec != std::errc{} || ptr != s.data() + s.size()) {
            throw Error("配置项 " + std::string(key) + " 必须是整数");
        }
    } else {
        throw Error("配置项 " + std::string(key) + " 类型错误（应为整数）");
    }
    if (parsed < minVal || parsed > maxVal) {
        throw Error("配置项 " + std::string(key) + " 超出有效范围 [" +
                    std::to_string(minVal) + ", " + std::to_string(maxVal) + "]");
    }
    return parsed;
}

double parseFiniteDouble(const nlohmann::json& j, std::string_view key, double defaultVal) {
    if (!j.contains(key) || j[key].is_null()) return defaultVal;
    const auto& v = j[key];
    if (v.is_number()) {
        double d = v.get<double>();
        if (!std::isfinite(d)) throw Error("配置项 " + std::string(key) + " 不能为无穷大或 NaN");
        return d;
    }
    throw Error("配置项 " + std::string(key) + " 类型错误（应为数值）");
}

bool parseStrictBool(const nlohmann::json& j, std::string_view key, bool defaultVal) {
    if (!j.contains(key) || j[key].is_null()) return defaultVal;
    const auto& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    throw Error("配置项 " + std::string(key) + " 类型错误（应为布尔值 true/false）");
}

std::string cleanOptionalChoice(std::string s) {
    s = base::trimAscii(s);
    if (s == "不发送" || s == "默认 (不传参)" || s == "默认") return {};
    return s;
}

SignaturePolicy parseSigPolicy(const nlohmann::json& j) {
    if (!j.contains("plugin_signature_policy")) return SignaturePolicy::Disabled;
    const auto& v = j["plugin_signature_policy"];
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s == "trusted") return SignaturePolicy::Trusted;
        if (s == "same_signer") return SignaturePolicy::SameSigner;
        return SignaturePolicy::Disabled;
    }
    return SignaturePolicy::Disabled;
}

const char* sigPolicyString(SignaturePolicy p) {
    switch (p) {
        case SignaturePolicy::Trusted:    return "trusted";
        case SignaturePolicy::SameSigner: return "same_signer";
        default:                          return "disabled";
    }
}

// Transparently upgrades legacy numeric enum keys ("106".."137") from old settings.json!
nlohmann::json normalizeLegacySettings(const nlohmann::json& raw) {
    if (!raw.is_object()) return nlohmann::json::object();
    nlohmann::json out = raw;
    auto mapLegacyStr = [&](const char* oldEnumId, const char* newKey) {
        if (!out.contains(newKey) && raw.contains(oldEnumId) && raw[oldEnumId].is_string()) {
            out[newKey] = raw[oldEnumId];
        }
    };
    auto mapLegacyBool = [&](const char* oldEnumId, const char* newKey) {
        if (!out.contains(newKey) && raw.contains(oldEnumId) && raw[oldEnumId].is_boolean()) {
            out[newKey] = raw[oldEnumId];
        }
    };

    mapLegacyStr("106", "base_url");
    mapLegacyStr("108", "model");
    mapLegacyStr("109", "quality");
    mapLegacyStr("110", "size");
    mapLegacyStr("111", "image_field");
    mapLegacyStr("132", "complexity");
    mapLegacyStr("133", "output_format");
    mapLegacyStr("134", "output_compression");
    mapLegacyBool("135", "stream");
    mapLegacyStr("136", "partial_images");
    mapLegacyStr("113", "timeout_seconds");
    mapLegacyStr("114", "frame_stride");
    mapLegacyStr("115", "max_frames");
    mapLegacyBool("116", "temporal_consistency");
    mapLegacyBool("117", "preserve_text");
    mapLegacyBool("118", "allow_resume");
    mapLegacyBool("119", "remember_key");
    mapLegacyStr("137", "copy_count");
    if (!out.contains("encrypted_api_key") && raw.contains("encrypted_key") && raw["encrypted_key"].is_string()) {
        out["encrypted_api_key"] = raw["encrypted_key"];
    }
    return out;
}

} // namespace

StudioSettings parseStudioSettingsJson(const nlohmann::json& rawData) {
    nlohmann::json j = normalizeLegacySettings(rawData);
    StudioSettings s;
    s.provider.baseUrl = j.value("base_url", s.provider.baseUrl);
    s.rememberKey = j.value("remember_key", false);
    if (s.rememberKey && j.contains("encrypted_api_key") && j["encrypted_api_key"].is_string()) {
        try {
            auto unlocked = base::dpapiUnprotect(j["encrypted_api_key"].get<std::string>());
            s.provider.apiKey.assign(unlocked.view());
        } catch (...) {
            s.provider.apiKey.clear();
        }
    }
    s.provider.model = j.value("model", s.provider.model);
    s.provider.quality = cleanOptionalChoice(j.value("quality", s.provider.quality));
    s.provider.size = cleanOptionalChoice(j.value("size", s.provider.size));
    s.provider.imageField = j.value("image_field", s.provider.imageField);
    s.provider.complexity = cleanOptionalChoice(j.value("complexity", std::string()));
    s.provider.outputFormat = cleanOptionalChoice(j.value("output_format", std::string()));
    try { s.provider.compression = parseBoundedInt(j, "output_compression", -1, -1, 100); } catch (...) {}
    if (j.contains("stream") && j["stream"].is_boolean()) s.provider.stream = j["stream"].get<bool>();
    try { s.provider.partialImages = parseBoundedInt(j, "partial_images", 2, 1, 3); } catch (...) {}
    try { s.provider.timeoutSeconds = parseBoundedInt(j, "timeout_seconds", 600, 5, 3600); } catch (...) {}
    if (j.contains("extra") && j["extra"].is_object()) {
        s.provider.extra = j["extra"];
    } else if (rawData.contains("112") && rawData["112"].is_string()) {
        auto parsedExtra = nlohmann::json::parse(rawData["112"].get<std::string>(), nullptr, false);
        if (parsedExtra.is_object()) s.provider.extra = std::move(parsedExtra);
    }

    try { s.frameStride = parseBoundedInt(j, "frame_stride", 1, 1, 600); } catch (...) {}
    try { s.maxFrames = parseBoundedInt(j, "max_frames", 0, 0, 600); } catch (...) {}
    if (j.contains("temporal_consistency") && j["temporal_consistency"].is_boolean()) {
        s.temporalConsistency = j["temporal_consistency"].get<bool>();
    }
    if (j.contains("preserve_text") && j["preserve_text"].is_boolean()) {
        s.preserveText = j["preserve_text"].get<bool>();
    }
    if (j.contains("allow_resume") && j["allow_resume"].is_boolean()) {
        s.allowResume = j["allow_resume"].get<bool>();
    }
    try { s.copyCount = parseBoundedInt(j, "copy_count", 1, 1, 10); } catch (...) {}

    if (j.contains("enabled_plugins") && j["enabled_plugins"].is_array()) {
        for (const auto& item : j["enabled_plugins"]) {
            if (item.is_string() && base::isValidIdentifier(item.get<std::string>())) {
                s.enabledPlugins.push_back(item.get<std::string>());
            }
        }
    }
    s.signaturePolicy = parseSigPolicy(j);
    try { s.leftPaneDip = parseBoundedInt(j, "left_pane_dip", 250, 220, 640); } catch (...) {}
    try { s.rightPaneDip = parseBoundedInt(j, "right_pane_dip", 336, 300, 680); } catch (...) {}
    return s;
}

nlohmann::json serializeStudioSettingsJson(const StudioSettings& s) {
    nlohmann::json out{
        {"schema_version", 2},
        {"base_url", s.provider.baseUrl},
        {"remember_key", s.rememberKey},
        {"encrypted_api_key", s.rememberKey && !s.provider.apiKey.empty() ? base::dpapiProtect(s.provider.apiKey) : ""},
        {"model", s.provider.model},
        {"quality", s.provider.quality},
        {"size", s.provider.size},
        {"image_field", s.provider.imageField},
        {"complexity", s.provider.complexity},
        {"output_format", s.provider.outputFormat},
        {"output_compression", s.provider.compression},
        {"stream", s.provider.stream},
        {"partial_images", s.provider.partialImages},
        {"timeout_seconds", s.provider.timeoutSeconds},
        {"extra", s.provider.extra},
        {"frame_stride", s.frameStride},
        {"max_frames", s.maxFrames},
        {"temporal_consistency", s.temporalConsistency},
        {"preserve_text", s.preserveText},
        {"allow_resume", s.allowResume},
        {"copy_count", s.copyCount},
        {"enabled_plugins", s.enabledPlugins},
        {"plugin_signature_policy", sigPolicyString(s.signaturePolicy)},
        {"left_pane_dip", s.leftPaneDip},
        {"right_pane_dip", s.rightPaneDip}
    };
    return out;
}

StudioSettings loadStudioSettings() {
    fs::path p = base::settingsFilePath();
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        // Also check legacy flat %LOCALAPPDATA%\MemeStudio\settings.json
        fs::path legacy = base::appDataRoot() / L"settings.json";
        if (!fs::exists(legacy, ec)) return {};
        p = legacy;
    }
    return parseStudioSettingsJson(base::readJsonFile(p));
}

void saveStudioSettings(const StudioSettings& settings) {
    base::writeJsonFile(base::settingsFilePath(), serializeStudioSettingsJson(settings));
}

nlohmann::json parseConfigDocument(std::string_view contentUtf8, bool isIniFormat) {
    if (contentUtf8.starts_with("\xEF\xBB\xBF")) {
        contentUtf8.remove_prefix(3);
    }
    if (!isIniFormat) {
        try {
            auto j = nlohmann::json::parse(contentUtf8.begin(), contentUtf8.end(), nullptr, true, true);
            if (!j.is_object()) throw Error("配置文件根节点必须是 JSON 对象");
            return j;
        } catch (const Error&) {
            throw;
        } catch (const std::exception& e) {
            throw Error(std::string("解析 JSON 配置失败：") + e.what());
        }
    }

    // Strict INI parser that treats quoted Windows paths literally (never eating \t or \n in D:\temp\new.png!)
    nlohmann::json root = nlohmann::json::object();
    std::string currentSection;
    std::istringstream stream{std::string(contentUtf8)};
    std::string rawLine;
    int lineNum = 0;

    auto unquoteLiteral = [](std::string val) -> std::pair<std::string, bool> {
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
            return {val.substr(1, val.size() - 2), true};
        }
        return {val, false};
    };

    while (std::getline(stream, rawLine)) {
        ++lineNum;
        std::string line = base::trimAscii(rawLine);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;

        if (line.front() == '[') {
            if (line.back() != ']') {
                throw Error("INI 第 " + std::to_string(lineNum) + " 行节标题缺少右括号 ']'");
            }
            currentSection = base::trimAscii(std::string_view(line).substr(1, line.size() - 2));
            if (currentSection.empty()) {
                throw Error("INI 第 " + std::to_string(lineNum) + " 行节名称不能为空");
            }
            if (!root.contains(currentSection)) {
                root[currentSection] = nlohmann::json::object();
            } else if (!root[currentSection].is_object()) {
                throw Error("INI 节名与同名配置键冲突：" + currentSection);
            }
            continue;
        }

        auto eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            throw Error("INI 第 " + std::to_string(lineNum) + " 行缺少 '=' 号");
        }
        std::string key = base::trimAscii(std::string_view(line).substr(0, eqPos));
        std::string rawVal = base::trimAscii(std::string_view(line).substr(eqPos + 1));
        if (key.empty()) {
            throw Error("INI 第 " + std::to_string(lineNum) + " 行键名不能为空");
        }

        auto [unquoted, wasQuoted] = unquoteLiteral(rawVal);
        nlohmann::json typedVal;
        if (wasQuoted) {
            typedVal = unquoted;
        } else if (rawVal == "true") {
            typedVal = true;
        } else if (rawVal == "false") {
            typedVal = false;
        } else if (!rawVal.empty() && (rawVal.front() == '[' || rawVal.front() == '{')) {
            try {
                typedVal = nlohmann::json::parse(rawVal);
            } catch (const std::exception& e) {
                throw Error("INI 第 " + std::to_string(lineNum) + " 行内联 JSON 无效：" + e.what());
            }
        } else {
            int64_t intVal = 0;
            auto [iPtr, iEc] = std::from_chars(rawVal.data(), rawVal.data() + rawVal.size(), intVal);
            if (iEc == std::errc{} && iPtr == rawVal.data() + rawVal.size()) {
                typedVal = intVal;
            } else {
                double dblVal = 0.0;
                auto [dPtr, dEc] = std::from_chars(rawVal.data(), rawVal.data() + rawVal.size(), dblVal);
                if (dEc == std::errc{} && dPtr == rawVal.data() + rawVal.size() && std::isfinite(dblVal)) {
                    typedVal = dblVal;
                } else {
                    typedVal = rawVal;
                }
            }
        }

        if (currentSection.empty()) {
            root[key] = std::move(typedVal);
        } else {
            root[currentSection][key] = std::move(typedVal);
        }
    }
    return root;
}

ReplaceTaskConfig parseReplaceTaskConfig(const nlohmann::json& doc, const fs::path& baseDir) {
    if (!doc.is_object()) {
        throw Error("任务配置必须是对象");
    }

    auto resolvePathField = [&](std::string_view key) -> fs::path {
        if (!doc.contains(key) || doc[key].is_null()) return {};
        if (!doc[key].is_string()) {
            throw Error("路径字段 " + std::string(key) + " 必须是字符串");
        }
        std::string s = base::trimAscii(doc[key].get<std::string>());
        if (s.empty()) return {};
        fs::path p = base::utf8ToWideStrict(s);
        if (!baseDir.empty() && p.is_relative()) {
            p = baseDir / p;
        }
        return p;
    };

    ReplaceTaskConfig cfg;
    cfg.provider.baseUrl = doc.value("base_url", cfg.provider.baseUrl);
    if (doc.contains("api_key") && doc["api_key"].is_string() && !doc["api_key"].get<std::string>().empty()) {
        cfg.provider.apiKey = doc["api_key"].get<std::string>();
    } else {
        std::string envName = doc.value("api_key_env", std::string("OPENAI_API_KEY"));
        auto secret = base::readEnvSecret(envName);
        cfg.provider.apiKey.assign(secret.view());
    }
    cfg.provider.model = doc.value("model", cfg.provider.model);
    cfg.provider.quality = cleanOptionalChoice(doc.value("quality", cfg.provider.quality));
    cfg.provider.size = cleanOptionalChoice(doc.value("size", cfg.provider.size));
    cfg.provider.imageField = doc.value("image_field", cfg.provider.imageField);
    cfg.provider.complexity = cleanOptionalChoice(doc.value("complexity", std::string()));
    cfg.provider.outputFormat = cleanOptionalChoice(doc.value("output_format", std::string()));
    cfg.provider.compression = parseBoundedInt(doc, "output_compression", -1, -1, 100);
    cfg.provider.stream = parseStrictBool(doc, "stream", false);
    cfg.provider.partialImages = parseBoundedInt(doc, "partial_images", 2, 1, 3);
    cfg.provider.timeoutSeconds = parseBoundedInt(doc, "timeout_seconds", 600, 5, 3600);
    if (doc.contains("extra")) {
        if (!doc["extra"].is_object()) throw Error("配置项 extra 必须是对象");
        cfg.provider.extra = doc["extra"];
    }

    cfg.sourcePath = resolvePathField("source");
    cfg.characterPath = resolvePathField("character");
    cfg.outputPath = resolvePathField("output");
    cfg.pluginDir = resolvePathField("plugin_directory");

    if (doc.contains("crop")) {
        const auto& c = doc["crop"];
        if (!c.is_object()) throw Error("配置项 crop 必须是对象");
        cfg.characterCrop.x = parseFiniteDouble(c, "x", 0.0);
        cfg.characterCrop.y = parseFiniteDouble(c, "y", 0.0);
        cfg.characterCrop.width = parseFiniteDouble(c, "width", 1.0);
        cfg.characterCrop.height = parseFiniteDouble(c, "height", 1.0);
        cfg.characterCrop.validate("人物裁剪框 crop");
    }

    if (doc.contains("styles")) {
        if (!doc["styles"].is_array()) throw Error("配置项 styles 必须是路径数组");
        for (const auto& item : doc["styles"]) {
            if (!item.is_string()) throw Error("styles 数组元素必须是字符串路径");
            fs::path sp = base::utf8ToWideStrict(base::trimAscii(item.get<std::string>()));
            if (!baseDir.empty() && sp.is_relative()) sp = baseDir / sp;
            cfg.stylePaths.push_back(std::move(sp));
        }
    }

    if (doc.contains("instruction") && !doc["instruction"].is_null()) {
        if (!doc["instruction"].is_string()) throw Error("instruction 必须是字符串");
        cfg.instruction = doc["instruction"].get<std::string>();
    } else if (doc.contains("prompt") && !doc["prompt"].is_null()) {
        if (!doc["prompt"].is_string()) throw Error("prompt 必须是字符串");
        cfg.instruction = doc["prompt"].get<std::string>();
    }

    cfg.frameStride = parseBoundedInt(doc, "stride", 1, 1, 600);
    cfg.maxFrames = parseBoundedInt(doc, "max_frames", 0, 0, 600);
    cfg.copyCount = parseBoundedInt(doc, "count", 1, 1, 10);
    cfg.temporalConsistency = parseStrictBool(doc, "consistency", true);
    cfg.preserveText = parseStrictBool(doc, "preserve_text", true);
    cfg.allowResume = parseStrictBool(doc, "resume", true);

    if (doc.contains("plugins")) {
        if (!doc["plugins"].is_array()) throw Error("配置项 plugins 必须是插件 ID 数组");
        for (const auto& item : doc["plugins"]) {
            if (!item.is_string()) throw Error("plugins 元素必须是字符串");
            std::string id = item.get<std::string>();
            if (!base::isValidIdentifier(id)) throw Error("无效的插件 ID：" + id);
            if (std::find(cfg.enabledPlugins.begin(), cfg.enabledPlugins.end(), id) == cfg.enabledPlugins.end()) {
                cfg.enabledPlugins.push_back(std::move(id));
            }
        }
    }
    return cfg;
}

} // namespace meme::core
