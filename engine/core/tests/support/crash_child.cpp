// Child process for the crash-handler tests: installs the handler into argv[1], then fails the way
// argv[2] selects, so the parent can verify that a report/minidump was written:
//   segv (default)  invalid memory access on the main thread (SEH exception / SIGSEGV)
//   fatal           HELIOS_LOG_FATAL -> abort() on the main thread (asserts take the same path)
//   thread-throw    an exception escapes a helios::Thread -> std::terminate on that thread
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>

#include "helios/core/crash.h"
#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "helios/core/thread.h"

namespace {
// A small non-null address in the never-mapped low pages (Windows reserves the first 64 KiB, Linux
// mmap_min_addr): a real access violation / SIGSEGV that UBSan's null-store check does not
// intercept, so the test also works in sanitizer builds. Read from a volatile integer so the
// compiler can neither elide the store nor see the constant address (-Warray-bounds).
volatile std::uintptr_t g_badAddress = 16;
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    helios::CrashHandlerOptions options;
    options.appName = "crashchild";
    if (!helios::installCrashHandler(helios::fs::pathFromUtf8(argv[1]), options)) return 3;
    const std::string_view mode = argc >= 3 ? std::string_view(argv[2]) : std::string_view("segv");
    if (mode == "fatal") HELIOS_LOG_FATAL("crash_child: deliberate fatal error");
    if (mode == "thread-throw") {
        helios::Thread thrower("Thrower", [] { throw std::runtime_error("escaped a worker thread"); });
        thrower.join();
        return 4; // unreachable: std::terminate ends the process
    }
    *reinterpret_cast<volatile int*>(g_badAddress) = 1; // crash
    return 0;
}
