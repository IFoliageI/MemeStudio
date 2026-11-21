#include "media/wic_codec.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"
#include <windows.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

namespace meme::media {

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IWICImagingFactory> createFactory() {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (hr == CO_E_NOTINITIALIZED) {
        checkHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "自动初始化线程 COM");
        hr = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    checkHr(hr, "创建 WIC 图像工厂");
    return factory;
}

uint32_t readMetaUInt(IWICMetadataQueryReader* reader, const wchar_t* name, uint32_t fallback = 0) {
    if (!reader) return fallback;
    PROPVARIANT val{};
    uint32_t result = fallback;
    if (SUCCEEDED(reader->GetMetadataByName(name, &val))) {
        ULONG num = 0;
        if (SUCCEEDED(PropVariantToUInt32(val, &num))) {
            result = num;
        }
    }
    PropVariantClear(&val);
    return result;
}

std::optional<uint16_t> readGifLoopCount(IWICMetadataQueryReader* globalReader) {
    if (!globalReader) return std::nullopt;
    PROPVARIANT app{}, data{};
    std::optional<uint16_t> loop;
    if (SUCCEEDED(globalReader->GetMetadataByName(L"/appext/application", &app)) &&
        app.vt == (VT_VECTOR | VT_UI1) && app.caub.cElems == 11 &&
        (std::memcmp(app.caub.pElems, "NETSCAPE2.0", 11) == 0 ||
         std::memcmp(app.caub.pElems, "ANIMEXTS1.0", 11) == 0) &&
        SUCCEEDED(globalReader->GetMetadataByName(L"/appext/data", &data)) &&
        data.vt == (VT_VECTOR | VT_UI1) && data.caub.cElems >= 4 &&
        data.caub.pElems[0] == 3 && data.caub.pElems[1] == 1) {
        uint16_t val = static_cast<uint16_t>(data.caub.pElems[2] | (uint16_t(data.caub.pElems[3]) << 8));
        loop = val;
    }
    PropVariantClear(&app);
    PropVariantClear(&data);
    return loop;
}

Image readBgraPixels(IWICImagingFactory* factory, IWICBitmapSource* source) {
    UINT w = 0, h = 0;
    checkHr(source->GetSize(&w, &h), "获取图像帧尺寸");
    Image img = allocateImage(w, h);
    ComPtr<IWICFormatConverter> converter;
    checkHr(factory->CreateFormatConverter(&converter), "创建像素转换器");
    checkHr(converter->Initialize(
        source, GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "初始化 BGRA 转换");
    checkHr(converter->CopyPixels(
        nullptr, img.stride(), static_cast<UINT>(img.pixels.size()), img.pixels.data()), "读取图像像素");
    return img;
}

ComPtr<IWICBitmap> createMemoryBitmap(IWICImagingFactory* factory, const Image& image) {
    ComPtr<IWICBitmap> bmp;
    checkHr(factory->CreateBitmapFromMemory(
        image.width, image.height, GUID_WICPixelFormat32bppBGRA,
        image.stride(), static_cast<UINT>(image.pixels.size()),
        const_cast<BYTE*>(image.pixels.data()), &bmp), "封装内存位图");
    return bmp;
}

Animation unpackDecoder(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, bool firstFrameOnly) {
    UINT count = 0;
    GUID container{};
    checkHr(decoder->GetFrameCount(&count), "读取帧数量");
    checkHr(decoder->GetContainerFormat(&container), "读取图像容器格式");
    if (count == 0) {
        throw Error("图像不包含任何有效帧");
    }

    bool isGif = (container == GUID_ContainerFormatGif);
    if (isGif && count > 600) {
        throw Error("GIF 帧数超过 600 帧上限，请先裁剪时长");
    }

    Animation anim;
    anim.animated = isGif && count > 1;
    if (!isGif || firstFrameOnly) {
        count = 1;
    }

    ComPtr<IWICBitmapFrameDecode> firstFrame;
    checkHr(decoder->GetFrame(0, &firstFrame), "读取首帧");
    if (!isGif) {
        anim.frames.push_back(Frame{readBgraPixels(factory, firstFrame.Get()), 100});
        return anim;
    }

    ComPtr<IWICMetadataQueryReader> globalMeta;
    decoder->GetMetadataQueryReader(&globalMeta);
    UINT canvasW = 0, canvasH = 0;
    checkHr(firstFrame->GetSize(&canvasW, &canvasH), "读取首帧尺寸");
    canvasW = readMetaUInt(globalMeta.Get(), L"/logscrdesc/Width", canvasW);
    canvasH = readMetaUInt(globalMeta.Get(), L"/logscrdesc/Height", canvasH);
    anim.loopCount = readGifLoopCount(globalMeta.Get());

    Image canvas = allocateImage(canvasW, canvasH);
    if (static_cast<uint64_t>(canvas.pixels.size()) * count > 384ull * 1024 * 1024) {
        throw Error("GIF 展开后像素内存超过 384 MB 上限，请先缩小尺寸或缩短帧数");
    }

    std::memset(canvas.pixels.data(), 0, canvas.pixels.size());
    anim.frames.reserve(count);

    for (UINT idx = 0; idx < count; ++idx) {
        ComPtr<IWICBitmapFrameDecode> frameDecode;
        checkHr(decoder->GetFrame(idx, &frameDecode), "解码动画帧");
        Image raw = readBgraPixels(factory, frameDecode.Get());

        ComPtr<IWICMetadataQueryReader> frameMeta;
        frameDecode->GetMetadataQueryReader(&frameMeta);
        uint32_t left = readMetaUInt(frameMeta.Get(), L"/imgdesc/Left", 0);
        uint32_t top = readMetaUInt(frameMeta.Get(), L"/imgdesc/Top", 0);
        uint32_t disposal = readMetaUInt(frameMeta.Get(), L"/grctlext/Disposal", 0);
        uint32_t delayMs = readMetaUInt(frameMeta.Get(), L"/grctlext/Delay", 10) * 10;
        if (delayMs == 0) delayMs = 100;

        Image savedBeforeFrame;
        if (disposal == 3 && idx + 1 < count) {
            savedBeforeFrame = canvas;
        }

        for (uint32_t y = 0; y < raw.height && (top + y) < canvasH; ++y) {
            const uint8_t* srcRow = raw.pixels.data() + static_cast<size_t>(y) * raw.stride();
            uint8_t* dstRow = canvas.pixels.data() + static_cast<size_t>(top + y) * canvas.stride();
            for (uint32_t x = 0; x < raw.width && (left + x) < canvasW; ++x) {
                const uint8_t* sp = srcRow + static_cast<size_t>(x) * 4;
                if (sp[3] != 0) {
                    std::memcpy(dstRow + static_cast<size_t>(left + x) * 4, sp, 4);
                }
            }
        }

        if (idx + 1 == count) {
            anim.frames.push_back(Frame{std::move(canvas), delayMs});
            break;
        }

        anim.frames.push_back(Frame{canvas, delayMs});
        if (disposal == 3) {
            canvas = std::move(savedBeforeFrame);
        } else if (disposal == 2) {
            if (left < canvasW && top < canvasH) {
                uint32_t clearW = std::min<uint32_t>(raw.width, canvasW - left);
                uint32_t clearH = std::min<uint32_t>(raw.height, canvasH - top);
                for (uint32_t y = 0; y < clearH; ++y) {
                    std::memset(
                        canvas.pixels.data() + (static_cast<size_t>(top + y) * canvasW + left) * 4,
                        0,
                        static_cast<size_t>(clearW) * 4);
                }
            }
        }
    }
    return anim;
}

void writeGifMetaUInt16(IWICMetadataQueryWriter* writer, const wchar_t* name, uint16_t val, bool asBool = false) {
    PROPVARIANT pv{};
    if (asBool) {
        pv.vt = VT_BOOL;
        pv.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
    } else {
        pv.vt = VT_UI2;
        pv.uiVal = val;
    }
    checkHr(writer->SetMetadataByName(name, &pv), "写入 GIF 帧元数据");
}

} // namespace

MediaMetadata probeMediaMetadata(const fs::path& path) {
    auto factory = createFactory();
    ComPtr<IWICBitmapDecoder> decoder;
    checkHr(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "打开图像头");

    UINT count = 0;
    GUID container{};
    checkHr(decoder->GetFrameCount(&count), "读取帧数");
    checkHr(decoder->GetContainerFormat(&container), "读取容器类型");
    if (count == 0) {
        throw Error("图像不包含有效帧");
    }

    bool isGif = (container == GUID_ContainerFormatGif);
    if (isGif && count > 600) {
        throw Error("GIF 帧数超过 600 帧上限");
    }

    ComPtr<IWICBitmapFrameDecode> first;
    checkHr(decoder->GetFrame(0, &first), "读取首帧头");
    UINT w = 0, h = 0;
    checkHr(first->GetSize(&w, &h), "读取尺寸");

    MediaMetadata meta;
    meta.width = w;
    meta.height = h;
    meta.frameCount = isGif ? count : 1u;
    meta.animated = isGif && count > 1;

    if (isGif) {
        ComPtr<IWICMetadataQueryReader> globalMeta;
        decoder->GetMetadataQueryReader(&globalMeta);
        meta.width = readMetaUInt(globalMeta.Get(), L"/logscrdesc/Width", meta.width);
        meta.height = readMetaUInt(globalMeta.Get(), L"/logscrdesc/Height", meta.height);
        meta.loopCount = readGifLoopCount(globalMeta.Get());
        meta.frameDelaysMs.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IWICBitmapFrameDecode> f;
            checkHr(decoder->GetFrame(i, &f), "读取帧延迟头");
            ComPtr<IWICMetadataQueryReader> fm;
            f->GetMetadataQueryReader(&fm);
            uint32_t d = readMetaUInt(fm.Get(), L"/grctlext/Delay", 10) * 10;
            meta.frameDelaysMs.push_back(d == 0 ? 100u : d);
        }
    } else {
        meta.frameDelaysMs.push_back(100u);
    }

    if (meta.width == 0 || meta.height == 0 || meta.width > 16384 || meta.height > 16384 ||
        static_cast<uint64_t>(meta.width) * meta.height > 32ull * 1024 * 1024) {
        throw Error("图像尺寸超出安全上限");
    }
    return meta;
}

Animation decodeAnimation(const fs::path& path) {
    auto factory = createFactory();
    ComPtr<IWICBitmapDecoder> decoder;
    checkHr(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "打开图像文件");
    return unpackDecoder(factory.Get(), decoder.Get(), false);
}

Image decodeFirstFrame(const fs::path& path) {
    auto factory = createFactory();
    ComPtr<IWICBitmapDecoder> decoder;
    checkHr(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "打开图像文件");
    Animation anim = unpackDecoder(factory.Get(), decoder.Get(), true);
    return std::move(anim.frames.front().image);
}

Image decodeImageBytes(std::span<const uint8_t> encodedBytes) {
    if (encodedBytes.empty() || encodedBytes.size() > 128ull * 1024 * 1024) {
        throw Error("响应图像为空或超过 128 MB 上限");
    }
    auto factory = createFactory();
    ComPtr<IWICStream> stream;
    checkHr(factory->CreateStream(&stream), "创建内存图像流");
    checkHr(stream->InitializeFromMemory(
        const_cast<BYTE*>(encodedBytes.data()), static_cast<DWORD>(encodedBytes.size())), "初始化内存图像流");
    ComPtr<IWICBitmapDecoder> decoder;
    checkHr(factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "解码内存图像");
    Animation anim = unpackDecoder(factory.Get(), decoder.Get(), true);
    return std::move(anim.frames.front().image);
}

base::Bytes encodePng(const Image& image) {
    if (image.empty()) {
        throw Error("无法编码空图像为 PNG");
    }
    auto factory = createFactory();
    ComPtr<IStream> stream;
    checkHr(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "创建 PNG 输出流");
    ComPtr<IWICBitmapEncoder> encoder;
    checkHr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "创建 PNG 编码器");
    checkHr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "初始化 PNG 编码器");

    ComPtr<IWICBitmapFrameEncode> frame;
    checkHr(encoder->CreateNewFrame(&frame, nullptr), "创建 PNG 帧");
    checkHr(frame->Initialize(nullptr), "初始化 PNG 帧");
    checkHr(frame->SetSize(image.width, image.height), "设置 PNG 尺寸");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    checkHr(frame->SetPixelFormat(&fmt), "设置 PNG 像素格式");
    auto bmp = createMemoryBitmap(factory.Get(), image);
    checkHr(frame->WriteSource(bmp.Get(), nullptr), "写入 PNG 像素");
    checkHr(frame->Commit(), "提交 PNG 帧");
    checkHr(encoder->Commit(), "完成 PNG 编码");

    STATSTG stat{};
    checkHr(stream->Stat(&stat, STATFLAG_NONAME), "获取 PNG 字节大小");
    base::Bytes out(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    checkHr(stream->Seek(zero, STREAM_SEEK_SET, nullptr), "重置 PNG 流指针");
    ULONG bytesRead = 0;
    checkHr(stream->Read(out.data(), static_cast<ULONG>(out.size()), &bytesRead), "读取 PNG 数据");
    if (bytesRead != out.size()) {
        throw Error("PNG 流读取不完整");
    }
    return out;
}

void savePngFile(const fs::path& path, const Image& image) {
    base::writeFileAtomic(path, encodePng(image));
}

void saveGifFile(const fs::path& path, std::span<const Frame> frames, std::optional<uint16_t> loopCount) {
    if (frames.empty()) {
        throw Error("GIF 帧序列不能为空");
    }
    const uint32_t width = frames[0].image.width;
    const uint32_t height = frames[0].image.height;
    for (const auto& f : frames) {
        if (f.image.empty() || f.image.width != width || f.image.height != height) {
            throw Error("GIF 所有帧必须具有相同且有效的宽高尺寸");
        }
    }

    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    fs::path tempPath = path;
    tempPath += L"." + base::utf8ToWideStrict(base::generateRandomHex(6)) + L".part";

    struct TempCleanupGuard {
        fs::path target;
        bool committed = false;
        ~TempCleanupGuard() {
            if (!committed) {
                std::error_code ec;
                fs::remove(target, ec);
            }
        }
    } guard{tempPath};

    auto factory = createFactory();

    size_t sampleCount = std::min<size_t>(frames.size(), 8);
    Image sampleStrip = allocateImage(width, static_cast<uint32_t>(std::min<uint64_t>(16384, uint64_t(height) * sampleCount)));
    uint32_t stripRowsPerSample = sampleStrip.height / static_cast<uint32_t>(sampleCount);
    bool anyTransparent = false;

    for (size_t s = 0; s < sampleCount; ++s) {
        size_t frameIdx = (s * (frames.size() - 1)) / std::max<size_t>(1, sampleCount - 1);
        const auto& srcImg = frames[frameIdx].image;
        for (uint32_t r = 0; r < stripRowsPerSample; ++r) {
            uint32_t srcY = static_cast<uint32_t>((uint64_t(r) * srcImg.height) / stripRowsPerSample);
            const uint8_t* srcRow = srcImg.pixels.data() + static_cast<size_t>(srcY) * srcImg.stride();
            uint8_t* dstRow = sampleStrip.pixels.data() +
                              static_cast<size_t>(s * stripRowsPerSample + r) * sampleStrip.stride();
            std::memcpy(dstRow, srcRow, srcImg.stride());
        }
    }
    for (const auto& f : frames) {
        for (size_t p = 3; p < f.image.pixels.size(); p += 4) {
            if (f.image.pixels[p] < 128) {
                anyTransparent = true;
                break;
            }
        }
        if (anyTransparent) break;
    }

    ComPtr<IWICPalette> masterPalette;
    checkHr(factory->CreatePalette(&masterPalette), "创建 GIF 主调色板");
    auto stripBmp = createMemoryBitmap(factory.Get(), sampleStrip);
    UINT opaqueSlots = anyTransparent ? 255u : 256u;
    checkHr(masterPalette->InitializeFromBitmap(stripBmp.Get(), opaqueSlots, FALSE), "量化 GIF 共享调色板");

    int transparentIndex = -1;
    if (anyTransparent) {
        WICColor colors[256]{};
        UINT actualColors = 0;
        checkHr(masterPalette->GetColors(255, colors, &actualColors), "读取主调色板颜色");
        for (UINT i = 0; i < actualColors; ++i) {
            colors[i] |= 0xFF000000u;
        }
        transparentIndex = static_cast<int>(actualColors);
        colors[actualColors] = 0x00000000u;
        checkHr(masterPalette->InitializeCustom(colors, actualColors + 1), "设置单一透明槽调色板");
    }

    {
        ComPtr<IWICStream> stream;
        checkHr(factory->CreateStream(&stream), "创建 GIF 文件流");
        checkHr(stream->InitializeFromFilename(tempPath.c_str(), GENERIC_WRITE), "打开 GIF 临时文件");

        ComPtr<IWICBitmapEncoder> encoder;
        checkHr(factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder), "创建 GIF 编码器");
        checkHr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "初始化 GIF 编码器");

        if (loopCount.has_value()) {
            ComPtr<IWICMetadataQueryWriter> globalWriter;
            checkHr(encoder->GetMetadataQueryWriter(&globalWriter), "获取 GIF 全局元数据写入器");
            BYTE appId[] = "NETSCAPE2.0";
            uint16_t lv = *loopCount;
            BYTE loopData[]{3, 1, static_cast<BYTE>(lv & 0xFF), static_cast<BYTE>((lv >> 8) & 0xFF), 0};
            PROPVARIANT pv{};
            pv.vt = VT_VECTOR | VT_UI1;
            pv.caub = {11, appId};
            checkHr(globalWriter->SetMetadataByName(L"/appext/application", &pv), "写入 GIF 循环扩展标识");
            pv.caub = {5, loopData};
            checkHr(globalWriter->SetMetadataByName(L"/appext/data", &pv), "写入 GIF 循环次数");
        }

        for (const auto& item : frames) {
            Image normalized = item.image;
            for (size_t p = 0; p < normalized.pixels.size(); p += 4) {
                if (normalized.pixels[p + 3] < 128) {
                    normalized.pixels[p] = 0;
                    normalized.pixels[p + 1] = 0;
                    normalized.pixels[p + 2] = 0;
                    normalized.pixels[p + 3] = 0;
                } else {
                    normalized.pixels[p + 3] = 255;
                }
            }
            auto frameBmp = createMemoryBitmap(factory.Get(), normalized);
            ComPtr<IWICFormatConverter> indexed;
            checkHr(factory->CreateFormatConverter(&indexed), "创建索引色转换器");
            // Fixed shared masterPalette + WICBitmapDitherTypeNone guarantees identical static background
            // pixels map deterministically to the exact same palette index across all frames!
            checkHr(indexed->Initialize(
                frameBmp.Get(), GUID_WICPixelFormat8bppIndexed,
                WICBitmapDitherTypeNone, masterPalette.Get(),
                50.0, WICBitmapPaletteTypeCustom), "转换 8 位索引帧");

            uint64_t remainingTicks = std::max<uint64_t>(1, (uint64_t(item.delayMs) + 5) / 10);
            while (remainingTicks > 0) {
                uint16_t chunkDelay = static_cast<uint16_t>(std::min<uint64_t>(65535, remainingTicks));
                remainingTicks -= chunkDelay;

                ComPtr<IWICBitmapFrameEncode> frameEncode;
                checkHr(encoder->CreateNewFrame(&frameEncode, nullptr), "新建 GIF 编码帧");
                checkHr(frameEncode->Initialize(nullptr), "初始化 GIF 编码帧");
                checkHr(frameEncode->SetSize(width, height), "设置 GIF 帧尺寸");
                WICPixelFormatGUID fmt = GUID_WICPixelFormat8bppIndexed;
                checkHr(frameEncode->SetPixelFormat(&fmt), "设置 GIF 索引格式");
                checkHr(frameEncode->SetPalette(masterPalette.Get()), "写入 GIF 帧调色板");

                ComPtr<IWICMetadataQueryWriter> writer;
                checkHr(frameEncode->GetMetadataQueryWriter(&writer), "获取 GIF 帧元数据写入器");
                writeGifMetaUInt16(writer.Get(), L"/grctlext/Delay", chunkDelay);
                PROPVARIANT disp{};
                disp.vt = VT_UI1;
                disp.bVal = 2;
                checkHr(writer->SetMetadataByName(L"/grctlext/Disposal", &disp), "写入 GIF 处置方式");
                if (transparentIndex >= 0) {
                    writeGifMetaUInt16(writer.Get(), L"/grctlext/TransparencyFlag", 1, true);
                    PROPVARIANT idxPv{};
                    idxPv.vt = VT_UI1;
                    idxPv.bVal = static_cast<BYTE>(transparentIndex);
                    checkHr(writer->SetMetadataByName(L"/grctlext/TransparentColorIndex", &idxPv), "写入透明色索引");
                }
                checkHr(frameEncode->WriteSource(indexed.Get(), nullptr), "写入 GIF 帧像素");
                checkHr(frameEncode->Commit(), "提交 GIF 帧");
            }
        }
        checkHr(encoder->Commit(), "提交 GIF 文件");
    }

    if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw Error("原子写入 GIF 输出文件失败");
    }
    guard.committed = true;
}

} // namespace meme::media
