#include "core/retouch_engine.hpp"
#include "media/image_ops.hpp"
#include "media/wic_codec.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"
#include <algorithm>
#include <cmath>

namespace meme::core {

namespace {

media::Animation sampleRetouchFrames(const media::Animation& src, int stride, int maxFrames) {
    if (!src.animated || src.frames.size() <= 1 || (stride <= 1 && maxFrames <= 0)) {
        return src;
    }
    stride = std::max(1, stride);
    media::Animation sampled;
    sampled.animated = src.animated;
    sampled.loopCount = src.loopCount;

    for (size_t i = 0; i < src.frames.size(); i += static_cast<size_t>(stride)) {
        if (maxFrames > 0 && static_cast<int>(sampled.frames.size()) >= maxFrames) break;
        uint32_t accDelay = 0;
        for (size_t j = i; j < std::min(src.frames.size(), i + static_cast<size_t>(stride)); ++j) {
            accDelay += src.frames[j].delayMs;
        }
        sampled.frames.push_back(media::Frame{src.frames[i].image, std::max<uint32_t>(20u, accDelay)});
    }
    sampled.animated = sampled.frames.size() > 1;
    return sampled;
}

std::vector<int> pickCompletedNeighbors(int targetIdx, const std::vector<bool>& done) {
    int prev = -1, next = -1;
    for (int i = targetIdx - 1; i >= 0; --i) {
        if (done[static_cast<size_t>(i)]) { prev = i; break; }
    }
    for (size_t i = static_cast<size_t>(targetIdx) + 1; i < done.size(); ++i) {
        if (done[i]) { next = static_cast<int>(i); break; }
    }
    std::vector<int> out;
    if (prev >= 0) out.push_back(prev);
    if (next >= 0 && next != prev) out.push_back(next);
    return out;
}

} // namespace

std::vector<fs::path> runImageRetouchJob(
    const RetouchTaskConfig& job,
    net::OpenAiImageClient& apiClient,
    const PluginHost* plugins,
    net::CancelToken& cancel,
    const ReplaceProgressFn& onProgress) {
    if (job.sourcePath.empty()) {
        throw Error("请选择需要修图的原始图片或 GIF");
    }
    std::string cleanInstruction = base::trimAscii(job.instruction);
    if (cleanInstruction.empty()) {
        throw Error("请输入修图提示词（如：加上一副墨镜、修复手部手指、把背景换成纯白贴纸等）");
    }
    if (job.outputPath.empty()) {
        throw Error("请指定修图结果的保存路径");
    }
    if (job.stylePaths.size() > 8) {
        throw Error("附加画风参考图最多支持 8 张");
    }
    if (job.mode == RetouchMode::RegionRedraw) {
        job.region.validate("局部精修选区");
    }

    int copyCount = std::clamp(job.copyCount, 1, 10);
    auto outPaths = resolveCopyOutputPaths(job.outputPath, copyCount);
    for (const auto& dest : outPaths) {
        if (base::isSameFilePath(dest, job.sourcePath)) {
            throw Error("修图保存路径不能直接覆盖原图，请另存为新文件名");
        }
        for (const auto& st : job.stylePaths) {
            if (base::isSameFilePath(dest, st)) {
                throw Error("修图保存路径不能覆盖参考图");
            }
        }
    }

    media::Animation sourceAnim = sampleRetouchFrames(
        media::decodeAnimation(job.sourcePath), job.frameStride, job.maxFrames);
    if (sourceAnim.frames.empty()) {
        throw Error("无法读取待修图素材像素帧");
    }

    std::vector<net::UploadPart> styleUploads;
    if (!job.stylePaths.empty()) {
        std::vector<media::Frame> styleFrames;
        styleFrames.reserve(job.stylePaths.size());
        for (const auto& sp : job.stylePaths) {
            cancel.verify();
            styleFrames.push_back(media::Frame{media::limitImageEdge(media::decodeFirstFrame(sp), 2048), 100});
        }
        if (plugins) {
            plugins->processFramesBatch("input", nlohmann::json{{"role", "retouch_styles"}}, styleFrames, cancel);
        }
        for (size_t i = 0; i < styleFrames.size(); ++i) {
            styleUploads.push_back(net::UploadPart{
                "style_" + std::to_string(i + 1) + ".png",
                media::encodePng(styleFrames[i].image)
            });
        }
    }

    const int frameCount = static_cast<int>(sourceAnim.frames.size());
    const int grandTotalFrames = frameCount * copyCount;
    const auto schedule = computeMidpointFrameSchedule(frameCount, sourceAnim.animated && job.temporalConsistency);
    int completedGlobal = 0;

    for (int copyIdx = 0; copyIdx < copyCount; ++copyIdx) {
        std::vector<media::Frame> outFrames(static_cast<size_t>(frameCount));
        std::vector<bool> doneFlags(static_cast<size_t>(frameCount), false);

        for (int frameIdx : schedule) {
            cancel.verify();
            const auto& origFrame = sourceAnim.frames[static_cast<size_t>(frameIdx)];

            if (onProgress) {
                ReplaceProgress prog;
                prog.completedFrames = completedGlobal;
                prog.totalFrames = grandTotalFrames;
                prog.currentCopyIndex = copyIdx;
                prog.totalCopies = copyCount;
                prog.activeFrameIndex = frameIdx;
                prog.messageUtf8 = sourceAnim.animated
                    ? ("正在修图第 " + std::to_string(frameIdx + 1) + " / " + std::to_string(frameCount) + " 帧…")
                    : (job.mode == RetouchMode::RegionRedraw ? "正在执行局部区域精修…" : "正在按提示词执行智能修图…");
                onProgress(prog);
            }

            std::vector<net::UploadPart> uploads;
            uploads.push_back(net::UploadPart{
                "source_frame.png",
                media::encodePng(media::limitImageEdge(origFrame.image, 2048))
            });
            uploads.insert(uploads.end(), styleUploads.begin(), styleUploads.end());

            std::vector<int> neighbors;
            bool includeFirstAnchor = false;
            if (sourceAnim.animated && job.temporalConsistency) {
                neighbors = pickCompletedNeighbors(frameIdx, doneFlags);
                for (int nb : neighbors) {
                    uploads.push_back(net::UploadPart{
                        "retouched_neighbor_" + std::to_string(nb + 1) + ".png",
                        media::encodePng(media::limitImageEdge(outFrames[static_cast<size_t>(nb)].image, 1024))
                    });
                }
                if (frameIdx != 0 && doneFlags[0] &&
                    std::find(neighbors.begin(), neighbors.end(), 0) == neighbors.end()) {
                    includeFirstAnchor = true;
                    uploads.push_back(net::UploadPart{
                        "retouched_anchor_1.png",
                        media::encodePng(media::limitImageEdge(outFrames[0].image, 1024))
                    });
                }
            }

            RetouchPromptSpec pspec;
            pspec.mode = job.mode;
            pspec.region = job.region;
            pspec.styleCount = styleUploads.size();
            pspec.preserveText = job.preserveText;
            pspec.animated = sourceAnim.animated;
            pspec.frameIndex = frameIdx;
            pspec.totalFrames = frameCount;
            pspec.hasFirstFrameAnchor = includeFirstAnchor;
            pspec.temporalNeighbors = neighbors;
            pspec.userInstruction = cleanInstruction;

            std::string prompt = buildImageRetouchPrompt(pspec);
            if (plugins) {
                prompt = plugins->transformPrompt(
                    nlohmann::json{{"workflow", "retouch"}, {"frame_index", frameIdx}}, prompt);
            }

            auto previewCb = [&](const media::Image& previewImg, int partialIdx) {
                if (!onProgress) return;
                ReplaceProgress prog;
                prog.completedFrames = completedGlobal;
                prog.totalFrames = grandTotalFrames;
                prog.currentCopyIndex = copyIdx;
                prog.totalCopies = copyCount;
                prog.activeFrameIndex = frameIdx;
                prog.partialPreviewIndex = partialIdx;
                prog.previewImage = std::make_shared<media::Image>(previewImg);
                prog.messageUtf8 = "正在接收实时修图预览…";
                onProgress(prog);
            };

            media::Image editedImg = apiClient.editImage(job.provider, prompt, uploads, cancel, previewCb);
            if (editedImg.width != origFrame.image.width || editedImg.height != origFrame.image.height) {
                editedImg = media::resizeImage(editedImg, origFrame.image.width, origFrame.image.height);
            }

            if (job.mode == RetouchMode::RegionRedraw && !job.region.isFullImage()) {
                editedImg = media::preserveRegionOutside(origFrame.image, editedImg, job.region);
            }

            outFrames[static_cast<size_t>(frameIdx)] = media::Frame{std::move(editedImg), origFrame.delayMs};
            doneFlags[static_cast<size_t>(frameIdx)] = true;
            ++completedGlobal;
        }

        if (plugins) {
            plugins->processFramesBatch("output", nlohmann::json{{"workflow", "retouch"}}, outFrames, cancel);
        }

        const auto& targetOut = outPaths[static_cast<size_t>(copyIdx)];
        if (sourceAnim.animated) {
            media::saveGifFile(targetOut, outFrames, sourceAnim.loopCount);
        } else {
            media::savePngFile(targetOut, outFrames[0].image);
        }
    }

    return outPaths;
}

} // namespace meme::core
