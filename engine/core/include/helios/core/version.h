#pragma once
// Engine version and build information.
//
// Version numbers come from the top-level CMake project() (HELIOS_VERSION_* definitions); the git
// hash and build configuration are injected into version.cpp only (CI sets -DHELIOS_GIT_HASH).
// Threading: constants and immutable data.

#include <string>
#include <string_view>

#include "helios/core/platform.h"
#include "helios/core/types.h"

#ifndef HELIOS_VERSION_MAJOR
#define HELIOS_VERSION_MAJOR 0
#endif
#ifndef HELIOS_VERSION_MINOR
#define HELIOS_VERSION_MINOR 1
#endif
#ifndef HELIOS_VERSION_PATCH
#define HELIOS_VERSION_PATCH 0
#endif

namespace helios::version {

inline constexpr u32 kMajor = HELIOS_VERSION_MAJOR;
inline constexpr u32 kMinor = HELIOS_VERSION_MINOR;
inline constexpr u32 kPatch = HELIOS_VERSION_PATCH;
/// Packed as major.minor.patch in 10.10.12 bits — monotonic, comparable.
inline constexpr u32 kPacked = (kMajor << 22) | (kMinor << 12) | kPatch;
/// "0.1.0"
inline constexpr const char* kString =
    HELIOS_STRINGIFY(HELIOS_VERSION_MAJOR) "." HELIOS_STRINGIFY(HELIOS_VERSION_MINOR) "." HELIOS_STRINGIFY(HELIOS_VERSION_PATCH);

constexpr u32 pack(u32 major, u32 minor, u32 patch) noexcept { return (major << 22) | (minor << 12) | patch; }

struct BuildInfo {
    std::string_view version;       ///< "0.1.0"
    std::string_view gitHash;       ///< CI-provided commit hash or "unknown"
    std::string_view configuration; ///< "Debug", "RelWithDebInfo", ...
    std::string_view compiler;      ///< e.g. "GCC 13.3.0", "MSVC 1940"
    std::string_view platform;      ///< "Windows", "Linux"
    std::string_view architecture;  ///< "x64"
    bool assertsEnabled = false;
};

const BuildInfo& buildInfo() noexcept;
/// One-line summary for logs/crash reports: "Helios 0.1.0 (abc123) RelWithDebInfo GCC 13.3.0 Linux x64".
std::string buildInfoString();

} // namespace helios::version
