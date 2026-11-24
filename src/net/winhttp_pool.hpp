#pragma once
#include "base/crypto.hpp"
#include "base/managed_process.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace meme::net {

class ActiveHttpSlot;

// Unified thread-safe cancellation token supporting both concurrent WinHTTP requests
// and managed child processes without handle ABA races.
class CancelToken {
    static constexpr uint64_t kAbiMagic = 0x4D454D4543414E43ull; // "MEMECANC"
    uint64_t abiMagic_ = kAbiMagic;
    base::ProcessCancelSignal procSignal_;
    mutable std::mutex mutex_;
    std::vector<std::weak_ptr<ActiveHttpSlot>> activeSlots_;
public:
    CancelToken() = default;
    ~CancelToken() { stop(); abiMagic_ = 0; }
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    bool hasValidAbiMagic() const noexcept { return abiMagic_ == kAbiMagic; }

    void stop() noexcept;
    void reset() noexcept;
    bool isStopped() const noexcept { return procSignal_.isCancelled(); }
    void verify() const { procSignal_.throwIfCancelled(); }
    const base::ProcessCancelSignal& processSignal() const noexcept { return procSignal_; }

    void registerSlot(const std::shared_ptr<ActiveHttpSlot>& slot);
};

struct SseEvent {
    std::string eventType;
    std::string data;
};

// Incremental Server-Sent Events parser handling UTF-8 BOM, split CRLF across TCP chunks,
// multi-line data fields, comments, and strict event size caps.
class SseParser {
    std::function<void(const SseEvent&)> onEvent_;
    std::string line_;
    std::string eventType_;
    std::string data_;
    bool skipNextLf_ = false;
    bool firstLine_ = true;
    bool doneReceived_ = false;

    void finishLine();
    void dispatchRecord();
public:
    explicit SseParser(std::function<void(const SseEvent&)> onEvent);
    void feed(std::string_view chunk);
    bool isDone() const noexcept { return doneReceived_; }
};

struct HttpRequestSpec {
    std::string url;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::span<const uint8_t> body;
    int timeoutSeconds = 600;
    bool expectEventStream = false;
    bool allowLoopbackHttp = true;
};

struct HttpResponse {
    uint32_t status = 0;
    bool streamed = false;
    base::Bytes body;
};

class WinHttpPool {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    WinHttpPool();
    ~WinHttpPool();
    WinHttpPool(const WinHttpPool&) = delete;
    WinHttpPool& operator=(const WinHttpPool&) = delete;

    HttpResponse send(
        const HttpRequestSpec& spec,
        CancelToken& cancel,
        const std::function<void(const SseEvent&)>& onSseEvent = {});
};

} // namespace meme::net
