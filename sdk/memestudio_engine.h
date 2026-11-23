#ifndef MEMESTUDIO_ENGINE_H
#define MEMESTUDIO_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MemeStudio Frontend-Independent Engine C ABI v1 (MemeStudioEngine.dll)
 *
 * Designed as the stable, language-neutral boundary for upgrading or swapping UI frontends:
 *  - WinUI 3 (C# .NET LibraryImport / PInvoke + WriteableBitmap / SoftwareBitmap PARGB32)
 *  - WinUI 3 (C++/WinRT)
 *  - WebView2 / Tauri (Rust FFI) / Qt 6
 */

#ifdef _WIN32
#define MEME_ENGINE_CALL __cdecl
#ifdef MEMESTUDIO_ENGINE_EXPORTS
#define MEME_ENGINE_API __declspec(dllexport)
#else
#define MEME_ENGINE_API __declspec(dllimport)
#endif
#else
#define MEME_ENGINE_CALL
#define MEME_ENGINE_API
#endif

typedef struct MemeEngineSession MemeEngineSession;

typedef struct MemeEngineBuffer {
    uint8_t* data;
    size_t size;
} MemeEngineBuffer;

enum MemeEngineFrameTarget {
    MEME_FRAME_SOURCE    = 0,
    MEME_FRAME_CHARACTER = 1,
    MEME_FRAME_RESULT    = 2
};

typedef void (MEME_ENGINE_CALL *MemeEngineEventCallback)(
    void* user_data,
    const char* event_type_utf8,
    const char* payload_json_utf8);

MEME_ENGINE_API const char* MEME_ENGINE_CALL meme_engine_version(void);
MEME_ENGINE_API void        MEME_ENGINE_CALL meme_engine_free_buffer(MemeEngineBuffer* buffer);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_create(
    MemeEngineEventCallback on_event,
    void* user_data,
    MemeEngineSession** out_session,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API void MEME_ENGINE_CALL meme_session_destroy(MemeEngineSession* session);

/* Multi-source queue & navigation (supports files & directories; index is 0-based) */
MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_import_sources(
    MemeEngineSession* session,
    const char* paths_json_array_utf8,
    int32_t append,
    MemeEngineBuffer* out_queue_json,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_select_source_index(
    MemeEngineSession* session,
    size_t zero_based_index,
    MemeEngineBuffer* out_entry_json,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_step_source_index(
    MemeEngineSession* session,
    int32_t delta,
    MemeEngineBuffer* out_entry_json,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_get_queue_summary(
    MemeEngineSession* session,
    MemeEngineBuffer* out_summary_json);

/* Character, styles, and settings */
MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_set_character(
    MemeEngineSession* session,
    const char* path_utf8,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_set_character_crop(
    MemeEngineSession* session,
    double x,
    double y,
    double width,
    double height,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_get_settings(
    MemeEngineSession* session,
    MemeEngineBuffer* out_settings_json);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_update_settings(
    MemeEngineSession* session,
    const char* settings_json_utf8,
    int32_t persist_to_disk,
    MemeEngineBuffer* out_err_utf8);

/*
 * Direct GPU-ready Premultiplied BGRA8 (PARGB32) frame readout for WinUI 3 WriteableBitmap / SoftwareBitmap.
 * Pass dst_pixels == NULL to query required dimensions and delay_ms without copying pixels.
 */
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
    MemeEngineBuffer* out_err_utf8);

/* Async job & model discovery */
MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_start_replace(
    MemeEngineSession* session,
    const char* output_target_utf8,
    const char* instruction_utf8,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_start_fetch_models(
    MemeEngineSession* session,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API void MEME_ENGINE_CALL meme_session_cancel(MemeEngineSession* session);

/* Plugin management & invocation (owner_hwnd may be WinUI 3 IWindowNative HWND or NULL) */
MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_list_plugins(
    MemeEngineSession* session,
    MemeEngineBuffer* out_plugins_json);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_reload_plugins(
    MemeEngineSession* session,
    const char* enabled_ids_json_array,
    int32_t signature_policy,
    MemeEngineBuffer* out_err_utf8);

MEME_ENGINE_API int32_t MEME_ENGINE_CALL meme_session_invoke_plugin(
    MemeEngineSession* session,
    const char* plugin_id_utf8,
    const char* command_id_utf8,
    const char* args_json_utf8,
    void* owner_hwnd_or_null,
    MemeEngineBuffer* out_result_json,
    MemeEngineBuffer* out_err_utf8);

#ifdef __cplusplus
}
#endif

#endif /* MEMESTUDIO_ENGINE_H */
