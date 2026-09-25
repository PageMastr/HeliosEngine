// helios-schemac command line: options, exit codes, outputs on disk, depfiles, lock handling and
// the "not yet implemented" generators.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cli.h"

namespace fs = std::filesystem;
using helios::schemac::runCli;

namespace {

struct Run {
    int status = 0;
    std::string out;
    std::string err;
};

Run cli(std::vector<std::string> args) {
    Run r;
    r.status = runCli(args, r.out, r.err);
    return r;
}

fs::path freshDir(const std::string& name) {
    const fs::path dir = fs::path(HELIOS_TEST_WORK_DIR) / "cli" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

const char* kSchema = R"(package cli.test;
/// A thing.
struct Thing { a: u8 = 1; b: string }
enum Kind : u8 { X, Y }
)";

} // namespace

TEST_CASE("cli: help, version and usage errors") {
    Run help = cli({"--help"});
    CHECK(help.status == 0);
    CHECK(help.out.starts_with("usage: helios-schemac [options] <file.hschema>..."));
    CHECK(help.out.find("planned generators (not yet implemented): luau, repl, sql, proto, editor, records, lint") != std::string::npos);
    Run version = cli({"--version"});
    CHECK(version.status == 0);
    CHECK(version.out.starts_with("helios-schemac "));

    Run unknown = cli({"--frobnicate", "x.hschema"});
    CHECK(unknown.status == 2);
    CHECK(unknown.err == "helios-schemac: error: unknown option '--frobnicate' (see --help)\n");
    Run missing = cli({"x.hschema", "--lock"});
    CHECK(missing.status == 2);
    CHECK(missing.err == "helios-schemac: error: --lock needs a value\n");
    Run none = cli({"--emit", "cpp"});
    CHECK(none.status == 2);
    CHECK(none.err == "helios-schemac: error: no input files (see --help)\n");
    Run badGen = cli({"--emit=cpp,rust", "x.hschema"});
    CHECK(badGen.status == 2);
    CHECK(badGen.err == "helios-schemac: error: unknown generator 'rust' (available: cpp, go, json)\n");
}

TEST_CASE("cli: planned generators fail with 'not yet implemented'") {
    for (const char* gen : {"luau", "repl", "sql", "proto", "editor", "records", "lint", "docs"}) {
        Run r = cli({"--emit", std::string("cpp,") + gen, "x.hschema"});
        CHECK(r.status == 2);
        CHECK_MESSAGE(r.err.starts_with(std::string("helios-schemac: error: --emit ") + gen + " is not yet implemented"), r.err);
        CHECK(r.out.empty());
    }
}

TEST_CASE("cli: generates outputs, lock and depfile; reruns are no-ops") {
    const fs::path dir = freshDir("generate");
    const fs::path schema = dir / "schemas" / "cli" / "thing.hschema";
    writeFile(schema, kSchema);
    const fs::path lock = dir / "schema.lock.jsonc";
    const fs::path depfile = dir / "out" / "thing.d";
    const std::vector<std::string> args = {"-I",        (dir / "schemas").string(), "--lock",   lock.string(),
                                           "--emit",    "cpp,go,json",              "--cpp-out", (dir / "out" / "cpp").string(),
                                           "--go-out",  (dir / "out" / "go").string(), "--json-out", (dir / "out" / "schema.json").string(),
                                           "--depfile", depfile.string(),           schema.string()};
    Run first = cli(args);
    REQUIRE_MESSAGE(first.status == 0, first.err);
    CHECK(first.err.empty());
    CHECK(first.out.find("updated schema lock") != std::string::npos);
    CHECK(first.out.find("  new type cli.test.Thing (id ") != std::string::npos);
    CHECK(first.out.find("helios-schemac: 1 file compiled, 7 outputs (7 updated)") != std::string::npos);
    CHECK(fs::exists(dir / "out" / "cpp" / "cli" / "thing.gen.h"));
    CHECK(fs::exists(dir / "out" / "cpp" / "cli" / "thing.gen.cpp"));
    CHECK(fs::exists(dir / "out" / "go" / "thing.go"));
    CHECK(fs::exists(dir / "out" / "go" / "helios_runtime.go"));
    CHECK(fs::exists(dir / "out" / "schema.json"));
    CHECK(readFile(dir / "out" / "go" / "thing.go").find("package test") != std::string::npos);
    const std::string lockText = readFile(lock);
    CHECK(lockText.find("\"cli.test.Thing\"") != std::string::npos);
    const std::string dep = readFile(depfile);
    CHECK(dep.find("thing.gen.h:") != std::string::npos);
    CHECK(dep.find("thing.hschema") != std::string::npos);

    const auto stamp = fs::last_write_time(dir / "out" / "cpp" / "cli" / "thing.gen.h");
    Run second = cli(args);
    REQUIRE_MESSAGE(second.status == 0, second.err);
    CHECK(second.out == "helios-schemac: 1 file compiled, 7 outputs (0 updated)\n");
    CHECK(fs::last_write_time(dir / "out" / "cpp" / "cli" / "thing.gen.h") == stamp);
    CHECK(readFile(lock) == lockText);
    // Now the lock is an input.
    CHECK(readFile(depfile).find("schema.lock.jsonc") != std::string::npos);

    // --check-lock: fine while current, exit 1 once the schema adds a field.
    std::vector<std::string> check = args;
    check.insert(check.begin(), "--check-lock");
    CHECK(cli(check).status == 0);
    writeFile(schema, std::string(kSchema) + "struct More { c: u8 }\n");
    Run stale = cli(check);
    CHECK(stale.status == 1);
    CHECK(stale.err.find("is out of date") != std::string::npos);
    CHECK(stale.err.find("note: new type cli.test.More") != std::string::npos);
    CHECK(readFile(lock) == lockText);

    // --quiet still reports that the build modified the lock.
    std::vector<std::string> quiet = args;
    quiet.insert(quiet.begin(), "--quiet");
    Run q = cli(quiet);
    REQUIRE(q.status == 0);
    CHECK(q.out.starts_with("helios-schemac: updated schema lock "));
    CHECK(q.out.find("new type") == std::string::npos);
}

TEST_CASE("cli: schema errors exit 1 with file:line:col and a source excerpt") {
    const fs::path dir = freshDir("errors");
    const fs::path schema = dir / "bad.hschema";
    writeFile(schema, "package p;\nstruct A { x: f33 }\n");
    Run r = cli({"--no-naming-lints", schema.string()});
    CHECK(r.status == 1);
    CHECK_MESSAGE(r.err.find("bad.hschema:2:15: error: unknown type 'f33'; did you mean 'f32'?\n    struct A { x: f33 }\n                  ^\n") !=
                      std::string::npos,
                  r.err);
    CHECK_FALSE(fs::exists(dir / "bad.gen.h"));

    Run missing = cli({(dir / "nope.hschema").string()});
    CHECK(missing.status == 1);
    CHECK(missing.err.find("error: cannot read schema file") != std::string::npos);

    // Warnings as errors.
    writeFile(schema, "package p;\nstruct lower { x: u8 }\n");
    Run warn = cli({"--cpp-out", (dir / "out").string(), schema.string()});
    CHECK(warn.status == 0);
    CHECK(warn.err.find("warning: type name 'lower' should be PascalCase") != std::string::npos);
    CHECK(warn.err.find("warning: no --lock file given") != std::string::npos);
    Run werror = cli({"--Werror", "--cpp-out", (dir / "out").string(), schema.string()});
    CHECK(werror.status == 1);
}

TEST_CASE("cli: unwritable outputs exit 3") {
    const fs::path dir = freshDir("io");
    const fs::path schema = dir / "ok.hschema";
    writeFile(schema, kSchema);
    writeFile(dir / "blocker", "a file where a directory is needed");
    Run r = cli({"--no-naming-lints", "--cpp-out", (dir / "blocker" / "sub").string(), schema.string()});
    CHECK(r.status == 3);
    CHECK_MESSAGE(r.err.find("helios-schemac: error: cannot ") != std::string::npos, r.err);
}

TEST_CASE("cli: parallel runs sharing one lock never lose each other's entries") {
    // Several helios_schema() calls (one per module) may point at the same committed lock and run
    // in parallel; each run reads, extends and rewrites it under a cross-process mutex.
    constexpr int kWriters = 8;
    for (int round = 0; round < 3; ++round) {
        const fs::path dir = freshDir("parallel" + std::to_string(round));
        const fs::path lock = dir / "lock" / "schema.lock.jsonc"; // the lock directory does not exist yet
        std::vector<std::vector<std::string>> argsOf;
        for (int i = 0; i < kWriters; ++i) {
            const fs::path schema = dir / "schemas" / ("m" + std::to_string(i)) / "types.hschema";
            writeFile(schema, "package m" + std::to_string(i) + ";\nstruct T" + std::to_string(i) + " { a: u8; b: string }\n");
            argsOf.push_back({"-I", (dir / "schemas").string(), "--lock", lock.string(), "--quiet", "--cpp-out",
                              (dir / "out").string(), schema.string()});
        }
        std::vector<Run> runs(kWriters);
        std::vector<std::thread> threads;
        for (int i = 0; i < kWriters; ++i) {
            threads.emplace_back([&, i] { runs[static_cast<size_t>(i)] = cli(argsOf[static_cast<size_t>(i)]); });
        }
        for (std::thread& t : threads) t.join();
        for (const Run& r : runs) CHECK_MESSAGE(r.status == 0, r.err);
        const std::string text = readFile(lock);
        for (int i = 0; i < kWriters; ++i) {
            const std::string name = "\"m" + std::to_string(i) + ".T" + std::to_string(i) + "\"";
            CHECK_MESSAGE(text.find(name) != std::string::npos, name << " missing from\n" << text);
        }
        CHECK_FALSE(fs::exists(fs::path(lock.string() + ".writing")));
        // Every module's run is now a no-op against the merged lock.
        for (int i = 0; i < kWriters; ++i) CHECK(cli(argsOf[static_cast<size_t>(i)]).out.empty());
    }
}

TEST_CASE("cli: a lock mutex left behind by a killed run is taken over") {
    const fs::path dir = freshDir("stale");
    const fs::path schema = dir / "schemas" / "cli" / "thing.hschema";
    writeFile(schema, kSchema);
    const fs::path lock = dir / "schema.lock.jsonc";
    const fs::path mutexDir = fs::path(lock.string() + ".writing");
    fs::create_directories(mutexDir);
    fs::last_write_time(mutexDir, fs::file_time_type::clock::now() - std::chrono::hours(1));
    Run r = cli({"-I", (dir / "schemas").string(), "--lock", lock.string(), "--cpp-out", (dir / "out").string(), schema.string()});
    CHECK_MESSAGE(r.status == 0, r.err);
    CHECK(fs::exists(lock));
    CHECK_FALSE(fs::exists(mutexDir));
}
