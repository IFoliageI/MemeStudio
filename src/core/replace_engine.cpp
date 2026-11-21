#include "core/replace_engine.hpp"
#include "core/prompt_builder.hpp"
#include "media/image_ops.hpp"
#include "media/wic_codec.hpp"
#include "net/endpoint.hpp"
#include "base/app_paths.hpp"
#include "base/crypto.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/json_io.hpp"
#include "base/utf8.hpp"
#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace meme::core {

std::vector<fs::path> resolveCopyOutputPaths(const fs::path& baseOutput, int copyCount) {
    if (copyCount < 1 || copyCount > 10) {
        throw Error("生成份数必须在 1 到 10 之间");
    }
    if (baseOutput.empty() || !baseOutput.has_filename()) {
        throw Error("请指定有效的输出文件路径");
    }
    if (copyCount == 1) {
        return {baseOutput};
    }
    std::vector<fs::path> out;
    out.reserve(static_cast<size_t>(copyCount));
    for (int i = 0; i < copyCount; ++i) {
        std::wostringstream stemName;
        stemName << baseOutput.stem().wstring() << L"_"
                 << std::setw(3) << std::setfill(L'0') << (i + 1)
                 << baseOutput.extension().wstring();
        out.push_back(baseOutput.parent_path() / stemName.str());
    }
    return out;
}

namespace {

void runSingleReplaceCopy(
    const ReplaceTaskConfig& job,
    const fs::path& destOutput,
    int copyIndex,
    const base::Bytes& sourceRawBytes,
    const std::string& sourceDigest,
    const media::Animation& sourceAnim,
    const std::vector<net::UploadPart>& staticRefs,
    const std::string& identityDigest,
    const std::vector<std::string>& styleDigests,
    net::OpenAiImageClient& apiClient,
    const PluginHost* plugins,
    net::CancelToken& cancel,
    const ReplaceProgressFn& report) {
    cancel.verify();

    int srcFrameCount = static_cast<int>(sourceAnim.frames.size());
    int stride = job.frameStride;
    if (job.maxFrames > 0) {
        stride = std::max(stride, (srcFrameCount + job.maxFrames - 1) / job.maxFrames);
    }
    int totalOutFrames = (srcFrameCount + stride - 1) / stride;

    std::wstring ext = destOutput.extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    if (sourceAnim.animated && ext != L".gif") {
        throw Error("动画素材的输出文件扩展名必须为 .gif");
    }
    if (!sourceAnim.animated && ext != L".png") {
        throw Error("静态图片素材的输出文件扩展名必须为 .png");
    }

    // Build deterministic manifest for resume cache inside %LOCALAPPDATA%\MemeStudio\cache\jobs\<hash>
    // (Zero .memestudio companion folders next to user output files!)
    nlohmann::json manifest{
        {"schema", 4},
        {"source_sha256", sourceDigest},
        {"identity_sha256", identityDigest},
        {"style_sha256", styleDigests},
        {"endpoint", net::resolveOpenAiUrl(job.provider.baseUrl, net::EndpointKind::ImageEdits)},
        {"model", base::trimAscii(job.provider.model)},
        {"quality", job.provider.quality},
        {"size", job.provider.size},
        {"complexity", job.provider.complexity},
        {"output_format", job.provider.outputFormat},
        {"output_compression", job.provider.compression},
        {"image_field", job.provider.imageField},
        {"extra", job.provider.extra},
        {"stride", stride},
        {"total_frames", totalOutFrames},
        {"preserve_text", job.preserveText},
        {"consistency", job.temporalConsistency},
        {"instruction", job.instruction},
        {"copy_index", copyIndex}
    };
    if (plugins) {
        auto fp = plugins->pipelineFingerprint();
        if (!fp.empty()) manifest["plugins"] = std::move(fp);
    }
    std::string signature = base::sha256Hex(manifest.dump());
    fs::path cacheDir = base::jobCacheDir(signature);
    if (job.allowResume) {
        fs::create_directories(cacheDir);
        base::writeJsonFile(cacheDir / L"manifest.json", manifest);
    }

    // Pre-process all target source frames through optional input batch filter plugins ONCE
    std::vector<media::Frame> preparedInputs;
    preparedInputs.reserve(static_cast<size_t>(totalOutFrames));
    for (int fIdx = 0; fIdx < totalOutFrames; ++fIdx) {
        const auto& orig = sourceAnim.frames[static_cast<size_t>(fIdx * stride)].image;
        preparedInputs.push_back(media::Frame{media::limitImageEdge(orig, 2048), 100});
    }
    if (plugins) {
        nlohmann::json inputCtx{
            {"role", "source_frames"},
            {"source", base::wideToUtf8(job.sourcePath.wstring())},
            {"animated", sourceAnim.animated}
        };
        plugins->processFramesBatch("input", inputCtx, preparedInputs, cancel);
    }

    std::vector<int> schedule = computeMidpointFrameSchedule(
        totalOutFrames, sourceAnim.animated && job.temporalConsistency);
    if (sourceAnim.animated && job.temporalConsistency && totalOutFrames > 2 && report) {
        report(ReplaceProgress{0, totalOutFrames, copyIndex, job.copyCount, -1, -1,
                               "多帧连贯模式：优先生成首尾与关键帧，再插值中间帧", nullptr});
    }

    std::vector<media::Frame> generated(static_cast<size_t>(totalOutFrames));
    base::Bytes anchorPng;
    int completedCount = 0;

    for (int frameIdx : schedule) {
        cancel.verify();
        int srcIdx = frameIdx * stride;
        const auto& origFrameImg = sourceAnim.frames[static_cast<size_t>(srcIdx)].image;

        std::wostringstream frameFile;
        frameFile << L"frame_" << std::setw(5) << std::setfill(L'0') << frameIdx << L".png";
        fs::path cachedFramePath = cacheDir / frameFile.str();

        media::Image frameResult;
        if (job.allowResume) {
            std::error_code ec;
            if (fs::exists(cachedFramePath, ec)) {
                try {
                    media::Image cached = media::decodeFirstFrame(cachedFramePath);
                    if (cached.width == origFrameImg.width && cached.height == origFrameImg.height) {
                        frameResult = std::move(cached);
                    }
                } catch (...) {
                    frameResult = {};
                }
            }
        }

        if (frameResult.empty()) {
            std::vector<net::UploadPart> uploads;
            uploads.push_back(net::UploadPart{"source", media::encodePng(preparedInputs[static_cast<size_t>(frameIdx)].image)});
            uploads.insert(uploads.end(), staticRefs.begin(), staticRefs.end());

            bool useAnchor = sourceAnim.animated && job.temporalConsistency && !anchorPng.empty() && frameIdx != 0;
            std::vector<int> neighbors;
            if (sourceAnim.animated && job.temporalConsistency) {
                int before = 0;
                for (int i = frameIdx - 1; i >= 0 && before < 2; --i) {
                    if (!generated[static_cast<size_t>(i)].image.empty()) {
                        neighbors.push_back(i);
                        ++before;
                    }
                }
                std::reverse(neighbors.begin(), neighbors.end());
                int after = 0;
                for (int i = frameIdx + 1; i < totalOutFrames && after < 2; ++i) {
                    if (!generated[static_cast<size_t>(i)].image.empty()) {
                        neighbors.push_back(i);
                        ++after;
                    }
                }
                if (useAnchor) {
                    std::erase(neighbors, 0);
                }
                for (int nbIdx : neighbors) {
                    uploads.push_back(net::UploadPart{
                        "temporal",
                        media::encodePng(media::limitImageEdge(generated[static_cast<size_t>(nbIdx)].image, 1024))
                    });
                }
            }
            if (useAnchor) {
                uploads.push_back(net::UploadPart{"anchor", anchorPng});
            }

            ReplacePromptSpec pSpec;
            pSpec.styleCount = job.stylePaths.size();
            pSpec.preserveText = job.preserveText;
            pSpec.animated = sourceAnim.animated;
            pSpec.frameIndex = frameIdx;
            pSpec.totalFrames = totalOutFrames;
            pSpec.hasFirstFrameAnchor = useAnchor;
            pSpec.temporalNeighbors = neighbors;
            pSpec.userInstruction = job.instruction;
            std::string promptText = buildCharacterReplacePrompt(pSpec);

            nlohmann::json hookCtx{
                {"mode", "replace"},
                {"source", base::wideToUtf8(job.sourcePath.wstring())},
                {"character", base::wideToUtf8(job.characterPath.wstring())},
                {"output", base::wideToUtf8(destOutput.wstring())},
                {"frame_index", frameIdx},
                {"frame_count", totalOutFrames},
                {"copy_index", copyIndex},
                {"instruction", job.instruction}
            };
            if (plugins) {
                promptText = plugins->transformPrompt(hookCtx, std::move(promptText));
            }

            if (report) {
                std::string statusMsg = sourceAnim.animated
                    ? ("正在生成第 " + std::to_string(frameIdx + 1) + " / " + std::to_string(totalOutFrames) + " 帧…")
                    : "正在发送图像生成请求…";
                report(ReplaceProgress{completedCount, totalOutFrames, copyIndex, job.copyCount, frameIdx, -1, statusMsg, nullptr});
            }

            frameResult = apiClient.editImage(
                job.provider,
                promptText,
                uploads,
                cancel,
                [&](const media::Image& partialImg, int partialIdx) {
                    cancel.verify();
                    if (report) {
                        report(ReplaceProgress{
                            completedCount,
                            totalOutFrames,
                            copyIndex,
                            job.copyCount,
                            frameIdx,
                            partialIdx,
                            "收到流式局部预览 " + std::to_string(partialIdx + 1),
                            std::make_shared<media::Image>(media::limitImageEdge(partialImg, 1024))
                        });
                    }
                });

            cancel.verify();
            frameResult = media::resizeImage(frameResult, origFrameImg.width, origFrameImg.height);
            if (job.allowResume) {
                media::savePngFile(cachedFramePath, frameResult);
            }
        } else if (report) {
            report(ReplaceProgress{
                completedCount, totalOutFrames, copyIndex, job.copyCount, frameIdx, -1,
                "已恢复断点缓存帧 " + std::to_string(frameIdx + 1), nullptr
            });
        }

        if (frameIdx == 0 && sourceAnim.animated && job.temporalConsistency) {
            anchorPng = media::encodePng(media::limitImageEdge(frameResult, 1024));
        }

        uint64_t mergedDelay = 0;
        for (int s = srcIdx; s < std::min(srcIdx + stride, srcFrameCount); ++s) {
            mergedDelay += sourceAnim.frames[static_cast<size_t>(s)].delayMs;
        }
        if (mergedDelay > std::numeric_limits<uint32_t>::max()) {
            throw Error("动画帧延迟时长溢出");
        }
        generated[static_cast<size_t>(frameIdx)] = media::Frame{std::move(frameResult), static_cast<uint32_t>(mergedDelay)};
        ++completedCount;

        if (report) {
            report(ReplaceProgress{
                completedCount,
                totalOutFrames,
                copyIndex,
                job.copyCount,
                frameIdx,
                -1,
                sourceAnim.animated
                    ? ("已完成 " + std::to_string(completedCount) + " / " + std::to_string(totalOutFrames) + " 帧")
                    : "图像生成完成",
                std::make_shared<media::Image>(media::limitImageEdge(generated[static_cast<size_t>(frameIdx)].image, 1024))
            });
        }
    }

    cancel.verify();
    // Batch post-process ALL generated frames in ONE call (enables single-process Waifu2x batch upscale!)
    if (plugins) {
        nlohmann::json outCtx{
            {"mode", "replace"},
            {"output", base::wideToUtf8(destOutput.wstring())},
            {"animated", sourceAnim.animated}
        };
        plugins->processFramesBatch("output", outCtx, generated, cancel);
    }

    cancel.verify();
    if (report) {
        report(ReplaceProgress{totalOutFrames, totalOutFrames, copyIndex, job.copyCount, -1, -1, "正在写入输出文件…", nullptr});
    }
    if (sourceAnim.animated) {
        media::saveGifFile(destOutput, generated, sourceAnim.loopCount);
    } else {
        media::savePngFile(destOutput, generated.front().image);
    }

    // Clean up resume cache directory automatically upon successful completion!
    if (job.allowResume) {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }
    (void)sourceRawBytes;
}

} // namespace

std::vector<fs::path> runCharacterReplaceJob(
    const ReplaceTaskConfig& job,
    net::OpenAiImageClient& apiClient,
    const PluginHost* plugins,
    net::CancelToken& cancel,
    const ReplaceProgressFn& onProgress) {
    cancel.verify();
    if (job.frameStride < 1 || job.frameStride > 600 || job.maxFrames < 0 || job.maxFrames > 600) {
        throw Error("无效的 GIF 抽帧参数");
    }
    if (job.stylePaths.size() > 8) {
        throw Error("最多支持 8 张附加画风参考图");
    }
    if (job.sourcePath.empty() || !fs::is_regular_file(job.sourcePath)) {
        throw Error("源表情图片文件不存在");
    }
    if (job.characterPath.empty() || !fs::is_regular_file(job.characterPath)) {
        throw Error("人物参考图文件不存在");
    }
    job.characterCrop.validate("人物参考图裁剪框");

    std::vector<fs::path> outputPaths = resolveCopyOutputPaths(job.outputPath, job.copyCount);
    std::vector<fs::path> allInputs{job.sourcePath, job.characterPath};
    for (const auto& sp : job.stylePaths) {
        if (sp.empty() || !fs::is_regular_file(sp)) {
            throw Error("附加画风参考图不存在：" + base::wideToUtf8(sp.wstring()));
        }
        allInputs.push_back(sp);
    }

    for (const auto& outPath : outputPaths) {
        for (const auto& inPath : allInputs) {
            if (base::isSameFilePath(outPath, inPath)) {
                throw Error("输出文件路径不能覆盖输入原图或参考图");
            }
        }
    }

    if (onProgress) {
        onProgress(ReplaceProgress{0, 0, 0, job.copyCount, -1, -1, "正在读取源素材与参考图…", nullptr});
    }

    // Read source file ONCE for SHA-256 + decode!
    base::Bytes sourceRaw = base::readFileBytes(job.sourcePath);
    std::string sourceDigest = base::sha256Hex(sourceRaw);
    media::Animation sourceAnim = media::decodeAnimation(job.sourcePath);

    // Prepare identity & style references ONCE outside BOTH copy loop and frame loop!
    std::vector<media::Frame> refFrames;
    refFrames.reserve(1 + job.stylePaths.size());
    media::Image croppedIdentity = media::limitImageEdge(
        media::cropImage(media::decodeFirstFrame(job.characterPath), job.characterCrop), 2048);
    refFrames.push_back(media::Frame{std::move(croppedIdentity), 100});
    for (const auto& sp : job.stylePaths) {
        refFrames.push_back(media::Frame{media::limitImageEdge(media::decodeFirstFrame(sp), 2048), 100});
    }

    if (plugins) {
        nlohmann::json refCtx{{"role", "reference_images"}};
        plugins->processFramesBatch("input", refCtx, refFrames, cancel);
    }

    std::vector<net::UploadPart> staticRefs;
    staticRefs.reserve(refFrames.size());
    base::Bytes idPng = media::encodePng(refFrames[0].image);
    std::string identityDigest = base::sha256Hex(idPng);
    staticRefs.push_back(net::UploadPart{"identity", std::move(idPng)});

    std::vector<std::string> styleDigests;
    for (size_t i = 1; i < refFrames.size(); ++i) {
        base::Bytes stPng = media::encodePng(refFrames[i].image);
        styleDigests.push_back(base::sha256Hex(stPng));
        staticRefs.push_back(net::UploadPart{"style", std::move(stPng)});
    }

    for (int copyIdx = 0; copyIdx < job.copyCount; ++copyIdx) {
        cancel.verify();
        runSingleReplaceCopy(
            job,
            outputPaths[static_cast<size_t>(copyIdx)],
            copyIdx,
            sourceRaw,
            sourceDigest,
            sourceAnim,
            staticRefs,
            identityDigest,
            styleDigests,
            apiClient,
            plugins,
            cancel,
            onProgress);
    }
    return outputPaths;
}

} // namespace meme::core
