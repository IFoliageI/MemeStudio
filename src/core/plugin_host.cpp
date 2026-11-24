#include "core/plugin_host.hpp"
#include "core/replace_engine.hpp"
#include "media/wic_codec.hpp"
#include "base/app_paths.hpp"
#include "base/crypto.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/json_io.hpp"
#include "base/utf8.hpp"
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace meme::core {

namespace {

// Retains a FILE_SHARE_READ lock across SHA-256 hashing, WinVerifyTrust, and LoadLibraryExW
// to completely eliminate TOCTOU file-swap windows!
struct LockedPluginFile {
    fs::path canonicalPath;
    HANDLE handle = INVALID_HANDLE_VALUE;

    explicit LockedPluginFile(const fs::path& file) : canonicalPath(fs::canonical(file)) {
        handle = CreateFileW(
            canonicalPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            throw Error("无法锁定打开文件（Windows 错误码 " + std::to_string(err) + "）：" +
                        base::wideToUtf8(canonicalPath.wstring()));
        }
    }

    ~LockedPluginFile() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
    LockedPluginFile(const LockedPluginFile&) = delete;
    LockedPluginFile& operator=(const LockedPluginFile&) = delete;

    base::Bytes readAllBytes() const {
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(handle, &fileSize) || fileSize.QuadPart < 0 ||
            static_cast<uint64_t>(fileSize.QuadPart) > 256ull * 1024 * 1024) {
            throw Error("插件 DLL 大小超出安全范围");
        }
        base::Bytes buf(static_cast<size_t>(fileSize.QuadPart));
        LARGE_INTEGER zero{};
        SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN);
        size_t offset = 0;
        while (offset < buf.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(buf.size() - offset, 1024 * 1024));
            DWORD readNow = 0;
            if (!ReadFile(handle, buf.data() + offset, chunk, &readNow, nullptr) || readNow == 0) {
                throw Error("读取插件二进制内容失败");
            }
            offset += readNow;
        }
        return buf;
    }
};

struct WinTrustSessionGuard {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    ~WinTrustSessionGuard() {
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    }
};

// Properly verifies BOTH primary AND secondary Authenticode signatures (fixes BUG-04 where
// data.pSignatureSettings was never assigned!).
std::vector<base::Bytes> extractVerifiedSigners(const LockedPluginFile& file, std::string_view labelUtf8) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = file.canonicalPath.c_str();
    fileInfo.hFile = file.handle;

    std::vector<base::Bytes> signers;
    DWORD secondaryCount = 0;
    LONG primaryStatus = TRUST_E_NOSIGNATURE;

    for (DWORD sigIndex = 0; sigIndex <= secondaryCount; ++sigIndex) {
        WINTRUST_SIGNATURE_SETTINGS sigSettings{};
        sigSettings.cbStruct = sizeof(sigSettings);
        sigSettings.dwIndex = sigIndex;
        sigSettings.dwFlags = WSS_VERIFY_SPECIFIC | (sigIndex == 0 ? WSS_GET_SECONDARY_SIG_COUNT : 0);

        WinTrustSessionGuard session;
        auto& wd = session.data;
        wd.cbStruct = sizeof(wd);
        wd.dwUIChoice = WTD_UI_NONE;
        wd.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        wd.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_DISABLE_MD2_MD4;
        wd.dwUnionChoice = WTD_CHOICE_FILE;
        wd.pFile = &fileInfo;
        wd.dwStateAction = WTD_STATEACTION_VERIFY;
        wd.pSignatureSettings = &sigSettings; // <--- Critical BUG-04 fix!

        LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &session.action, &wd);
        if (sigIndex == 0) {
            primaryStatus = status;
            secondaryCount = sigSettings.cSecondarySigs;
        }
        if (status != ERROR_SUCCESS) {
            continue;
        }
        auto* provData = WTHelperProvDataFromStateData(wd.hWVTStateData);
        auto* provSigner = provData ? WTHelperGetProvSignerFromChain(provData, 0, FALSE, 0) : nullptr;
        if (provSigner && provSigner->csCertChain > 0 && provSigner->pasCertChain &&
            provSigner->pasCertChain[0].pCert) {
            const auto* leafCert = provSigner->pasCertChain[0].pCert;
            signers.emplace_back(
                leafCert->pbCertEncoded,
                leafCert->pbCertEncoded + leafCert->cbCertEncoded);
        }
    }

    if (signers.empty()) {
        if (primaryStatus == TRUST_E_NOSIGNATURE) {
            throw Error(std::string(labelUtf8) + "缺少有效的 Authenticode 数字签名");
        }
        throw Error(std::string(labelUtf8) + "数字签名验证未通过（状态码 " + std::to_string(primaryStatus) + "）");
    }
    return signers;
}

void writeAbiBytes(MemeStudioBytesV2* out, std::string_view text) {
    if (!out) return;
    out->data = nullptr;
    out->size = 0;
    if (text.empty()) return;
    auto* buf = static_cast<uint8_t*>(std::malloc(text.size() + 1));
    if (!buf) return;
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = 0;
    out->data = buf;
    out->size = text.size();
}

void writeAbiRawBytes(MemeStudioBytesV2* out, std::span<const uint8_t> bytes) {
    if (!out) return;
    out->data = nullptr;
    out->size = 0;
    if (bytes.empty()) return;
    auto* buf = static_cast<uint8_t*>(std::malloc(bytes.size()));
    if (!buf) return;
    std::memcpy(buf, bytes.data(), bytes.size());
    out->data = buf;
    out->size = bytes.size();
}

void freeAbiBytes(MemeStudioBytesV2& b) noexcept {
    if (b.data) {
        std::free(b.data);
        b.data = nullptr;
    }
    b.size = 0;
}

std::string takeAbiString(MemeStudioBytesV2& b) {
    if (!b.data || b.size == 0) {
        freeAbiBytes(b);
        return {};
    }
    std::string s(reinterpret_cast<const char*>(b.data), b.size);
    freeAbiBytes(b);
    return s;
}

} // namespace

std::vector<PluginManifest> discoverPluginsInDirectory(const fs::path& rootDir) {
    std::vector<PluginManifest> result;
    std::error_code ec;
    if (!fs::exists(rootDir, ec) || !fs::is_directory(rootDir, ec)) {
        return result;
    }

    std::set<std::string> seenIds;
    for (const auto& entry : fs::directory_iterator(rootDir, ec)) {
        if (!entry.is_directory()) continue;
        fs::path manifestFile = entry.path() / L"plugin.json";
        if (!fs::is_regular_file(manifestFile, ec)) continue;

        PluginManifest info;
        info.directory = entry.path();
        info.name = base::wideToUtf8(entry.path().filename().wstring());
        try {
            auto j = base::readJsonFile(manifestFile, 256 * 1024);
            int apiVer = j.at("api_version").get<int>();
            if (apiVer != static_cast<int>(MEMESTUDIO_PLUGIN_ABI_V2)) {
                throw Error("插件 ABI 版本不匹配（要求 v2，当前为 v" + std::to_string(apiVer) + "）");
            }
            info.id = j.at("id").get<std::string>();
            if (!base::isValidIdentifier(info.id)) {
                throw Error("插件 ID 格式无效");
            }
            if (!seenIds.insert(info.id).second) {
                throw Error("插件 ID 重复：" + info.id);
            }
            info.name = j.at("name").get<std::string>();
            info.version = j.at("version").get<std::string>();
            info.description = j.value("description", std::string{});

            std::string libRel = j.at("library").get<std::string>();
            fs::path libName = base::utf8ToWideStrict(libRel);
            if (libName != libName.filename() || libName.extension() != L".dll") {
                throw Error("library 字段必须是插件目录内的 .dll 文件名");
            }
            info.libraryPath = entry.path() / libName;
            if (!fs::is_regular_file(info.libraryPath, ec)) {
                throw Error("找不到插件动态库：" + libRel);
            }

            std::set<std::string> cmdIds;
            if (j.contains("commands") && j["commands"].is_array()) {
                for (const auto& cmd : j["commands"]) {
                    std::string cid = cmd.at("id").get<std::string>();
                    if (!base::isValidIdentifier(cid) || !cmdIds.insert(cid).second) {
                        throw Error("插件命令 ID 无效或重复：" + cid);
                    }
                    info.commands.push_back(PluginCommandManifest{
                        cid,
                        cmd.at("title").get<std::string>(),
                        cmd.value("toolbar", false)
                    });
                }
            }
        } catch (const std::exception& e) {
            info.loadError = e.what();
        }
        result.push_back(std::move(info));
    }

    // Sort deterministically by stable plugin ID (fallback to directory name)
    std::sort(result.begin(), result.end(), [](const PluginManifest& a, const PluginManifest& b) {
        if (a.id != b.id) return a.id < b.id;
        return a.name < b.name;
    });
    return result;
}

struct PluginHost::LoadedModule {
    PluginManifest manifest;
    std::string sha256;
    HMODULE dllHandle = nullptr;
    const MemeStudioPluginV2* descriptor = nullptr;
    void* instance = nullptr;
    bool instanceCreated = false;

    ~LoadedModule() {
        // Only invoke destroy if create() actually succeeded (fixes premature shutdown bug!)
        if (instanceCreated && descriptor && descriptor->destroy) {
            descriptor->destroy(instance);
        }
        if (dllHandle) {
            FreeLibrary(dllHandle);
        }
    }
};

MemeStudioHostV2 PluginHost::makeHostTable(PluginHost* self) {
    MemeStudioHostV2 t{};
    t.struct_size = sizeof(MemeStudioHostV2);
    t.abi_version = MEMESTUDIO_PLUGIN_ABI_V2;
    t.host_ctx = self;

    t.alloc = [](void*, size_t sz) -> void* {
        return sz == 0 ? nullptr : std::malloc(sz);
    };
    t.free = [](void*, void* p) {
        if (p) std::free(p);
    };
    t.free_bytes = [](void*, MemeStudioBytesV2* b) {
        if (b) freeAbiBytes(*b);
    };
    t.log = [](void* ctx, int32_t level, const char* pluginId, const char* msg) {
        try {
            auto* host = static_cast<PluginHost*>(ctx);
            if (host && host->callbacks_.onLog && msg) {
                host->callbacks_.onLog(
                    level,
                    pluginId ? std::string_view(pluginId) : std::string_view{},
                    std::string_view(msg));
            }
        } catch (...) {}
    };
    t.is_cancelled = [](const MemeStudioCancelTokenV2* token) -> int32_t {
        if (!token) return 0;
        const auto* ct = reinterpret_cast<const net::CancelToken*>(token);
        if (!ct->hasValidAbiMagic()) return 0;
        return ct->isStopped() ? 1 : 0;
    };
    t.get_data_dir = [](void*, const char* pluginId, MemeStudioBytesV2* outPath) -> int32_t {
        try {
            if (!pluginId || !outPath) return -1;
            fs::path dir = base::pluginDataDir(pluginId);
            writeAbiBytes(outPath, base::wideToUtf8(dir.wstring()));
            return 0;
        } catch (...) {
            return -1;
        }
    };
    t.protect_secret = [](void*, const char* plain, MemeStudioBytesV2* outCipher) -> int32_t {
        try {
            if (!plain || !outCipher) return -1;
            writeAbiBytes(outCipher, base::dpapiProtect(plain));
            return 0;
        } catch (...) {
            return -1;
        }
    };
    t.unprotect_secret = [](void*, const char* cipher, MemeStudioBytesV2* outPlain) -> int32_t {
        try {
            if (!cipher || !outPlain) return -1;
            auto unlocked = base::dpapiUnprotect(cipher);
            writeAbiBytes(outPlain, unlocked.view());
            return 0;
        } catch (...) {
            return -1;
        }
    };
    t.http_send = [](
        void* ctx,
        const char* reqJson,
        const uint8_t* body,
        size_t bodyLen,
        MemeStudioCancelTokenV2* cancelTok,
        void (MEMESTUDIO_CALL* onSse)(void*, const char*, const char*),
        void* user,
        uint32_t* outStatus,
        int32_t* outStreamed,
        MemeStudioBytesV2* outResp,
        MemeStudioBytesV2* outErr) -> int32_t {
        try {
            auto* host = static_cast<PluginHost*>(ctx);
            if (!host || !reqJson) throw Error("无效的 HTTP 请求参数");
            auto j = nlohmann::json::parse(reqJson);

            net::HttpRequestSpec spec;
            spec.url = j.at("url").get<std::string>();
            spec.method = j.value("method", std::string("GET"));
            spec.timeoutSeconds = j.value("timeout_seconds", 600);
            spec.expectEventStream = j.value("expect_event_stream", false);
            spec.allowLoopbackHttp = j.value("allow_loopback_http", true);
            if (j.contains("headers") && j["headers"].is_object()) {
                for (const auto& [hk, hv] : j["headers"].items()) {
                    if (hv.is_string()) spec.headers.emplace_back(hk, hv.get<std::string>());
                }
            }
            if (body && bodyLen > 0) {
                spec.body = std::span<const uint8_t>(body, bodyLen);
            }

            net::CancelToken localCancel;
            auto* incomingCancel = reinterpret_cast<net::CancelToken*>(cancelTok);
            auto& activeCancel = (incomingCancel && incomingCancel->hasValidAbiMagic()) ? *incomingCancel : localCancel;

            net::HttpResponse resp = host->httpPool_.send(spec, activeCancel, [&](const net::SseEvent& ev) {
                if (onSse) {
                    onSse(user, ev.eventType.c_str(), ev.data.c_str());
                }
            });
            if (outStatus) *outStatus = resp.status;
            if (outStreamed) *outStreamed = resp.streamed ? 1 : 0;
            writeAbiRawBytes(outResp, resp.body);
            return 0;
        } catch (const CancelledError& e) {
            writeAbiBytes(outErr, e.what());
            return -2;
        } catch (const ApiConnectionError& e) {
            writeAbiBytes(outErr, e.what());
            return -3;
        } catch (const std::exception& e) {
            writeAbiBytes(outErr, e.what());
            return -1;
        }
    };
    t.probe_media = [](void*, const char* pathUtf8, MemeStudioBytesV2* outMeta, MemeStudioBytesV2* outErr) -> int32_t {
        try {
            if (!pathUtf8 || !outMeta) throw Error("无效的媒体探测参数");
            auto meta = media::probeMediaMetadata(base::utf8ToWideStrict(pathUtf8));
            nlohmann::json j{
                {"width", meta.width},
                {"height", meta.height},
                {"frames", meta.frameCount},
                {"animated", meta.animated},
                {"delays", meta.frameDelaysMs}
            };
            if (meta.loopCount.has_value()) j["loop"] = *meta.loopCount;
            else j["loop"] = nullptr;
            writeAbiBytes(outMeta, j.dump());
            return 0;
        } catch (const std::exception& e) {
            writeAbiBytes(outErr, e.what());
            return -1;
        }
    };
    t.decode_media = [](
        void*,
        const char* pathUtf8,
        const uint8_t* rawBytes,
        size_t rawLen,
        MemeStudioBytesV2* outMeta,
        MemeStudioImageBufferV2** outFrames,
        size_t* outCount,
        MemeStudioBytesV2* outErr) -> int32_t {
        try {
            if (!outFrames || !outCount) throw Error("无效的图像解码输出指针");
            media::Animation anim;
            if (pathUtf8 && pathUtf8[0] != '\0') {
                anim = media::decodeAnimation(base::utf8ToWideStrict(pathUtf8));
            } else if (rawBytes && rawLen > 0) {
                anim.frames.push_back(media::Frame{media::decodeImageBytes(std::span<const uint8_t>(rawBytes, rawLen)), 100});
            } else {
                throw Error("未提供图像路径或字节数据");
            }

            size_t count = anim.frames.size();
            auto* arr = static_cast<MemeStudioImageBufferV2*>(std::calloc(count, sizeof(MemeStudioImageBufferV2)));
            if (!arr) throw Error("图像帧数组分配失败");

            for (size_t i = 0; i < count; ++i) {
                const auto& f = anim.frames[i];
                size_t byteLen = f.image.pixels.size();
                auto* px = static_cast<uint8_t*>(std::malloc(byteLen));
                if (!px) {
                    for (size_t k = 0; k < i; ++k) std::free(arr[k].bgra_pixels);
                    std::free(arr);
                    throw Error("图像像素内存分配失败");
                }
                std::memcpy(px, f.image.pixels.data(), byteLen);
                arr[i] = MemeStudioImageBufferV2{f.image.width, f.image.height, f.image.stride(), f.delayMs, px};
            }

            if (outMeta) {
                nlohmann::json mj{
                    {"animated", anim.animated},
                    {"frames", count}
                };
                if (anim.loopCount.has_value()) mj["loop"] = *anim.loopCount;
                else mj["loop"] = nullptr;
                writeAbiBytes(outMeta, mj.dump());
            }
            *outFrames = arr;
            *outCount = count;
            return 0;
        } catch (const std::exception& e) {
            writeAbiBytes(outErr, e.what());
            return -1;
        }
    };
    t.free_images = [](void*, MemeStudioImageBufferV2* frames, size_t count) {
        if (!frames) return;
        for (size_t i = 0; i < count; ++i) {
            if (frames[i].bgra_pixels) std::free(frames[i].bgra_pixels);
        }
        std::free(frames);
    };
    t.encode_media = [](
        void*,
        const char* pathOrNull,
        const char* formatJson,
        const MemeStudioImageBufferV2* frames,
        size_t count,
        MemeStudioBytesV2* outEncoded,
        MemeStudioBytesV2* outErr) -> int32_t {
        try {
            if (!frames || count == 0) throw Error("编码图像帧不能为空");
            nlohmann::json opts = formatJson && formatJson[0] ? nlohmann::json::parse(formatJson) : nlohmann::json::object();
            std::string fmt = opts.value("format", count > 1 ? std::string("gif") : std::string("png"));

            std::vector<media::Frame> vec;
            vec.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                media::Image img = media::allocateImage(frames[i].width, frames[i].height);
                for (uint32_t y = 0; y < img.height; ++y) {
                    std::memcpy(
                        img.pixels.data() + static_cast<size_t>(y) * img.stride(),
                        frames[i].bgra_pixels + static_cast<size_t>(y) * frames[i].stride,
                        img.stride());
                }
                vec.push_back(media::Frame{std::move(img), frames[i].delay_ms ? frames[i].delay_ms : 100u});
            }

            if (fmt == "gif") {
                if (!pathOrNull || !pathOrNull[0]) throw Error("GIF 编码需要指定输出文件路径");
                std::optional<uint16_t> loop;
                if (opts.contains("loop") && opts["loop"].is_number_unsigned()) {
                    loop = static_cast<uint16_t>(opts["loop"].get<uint32_t>());
                }
                media::saveGifFile(base::utf8ToWideStrict(pathOrNull), vec, loop);
            } else {
                base::Bytes pngBytes = media::encodePng(vec.front().image);
                if (pathOrNull && pathOrNull[0]) {
                    base::writeFileAtomic(base::utf8ToWideStrict(pathOrNull), pngBytes);
                }
                if (outEncoded) {
                    writeAbiRawBytes(outEncoded, pngBytes);
                }
            }
            return 0;
        } catch (const std::exception& e) {
            writeAbiBytes(outErr, e.what());
            return -1;
        }
    };
    t.get_workspace_state = [](void* ctx, MemeStudioBytesV2* outJson) -> int32_t {
        try {
            auto* host = static_cast<PluginHost*>(ctx);
            nlohmann::json st = (host && host->callbacks_.onGetWorkspaceState)
                ? host->callbacks_.onGetWorkspaceState()
                : nlohmann::json::object();
            writeAbiBytes(outJson, st.dump());
            return 0;
        } catch (...) {
            return -1;
        }
    };
    t.set_session_endpoint_override = [](void* ctx, const char* pluginId, const char* endpointJson) -> int32_t {
        try {
            auto* host = static_cast<PluginHost*>(ctx);
            if (!host || !pluginId) return -1;
            std::optional<SessionEndpointOverride> next;
            if (endpointJson && endpointJson[0] != '\0') {
                auto j = nlohmann::json::parse(endpointJson);
                next = SessionEndpointOverride{
                    pluginId,
                    j.at("base_url").get<std::string>(),
                    j.value("api_key", std::string{}),
                    j.value("model", std::string{})
                };
            }
            {
                std::lock_guard lock(host->stateMutex_);
                host->endpointOverride_ = next;
            }
            if (host->callbacks_.onEndpointOverrideChanged) {
                host->callbacks_.onEndpointOverrideChanged(next);
            }
            return 0;
        } catch (...) {
            return -1;
        }
    };
    t.present_output_file = [](void* ctx, const char* pathUtf8) -> int32_t {
        try {
            auto* host = static_cast<PluginHost*>(ctx);
            if (host && host->callbacks_.onPresentOutputFile && pathUtf8) {
                host->callbacks_.onPresentOutputFile(base::utf8ToWideStrict(pathUtf8));
            }
            return 0;
        } catch (...) {
            return -1;
        }
    };
    t.run_core_replace_job = [](
        void* ctx,
        const char* jobJson,
        MemeStudioCancelTokenV2* cancelTok,
        void (MEMESTUDIO_CALL* onProg)(void*, const char*),
        void* user,
        MemeStudioBytesV2* outResult,
        MemeStudioBytesV2* outErr) -> int32_t {
        try {
            auto* host = static_cast<PluginHost*>(ctx);
            if (!host || !jobJson) throw Error("无效的任务调用参数");
            auto doc = nlohmann::json::parse(jobJson);
            ReplaceTaskConfig cfg = parseReplaceTaskConfig(doc);

            net::CancelToken localCancel;
            auto* incomingCancel = reinterpret_cast<net::CancelToken*>(cancelTok);
            auto& activeCancel = (incomingCancel && incomingCancel->hasValidAbiMagic()) ? *incomingCancel : localCancel;

            net::OpenAiImageClient client(host->httpPool_, [host](
                std::string_view op,
                const net::ProviderConfig& prov,
                const nlohmann::json& meta,
                std::span<const net::UploadPart> ups,
                net::CancelToken& c,
                const net::ImagePreviewFn& prev,
                base::Bytes& resp) {
                return host->tryInterceptApi(op, prov, meta, ups, c, prev, resp);
            });

            auto outputs = runCharacterReplaceJob(cfg, client, host, activeCancel, [&](const ReplaceProgress& p) {
                if (onProg) {
                    nlohmann::json pj{
                        {"completed_frames", p.completedFrames},
                        {"total_frames", p.totalFrames},
                        {"copy_index", p.currentCopyIndex},
                        {"total_copies", p.totalCopies},
                        {"message", p.messageUtf8}
                    };
                    std::string dumped = pj.dump();
                    onProg(user, dumped.c_str());
                }
            });

            nlohmann::json outArr = nlohmann::json::array();
            for (const auto& op : outputs) {
                outArr.push_back(base::wideToUtf8(op.wstring()));
            }
            writeAbiBytes(outResult, nlohmann::json{{"outputs", outArr}}.dump());
            return 0;
        } catch (const CancelledError& e) {
            writeAbiBytes(outErr, e.what());
            return -2;
        } catch (const std::exception& e) {
            writeAbiBytes(outErr, e.what());
            return -1;
        }
    };
    t.open_panel = [](
        void* ctx,
        const char* panelKey,
        void* ownerHwnd,
        int32_t wDip,
        int32_t hDip,
        const char* titleUtf8,
        intptr_t(MEMESTUDIO_CALL* wndProc)(void*, void*, uint32_t, uintptr_t, intptr_t),
        void* panelCtx,
        void(MEMESTUDIO_CALL* onDestroy)(void*)) -> int32_t {
        try {
            auto* host = static_cast<PluginHost*>(ctx);
            if (!host || !host->callbacks_.onOpenPanel || !panelKey || !wndProc) return -1;
            return host->callbacks_.onOpenPanel(
                panelKey, ownerHwnd, wDip, hDip, titleUtf8 ? titleUtf8 : "MemeStudio", wndProc, panelCtx, onDestroy);
        } catch (...) {
            return -1;
        }
    };
    return t;
}

PluginHost::PluginHost(
    net::WinHttpPool& httpPool,
    std::span<const PluginManifest> discovered,
    std::span<const std::string> enabledIds,
    SignaturePolicy sigPolicy,
    HostUiCallbacks callbacks)
    : httpPool_(httpPool), callbacks_(std::move(callbacks)), hostTable_(makeHostTable(this)) {
    if (enabledIds.empty()) return;

    const bool requireSig = (sigPolicy != SignaturePolicy::Disabled);
    const bool requireSameSigner = (sigPolicy == SignaturePolicy::SameSigner);
    std::vector<base::Bytes> hostSigners;
    if (requireSameSigner) {
        LockedPluginFile hostExe(base::executableFilePath());
        hostSigners = extractVerifiedSigners(hostExe, "主程序 ");
    }

    struct PendingPlugin {
        const PluginManifest* manifest = nullptr;
        std::unique_ptr<LockedPluginFile> lockedFile;
        std::string sha256;
    };

    std::vector<PendingPlugin> staged;
    std::set<std::string> uniqueEnabled;
    for (const auto& id : enabledIds) {
        if (!uniqueEnabled.insert(id).second) continue;
        auto it = std::find_if(discovered.begin(), discovered.end(), [&](const PluginManifest& m) {
            return m.id == id;
        });
        if (it == discovered.end()) {
            throw Error("未找到已请求的插件：" + id);
        }
        if (!it->loadError.empty()) {
            throw Error("插件不可用（" + id + "）：" + it->loadError);
        }

        PendingPlugin item;
        item.manifest = &(*it);
        // Always hold FILE_SHARE_READ lock across hashing, signature check, and LoadLibraryExW!
        item.lockedFile = std::make_unique<LockedPluginFile>(it->libraryPath);
        item.sha256 = base::sha256Hex(item.lockedFile->readAllBytes());

        if (requireSig) {
            auto pluginSigners = extractVerifiedSigners(*item.lockedFile, "插件 " + id + " ");
            if (requireSameSigner) {
                bool matched = std::any_of(pluginSigners.begin(), pluginSigners.end(), [&](const base::Bytes& ps) {
                    return std::find(hostSigners.begin(), hostSigners.end(), ps) != hostSigners.end();
                });
                if (!matched) {
                    throw Error("插件 " + id + " 的数字签名者证书与主程序不一致");
                }
            }
        }
        staged.push_back(std::move(item));
    }

    try {
        for (auto& item : staged) {
            const auto& info = *item.manifest;
            auto mod = std::make_unique<LoadedModule>();
            mod->manifest = info;
            mod->sha256 = std::move(item.sha256);

            mod->dllHandle = LoadLibraryExW(
                item.lockedFile->canonicalPath.c_str(),
                nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!mod->dllHandle) {
                throw Error("加载插件 DLL 失败（" + info.id + "，错误码 " + std::to_string(GetLastError()) + "）");
            }

            auto entryFn = reinterpret_cast<MemeStudioPluginEntryV2>(
                GetProcAddress(mod->dllHandle, MEMESTUDIO_PLUGIN_ENTRY_V2));
            if (!entryFn) {
                throw Error("插件缺少导出入口 MemeStudioGetPluginV2：" + info.id);
            }

            mod->descriptor = entryFn();
            if (!mod->descriptor ||
                mod->descriptor->struct_size < offsetof(MemeStudioPluginV2, invoke_command) ||
                mod->descriptor->abi_version != MEMESTUDIO_PLUGIN_ABI_V2 ||
                !mod->descriptor->id ||
                info.id != mod->descriptor->id) {
                throw Error("插件描述符 ABI 或 ID 校验失败：" + info.id);
            }

            if (mod->descriptor->create) {
                MemeStudioBytesV2 errBytes{};
                int32_t rc = mod->descriptor->create(&hostTable_, &mod->instance, &errBytes);
                std::string errMsg = takeAbiString(errBytes);
                if (rc != 0) {
                    throw Error("初始化插件失败（" + info.id + "）" + (errMsg.empty() ? "" : "：" + errMsg));
                }
            }
            // Mark instanceCreated ONLY after create() returns 0!
            mod->instanceCreated = true;
            modules_.push_back(std::move(mod));
        }
    } catch (...) {
        modules_.clear();
        throw;
    }
}

PluginHost::~PluginHost() {
    modules_.clear();
}

bool PluginHost::isLoaded(std::string_view pluginId) const noexcept {
    return std::any_of(modules_.begin(), modules_.end(), [&](const auto& m) {
        return m->manifest.id == pluginId;
    });
}

nlohmann::json PluginHost::pipelineFingerprint() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : modules_) {
        if (!m->descriptor->transform_prompt && !m->descriptor->process_frames_batch) {
            continue;
        }
        arr.push_back(nlohmann::json{
            {"id", m->manifest.id},
            {"version", m->manifest.version},
            {"sha256", m->sha256}
        });
    }
    return arr;
}

std::optional<SessionEndpointOverride> PluginHost::sessionEndpointOverride() const {
    std::lock_guard lock(stateMutex_);
    return endpointOverride_;
}

void PluginHost::clearSessionEndpointOverride() {
    {
        std::lock_guard lock(stateMutex_);
        endpointOverride_.reset();
    }
    if (callbacks_.onEndpointOverrideChanged) {
        callbacks_.onEndpointOverrideChanged(std::nullopt);
    }
}

std::string PluginHost::transformPrompt(const nlohmann::json& context, std::string promptUtf8) const {
    std::string ctxDump = context.dump();
    for (const auto& m : modules_) {
        if (!m->descriptor->transform_prompt) continue;
        MemeStudioBytesV2 outPrompt{};
        MemeStudioBytesV2 outErr{};
        int32_t rc = m->descriptor->transform_prompt(
            m->instance, ctxDump.c_str(), promptUtf8.c_str(), &outPrompt, &outErr);
        std::string errText = takeAbiString(outErr);
        if (rc != 0) {
            freeAbiBytes(outPrompt);
            throw Error("插件提示词变换失败（" + m->manifest.id + "）" + (errText.empty() ? "" : "：" + errText));
        }
        if (outPrompt.data && outPrompt.size > 0) {
            if (outPrompt.size > 256 * 1024) {
                freeAbiBytes(outPrompt);
                throw Error("插件生成的提示词超出 256 KB 限制：" + m->manifest.id);
            }
            promptUtf8 = takeAbiString(outPrompt);
        } else {
            freeAbiBytes(outPrompt);
        }
    }
    return promptUtf8;
}

void PluginHost::processFramesBatch(
    std::string_view stage,
    const nlohmann::json& context,
    std::span<media::Frame> frames,
    net::CancelToken& cancel) const {
    if (frames.empty()) return;
    std::string stageStr(stage);
    std::string ctxDump = context.dump();

    for (const auto& m : modules_) {
        if (!m->descriptor->process_frames_batch) continue;
        cancel.verify();

        std::vector<MemeStudioImageBufferV2> views;
        views.reserve(frames.size());
        for (auto& f : frames) {
            size_t bytes = f.image.pixels.size();
            auto* px = static_cast<uint8_t*>(std::malloc(bytes));
            if (!px) {
                for (auto& prev : views) std::free(prev.bgra_pixels);
                throw Error("帧缓冲区分配失败");
            }
            std::memcpy(px, f.image.pixels.data(), bytes);
            views.push_back(MemeStudioImageBufferV2{
                f.image.width, f.image.height, f.image.stride(), f.delayMs, px
            });
        }

        MemeStudioBytesV2 outErr{};
        int32_t rc = m->descriptor->process_frames_batch(
            m->instance,
            stageStr.c_str(),
            ctxDump.c_str(),
            views.data(),
            views.size(),
            reinterpret_cast<MemeStudioCancelTokenV2*>(&cancel),
            &outErr);
        std::string errMsg = takeAbiString(outErr);

        if (rc != 0) {
            for (auto& v : views) if (v.bgra_pixels) std::free(v.bgra_pixels);
            if (cancel.isStopped()) throw CancelledError();
            throw Error("插件帧批处理失败（" + m->manifest.id + "）" + (errMsg.empty() ? "" : "：" + errMsg));
        }

        // Adopt updated frame buffers (plugins like Waifu2x may reallocate bgra_pixels via host->alloc for 2x/4x upscale!)
        try {
            for (size_t i = 0; i < frames.size(); ++i) {
                const auto& v = views[i];
                media::Image nextImg = media::allocateImage(v.width, v.height);
                for (uint32_t y = 0; y < nextImg.height; ++y) {
                    std::memcpy(
                        nextImg.pixels.data() + static_cast<size_t>(y) * nextImg.stride(),
                        v.bgra_pixels + static_cast<size_t>(y) * v.stride,
                        nextImg.stride());
                }
                frames[i].image = std::move(nextImg);
                frames[i].delayMs = v.delay_ms;
            }
        } catch (...) {
            for (auto& v : views) if (v.bgra_pixels) std::free(v.bgra_pixels);
            throw;
        }
        for (auto& v : views) if (v.bgra_pixels) std::free(v.bgra_pixels);
    }
}

bool PluginHost::tryInterceptApi(
    std::string_view op,
    const net::ProviderConfig& provider,
    const nlohmann::json& requestMeta,
    std::span<const net::UploadPart> uploads,
    net::CancelToken& cancel,
    const net::ImagePreviewFn& onPreview,
    base::Bytes& outResponseBytes) const {
    // Prioritize whichever plugin currently owns the active session endpoint override (e.g. local engine takeover)
    // before general transport routers (e.g. multi-channel load balancer)!
    std::string overrideOwnerId;
    {
        std::lock_guard lock(stateMutex_);
        if (endpointOverride_.has_value()) {
            overrideOwnerId = endpointOverride_->pluginId;
        }
    }
    std::vector<const LoadedModule*> orderedModules;
    orderedModules.reserve(modules_.size());
    for (const auto& m : modules_) {
        if (m->descriptor->intercept_image_api && m->manifest.id == overrideOwnerId) {
            orderedModules.push_back(m.get());
        }
    }
    for (const auto& m : modules_) {
        if (m->descriptor->intercept_image_api && m->manifest.id != overrideOwnerId) {
            orderedModules.push_back(m.get());
        }
    }

    for (const LoadedModule* m : orderedModules) {

        nlohmann::json provJson{
            {"base_url", provider.baseUrl},
            {"api_key", provider.apiKey},
            {"model", provider.model},
            {"quality", provider.quality},
            {"size", provider.size},
            {"image_field", provider.imageField},
            {"complexity", provider.complexity},
            {"output_format", provider.outputFormat},
            {"output_compression", provider.compression},
            {"stream", provider.stream},
            {"partial_images", provider.partialImages},
            {"extra", provider.extra},
            {"timeout_seconds", provider.timeoutSeconds}
        };
        std::string opStr(op);
        std::string provStr = provJson.dump();
        std::string reqStr = requestMeta.dump();

        std::vector<MemeStudioBytesV2> abiUploads;
        abiUploads.reserve(uploads.size());
        for (const auto& up : uploads) {
            abiUploads.push_back(MemeStudioBytesV2{
                const_cast<uint8_t*>(up.pngBytes.data()),
                up.pngBytes.size()
            });
        }

        auto previewThunk = [](void* userPtr, const uint8_t* pngData, size_t pngLen, int32_t idx) {
            const auto* fn = static_cast<const net::ImagePreviewFn*>(userPtr);
            if (fn && *fn && pngData && pngLen > 0) {
                try {
                    auto img = media::decodeImageBytes(std::span<const uint8_t>(pngData, pngLen));
                    (*fn)(img, idx);
                } catch (...) {}
            }
        };

        MemeStudioBytesV2 respBytes{};
        MemeStudioBytesV2 errBytes{};
        int32_t rc = m->descriptor->intercept_image_api(
            m->instance,
            opStr.c_str(),
            provStr.c_str(),
            reqStr.c_str(),
            abiUploads.empty() ? nullptr : abiUploads.data(),
            abiUploads.size(),
            reinterpret_cast<MemeStudioCancelTokenV2*>(&cancel),
            previewThunk,
            const_cast<net::ImagePreviewFn*>(&onPreview),
            &respBytes,
            &errBytes);
        std::string errMsg = takeAbiString(errBytes);

        // Return convention: 1 = pass-through (not intercepted by this plugin), 0 = handled, <0 = error
        if (rc == 1) {
            freeAbiBytes(respBytes);
            continue;
        }
        if (rc != 0) {
            freeAbiBytes(respBytes);
            if (cancel.isStopped()) throw CancelledError();
            throw Error("插件接口拦截执行失败（" + m->manifest.id + "）" + (errMsg.empty() ? "" : "：" + errMsg));
        }
        outResponseBytes.assign(respBytes.data, respBytes.data + respBytes.size);
        freeAbiBytes(respBytes);
        return true;
    }
    return false;
}

nlohmann::json PluginHost::invokeCommand(
    std::string_view pluginId,
    std::string_view commandId,
    const nlohmann::json& args,
    void* ownerHwnd,
    net::CancelToken& cancel) const {
    for (const auto& m : modules_) {
        if (m->manifest.id != pluginId) continue;
        bool declared = std::any_of(m->manifest.commands.begin(), m->manifest.commands.end(), [&](const auto& c) {
            return c.id == commandId;
        });
        if (!declared) {
            throw Error("插件未声明该命令：" + std::string(commandId));
        }
        if (!m->descriptor->invoke_command) {
            throw Error("插件未实现命令处理函数：" + std::string(pluginId));
        }

        std::string cmdStr(commandId);
        std::string argsStr = args.dump();
        MemeStudioBytesV2 outRes{};
        MemeStudioBytesV2 outErr{};
        int32_t rc = m->descriptor->invoke_command(
            m->instance,
            cmdStr.c_str(),
            argsStr.c_str(),
            ownerHwnd,
            reinterpret_cast<MemeStudioCancelTokenV2*>(&cancel),
            &outRes,
            &outErr);
        std::string errMsg = takeAbiString(outErr);
        if (rc != 0) {
            freeAbiBytes(outRes);
            if (cancel.isStopped()) throw CancelledError();
            throw Error(errMsg.empty() ? ("执行插件命令失败：" + cmdStr) : errMsg);
        }
        std::string resStr = takeAbiString(outRes);
        if (resStr.empty()) return nlohmann::json::object();
        return nlohmann::json::parse(resStr);
    }
    throw Error("插件未启用或未加载：" + std::string(pluginId));
}

} // namespace meme::core
