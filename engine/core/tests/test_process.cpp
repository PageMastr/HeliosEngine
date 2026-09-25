#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "helios/core/cmdline.h"
#include "helios/core/fs.h"
#include "helios/core/process.h"
#include "helios/core/random.h"
#include "helios/core/time.h"
#include "helios/core/utf.h"

#ifndef HELIOS_PROCESS_CHILD_PATH
#error "HELIOS_PROCESS_CHILD_PATH must be defined by the build"
#endif

using namespace helios;
using namespace std::chrono_literals;

namespace {

ProcessDesc childDesc(std::vector<std::string> args) {
    ProcessDesc d;
    d.executable = fs::pathFromUtf8(HELIOS_PROCESS_CHILD_PATH);
    d.args = std::move(args);
    return d;
}

std::string unhex(std::string_view h) {
    std::string r;
    auto val = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (usize i = 0; i + 1 < h.size(); i += 2) r.push_back(static_cast<char>(val(h[i]) * 16 + val(h[i + 1])));
    return r;
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> out;
    usize start = 0;
    while (start < text.size()) {
        usize end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        start = end + 1;
    }
    return out;
}

// Arguments that break naive quoting on Windows or naive splitting anywhere.
const std::vector<std::string> kTrickyArgs = {
    "plain",
    "",
    "two words",
    "tab\there",
    "quote\"inside",
    "\"quoted\"",
    "trailing\\",
    "trailing space\\",
    "back\\\\slashes\\\\\"quote",
    "C:\\Program Files\\Helios\\",
    "\\\\server\\share",
    "new\nline",
    "unicode \xc3\xa9\xe2\x82\xac \xf0\x9f\x9a\x80",
    "--key=value with \"quotes\" and \\\" mixes",
    "%PATH%",
    "$HOME `x` ;|&",
};

} // namespace

TEST_CASE("process: Windows argument quoting round-trips through the MSVC CRT rules") {
    for (const std::string& a : kTrickyArgs) {
        const std::string line = buildWindowsCommandLine("C:\\Program Files\\Helios\\helios-cell.exe", {a, "next"});
        const std::vector<std::string> split = CommandLine::splitWindows(utf8ToWide(line));
        REQUIRE(split.size() == 3);
        CHECK(split[0] == "C:\\Program Files\\Helios\\helios-cell.exe");
        CHECK(split[1] == a);
        CHECK(split[2] == "next");
    }
    CHECK(quoteWindowsArgument("simple") == "simple");
    CHECK(quoteWindowsArgument("") == "\"\"");
    CHECK(quoteWindowsArgument("a b") == "\"a b\"");
    CHECK(quoteWindowsArgument("a\\\"b") == "\"a\\\\\\\"b\"");
    CHECK(quoteWindowsArgument("dir\\ x\\") == "\"dir\\ x\\\\\"");
    // Randomized: arbitrary mixes of the dangerous characters.
    Pcg32 rng(1234);
    const char alphabet[] = {'a', ' ', '\t', '"', '\\', 'z', '\n'};
    for (int i = 0; i < 2000; ++i) {
        std::string a;
        const u32 len = rng.next() % 12;
        for (u32 k = 0; k < len; ++k) a.push_back(alphabet[rng.next() % sizeof(alphabet)]);
        const std::vector<std::string> split = CommandLine::splitWindows(utf8ToWide(buildWindowsCommandLine("p", {a})));
        REQUIRE(split.size() == 2);
        CHECK(split[1] == a);
    }
}

TEST_CASE("process: arguments reach the child byte-exact") {
    std::vector<std::string> args = {"args"};
    args.insert(args.end(), kTrickyArgs.begin(), kTrickyArgs.end());
    Result<ProcessOutput> r = runProcess(childDesc(args));
    REQUIRE(r.ok());
    CHECK(r->exitCode == 0);
    const std::vector<std::string> got = lines(r->out);
    REQUIRE(got.size() == kTrickyArgs.size());
    for (usize i = 0; i < got.size(); ++i) CHECK(unhex(got[i]) == kTrickyArgs[i]);
}

TEST_CASE("process: exit codes") {
    for (int code : {0, 3, 42, 255}) {
        Result<Process> p = Process::spawn(childDesc({"exit", std::to_string(code)}));
        REQUIRE(p.ok());
        CHECK(p->pid() != 0);
        Result<std::optional<i32>> e = p->wait();
        REQUIRE(e.ok());
        CHECK(e->value_or(-1) == code);
        // Idempotent after exit.
        CHECK(p->wait(0ms)->value_or(-1) == code);
        CHECK(!p->isRunning());
        CHECK(p->kill().ok()); // no-op once exited; keeps the exit code
        CHECK(p->tryWait().value_or(-1) == code);
    }
}

TEST_CASE("process: stdin, stdout and stderr pipes do not deadlock on large transfers") {
    std::string big(3 * 1024 * 1024 + 17, '\0');
    for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 131 + (i >> 9)) & 0xff);
    ProcessDesc d = childDesc({"cat"});
    d.stdinMode = StdioMode::Pipe;
    d.stdoutMode = StdioMode::Pipe;
    Result<Process> p = Process::spawn(d);
    REQUIRE(p.ok());
    Result<ProcessOutput> r = p->communicate(big);
    REQUIRE(r.ok());
    CHECK(r->exitCode == 0);
    CHECK(r->out.size() == big.size());
    CHECK(r->out == big); // binary-exact (no CRLF or ^Z translation on Windows)

    Result<ProcessOutput> e = runProcess(childDesc({"stderr", "to stderr"}));
    REQUIRE(e.ok());
    CHECK(e->out == "out");
    CHECK(e->err == "to stderr");

    ProcessDesc merged = childDesc({"stderr", "E"});
    merged.stdoutMode = StdioMode::Pipe;
    merged.mergeStderrIntoStdout = true;
    Result<ProcessOutput> m = runProcess(merged);
    REQUIRE(m.ok());
    CHECK(m->out.size() == 4); // "E" and "out" in the order the child wrote them
    CHECK(m->out == "Eout");
    CHECK(m->err.empty());
}

TEST_CASE("process: null stdio") {
    ProcessDesc d = childDesc({"cat"});
    d.stdinMode = StdioMode::Null;
    d.stdoutMode = StdioMode::Pipe;
    d.stderrMode = StdioMode::Null;
    Result<Process> p = Process::spawn(d);
    REQUIRE(p.ok());
    CHECK(!p->stdinPipe().isOpen());
    CHECK(!p->stderrPipe().isOpen());
    Result<ProcessOutput> r = p->communicate();
    REQUIRE(r.ok());
    CHECK(r->exitCode == 0);
    CHECK(r->out.empty());
}

TEST_CASE("process: environment overrides, removal and a clean environment") {
    ProcessDesc d = childDesc({"env", "HELIOS_TEST_A", "HELIOS_TEST_UNICODE", "PATH"});
    d.environment = {{"HELIOS_TEST_A", "alpha beta"}, {"HELIOS_TEST_UNICODE", "\xc3\xa9t\xc3\xa9"}};
    d.unsetEnvironment = {"PATH"};
    Result<ProcessOutput> r = runProcess(d);
    REQUIRE(r.ok());
    const std::vector<std::string> got = lines(r->out);
    REQUIRE(got.size() == 3);
    CHECK(got[0] == "HELIOS_TEST_A=alpha beta");
    CHECK(got[1] == "HELIOS_TEST_UNICODE=\xc3\xa9t\xc3\xa9");
    CHECK(got[2] == "PATH unset");

    // Overriding twice keeps the last value.
    ProcessDesc twice = childDesc({"env", "HELIOS_TEST_A"});
    twice.environment = {{"HELIOS_TEST_A", "1"}, {"HELIOS_TEST_A", "2"}};
    CHECK(lines(runProcess(twice)->out).at(0) == "HELIOS_TEST_A=2");

    ProcessDesc clean = childDesc({"env", "HOME", "USERPROFILE", "HELIOS_ONLY"});
    clean.inheritEnvironment = false;
    clean.environment = {{"HELIOS_ONLY", "yes"}};
#if defined(_WIN32)
    clean.environment.push_back({"SystemRoot", "C:\\Windows"}); // the Windows loader wants it
#endif
    const std::vector<std::string> cleanOut = lines(runProcess(clean)->out);
    REQUIRE(cleanOut.size() == 3);
    CHECK(cleanOut[0] == "HOME unset");
    CHECK(cleanOut[1] == "USERPROFILE unset");
    CHECK(cleanOut[2] == "HELIOS_ONLY=yes");
}

TEST_CASE("process: working directory") {
    Result<fs::Path> dir = fs::createUniqueTempDirectory("helios-proc \xc3\xa9");
    REQUIRE(dir.ok());
    ProcessDesc d = childDesc({"cwd"});
    d.workingDirectory = *dir;
    Result<ProcessOutput> r = runProcess(d);
    REQUIRE(r.ok());
    CHECK(r->exitCode == 0);
    // canonical() on both sides: Windows temp paths may come back in 8.3 short form.
    CHECK(std::filesystem::canonical(fs::pathFromUtf8(r->out)) == std::filesystem::canonical(*dir));
    CHECK(fs::removeAll(*dir).ok());

    ProcessDesc missing = childDesc({"cwd"});
    missing.workingDirectory = fs::pathFromUtf8("/definitely/not/a/helios/dir");
    // Either spawning fails, or (never on supported platforms) the child fails: never success.
    Result<ProcessOutput> m = runProcess(missing);
    CHECK((!m.ok() || m->exitCode != 0));
}

TEST_CASE("process: wait with a timeout, kill and exit code 137") {
    Result<Process> p = Process::spawn(childDesc({"sleep", "20000"}));
    REQUIRE(p.ok());
    Stopwatch sw;
    Result<std::optional<i32>> w = p->wait(50ms);
    REQUIRE(w.ok());
    CHECK(!w->has_value());
    CHECK(sw.elapsedMillis() >= 40.0);
    CHECK(p->isRunning());
    REQUIRE(p->kill().ok());
    Result<std::optional<i32>> done = p->wait(10000ms);
    REQUIRE(done.ok());
    CHECK(done->value_or(-1) == kKilledExitCode);
    CHECK(sw.elapsedMillis() < 10000.0);
}

TEST_CASE("process: spawn failures are errors, not children") {
    ProcessDesc d;
    d.executable = fs::pathFromUtf8("definitely-not-a-helios-program-1234");
    Result<Process> p = Process::spawn(d);
    CHECK(!p.ok());
    CHECK(p.errorCode() == ErrorCode::NotFound);
    ProcessDesc empty;
    CHECK(Process::spawn(empty).errorCode() == ErrorCode::InvalidArgument);
    CHECK(Process().wait().errorCode() == ErrorCode::InvalidState);
}

TEST_CASE("process: only whitelisted handles are inherited") {
    // Positive: the child writes to a pipe it inherited through the whitelist (the launcher's
    // launch-code channel works this way).
    Result<Pipe> pipe = Pipe::create();
    REQUIRE(pipe.ok());
    ProcessDesc d = childDesc({"write-handle", std::to_string(pipe->write.native()), "launch-code:1234"});
    d.inheritHandles = {pipe->write.native()};
    Result<Process> p = Process::spawn(d);
    REQUIRE(p.ok());
    pipe->write.close(); // the child holds the only other copy
    Result<std::string> got = pipe->read.readAll();
    REQUIRE(got.ok());
    CHECK(*got == "launch-code:1234");
    CHECK(p->wait()->value_or(-1) == 0);

    // Negative: a pipe that is not listed must not leak into a long-running child. If it leaked,
    // the read below would only see end-of-stream when the child exits (20 s).
    Result<Pipe> secret = Pipe::create();
    REQUIRE(secret.ok());
    Result<Process> sleeper = Process::spawn(childDesc({"sleep", "20000"}));
    REQUIRE(sleeper.ok());
    secret->write.close();
    char c = 0;
    Result<usize> n = secret->read.read(&c, 1);
    REQUIRE(n.ok());
    CHECK(*n == 0);
    CHECK(sleeper->isRunning());
    REQUIRE(sleeper->kill().ok());
    CHECK(sleeper->wait()->value_or(-1) == kKilledExitCode);
}

TEST_CASE("process: pipes and adopted handles") {
    Result<Pipe> pipe = Pipe::create();
    REQUIRE(pipe.ok());
    // More than any pipe buffer: the writer blocks until the reader drains it.
    std::thread writer([&] {
        CHECK(pipe->write.write("hello ").ok());
        CHECK(pipe->write.write(std::string(100000, 'x')).ok());
        pipe->write.close();
    });
    Result<std::string> all = pipe->read.readAll();
    writer.join();
    REQUIRE(all.ok());
    CHECK(all->size() == 6 + 100000);
    CHECK(all->substr(0, 6) == "hello ");
    // Writing to a pipe whose reader is gone fails instead of killing the process (SIGPIPE).
    Result<Pipe> broken = Pipe::create();
    REQUIRE(broken.ok());
    broken->read.close();
    CHECK(!broken->write.write("x").ok());
    PipeEnd closed;
    CHECK(closed.read(&closed, 1).errorCode() == ErrorCode::InvalidState);
    const NativeHandle raw = broken->write.release();
    PipeEnd adopted = PipeEnd::adopt(raw);
    CHECK(adopted.native() == raw);
}

TEST_CASE("process: UTF-8 code page") {
    // POSIX: always. Windows: set by the Helios manifest every test executable embeds.
    CHECK(activeCodePageIsUtf8());
    CHECK(standardOutputHandle() != kInvalidNativeHandle);
}

TEST_CASE("process: a relative executable path is relative to the parent, also with a working directory") {
    // Regression (POSIX): posix_spawn resolved the relative path after the child's chdir, so the
    // same ProcessDesc worked on Windows (CreateProcessW resolves against the parent) and failed on
    // Linux.
    const std::filesystem::path absolute = fs::pathFromUtf8(HELIOS_PROCESS_CHILD_PATH);
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(absolute, ec);
    REQUIRE(!ec);
    REQUIRE(!relative.empty());
    REQUIRE(relative.is_relative());
    Result<fs::Path> dir = fs::createUniqueTempDirectory("helios-proc-rel");
    REQUIRE(dir.ok());
    ProcessDesc d;
    d.executable = relative;
    d.args = {"cwd"};
    d.workingDirectory = *dir;
    Result<ProcessOutput> r = runProcess(d);
    REQUIRE(r.ok());
    CHECK(r->exitCode == 0);
    CHECK(std::filesystem::canonical(fs::pathFromUtf8(r->out)) == std::filesystem::canonical(*dir));
    CHECK(fs::removeAll(*dir).ok());
}

TEST_CASE("process: strings that would be truncated or reinterpreted are rejected") {
    ProcessDesc nulArg = childDesc({"args", std::string("a\0b", 3)});
    CHECK(Process::spawn(nulArg).errorCode() == ErrorCode::InvalidArgument);
    // "PATH=/evil:" as a *name* would otherwise produce the entry "PATH=/evil:=x", i.e. set PATH.
    ProcessDesc eqName = childDesc({"env", "PATH"});
    eqName.environment = {{"PATH=/evil:", "x"}};
    CHECK(Process::spawn(eqName).errorCode() == ErrorCode::InvalidArgument);
    ProcessDesc emptyName = childDesc({"env"});
    emptyName.environment = {{"", "x"}};
    CHECK(Process::spawn(emptyName).errorCode() == ErrorCode::InvalidArgument);
    ProcessDesc nulValue = childDesc({"env"});
    nulValue.environment = {{"HELIOS_X", std::string("a\0b", 3)}};
    CHECK(Process::spawn(nulValue).errorCode() == ErrorCode::InvalidArgument);
    ProcessDesc badUnset = childDesc({"env"});
    badUnset.unsetEnvironment = {"A=B"};
    CHECK(Process::spawn(badUnset).errorCode() == ErrorCode::InvalidArgument);
}

#if defined(_WIN32)
TEST_CASE("process: batch files are refused (cmd.exe re-parses their arguments)") {
    for (const char* name : {"C:\\helios\\run.bat", "C:\\helios\\RUN.CMD", "run.bat. ", "tools\\x.Bat..."}) {
        ProcessDesc d;
        d.executable = fs::pathFromUtf8(name);
        d.args = {"\"&calc.exe"};
        CHECK(Process::spawn(d).errorCode() == ErrorCode::InvalidArgument);
    }
}
#else
TEST_CASE("process: descriptors opened without close-on-exec do not leak into the child") {
    // fopen() without "e" (like many third-party libraries and sockets created without
    // SOCK_CLOEXEC) makes an inheritable descriptor. The whitelist must still be exact.
    Result<fs::Path> dir = fs::createUniqueTempDirectory("helios-proc-fd");
    REQUIRE(dir.ok());
    const fs::Path file = *dir / "leak.txt";
    REQUIRE(fs::writeTextFile(file, "x").ok());
    std::FILE* leaky = std::fopen(fs::pathToUtf8(file).c_str(), "rb");
    REQUIRE(leaky != nullptr);
    const int leakyFd = fileno(leaky);
    Result<Pipe> listed = Pipe::create();
    REQUIRE(listed.ok());
    ProcessDesc d = childDesc({"fd-open", std::to_string(leakyFd), std::to_string(listed->write.native())});
    d.inheritHandles = {listed->write.native()};
    Result<ProcessOutput> r = runProcess(d);
    std::fclose(leaky);
    REQUIRE(r.ok());
    const std::vector<std::string> got = lines(r->out);
    REQUIRE(got.size() == 2);
    CHECK(got[0] == "closed"); // not whitelisted: must not reach the child
    CHECK(got[1] == "open");   // whitelisted: still inherited
    CHECK(fs::removeAll(*dir).ok());
}
#endif
