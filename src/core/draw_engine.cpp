#include "core/draw_engine.hpp"
#include "core/prompt_builder.hpp"
#include "media/image_ops.hpp"
#include "media/wic_codec.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"
#include <algorithm>

namespace meme::core {

std::vector<fs::path> runPromptDrawJob(
    const PromptDrawTaskConfig& job,
    net::OpenAiImageClient& apiClient,
    const PluginHost* plugins,
    net::CancelToken& cancel,
    const ReplaceProgressFn& onProgress) {
    std::string cleanPrompt = base::trimAscii(job.prompt);
    if (cleanPrompt.empty()) {
        throw Error("请输入生图提示词描述");
    }
    if (job.outputPath.empty()) {
        throw Error("请指定生成图片的保存路径");
    }
    if (job.stylePaths.size() > 8) {
        throw Error("参考画风图片最多支持 8 张");
    }
    int copyCount = std::clamp(job.copyCount, 1, 10);
    auto outPaths = resolveCopyOutputPaths(job.outputPath, copyCount);

    for (const auto& dest : outPaths) {
        for (const auto& st : job.stylePaths) {
            if (base::isSameFilePath(dest, st)) {
                throw Error("保存路径不能覆盖画风参考图：" + base::wideToUtf8(dest.wstring()));
            }
        }
    }

    // Encode optional style reference images once outside the copy loop
    std::vector<net::UploadPart> styleUploads;
    if (!job.stylePaths.empty()) {
        std::vector<media::Frame> styleFrames;
        styleFrames.reserve(job.stylePaths.size());
        for (const auto& sp : job.stylePaths) {
            cancel.verify();
            styleFrames.push_back(media::Frame{media::limitImageEdge(media::decodeFirstFrame(sp), 2048), 100});
        }
        if (plugins) {
            plugins->processFramesBatch("input", nlohmann::json{{"role", "draw_styles"}}, styleFrames, cancel);
        }
        for (size_t i = 0; i < styleFrames.size(); ++i) {
            styleUploads.push_back(net::UploadPart{
                "style_" + std::to_string(i + 1) + ".png",
                media::encodePng(styleFrames[i].image)
            });
        }
    }

    std::string prompt = buildPromptDrawPrompt(cleanPrompt, styleUploads.size());
    if (plugins) {
        prompt = plugins->transformPrompt(nlohmann::json{{"workflow", "draw"}}, prompt);
    }

    for (int copyIdx = 0; copyIdx < copyCount; ++copyIdx) {
        cancel.verify();
        if (onProgress) {
            ReplaceProgress prog;
            prog.completedFrames = copyIdx;
            prog.totalFrames = copyCount;
            prog.currentCopyIndex = copyIdx;
            prog.totalCopies = copyCount;
            prog.messageUtf8 = copyCount > 1
                ? ("正在绘制第 " + std::to_string(copyIdx + 1) + " / " + std::to_string(copyCount) + " 张图片…")
                : "正在根据提示词绘制图片…";
            onProgress(prog);
        }

        auto previewCb = [&](const media::Image& previewImg, int partialIdx) {
            if (!onProgress) return;
            ReplaceProgress prog;
            prog.completedFrames = copyIdx;
            prog.totalFrames = copyCount;
            prog.currentCopyIndex = copyIdx;
            prog.totalCopies = copyCount;
            prog.partialPreviewIndex = partialIdx;
            prog.previewImage = std::make_shared<media::Image>(previewImg);
            prog.messageUtf8 = "正在接收实时绘制草图…";
            onProgress(prog);
        };

        media::Image generated = apiClient.generateImage(job.provider, prompt, styleUploads, cancel, previewCb);
        std::vector<media::Frame> outFrame{media::Frame{std::move(generated), 100}};
        if (plugins) {
            plugins->processFramesBatch("output", nlohmann::json{{"workflow", "draw"}}, outFrame, cancel);
        }
        media::savePngFile(outPaths[static_cast<size_t>(copyIdx)], outFrame[0].image);
    }
    return outPaths;
}

} // namespace meme::core
