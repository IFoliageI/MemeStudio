#pragma once
#include "media/image_types.hpp"
#include "net/winhttp_pool.hpp"
#include <functional>
#include <span>
#include <string>
#include <vector>
#include "../../vendor/nlohmann/json.hpp"

namespace meme::net {

struct ProviderConfig {
    std::string baseUrl = "https://api.openai.com/v1";
    std::string apiKey;
    std::string model = "gpt-image-1";
    std::string quality = "auto";
    std::string size = "auto";
    std::string imageField = "image[]";
    std::string complexity;
    std::string outputFormat;
    int compression = -1;
    bool stream = false;
    int partialImages = 2;
    nlohmann::json extra = nlohmann::json::object();
    int timeoutSeconds = 600;
};

struct UploadPart {
    std::string role;
    base::Bytes pngBytes;
};

struct MultipartPayload {
    std::string contentType;
    base::Bytes body;
};

using ImagePreviewFn = std::function<void(const media::Image& preview, int partialIndex)>;

// Generic plugin transport interceptor signature (decouples Host completely from routing/local plugins).
using ApiTransportInterceptor = std::function<bool(
    std::string_view op,
    const ProviderConfig& provider,
    const nlohmann::json& requestMeta,
    std::span<const UploadPart> uploads,
    CancelToken& cancel,
    const ImagePreviewFn& onPreview,
    base::Bytes& outResponseBytes)>;

void validateProviderConfig(const ProviderConfig& provider, bool checkImageOptions = true);
nlohmann::json buildImageRequestFields(const ProviderConfig& provider, std::string_view promptUtf8);
MultipartPayload buildMultipartEditPayload(
    const ProviderConfig& provider,
    std::string_view promptUtf8,
    std::span<const UploadPart> images);

class OpenAiImageClient {
    WinHttpPool& httpPool_;
    ApiTransportInterceptor interceptor_;
public:
    explicit OpenAiImageClient(WinHttpPool& pool, ApiTransportInterceptor interceptor = {});

    void setInterceptor(ApiTransportInterceptor interceptor) { interceptor_ = std::move(interceptor); }
    WinHttpPool& pool() noexcept { return httpPool_; }

    std::vector<std::string> listModels(const ProviderConfig& provider, CancelToken& cancel);

    media::Image editImage(
        const ProviderConfig& provider,
        std::string_view promptUtf8,
        std::span<const UploadPart> images,
        CancelToken& cancel,
        const ImagePreviewFn& onPreview = {});

    media::Image generateImage(
        const ProviderConfig& provider,
        std::string_view promptUtf8,
        std::span<const UploadPart> styleImages,
        CancelToken& cancel,
        const ImagePreviewFn& onPreview = {});
};

} // namespace meme::net
