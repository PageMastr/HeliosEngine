#include <doctest/doctest.h>

#include <cstdlib>
#include <string>
#include <string_view>

#include "helios/core/crash.h"
#include "helios/core/fs.h"
#include "helios/core/version.h"

using namespace helios;

#ifndef HELIOS_CRASH_CHILD_PATH
#error "HELIOS_CRASH_CHILD_PATH must be defined by the build"
#endif

namespace {
struct TempDir {
    fs::Path path;
    TempDir() {
        auto dir = fs::createUniqueTempDirectory("helios-crash-test");
        REQUIRE(dir);
        path = *dir;
    }
    ~TempDir() { (void)fs::removeAll(path); }
};

/// Runs tests/support/crash_child.cpp in `mode`, writing its report into `dir`; returns the status.
int runCrashChild(const fs::Path& dir, std::string_view mode) {
    // Quote both paths for the platform shell (cmd.exe needs an extra pair around the whole line).
    std::string command = "\"" HELIOS_CRASH_CHILD_PATH "\" \"" + fs::pathToUtf8(dir) + "\" " + std::string(mode);
    if constexpr (platform::kIsWindows) command = "\"" + command + "\"";
    if constexpr (!platform::kIsWindows) command = "ulimit -c 0; " + command + " 2>/dev/null"; // no core files
    return std::system(command.c_str());
}
} // namespace

TEST_CASE("crash: handler install, on-demand report and uninstall") {
    CHECK(!isCrashHandlerInstalled());
    CHECK(writeCrashReport("no handler").error().code == ErrorCode::InvalidState);

    TempDir tmp;
    const fs::Path dumpDir = tmp.path / "dumps";
    CrashHandlerOptions options;
    options.appName = "coretests";
    REQUIRE(installCrashHandler(dumpDir, options));
    CHECK(isCrashHandlerInstalled());
    CHECK(fs::isDirectory(dumpDir)); // created on install

    auto report = writeCrashReport("unit test snapshot");
    REQUIRE_MESSAGE(report, (report ? "" : report.error().toString()));
    CHECK(fs::isFile(*report));
    CHECK(fs::fileSize(*report).value() > 0);
    CHECK(fs::pathToUtf8(report->filename()).rfind("coretests-", 0) == 0);
    if constexpr (!platform::kIsWindows) {
        const auto text = fs::readTextFile(*report).value();
        CHECK(text.find("unit test snapshot") != std::string::npos);
    }
    auto second = writeCrashReport("again");
    REQUIRE(second);
    CHECK(*second != *report);

    uninstallCrashHandler();
    CHECK(!isCrashHandlerInstalled());
}

TEST_CASE("crash: a crashing process writes a report") {
    TempDir tmp;
    CHECK(runCrashChild(tmp.path, "segv") != 0);
    auto files = fs::listDirectory(tmp.path);
    REQUIRE(files);
    REQUIRE(files->size() == 1);
    const std::string name = (*files)[0].relativePath;
    CHECK(name.rfind("crashchild-", 0) == 0);
    CHECK((*files)[0].size > 0);
    if constexpr (!platform::kIsWindows) {
        const auto text = fs::readTextFile((*files)[0].path).value();
        CHECK(text.find("SIGSEGV") != std::string::npos);
        CHECK(text.find("backtrace") != std::string::npos);
    }
}

TEST_CASE("crash: abort() paths (fatal errors, exceptions escaping threads) write a report") {
    // Regression (Windows): abort() fast-fails past the unhandled-exception filter, and MSVC keeps
    // set_terminate() per thread, so failed asserts, HELIOS_LOG_FATAL and exceptions escaping a
    // worker thread produced no minidump. The handler now hooks SIGABRT (process-wide) as well.
    for (const char* mode : {"fatal", "thread-throw"}) {
        CAPTURE(mode);
        TempDir tmp;
        CHECK(runCrashChild(tmp.path, mode) != 0);
        auto files = fs::listDirectory(tmp.path);
        REQUIRE(files);
        REQUIRE(files->size() == 1);
        CHECK((*files)[0].relativePath.rfind("crashchild-", 0) == 0);
        CHECK((*files)[0].size > 0);
        if constexpr (!platform::kIsWindows) {
            const auto text = fs::readTextFile((*files)[0].path).value();
            CHECK(text.find("SIGABRT") != std::string::npos);
        }
    }
}

TEST_CASE("version: constants and build info") {
    CHECK(version::kMajor == HELIOS_VERSION_MAJOR);
    CHECK(std::string(version::kString) ==
          std::to_string(version::kMajor) + "." + std::to_string(version::kMinor) + "." + std::to_string(version::kPatch));
    CHECK(version::kPacked == version::pack(version::kMajor, version::kMinor, version::kPatch));
    CHECK(version::pack(1, 2, 3) > version::pack(1, 1, 99));
    const auto& info = version::buildInfo();
    CHECK(info.version == version::kString);
    CHECK(!info.gitHash.empty());
    CHECK(!info.configuration.empty());
    CHECK(!info.compiler.empty());
    CHECK(info.platform == platform::kPlatformName);
    CHECK(info.architecture == platform::kArchName);
    const std::string summary = version::buildInfoString();
    CHECK(summary.find(version::kString) != std::string::npos);
    CHECK(summary.find(std::string(info.compiler)) != std::string::npos);
    MESSAGE(summary);
}
