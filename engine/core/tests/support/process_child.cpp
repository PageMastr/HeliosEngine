// Child program for the process tests (tests/test_process.cpp). argv[1] selects the mode:
//   args                 print every argument after the mode, hex-encoded, one per line
//   cat                  copy stdin to stdout (binary)
//   stderr <text>        write <text> to stderr and "out" to stdout
//   exit <n>             exit with code n
//   env <NAME>...        print NAME=value (or NAME unset) per line
//   cwd                  print the working directory (UTF-8, generic separators)
//   sleep <ms>           sleep, then exit 0
//   write-handle <h> <text>  write <text> to the inherited handle value h and exit
//   fd-open <fd>...      POSIX: print "open" or "closed" per descriptor (Windows: "unsupported")
// Output goes through helios::PipeEnd on the raw standard handles, so it is binary-exact on
// Windows too (no CRT text-mode translation).
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#endif

#include "helios/core/cmdline.h"
#include "helios/core/fs.h"
#include "helios/core/process.h"

using namespace helios;

namespace {

void writeTo(NativeHandle h, std::string_view text) {
    PipeEnd end = PipeEnd::adopt(h);
    (void)end.write(text);
    end.release();
}

void out(std::string_view text) { writeTo(standardOutputHandle(), text); }

std::string hex(std::string_view s) {
    static const char* digits = "0123456789abcdef";
    std::string r;
    for (unsigned char c : s) {
        r.push_back(digits[c >> 4]);
        r.push_back(digits[c & 15]);
    }
    return r;
}

} // namespace

int main() {
    const CommandLine cmd = CommandLine::fromProcess();
    const std::vector<std::string>& args = cmd.arguments();
    if (args.empty()) return 2;
    const std::string& mode = args[0];
    if (mode == "args") {
        std::string text;
        for (usize i = 1; i < args.size(); ++i) text += hex(args[i]) + "\n";
        out(text);
        return 0;
    }
    if (mode == "cat") {
        PipeEnd in = PipeEnd::adopt(standardInputHandle());
        char buf[8192];
        for (;;) {
            Result<usize> n = in.read(buf, sizeof(buf));
            if (!n || *n == 0) break;
            out(std::string_view(buf, *n));
        }
        in.release();
        return 0;
    }
    if (mode == "stderr" && args.size() >= 2) {
        writeTo(standardErrorHandle(), args[1]);
        out("out");
        return 0;
    }
    if (mode == "exit" && args.size() >= 2) return std::atoi(args[1].c_str());
    if (mode == "env") {
        std::string text;
        for (usize i = 1; i < args.size(); ++i) {
            const char* v = std::getenv(args[i].c_str());
            text += args[i] + (v ? "=" + std::string(v) : std::string(" unset")) + "\n";
        }
        out(text);
        return 0;
    }
    if (mode == "cwd") {
        out(fs::pathToGenericUtf8(std::filesystem::current_path()));
        return 0;
    }
    if (mode == "sleep" && args.size() >= 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(args[1].c_str())));
        return 0;
    }
    if (mode == "fd-open") {
        std::string text;
        for (usize i = 1; i < args.size(); ++i) {
#if defined(_WIN32)
            text += "unsupported\n";
#else
            text += ::fcntl(std::atoi(args[i].c_str()), F_GETFD) != -1 ? "open\n" : "closed\n";
#endif
        }
        out(text);
        return 0;
    }
    if (mode == "write-handle" && args.size() >= 3) {
        const NativeHandle h = static_cast<NativeHandle>(std::strtoll(args[1].c_str(), nullptr, 10));
        PipeEnd end = PipeEnd::adopt(h);
        return end.write(args[2]) ? 0 : 5;
    }
    return 3;
}
