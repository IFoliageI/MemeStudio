#include "net/winhttp_pool.hpp"
#include "net/endpoint.hpp"
#include "base/error.hpp"
#include "base/utf8.hpp"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>

namespace meme::net {

class ActiveHttpSlot {
    std::mutex mtx_;
    HINTERNET handle_ = nullptr;
public:
    explicit ActiveHttpSlot(HINTERNET h) : handle_(h) {}
    ~ActiveHttpSlot() { closeOnce(); }

    void closeOnce() noexcept {
        HINTERNET toClose = nullptr;
        {
            std::lock_guard lock(mtx_);
            toClose = handle_;
            handle_ = nullptr;
        }
        if (toClose) {
            WinHttpCloseHandle(toClose);
        }
    }

    HINTERNET get() noexcept {
        std::lock_guard lock(mtx_);
        return handle_;
    }
};

void CancelToken::stop() noexcept {
    procSignal_.cancel();
    std::vector<std::shared_ptr<ActiveHttpSlot>> snapshot;
    {
        std::lock_guard lock(mutex_);
        for (const auto& wp : activeSlots_) {
            if (auto sp = wp.lock()) {
                snapshot.push_back(std::move(sp));
            }
        }
        activeSlots_.clear();
    }
    for (auto& slot : snapshot) {
        slot->closeOnce();
    }
}

void CancelToken::reset() noexcept {
    stop();
    procSignal_.reset();
}

void CancelToken::registerSlot(const std::shared_ptr<ActiveHttpSlot>& slot) {
    std::lock_guard lock(mutex_);
    if (procSignal_.isCancelled()) {
        slot->closeOnce();
        throw CancelledError();
    }
    std::erase_if(activeSlots_, [](const auto& wp) { return wp.expired(); });
    activeSlots_.push_back(slot);
}

SseParser::SseParser(std::function<void(const SseEvent&)> onEvent)
    : onEvent_(std::move(onEvent)) {}

void SseParser::dispatchRecord() {
    if (data_.empty()) {
        eventType_.clear();
        return;
    }
    if (data_ == "[DONE]") {
        doneReceived_ = true;
        data_.clear();
        eventType_.clear();
        return;
    }
    if (!doneReceived_ && onEvent_) {
        onEvent_(SseEvent{eventType_, data_});
    }
    data_.clear();
    eventType_.clear();
}

void SseParser::finishLine() {
    if (firstLine_) {
        if (line_.starts_with("\xEF\xBB\xBF")) {
            line_.erase(0, 3);
        }
        firstLine_ = false;
    }
    if (line_.empty()) {
        dispatchRecord();
    } else if (line_.front() != ':') { // Ignore SSE comment lines starting with ':'
        auto colon = line_.find(':');
        std::string_view field = colon == std::string::npos
            ? std::string_view(line_)
            : std::string_view(line_).substr(0, colon);
        std::string_view val = colon == std::string::npos
            ? std::string_view{}
            : std::string_view(line_).substr(colon + 1);
        if (!val.empty() && val.front() == ' ') {
            val.remove_prefix(1);
        }
        if (field == "event") {
            eventType_.assign(val);
        } else if (field == "data") {
            if (!data_.empty()) data_.push_back('\n');
            data_.append(val);
        }
    }
    line_.clear();
}

void SseParser::feed(std::string_view chunk) {
    for (char c : chunk) {
        if (skipNextLf_ && c == '\n') {
            skipNextLf_ = false;
            continue;
        }
        skipNextLf_ = false;
        if (c == '\r' || c == '\n') {
            finishLine();
            skipNextLf_ = (c == '\r');
        } else {
            line_.push_back(c);
        }
        if (line_.size() + data_.size() > 72ull * 1024 * 1024) {
            throw Error("单个 SSE 流式事件超过 72 MB 上限");
        }
    }
}

struct WinHttpPool::Impl {
    HINTERNET session = nullptr;

    Impl() {
        session = WinHttpOpen(
            L"MemeStudio/2.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (!session) {
            throw Error("初始化 WinHTTP 会话失败（错误码 " + std::to_string(GetLastError()) + "）");
        }
    }

    ~Impl() {
        if (session) {
            WinHttpCloseHandle(session);
        }
    }
};

WinHttpPool::WinHttpPool() : impl_(std::make_unique<Impl>()) {}
WinHttpPool::~WinHttpPool() = default;

HttpResponse WinHttpPool::send(
    const HttpRequestSpec& spec,
    CancelToken& cancel,
    const std::function<void(const SseEvent&)>& onSseEvent) {
    cancel.verify();
    if (spec.timeoutSeconds < 5 || spec.timeoutSeconds > 3600) {
        throw Error("网络请求超时时间必须在 5 到 3600 秒之间");
    }

    CrackedUrl parsed = parseAndValidateUrl(spec.url, spec.allowLoopbackHttp);
    std::wstring methodW = base::utf8ToWideStrict(spec.method);

    struct ScopedInternet {
        HINTERNET h = nullptr;
        ~ScopedInternet() { if (h) WinHttpCloseHandle(h); }
    };

    ScopedInternet connection{WinHttpConnect(impl_->session, parsed.host.c_str(), parsed.port, 0)};
    if (!connection.h) {
        throw ApiConnectionError("创建网络连接失败（错误码 " + std::to_string(GetLastError()) + "）");
    }

    HINTERNET rawReq = WinHttpOpenRequest(
        connection.h,
        methodW.c_str(),
        parsed.pathAndQuery.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parsed.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!rawReq) {
        throw ApiConnectionError("创建 HTTP 请求失败（错误码 " + std::to_string(GetLastError()) + "）");
    }

    auto reqSlot = std::make_shared<ActiveHttpSlot>(rawReq);
    struct SlotGuard {
        std::shared_ptr<ActiveHttpSlot> slot;
        ~SlotGuard() { if (slot) slot->closeOnce(); }
    } slotGuard{reqSlot};
    cancel.registerSlot(reqSlot);

    int timeoutMs = spec.timeoutSeconds * 1000;
    WinHttpSetTimeouts(rawReq, 15000, 15000, timeoutMs, timeoutMs);
    if (parsed.isLoopback) {
        WINHTTP_PROXY_INFO directProxy{WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr};
        WinHttpSetOption(rawReq, WINHTTP_OPTION_PROXY, &directProxy, sizeof(directProxy));
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(rawReq, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    DWORD disableCookies = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(rawReq, WINHTTP_OPTION_DISABLE_FEATURE, &disableCookies, sizeof(disableCookies));

    std::wstring headerBlock;
    for (const auto& [hk, hv] : spec.headers) {
        if (!headerBlock.empty()) headerBlock += L"\r\n";
        headerBlock += base::utf8ToWideStrict(hk) + L": " + base::utf8ToWideStrict(hv);
    }

    auto throwClassifiedWinHttpError = [&]() -> void {
        DWORD code = GetLastError();
        cancel.verify();
        if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED ||
            code == ERROR_WINHTTP_CANNOT_CONNECT ||
            code == ERROR_WINHTTP_CONNECTION_ERROR ||
            code == ERROR_WINHTTP_SECURE_FAILURE ||
            code == ERROR_WINHTTP_TIMEOUT) {
            if (code == ERROR_WINHTTP_TIMEOUT) {
                throw ApiConnectionError("网络请求超时（超过 " + std::to_string(spec.timeoutSeconds) + " 秒）");
            }
            throw ApiConnectionError("网络连接或 TLS 传输异常（Windows 错误码 " + std::to_string(code) + "）");
        }
        throw Error("HTTP 请求失败（Windows 错误码 " + std::to_string(code) + "）");
    };

    if (!WinHttpSendRequest(
            rawReq,
            headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
            headerBlock.empty() ? 0 : static_cast<DWORD>(headerBlock.size()),
            spec.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<uint8_t*>(spec.body.data()),
            static_cast<DWORD>(spec.body.size()),
            static_cast<DWORD>(spec.body.size()),
            0)) {
        throwClassifiedWinHttpError();
    }

    if (!WinHttpReceiveResponse(rawReq, nullptr)) {
        throwClassifiedWinHttpError();
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(
            rawReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        throwClassifiedWinHttpError();
    }

    wchar_t contentType[256]{};
    DWORD ctSize = sizeof(contentType);
    bool isEventStream = spec.expectEventStream &&
                         status >= 200 && status < 300 &&
                         onSseEvent &&
                         WinHttpQueryHeaders(
                             rawReq, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                             contentType, &ctSize, WINHTTP_NO_HEADER_INDEX) &&
                         _wcsnicmp(contentType, L"text/event-stream", 17) == 0;

    SseParser sseParser(onSseEvent);
    base::Bytes respBytes;
    std::array<uint8_t, 65536> buf{};
    size_t totalRead = 0;

    for (;;) {
        cancel.verify();
        DWORD toRead = static_cast<DWORD>(buf.size());
        if (isEventStream) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(rawReq, &avail)) {
                throwClassifiedWinHttpError();
            }
            if (avail == 0) break;
            toRead = std::min(toRead, avail);
        }
        DWORD readCount = 0;
        if (!WinHttpReadData(rawReq, buf.data(), toRead, &readCount)) {
            throwClassifiedWinHttpError();
        }
        if (readCount == 0) break;
        totalRead += readCount;
        if (totalRead > 180ull * 1024 * 1024) {
            throw Error("HTTP 响应体超过 180 MB 上限");
        }
        if (isEventStream) {
            sseParser.feed(std::string_view(reinterpret_cast<const char*>(buf.data()), readCount));
        } else {
            respBytes.insert(respBytes.end(), buf.begin(), buf.begin() + readCount);
        }
    }

    cancel.verify();
    return HttpResponse{status, isEventStream, std::move(respBytes)};
}

} // namespace meme::net
