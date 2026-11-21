#include "net/openai_client.hpp"
#include "media/wic_codec.hpp"
#include "net/endpoint.hpp"
#include "net/redactor.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"
#include <algorithm>
#include <cctype>

namespace meme::net {

namespace {

bool isSafeFieldName(std::string_view name) noexcept {
    return !name.empty() && name.size() < 128 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '[' || c == ']' || c == '.';
           });
}

void appendAscii(base::Bytes& dst, std::string_view text) {
    dst.insert(dst.end(), text.begin(), text.end());
}

nlohmann::json parseAndCheckJsonResponse(const HttpResponse& resp, const ProviderConfig& provider) {
    std::vector<std::string> secrets;
    if (!provider.apiKey.empty()) secrets.push_back(provider.apiKey);

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(resp.body.begin(), resp.body.end());
    } catch (...) {
        std::string snippet(
            reinterpret_cast<const char*>(resp.body.data()),
            std::min<size_t>(resp.body.size(), 240));
        std::string cleanSnippet = redactAndTruncateUtf8(base::wideToUtf8(base::utf8ToWideLossy(snippet)), secrets, 120);
        std::string msg = "HTTP " + std::to_string(resp.status) + "：接口返回的不是有效 JSON";
        if (!cleanSnippet.empty()) {
            msg += "（响应摘要：" + cleanSnippet + "）";
        }
        if (resp.status >= 400) {
            throw ApiHttpError(resp.status, msg);
        }
        throw Error(msg);
    }

    if (resp.status < 200 || resp.status >= 300 || parsed.contains("error")) {
        nlohmann::json errNode = parsed.contains("error") ? parsed["error"] : parsed;
        nlohmann::json safeNode = redactJsonSecrets(errNode, secrets);
        std::string detail = redactAndTruncateUtf8(safeNode.dump(), secrets, 600);
        uint32_t effectiveStatus = (resp.status >= 200 && resp.status < 300) ? 502u : resp.status;
        throw ApiHttpError(effectiveStatus, "HTTP " + std::to_string(resp.status) + "：" + detail);
    }
    return parsed;
}

} // namespace

void validateProviderConfig(const ProviderConfig& provider, bool checkImageOptions) {
    (void)resolveOpenAiUrl(provider.baseUrl, EndpointKind::Models);
    validateApiKeyFormat(provider.apiKey);
    if (provider.timeoutSeconds < 5 || provider.timeoutSeconds > 3600) {
        throw Error("请求超时时间必须在 5 到 3600 秒之间");
    }
    if (!checkImageOptions) return;

    std::string model = base::trimAscii(provider.model);
    if (model.empty()) {
        throw Error("请填写模型名称");
    }
    if (model.size() > 512 || std::any_of(model.begin(), model.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
        throw Error("模型名称包含非法控制字符或超出长度上限");
    }
    if (!isSafeFieldName(provider.imageField)) {
        throw Error("图片上传字段名无效");
    }
    if (!provider.extra.is_object()) {
        throw Error("扩展参数必须是 JSON 对象");
    }
    if (!provider.complexity.empty() &&
        provider.complexity != "low" && provider.complexity != "medium" && provider.complexity != "high") {
        throw Error("推理档位必须为 low、medium、high 或留空");
    }
    if (!provider.outputFormat.empty() &&
        provider.outputFormat != "png" && provider.outputFormat != "jpeg" && provider.outputFormat != "webp") {
        throw Error("输出格式必须为 png、jpeg、webp 或留空");
    }
    if (provider.compression < -1 || provider.compression > 100) {
        throw Error("压缩率必须在 0 到 100 之间");
    }
    if (provider.compression >= 0 && provider.outputFormat != "jpeg" && provider.outputFormat != "webp") {
        throw Error("压缩率仅适用于 jpeg 或 webp 输出格式");
    }
    if (provider.stream && (provider.partialImages < 1 || provider.partialImages > 3)) {
        throw Error("流式局部预览数量必须在 1 到 3 之间");
    }

    for (const auto& [k, v] : provider.extra.items()) {
        (void)v;
        if (!isSafeFieldName(k)) {
            throw Error("扩展参数字段名无效：" + k);
        }
        if (k == "model" || k == "prompt" || k == "image" || k == "image[]" ||
            k == provider.imageField || k == "n" || k == "stream" ||
            k == "quality" || k == "size" || k == "complexity" ||
            k == "output_format" || k == "output_compression" || k == "partial_images") {
            throw Error("扩展参数不能覆盖内置标准字段：" + k);
        }
    }
}

nlohmann::json buildImageRequestFields(const ProviderConfig& provider, std::string_view promptUtf8) {
    if (base::trimAscii(promptUtf8).empty()) {
        throw Error("提示词不能为空");
    }
    validateProviderConfig(provider, true);
    nlohmann::json fields{
        {"model", base::trimAscii(provider.model)},
        {"prompt", std::string(promptUtf8)}
    };
    if (!provider.quality.empty()) fields["quality"] = provider.quality;
    if (!provider.size.empty()) fields["size"] = provider.size;
    if (!provider.complexity.empty()) fields["complexity"] = provider.complexity;
    if (!provider.outputFormat.empty()) fields["output_format"] = provider.outputFormat;
    if (provider.compression >= 0) fields["output_compression"] = provider.compression;
    if (provider.stream) {
        fields["stream"] = true;
        fields["partial_images"] = provider.partialImages;
    }
    for (const auto& [k, v] : provider.extra.items()) {
        if (!v.is_null()) {
            fields[k] = v;
        }
    }
    return fields;
}

MultipartPayload buildMultipartEditPayload(
    const ProviderConfig& provider,
    std::string_view promptUtf8,
    std::span<const UploadPart> images) {
    if (images.empty() || images.size() > 16) {
        throw Error("单次上传参考图片数量必须在 1 到 16 张之间");
    }
    nlohmann::json fields = buildImageRequestFields(provider, promptUtf8);
    std::string boundary = "MemeStudioBoundary" + base::generateRandomHex(16);

    MultipartPayload payload;
    payload.contentType = "multipart/form-data; boundary=" + boundary;

    for (const auto& [k, v] : fields.items()) {
        std::string valText = v.is_string() ? v.get<std::string>() : v.dump();
        appendAscii(payload.body, "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + k + "\"\r\n\r\n");
        appendAscii(payload.body, valText);
        appendAscii(payload.body, "\r\n");
    }

    for (size_t idx = 0; idx < images.size(); ++idx) {
        const auto& img = images[idx];
        if (img.pngBytes.empty() || img.pngBytes.size() > 50ull * 1024 * 1024) {
            throw Error("单张上传图片为空或超过 50 MB 上限");
        }
        appendAscii(
            payload.body,
            "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + provider.imageField +
            "\"; filename=\"image_" + std::to_string(idx) + ".png\"\r\nContent-Type: image/png\r\n\r\n");
        payload.body.insert(payload.body.end(), img.pngBytes.begin(), img.pngBytes.end());
        appendAscii(payload.body, "\r\n");
    }
    appendAscii(payload.body, "--" + boundary + "--\r\n");

    if (payload.body.size() > 180ull * 1024 * 1024) {
        throw Error("单次上传总负载超过 180 MB 上限");
    }
    return payload;
}

OpenAiImageClient::OpenAiImageClient(WinHttpPool& pool, ApiTransportInterceptor interceptor)
    : httpPool_(pool), interceptor_(std::move(interceptor)) {}

std::vector<std::string> OpenAiImageClient::listModels(const ProviderConfig& provider, CancelToken& cancel) {
    if (interceptor_) {
        base::Bytes interceptedResp;
        if (interceptor_("models", provider, nlohmann::json::object(), {}, cancel, {}, interceptedResp)) {
            auto j = nlohmann::json::parse(interceptedResp.begin(), interceptedResp.end());
            return j.get<std::vector<std::string>>();
        }
    }

    validateProviderConfig(provider, false);
    HttpRequestSpec req;
    req.url = resolveOpenAiUrl(provider.baseUrl, EndpointKind::Models);
    req.method = "GET";
    req.headers = {{"Authorization", "Bearer " + provider.apiKey}};
    req.timeoutSeconds = std::clamp(provider.timeoutSeconds, 5, 30);

    HttpResponse resp = httpPool_.send(req, cancel);
    nlohmann::json parsed = parseAndCheckJsonResponse(resp, provider);
    if (!parsed.contains("data") || !parsed["data"].is_array()) {
        throw Error("接口响应缺少 data 模型列表数组");
    }

    std::vector<std::string> models;
    for (const auto& item : parsed["data"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) continue;
        std::string id = base::trimAscii(item["id"].get<std::string>());
        if (!id.empty() && id.size() <= 512 &&
            std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
            models.push_back(std::move(id));
        }
    }
    std::sort(models.begin(), models.end());
    models.erase(std::unique(models.begin(), models.end()), models.end());
    if (models.empty()) {
        throw Error("接口未返回可用模型 ID，仍可手动输入模型名称");
    }
    return models;
}

namespace {

media::Image executeImageHttpCall(
    WinHttpPool& pool,
    const ProviderConfig& provider,
    const std::string& url,
    const std::string& contentType,
    std::span<const uint8_t> body,
    CancelToken& cancel,
    const ImagePreviewFn& onPreview) {
    HttpRequestSpec req;
    req.url = url;
    req.method = "POST";
    req.headers = {
        {"Authorization", "Bearer " + provider.apiKey},
        {"Content-Type", contentType}
    };
    if (provider.stream) {
        req.headers.emplace_back("Accept", "text/event-stream");
    }
    req.body = body;
    req.timeoutSeconds = provider.timeoutSeconds;
    req.expectEventStream = provider.stream;

    media::Image streamFinalImage;
    HttpResponse resp = pool.send(req, cancel, [&](const SseEvent& ev) {
        nlohmann::json val;
        try {
            val = nlohmann::json::parse(ev.data);
        } catch (...) {
            throw Error("流式 SSE 事件包含无效的 JSON 数据");
        }
        std::string type = val.value("type", ev.eventType);
        if (val.contains("error") || type == "error") {
            nlohmann::json errPayload{{"error", val.contains("error") ? val["error"] : val}};
            std::string dumped = errPayload.dump();
            HttpResponse fakeErrResp{502, false, base::Bytes(dumped.begin(), dumped.end())};
            (void)parseAndCheckJsonResponse(fakeErrResp, provider);
        }
        if (type == "image_edit.partial_image" || type == "image_generation.partial_image") {
            if (streamFinalImage.empty() && onPreview && val.contains("b64_json") && val["b64_json"].is_string()) {
                auto decoded = media::decodeImageBytes(base::base64Decode(val["b64_json"].get<std::string>()));
                onPreview(decoded, val.value("partial_image_index", 0));
            }
        } else if (type == "image_edit.completed" || type == "image_generation.completed") {
            if (!streamFinalImage.empty()) {
                throw Error("流式传输收到重复的完成图像帧");
            }
            if (!val.contains("b64_json") || !val["b64_json"].is_string()) {
                throw Error("流式完成事件缺少 b64_json 图像数据");
            }
            streamFinalImage = media::decodeImageBytes(base::base64Decode(val["b64_json"].get<std::string>()));
        }
    });

    if (resp.status < 200 || resp.status >= 300) {
        (void)parseAndCheckJsonResponse(resp, provider);
    }
    if (resp.streamed) {
        if (streamFinalImage.empty()) {
            throw ApiConnectionError("流式传输中断且未收到最终完成图像（局部预览不会保存为成品）");
        }
        return streamFinalImage;
    }

    nlohmann::json parsed = parseAndCheckJsonResponse(resp, provider);
    if (!parsed.contains("data") || !parsed["data"].is_array() || parsed["data"].empty() || !parsed["data"][0].is_object()) {
        throw Error("接口响应缺少 data[0] 图像条目");
    }
    const auto& item = parsed["data"][0];
    if (item.contains("b64_json") && item["b64_json"].is_string()) {
        return media::decodeImageBytes(base::base64Decode(item["b64_json"].get<std::string>()));
    }
    if (item.contains("url") && item["url"].is_string()) {
        std::string imgUrl = item["url"].get<std::string>();
        if (imgUrl.starts_with("data:")) {
            return media::decodeImageBytes(base::base64Decode(imgUrl));
        }
        HttpRequestSpec dlSpec;
        dlSpec.url = imgUrl;
        dlSpec.method = "GET";
        dlSpec.timeoutSeconds = provider.timeoutSeconds;
        HttpResponse dlResp = pool.send(dlSpec, cancel);
        if (dlResp.status != 200) {
            throw ApiHttpError(dlResp.status, "下载生成的远程图片失败（HTTP " + std::to_string(dlResp.status) + "）");
        }
        return media::decodeImageBytes(dlResp.body);
    }
    throw Error("接口响应既不包含 b64_json 也不包含图像下载 url");
}

} // namespace

media::Image OpenAiImageClient::editImage(
    const ProviderConfig& provider,
    std::string_view promptUtf8,
    std::span<const UploadPart> images,
    CancelToken& cancel,
    const ImagePreviewFn& onPreview) {
    if (interceptor_) {
        base::Bytes interceptedPng;
        nlohmann::json meta{{"prompt", std::string(promptUtf8)}};
        if (interceptor_("edit", provider, meta, images, cancel, onPreview, interceptedPng)) {
            return media::decodeImageBytes(interceptedPng);
        }
    }
    MultipartPayload mp = buildMultipartEditPayload(provider, promptUtf8, images);
    std::string url = resolveOpenAiUrl(provider.baseUrl, EndpointKind::ImageEdits);
    return executeImageHttpCall(httpPool_, provider, url, mp.contentType, mp.body, cancel, onPreview);
}

media::Image OpenAiImageClient::generateImage(
    const ProviderConfig& provider,
    std::string_view promptUtf8,
    std::span<const UploadPart> styleImages,
    CancelToken& cancel,
    const ImagePreviewFn& onPreview) {
    if (!styleImages.empty()) {
        return editImage(provider, promptUtf8, styleImages, cancel, onPreview);
    }
    if (interceptor_) {
        base::Bytes interceptedPng;
        nlohmann::json meta{{"prompt", std::string(promptUtf8)}};
        if (interceptor_("generate", provider, meta, {}, cancel, onPreview, interceptedPng)) {
            return media::decodeImageBytes(interceptedPng);
        }
    }
    std::string jsonBody = buildImageRequestFields(provider, promptUtf8).dump();
    std::string url = resolveOpenAiUrl(provider.baseUrl, EndpointKind::ImageGenerations);
    return executeImageHttpCall(
        httpPool_,
        provider,
        url,
        "application/json",
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(jsonBody.data()), jsonBody.size()),
        cancel,
        onPreview);
}

} // namespace meme::net
