#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace meme::base {

namespace fs = std::filesystem;

class ProcessCancelSignal {
    std::atomic_bool cancelled_{false};
    void* eventHandle_ = nullptr; // Manual-reset Win32 event for zero-polling WaitForMultipleObjects
public:
    ProcessCancelSignal();
    ~ProcessCancelSignal();
    ProcessCancelSignal(const ProcessCancelSignal&) = delete;
    ProcessCancelSignal& operator=(const ProcessCancelSignal&) = delete;

    void cancel() noexcept;
    void reset() noexcept;
    bool isCancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
    void throwIfCancelled() const;
    void* nativeEvent() const noexcept { return eventHandle_; }
};

struct ProcessRunSpec {
    fs::path executable;
    std::vector<std::wstring> args;
    fs::path workingDirectory;
    fs::path logFilePath; // Optional file receiving merged stdout + stderr
    int timeoutSeconds = 600;
};

struct ProcessRunResult {
    uint32_t exitCode = 0;
    std::string logTailUtf8;
};

std::wstring quoteCommandArg(std::wstring_view arg);

// Executes a child process inside a Windows Job Object (KILL_ON_JOB_CLOSE) with an explicit
// handle-inheritance whitelist, event-driven cancellation, timeout kill, and error log tail capture.
ProcessRunResult runManagedProcess(const ProcessRunSpec& spec, const ProcessCancelSignal& cancelSignal);

} // namespace meme::base
