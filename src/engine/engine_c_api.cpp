#define MEMESTUDIO_ENGINE_EXPORTS 1
#include "memestudio_engine.h"
#include "core/studio_session.hpp"
#include "media/image_ops.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"

#include <cstdlib>
#include <cstring>

struct MemeEngineSession {
    std::unique_ptr<meme::core::StudioSession> impl;
};

namespace {

using Json = nlohmann::json;

void writeEngineBuf(MemeEngineBuffer* out, std::string_view text) noexcept {
    if (!out) return;
    out->data = nullptr;
    out->size = 0;
    if (text.empty()) return;
    auto* p = static_cast<uint8_t*>(std::malloc(text.size() + 1));
    if (!p) return;
    std::memcpy(p, text.data(), text.size());
    p[text.size()] = 0;
    out->data = p;
    out->size = text.size();
}

Json buildQueueSummaryJson(const meme::core::StudioSession& s) {
    auto list = s.listSources();
    Json arr = Json::array();
    for (const auto& item : list) {
        arr.push_back(Json{
            {"path", meme::base::wideToUtf8(item.path.wstring())},
            {"filename", meme::base::wideToUtf8(item.path.filename().wstring())},
            {"width", item.width},
            {"height", item.height},
            {"frames", item.frameCount},
            {"animated", item.animated}
        });
    }
    return Json{
        {"count", list.size()},
        {"current_index", s.currentSourceIndex()},
        {"suggested_output", meme::base::wideToUtf8(s.suggestedOutputTarget().wstring())},
        {"items", arr}
    };
}

} // namespace

extern "C" {

MEME_ENGINE_API const char* MEME_ENGINE_CALL meme_engine_version(void) {
    return "2.1.0";
}

MEME_ENGINE_API void MEME_ENGINE_CALL meme_engine_free_buffer(MemeEngineBuffer* buffer) {
    if (buffer && buffer->data) {
        std::free(buffer->data);
        buffer->data = nullptr;
        buffer->size = 0;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_create(
    MemeEngineEventCallback on_event,
    void* user_data,
    MemeEngineSession** out_session,
    MemeEngineBuffer* out_err_utf8) {
    if (!out_session) return -1;
    *out_session = nullptr;
    try {
        auto cb = [on_event, user_data](meme::core::SessionEvent ev) {
            if (!on_event) return;
            using K = meme::core::SessionEvent::Kind;
            const char* typeName = "unknown";
            Json payload = Json::object();
            switch (ev.kind) {
                case K::Progress:
                    typeName = "progress";
                    payload = {
                        {"completed_frames", ev.progress.completedFrames},
                        {"total_frames", ev.progress.totalFrames},
                        {"message", ev.progress.messageUtf8},
                        {"has_preview", ev.progress.previewImage != nullptr}
                    };
                    break;
                case K::JobSucceeded: {
                    typeName = "job_succeeded";
                    Json outs = Json::array();
                    for (const auto& p : ev.outputs) outs.push_back(meme::base::wideToUtf8(p.wstring()));
                    payload = {{"outputs", outs}};
                    break;
                }
                case K::JobFailed:
                    typeName = "job_failed";
                    payload = {{"error", ev.message}};
                    break;
                case K::ModelsFetched:
                    typeName = "models_fetched";
                    payload = {{"models", ev.models}};
                    break;
                case K::EndpointOverrideChanged:
                    typeName = "endpoint_override_changed";
                    if (ev.endpointOverride.has_value()) {
                        payload = {
                            {"active", true},
                            {"plugin_id", ev.endpointOverride->pluginId},
                            {"base_url", ev.endpointOverride->baseUrl},
                            {"model", ev.endpointOverride->model}
                        };
                    } else {
                        payload = {{"active", false}};
                    }
                    break;
                case K::PresentOutputFile:
                    typeName = "present_output";
                    payload = {{"path", meme::base::wideToUtf8(ev.presentedFile.wstring())}};
                    break;
                case K::Log:
                    typeName = "log";
                    payload = {{"message", ev.message}};
                    break;
            }
            std::string dumped = payload.dump();
            on_event(user_data, typeName, dumped.c_str());
        };

        auto* handle = new MemeEngineSession{std::make_unique<meme::core::StudioSession>(std::move(cb))};
        *out_session = handle;
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API void MEME_ENGINE_CALL meme_session_destroy(MemeEngineSession* session) {
    delete session;
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_import_sources(
    MemeEngineSession* session,
    const char* paths_json_array_utf8,
    int32_t append,
    MemeEngineBuffer* out_queue_json,
    MemeEngineBuffer* out_err_utf8) {
    if (!session || !paths_json_array_utf8) return -1;
    try {
        Json arr = Json::parse(paths_json_array_utf8);
        if (!arr.is_array()) throw meme::Error("paths_json_array_utf8 必须是 JSON 字符串数组");
        std::vector<std::filesystem::path> inputs;
        for (const auto& el : arr) {
            inputs.push_back(meme::base::utf8ToWideStrict(el.get<std::string>()));
        }
        session->impl->importSources(inputs, append != 0);
        writeEngineBuf(out_queue_json, buildQueueSummaryJson(*session->impl).dump());
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_select_source_index(
    MemeEngineSession* session,
    size_t zero_based_index,
    MemeEngineBuffer* out_entry_json,
    MemeEngineBuffer* out_err_utf8) {
    if (!session) return -1;
    try {
        if (!session->impl->selectSourceIndex(zero_based_index)) {
            throw meme::Error("源素材索引超出队列范围");
        }
        writeEngineBuf(out_entry_json, buildQueueSummaryJson(*session->impl).dump());
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_step_source_index(
    MemeEngineSession* session,
    int32_t delta,
    MemeEngineBuffer* out_entry_json,
    MemeEngineBuffer* out_err_utf8) {
    if (!session) return -1;
    try {
        if (!session->impl->stepSourceIndex(delta)) {
            throw meme::Error("源素材队列为空");
        }
        writeEngineBuf(out_entry_json, buildQueueSummaryJson(*session->impl).dump());
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_get_queue_summary(
    MemeEngineSession* session,
    MemeEngineBuffer* out_summary_json) {
    if (!session) return -1;
    writeEngineBuf(out_summary_json, buildQueueSummaryJson(*session->impl).dump());
    return 0;
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_set_character(
    MemeEngineSession* session,
    const char* path_utf8,
    MemeEngineBuffer* out_err_utf8) {
    if (!session || !path_utf8) return -1;
    try {
        session->impl->loadCharacterFile(meme::base::utf8ToWideStrict(path_utf8));
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_set_character_crop(
    MemeEngineSession* session,
    double x,
    double y,
    double width,
    double height,
    MemeEngineBuffer* out_err_utf8) {
    if (!session) return -1;
    try {
        session->impl->setCharacterCrop({x, y, width, height});
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_get_settings(
    MemeEngineSession* session,
    MemeEngineBuffer* out_settings_json) {
    if (!session) return -1;
    auto st = session->impl->settingsSnapshot();
    Json j{
        {"base_url", st.provider.baseUrl},
        {"remember_key", st.rememberKey},
        {"model", st.provider.model},
        {"quality", st.provider.quality},
        {"size", st.provider.size},
        {"output_format", st.provider.outputFormat},
        {"stream", st.provider.stream},
        {"copy_count", st.copyCount},
        {"frame_stride", st.frameStride},
        {"max_frames", st.maxFrames},
        {"consistency", st.temporalConsistency},
        {"preserve_text", st.preserveText},
        {"allow_resume", st.allowResume},
        {"enabled_plugins", st.enabledPlugins},
        {"signature_policy", static_cast<int>(st.signaturePolicy)}
    };
    writeEngineBuf(out_settings_json, j.dump());
    return 0;
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_update_settings(
    MemeEngineSession* session,
    const char* settings_json_utf8,
    int32_t persist_to_disk,
    MemeEngineBuffer* out_err_utf8) {
    if (!session || !settings_json_utf8) return -1;
    try {
        auto j = Json::parse(settings_json_utf8);
        auto st = session->impl->settingsSnapshot();
        if (j.contains("base_url")) st.provider.baseUrl = j["base_url"].get<std::string>();
        if (j.contains("api_key")) st.provider.apiKey = j["api_key"].get<std::string>();
        if (j.contains("remember_key")) st.rememberKey = j["remember_key"].get<bool>();
        if (j.contains("model")) st.provider.model = j["model"].get<std::string>();
        if (j.contains("quality")) st.provider.quality = j["quality"].get<std::string>();
        if (j.contains("size")) st.provider.size = j["size"].get<std::string>();
        if (j.contains("output_format")) st.provider.outputFormat = j["output_format"].get<std::string>();
        if (j.contains("stream")) st.provider.stream = j["stream"].get<bool>();
        if (j.contains("copy_count")) st.copyCount = j["copy_count"].get<int>();
        if (j.contains("frame_stride")) st.frameStride = j["frame_stride"].get<int>();
        if (j.contains("max_frames")) st.maxFrames = j["max_frames"].get<int>();
        if (j.contains("consistency")) st.temporalConsistency = j["consistency"].get<bool>();
        if (j.contains("preserve_text")) st.preserveText = j["preserve_text"].get<bool>();
        if (j.contains("allow_resume")) st.allowResume = j["allow_resume"].get<bool>();
        session->impl->updateSettings(st, persist_to_disk != 0);
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_read_frame_pargb32(
    MemeEngineSession* session,
    int32_t frame_target,
    size_t frame_index,
    uint32_t* out_width,
    uint32_t* out_height,
    uint32_t* out_delay_ms,
    size_t* out_frame_count,
    uint8_t* dst_pixels,
    size_t dst_stride_bytes,
    MemeEngineBuffer* out_err_utf8) {
    if (!session) return -1;
    try {
        if (frame_target == MEME_FRAME_CHARACTER) {
            auto img = session->impl->characterImageSnapshot();
            if (img.empty()) throw meme::Error("尚未加载人物参考图");
            if (out_width) *out_width = img.width;
            if (out_height) *out_height = img.height;
            if (out_delay_ms) *out_delay_ms = 0;
            if (out_frame_count) *out_frame_count = 1;
            if (dst_pixels) {
                meme::media::premultiplyBgraToPargb32(img, dst_pixels, dst_stride_bytes);
            }
            return 0;
        }

        auto anim = (frame_target == MEME_FRAME_RESULT)
            ? session->impl->currentResultAnimationSnapshot()
            : session->impl->currentSourceAnimationSnapshot();
        if (anim.frames.empty()) throw meme::Error("目标预览帧队列为空");
        if (frame_index >= anim.frames.size()) frame_index = 0;
        const auto& fr = anim.frames[frame_index];
        if (out_width) *out_width = fr.image.width;
        if (out_height) *out_height = fr.image.height;
        if (out_delay_ms) *out_delay_ms = fr.delayMs;
        if (out_frame_count) *out_frame_count = anim.frames.size();
        if (dst_pixels) {
            meme::media::premultiplyBgraToPargb32(fr.image, dst_pixels, dst_stride_bytes);
        }
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_start_replace(
    MemeEngineSession* session,
    const char* output_target_utf8,
    const char* instruction_utf8,
    MemeEngineBuffer* out_err_utf8) {
    if (!session || !output_target_utf8) return -1;
    try {
        session->impl->startReplaceJob(
            meme::base::utf8ToWideStrict(output_target_utf8),
            instruction_utf8 ? instruction_utf8 : "");
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_start_draw(
    MemeEngineSession* session,
    const char* draw_request_json_utf8,
    MemeEngineBuffer* out_err_utf8) {
    if (!session || !draw_request_json_utf8) return -1;
    try {
        Json j = Json::parse(draw_request_json_utf8);
        meme::core::PromptDrawTaskConfig cfg;
        cfg.provider = session->impl->effectiveProvider();
        cfg.prompt = j.at("prompt").get<std::string>();
        cfg.outputPath = meme::base::utf8ToWideStrict(j.at("output").get<std::string>());
        cfg.copyCount = j.value("count", 1);
        if (j.contains("quality")) cfg.provider.quality = j["quality"].get<std::string>();
        if (j.contains("size")) cfg.provider.size = j["size"].get<std::string>();
        for (const auto& sp : j.value("styles", Json::array())) {
            cfg.stylePaths.push_back(meme::base::utf8ToWideStrict(sp.get<std::string>()));
        }
        session->impl->startPromptDrawJob(std::move(cfg));
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_start_retouch(
    MemeEngineSession* session,
    const char* retouch_request_json_utf8,
    MemeEngineBuffer* out_err_utf8) {
    if (!session || !retouch_request_json_utf8) return -1;
    try {
        Json j = Json::parse(retouch_request_json_utf8);
        meme::core::RetouchTaskConfig cfg;
        cfg.provider = session->impl->effectiveProvider();
        cfg.sourcePath = meme::base::utf8ToWideStrict(j.at("source").get<std::string>());
        cfg.outputPath = meme::base::utf8ToWideStrict(j.at("output").get<std::string>());
        cfg.instruction = j.at("instruction").get<std::string>();
        std::string modeStr = j.value("mode", "prompt");
        cfg.mode = (modeStr == "region") ? meme::core::RetouchMode::RegionRedraw : meme::core::RetouchMode::PromptEdit;
        if (j.contains("region") && j["region"].is_object()) {
            const auto& r = j["region"];
            cfg.region = {r.value("x", 0.0), r.value("y", 0.0), r.value("width", 1.0), r.value("height", 1.0)};
        }
        cfg.copyCount = j.value("count", 1);
        cfg.preserveText = j.value("preserve_text", true);
        cfg.temporalConsistency = j.value("consistency", true);
        for (const auto& sp : j.value("styles", Json::array())) {
            cfg.stylePaths.push_back(meme::base::utf8ToWideStrict(sp.get<std::string>()));
        }
        session->impl->startRetouchJob(std::move(cfg));
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_start_fetch_models(
    MemeEngineSession* session,
    MemeEngineBuffer* out_err_utf8) {
    if (!session) return -1;
    try {
        session->impl->startFetchModels();
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API void MEME_ENGINE_CALL meme_session_cancel(MemeEngineSession* session) {
    if (session && session->impl) {
        session->impl->cancelActiveJob();
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_list_plugins(
    MemeEngineSession* session,
    MemeEngineBuffer* out_plugins_json) {
    if (!session) return -1;
    auto list = session->impl->discoverAndListPlugins();
    Json arr = Json::array();
    for (const auto& p : list) {
        Json cmds = Json::array();
        for (const auto& c : p.commands) {
            cmds.push_back({{"id", c.id}, {"title", c.title}, {"toolbar", c.showInToolbar}});
        }
        arr.push_back({
            {"id", p.id},
            {"name", p.name},
            {"version", p.version},
            {"description", p.description},
            {"loaded", session->impl->isPluginLoaded(p.id)},
            {"load_error", p.loadError},
            {"commands", cmds}
        });
    }
    writeEngineBuf(out_plugins_json, arr.dump());
    return 0;
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_reload_plugins(
    MemeEngineSession* session,
    const char* enabled_ids_json_array,
    int32_t signature_policy,
    MemeEngineBuffer* out_err_utf8) {
    if (!session || !enabled_ids_json_array) return -1;
    try {
        auto arr = Json::parse(enabled_ids_json_array);
        std::vector<std::string> ids;
        for (const auto& item : arr) ids.push_back(item.get<std::string>());
        auto pol = meme::core::SignaturePolicy::Disabled;
        if (signature_policy == 1) pol = meme::core::SignaturePolicy::Trusted;
        else if (signature_policy == 2) pol = meme::core::SignaturePolicy::SameSigner;

        std::string err;
        if (!session->impl->reloadPlugins(ids, pol, err)) {
            writeEngineBuf(out_err_utf8, err);
            return -1;
        }
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_invoke_plugin(
    MemeEngineSession* session,
    const char* plugin_id_utf8,
    const char* command_id_utf8,
    const char* args_json_utf8,
    void* owner_hwnd_or_null,
    MemeEngineBuffer* out_result_json,
    MemeEngineBuffer* out_err_utf8) {
    if (!session || !plugin_id_utf8 || !command_id_utf8) return -1;
    try {
        Json args = (args_json_utf8 && args_json_utf8[0]) ? Json::parse(args_json_utf8) : Json::object();
        Json res = session->impl->invokePluginCommand(plugin_id_utf8, command_id_utf8, args, owner_hwnd_or_null);
        writeEngineBuf(out_result_json, res.dump());
        return 0;
    } catch (const std::exception& e) {
        writeEngineBuf(out_err_utf8, e.what());
        return -1;
    }
}

} // extern "C"
