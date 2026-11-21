#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace meme {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct CancelledError : Error {
    CancelledError() : Error("任务已取消") {}
    explicit CancelledError(const std::string& msg) : Error(msg) {}
};

struct ApiConnectionError : Error {
    using Error::Error;
};

struct ApiHttpError : Error {
    uint32_t status = 0;
    ApiHttpError(uint32_t code, const std::string& msg) : Error(msg), status(code) {}
};

void checkHr(long hr, std::string_view operation);

} // namespace meme
