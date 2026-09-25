#pragma once
// Shared Win32 includes and helpers for engine/core's Windows platform layer (MSVC, clang-cl and
// MinGW-w64). Only included by src/platform/win32/*.cpp.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10
#endif
#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>

#include <cstring>
#include <string>
#include <string_view>

#include "helios/core/result.h"
#include "helios/core/utf.h"

namespace helios::win32 {

/// UTF-8 -> UTF-16 for W APIs.
inline std::wstring widen(std::string_view utf8) { return utf8ToWide(utf8); }
/// UTF-16 -> UTF-8.
inline std::string narrow(std::wstring_view wide) { return wideToUtf8(wide); }

/// FormatMessageW text for a Win32 error code (UTF-8, trailing newline stripped).
std::string errorMessage(DWORD code);
ErrorCode mapError(DWORD code) noexcept;
Error makeWin32Error(DWORD code, std::string_view context);

inline HANDLE toHandle(std::intptr_t h) noexcept { return reinterpret_cast<HANDLE>(h); }
inline std::intptr_t fromHandle(HANDLE h) noexcept { return reinterpret_cast<std::intptr_t>(h); }

/// Converts a GetProcAddress result to a typed function pointer without -Wcast-function-type noise.
template <class Fn>
Fn procAs(FARPROC proc) noexcept {
    static_assert(sizeof(Fn) == sizeof(FARPROC));
    Fn fn = nullptr;
    std::memcpy(&fn, &proc, sizeof(fn));
    return fn;
}

} // namespace helios::win32
