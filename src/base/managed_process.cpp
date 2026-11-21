#include "base/managed_process.hpp"
#include "base/error.hpp"
#include "base/fs_io.hpp"
#include "base/utf8.hpp"
#include <windows.h>
#include <algorithm>
#include <vector>

namespace meme::base {

namespace {
struct KernelHandle {
    HANDLE value = nullptr;
    KernelHandle() = default;
    explicit KernelHandle(HANDLE h) : value(h) {}
    ~KernelHandle() {
        if (value && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;
    bool valid() const noexcept { return value != nullptr && value != INVALID_HANDLE_VALUE; }
};
} // namespace

ProcessCancelSignal::ProcessCancelSignal() {
    eventHandle_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!eventHandle_) {
        throw Error("创建取消事件句柄失败");
    }
}

ProcessCancelSignal::~ProcessCancelSignal() {
    if (eventHandle_) {
        CloseHandle(static_cast<HANDLE>(eventHandle_));
    }
}

void ProcessCancelSignal::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
    if (eventHandle_) {
        SetEvent(static_cast<HANDLE>(eventHandle_));
    }
}

void ProcessCancelSignal::reset() noexcept {
    cancelled_.store(false, std::memory_order_release);
    if (eventHandle_) {
        ResetEvent(static_cast<HANDLE>(eventHandle_));
    }
}

void ProcessCancelSignal::throwIfCancelled() const {
    if (isCancelled()) {
        throw CancelledError();
    }
}

std::wstring quoteCommandArg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

ProcessRunResult runManagedProcess(const ProcessRunSpec& spec, const ProcessCancelSignal& cancelSignal) {
    cancelSignal.throwIfCancelled();
    if (spec.executable.empty() || !fs::is_regular_file(spec.executable)) {
        throw Error("外部可执行文件不存在：" + wideToUtf8(spec.executable.wstring()));
    }

    KernelHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) {
        throw Error("创建进程组 Job Object 失败");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits))) {
        throw Error("配置 Job Object 自动回收策略失败");
    }

    fs::path actualLogPath = spec.logFilePath;
    bool tempLog = false;
    if (actualLogPath.empty()) {
        actualLogPath = fs::temp_directory_path() / (L"memestudio-proc-" + utf8ToWideStrict(generateRandomHex(8)) + L".log");
        tempLog = true;
    } else if (!actualLogPath.parent_path().empty()) {
        fs::create_directories(actualLogPath.parent_path());
    }

    struct TempLogGuard {
        fs::path path;
        bool active = false;
        ~TempLogGuard() {
            if (active) {
                std::error_code ec;
                fs::remove(path, ec);
            }
        }
    } logGuard{actualLogPath, tempLog};

    SECURITY_ATTRIBUTES inheritSa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    KernelHandle logHandle(CreateFileW(
        actualLogPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inheritSa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!logHandle.valid()) {
        throw Error("无法创建子进程输出日志：" + wideToUtf8(actualLogPath.wstring()));
    }

    std::wstring cmdLine = quoteCommandArg(spec.executable.wstring());
    for (const auto& arg : spec.args) {
        cmdLine.push_back(L' ');
        cmdLine += quoteCommandArg(arg);
    }

    // Explicit handle inheritance whitelist via PROC_THREAD_ATTRIBUTE_HANDLE_LIST
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrStorage(attrSize);
    auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
        throw Error("初始化进程句柄白名单失败");
    }
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttrGuard() { DeleteProcThreadAttributeList(list); }
    } attrGuard{attrList};

    HANDLE inheritedHandles[1] = {logHandle.value};
    if (!UpdateProcThreadAttribute(
            attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
        throw Error("设置子进程标准流继承白名单失败");
    }

    STARTUPINFOEXW siEx{};
    siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    siEx.StartupInfo.wShowWindow = SW_HIDE;
    siEx.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    siEx.StartupInfo.hStdOutput = logHandle.value;
    siEx.StartupInfo.hStdError = logHandle.value;
    siEx.lpAttributeList = attrList;

    fs::path workDir = spec.workingDirectory.empty() ? spec.executable.parent_path() : spec.workingDirectory;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            spec.executable.c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            workDir.empty() ? nullptr : workDir.c_str(),
            &siEx.StartupInfo,
            &pi)) {
        DWORD err = GetLastError();
        throw Error("启动外部进程失败（Windows 错误码 " + std::to_string(err) + "）：" + wideToUtf8(spec.executable.wstring()));
    }

    KernelHandle procHandle(pi.hProcess);
    KernelHandle threadHandle(pi.hThread);
    // Close parent's write handle copy so file flushes cleanly when child exits
    CloseHandle(logHandle.value);
    logHandle.value = nullptr;

    if (!AssignProcessToJobObject(job.value, procHandle.value)) {
        TerminateProcess(procHandle.value, 1);
        throw Error("无法将子进程绑定至 Job Object");
    }
    ResumeThread(threadHandle.value);

    DWORD timeoutMs = spec.timeoutSeconds <= 0
        ? INFINITE
        : static_cast<DWORD>(std::min<int64_t>(int64_t(spec.timeoutSeconds) * 1000, 0x7FFFFFFFll));

    HANDLE waitHandles[2] = {procHandle.value, static_cast<HANDLE>(cancelSignal.nativeEvent())};
    DWORD waitCount = cancelSignal.nativeEvent() ? 2u : 1u;
    DWORD waitRes = WaitForMultipleObjects(waitCount, waitHandles, FALSE, timeoutMs);

    if (waitRes == WAIT_OBJECT_0 + 1 || cancelSignal.isCancelled()) {
        TerminateJobObject(job.value, 130);
        WaitForSingleObject(procHandle.value, 2000);
        throw CancelledError();
    }
    if (waitRes == WAIT_TIMEOUT) {
        TerminateJobObject(job.value, 124);
        WaitForSingleObject(procHandle.value, 2000);
        std::string tail = readFileTailUtf8(actualLogPath, 2048);
        throw Error("外部进程执行超时（超过 " + std::to_string(spec.timeoutSeconds) + " 秒）" +
                    (tail.empty() ? "" : "：\n" + tail));
    }
    if (waitRes != WAIT_OBJECT_0) {
        TerminateJobObject(job.value, 1);
        throw Error("等待外部进程结束时发生系统异常");
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(procHandle.value, &exitCode);
    std::string logTail = readFileTailUtf8(actualLogPath, 4096);
    if (exitCode != 0) {
        std::string msg = "外部进程异常退出（退出码 " + std::to_string(exitCode) + "）";
        if (!logTail.empty()) {
            msg += "：\n" + trimAscii(logTail);
        }
        throw Error(msg);
    }
    return {exitCode, std::move(logTail)};
}

} // namespace meme::base
