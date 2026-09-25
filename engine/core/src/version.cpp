#include "helios/core/version.h"

#include <format>

#include "helios/core/assert.h"

// Injected for this file only by engine/core/CMakeLists.txt (bare tokens, stringified here so no
// shell/generator quoting is involved).
#ifndef HELIOS_BUILD_CONFIG_NAME
#define HELIOS_BUILD_CONFIG_NAME unknown
#endif
#ifndef HELIOS_GIT_HASH_NAME
#define HELIOS_GIT_HASH_NAME unknown
#endif

namespace helios::version {

namespace {

constexpr std::string_view kConfig = HELIOS_STRINGIFY(HELIOS_BUILD_CONFIG_NAME);
constexpr std::string_view kGitHash = HELIOS_STRINGIFY(HELIOS_GIT_HASH_NAME);

#if defined(HELIOS_COMPILER_CLANG)
constexpr std::string_view kCompiler = "Clang " __clang_version__;
#elif defined(HELIOS_COMPILER_MSVC)
constexpr std::string_view kCompiler = "MSVC " HELIOS_STRINGIFY(_MSC_FULL_VER);
#elif defined(HELIOS_TOOLCHAIN_MINGW)
constexpr std::string_view kCompiler = "GCC " HELIOS_STRINGIFY(__GNUC__) "." HELIOS_STRINGIFY(
    __GNUC_MINOR__) "." HELIOS_STRINGIFY(__GNUC_PATCHLEVEL__) " (MinGW-w64)";
#else
constexpr std::string_view kCompiler = "GCC " HELIOS_STRINGIFY(__GNUC__) "." HELIOS_STRINGIFY(
    __GNUC_MINOR__) "." HELIOS_STRINGIFY(__GNUC_PATCHLEVEL__);
#endif

} // namespace

const BuildInfo& buildInfo() noexcept {
    static const BuildInfo info = [] {
        BuildInfo b;
        b.version = kString;
        b.gitHash = kGitHash.empty() ? std::string_view("unknown") : kGitHash;
        b.configuration = kConfig.empty() ? std::string_view("unknown") : kConfig;
        b.compiler = kCompiler;
        b.platform = platform::kPlatformName;
        b.architecture = platform::kArchName;
        b.assertsEnabled = HELIOS_ENABLE_ASSERTS != 0;
        return b;
    }();
    return info;
}

std::string buildInfoString() {
    const BuildInfo& b = buildInfo();
    return std::format("Helios {} ({}) {} {} {} {}{}", b.version, b.gitHash, b.configuration, b.compiler, b.platform,
                       b.architecture, b.assertsEnabled ? " +asserts" : "");
}

} // namespace helios::version
