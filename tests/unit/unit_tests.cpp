#include "base/app_paths.hpp"
#include "base/crypto.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/json_io.hpp"
#include "base/managed_process.hpp"
#include "base/utf8.hpp"
#include "core/config_store.hpp"
#include "core/plugin_host.hpp"
#include "core/prompt_builder.hpp"
#include "core/replace_engine.hpp"
#include "media/image_ops.hpp"
#include "media/wic_codec.hpp"
#include "net/endpoint.hpp"
#include "net/openai_client.hpp"
#include "net/redactor.hpp"
#include "net/winhttp_pool.hpp"

#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace {

int g_passed = 0;
int g_failed = 0;

void expectTrue(bool cond, const char* expr, int line) {
    if (cond) {
        ++g_passed;
    } else {
        ++g_failed;
        std::cerr << "[FAIL] Line " << line << ": " << expr << "\n";
    }
}

#define EXPECT(expr) expectTrue((expr), #expr, __LINE__)

void testUtf8AndStrictErrorHandling() {
    using namespace meme;
    // Valid round-trip including Chinese & emoji surrogate pairs
    std::string orig = "表情重绘 MemeStudio 😀";
    std::wstring wide = base::utf8ToWideStrict(orig);
    EXPECT(base::wideToUtf8(wide) == orig);

    // BUG-03 Regression check: Invalid UTF-8 MUST throw Error, NEVER return fake sentinel filename!
    std::string badUtf8 = "\xFF\xFE\xC0\xAF";
    bool threw = false;
    try {
        (void)base::utf8ToWideStrict(badUtf8);
    } catch (const Error&) {
        threw = true;
    }
    EXPECT(threw);

    // BUG-15 Regression check: Lossy log decoder MUST NOT return empty string when 1 bad byte appears!
    std::string mixedLog = "Server started OK \xB9\xFA EndOfLog";
    std::wstring lossy = base::utf8ToWideLossy(mixedLog);
    EXPECT(!lossy.empty());
    EXPECT(base::wideToUtf8(lossy).find("Server started OK") != std::string::npos);
}

void testCryptoBase64DpapiAndAtomicIo(const std::filesystem::path& sandbox) {
    using namespace meme;
    // BUG-21 Regression check: Empty base64 encode/decode must return empty cleanly without throwing
    EXPECT(base::base64Encode({}).empty());
    EXPECT(base::base64Decode("").empty());

    std::string sample = "MemeStudio-2026-Payload";
    base::Bytes raw(sample.begin(), sample.end());
    std::string b64 = base::base64Encode(raw);
    EXPECT(base::base64Decode(b64) == raw);
    EXPECT(base::base64Decode("data:image/png;base64," + b64) == raw);

    // SHA-256 known test vector ("abc")
    EXPECT(base::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // DPAPI + SecureString round-trip
    std::string cipher = base::dpapiProtect("sk-secret-key-12345");
    auto unlocked = base::dpapiUnprotect(cipher);
    EXPECT(unlocked.view() == "sk-secret-key-12345");
    unlocked.clear();
    EXPECT(unlocked.empty());

    // Atomic file write & path equivalence
    auto f1 = sandbox / L"atomic_test.txt";
    base::writeTextAtomic(f1, "hello-atomic");
    auto readBack = base::readFileBytes(f1);
    EXPECT(std::string(readBack.begin(), readBack.end()) == "hello-atomic");
    EXPECT(base::isSameFilePath(f1, sandbox / L"." / L"ATOMIC_TEST.TXT"));
}

void testMediaGifDisposalAndQuantizer(const std::filesystem::path& sandbox) {
    using namespace meme;
    // BUG-11 Regression check: cropImage on empty image must throw Error cleanly (no UINT_MAX underflow!)
    media::Image emptyImg;
    bool cropThrew = false;
    try {
        (void)media::cropImage(emptyImg, media::NormalizedRect{0.1, 0.1, 0.5, 0.5});
    } catch (const Error&) {
        cropThrew = true;
    }
    EXPECT(cropThrew);

    // Create a 3-frame animated GIF with transparent + solid pixels, verify round-trip & metadata fast probe
    std::vector<media::Frame> frames;
    for (int f = 0; f < 3; ++f) {
        media::Image img = media::allocateImage(32, 32);
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 32; ++x) {
                uint8_t* px = img.pixels.data() + (size_t(y) * 32 + x) * 4;
                if (x < 8 && y < 8) {
                    px[0] = px[1] = px[2] = px[3] = 0; // Transparent corner
                } else if (int(x) >= f * 6 && int(x) < f * 6 + 8) {
                    px[0] = 20; px[1] = 180; px[2] = 240; px[3] = 255; // Moving sprite
                } else {
                    px[0] = 220; px[1] = 220; px[2] = 220; px[3] = 255; // Static background
                }
            }
        }
        frames.push_back(media::Frame{std::move(img), 90});
    }

    auto gifPath = sandbox / L"roundtrip.gif";
    media::saveGifFile(gifPath, frames, uint16_t(2));

    // Fast header-only metadata probe (BUG-19 fix)
    auto meta = media::probeMediaMetadata(gifPath);
    EXPECT(meta.animated);
    EXPECT(meta.width == 32 && meta.height == 32);
    EXPECT(meta.frameCount == 3);
    EXPECT(meta.loopCount.has_value() && *meta.loopCount == 2);

    // Full decode & verify transparent corner stays transparent and static background stays consistent
    auto decoded = media::decodeAnimation(gifPath);
    EXPECT(decoded.frames.size() == 3);
    EXPECT(decoded.frames[1].image.pixels[3] == 0); // Transparent corner preserved

    // Region preservation test
    media::Image orig = media::allocateImage(20, 20);
    std::fill(orig.pixels.begin(), orig.pixels.end(), uint8_t(10));
    media::Image edited = media::allocateImage(20, 20);
    std::fill(edited.pixels.begin(), edited.pixels.end(), uint8_t(200));
    media::Image composited = media::preserveRegionOutside(orig, edited, media::NormalizedRect{0.25, 0.25, 0.5, 0.5});
    EXPECT(composited.pixels[0] == 10);
    EXPECT(composited.pixels[(10 * 20 + 10) * 4] == 200);
}

void testNetEndpointsRedactorAndSse() {
    using namespace meme;
    // Endpoint resolution & loopback policy
    EXPECT(net::resolveOpenAiUrl("https://api.openai.com/v1/", net::EndpointKind::ImageEdits) ==
           "https://api.openai.com/v1/images/edits");
    EXPECT(net::resolveOpenAiUrl("http://127.0.0.2:8189/v1/images/generations?foo=1", net::EndpointKind::ImageEdits) ==
           "http://127.0.0.2:8189/v1/images/edits?foo=1");
    EXPECT(net::resolveOpenAiUrl("http://localhost:8080/v1", net::EndpointKind::Models) ==
           "http://localhost:8080/v1/models");

    bool insecureRejected = false;
    try {
        (void)net::resolveOpenAiUrl("http://example.com/v1", net::EndpointKind::Models);
    } catch (const Error&) {
        insecureRejected = true;
    }
    EXPECT(insecureRejected);

    // BUG-12 Regression check: Short secret ("id") MUST NOT corrupt JSON object keys!
    nlohmann::json rawReport{
        {"id", "req_123"},
        {"detail", "Bearer id failed"},
        {"nested", {{"key_echo", "id"}}}
    };
    std::vector<std::string> secrets{"id"};
    nlohmann::json safeReport = net::redactJsonSecrets(rawReport, secrets);
    EXPECT(safeReport.contains("id"));
    EXPECT(safeReport["detail"] == "Bearer [REDACTED] failed");
    EXPECT(safeReport["nested"]["key_echo"] == "[REDACTED]");

    // UTF-8 safe codepoint boundary truncation (must not slice 3-byte Chinese chars)
    std::string cn = "错误详情一二三四五六七八九十";
    std::string trunc = net::redactAndTruncateUtf8(cn, {}, 5);
    EXPECT(!base::utf8ToWideStrict(trunc).empty());

    // SSE chunked parser (BOM + split CRLF + multi-chunk)
    std::vector<net::SseEvent> events;
    net::SseParser parser([&](const net::SseEvent& ev) { events.push_back(ev); });
    parser.feed("\xEF\xBB\xBF: comment\r");
    parser.feed("\nevent: image_edit.partial_image\r\ndata: {\"partial\":1}\r\n\r");
    parser.feed("\ndata: [DONE]\r\n\r\n");
    EXPECT(events.size() == 1);
    EXPECT(events[0].eventType == "image_edit.partial_image");
    EXPECT(events[0].data == "{\"partial\":1}");
    EXPECT(parser.isDone());
}

void testConfigMigrationIniAndFrameSchedule(const std::filesystem::path& sandbox) {
    using namespace meme;
    base::setAppDataRootOverride(sandbox / L"appdata");

    // BUG-05 Regression check: Legacy numeric enum keys migrate cleanly to named schema v2
    nlohmann::json legacy{
        {"106", "https://custom.example.com/v1"},
        {"108", "gpt-image-1"},
        {"114", "2"},
        {"137", "3"}
    };
    auto migrated = core::parseStudioSettingsJson(legacy);
    EXPECT(migrated.provider.baseUrl == "https://custom.example.com/v1");
    EXPECT(migrated.frameStride == 2);
    EXPECT(migrated.copyCount == 3);

    core::saveStudioSettings(migrated);
    auto reloaded = core::loadStudioSettings();
    EXPECT(reloaded.provider.baseUrl == "https://custom.example.com/v1");

    // BUG-13 Regression check: Quoted Windows path with \t and \n in INI is preserved literally,
    // and invalid section types fail validation instead of silently turning into strings!
    std::string iniText =
        "source = \"D:\\temp\\new_meme.png\"\n"
        "stride = 3\n"
        "[crop]\n"
        "x = 0.1\n"
        "y = 0.2\n"
        "width = 0.5\n"
        "height = 0.5\n";
    auto iniJson = core::parseConfigDocument(iniText, true);
    auto taskCfg = core::parseReplaceTaskConfig(iniJson);
    EXPECT(base::wideToUtf8(taskCfg.sourcePath.wstring()) == "D:\\temp\\new_meme.png");
    EXPECT(taskCfg.frameStride == 3);
    EXPECT(std::abs(taskCfg.characterCrop.x - 0.1) < 1e-9);

    auto badIniJson = core::parseConfigDocument("[crop]\nx = not_a_number\n", true);
    bool badCropRejected = false;
    try {
        (void)core::parseReplaceTaskConfig(badIniJson);
    } catch (const Error&) {
        badCropRejected = true;
    }
    EXPECT(badCropRejected);

    // Midpoint temporal schedule for 5 frames -> [0, 4, 2, 1, 3]
    auto sched = core::computeMidpointFrameSchedule(5, true);
    std::vector<int> expected{0, 4, 2, 1, 3};
    EXPECT(sched == expected);

    // Prompt builder grammar check (1 style ref -> singular "Image 3 is a STYLE reference", static -> no "frame 1 of 1")
    core::ReplacePromptSpec ps;
    ps.styleCount = 1;
    ps.animated = false;
    std::string p = core::buildCharacterReplacePrompt(ps);
    EXPECT(p.find("Image 3 is a STYLE reference") != std::string::npos);
    EXPECT(p.find("frame 1 of 1") == std::string::npos);
}

void testRealPluginHostLoadsExternalDll(const std::filesystem::path& sandbox) {
    using namespace meme;
    base::setAppDataRootOverride(sandbox / L"appdata_plugin_host");

    std::filesystem::path externalPluginsRoot = base::defaultPluginsDir();
    if (!std::filesystem::exists(externalPluginsRoot / L"example" / L"example.dll")) {
        externalPluginsRoot =
            base::executableFilePath().parent_path().parent_path().parent_path().parent_path().parent_path() /
            L"MemeStudioPlugins" / L"out" / L"dist" / L"x64" / L"plugins";
    }
    if (!std::filesystem::exists(externalPluginsRoot / L"example" / L"example.dll")) {
        return;
    }

    auto discovered = core::discoverPluginsInDirectory(externalPluginsRoot);
    EXPECT(!discovered.empty());
    bool foundExample = std::any_of(discovered.begin(), discovered.end(), [](const core::PluginManifest& m) {
        return m.id == "memestudio.example" && m.loadError.empty();
    });
    EXPECT(foundExample);

    net::WinHttpPool pool;
    net::CancelToken cancel;
    std::vector<std::string> enabled{"memestudio.example"};
    core::PluginHost host(pool, discovered, enabled, core::SignaturePolicy::Disabled);
    EXPECT(host.loadedCount() == 1);
    EXPECT(host.isLoaded("memestudio.example"));

    std::string outPrompt = host.transformPrompt(nlohmann::json::object(), "Original prompt");
    EXPECT(outPrompt.find("Original prompt") != std::string::npos);
    EXPECT(outPrompt.find("Maintain consistent character identity") != std::string::npos);

    auto statusJson = host.invokeCommand("memestudio.example", "status", nlohmann::json::object(), nullptr, cancel);
    EXPECT(statusJson.value("enabled", false) == true);
}

} // namespace

int wmain() {
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::filesystem::path sandbox;

    int exitCode = 0;
    try {
        auto suffix = L"memestudio-unit-" + meme::base::utf8ToWideStrict(meme::base::generateRandomHex(8));
        std::error_code tmpEc;
        sandbox = std::filesystem::temp_directory_path(tmpEc) / suffix;
        if (tmpEc || !std::filesystem::create_directories(sandbox, tmpEc)) {
            sandbox = std::filesystem::current_path() / suffix;
            std::filesystem::create_directories(sandbox);
        }

        testUtf8AndStrictErrorHandling();
        testCryptoBase64DpapiAndAtomicIo(sandbox);
        testMediaGifDisposalAndQuantizer(sandbox);
        testNetEndpointsRedactorAndSse();
        testConfigMigrationIniAndFrameSchedule(sandbox);
        testRealPluginHostLoadsExternalDll(sandbox);
    } catch (const std::exception& e) {
        std::cerr << "[UNCAUGHT EXCEPTION] " << e.what() << "\n";
        ++g_failed;
    }

    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);
    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }

    std::cout << "MemeStudioTests summary: " << g_passed << " passed, " << g_failed << " failed.\n";
    exitCode = (g_failed == 0) ? 0 : 1;
    return exitCode;
}
