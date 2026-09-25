#include "helios/core/result.h"

#include <cstdlib>

#include "helios/core/log.h"

namespace helios {

std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::Unknown: return "Unknown";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::EndOfFile: return "EndOfFile";
    case ErrorCode::Timeout: return "Timeout";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::OutOfMemory: return "OutOfMemory";
    case ErrorCode::ParseError: return "ParseError";
    case ErrorCode::InvalidState: return "InvalidState";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Busy: return "Busy";
    case ErrorCode::Corrupt: return "Corrupt";
    case ErrorCode::VersionMismatch: return "VersionMismatch";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    }
    return "Unknown";
}

std::string Error::toString() const {
    std::string out(errorCodeName(code));
    if (!message.empty()) {
        out.append(": ");
        out.append(message);
    }
    return out;
}

namespace detail {

void resultValueAccessFailed(const Error& error) noexcept {
    log::writeMessage(log::Level::Fatal, LogCore, log::SourceLocation{__FILE__, __LINE__, "Result::value"},
                      "Result::value() called on an error result: " + error.toString());
    log::flush();
    std::abort();
}

void resultErrorAccessFailed() noexcept {
    log::writeMessage(log::Level::Fatal, LogCore, log::SourceLocation{__FILE__, __LINE__, "Result::error"},
                      "Result::error() called on a success result");
    log::flush();
    std::abort();
}

} // namespace detail
} // namespace helios
