// `--emit lint` (02 §3.5): the size-budget lints and the lint report. The rule lints (SEC-1, SEC-4,
// ledger/persist, keyed lists, naming, fuel) fail every compilation and are tested in test_sema.cpp;
// here the report must list what they checked and classify every client->server rpc.

#include "test_util.h"

using namespace schemac_test;

namespace {

CompileOptions lintOptions() {
    CompileOptions options;
    options.emitLint = true;
    options.lintOut = "lint.json";
    options.namingLints = false;
    return options;
}

TEST_CASE("lint: network input without @max is a size.unbounded warning") {
    auto c = compileText(R"(package test;
struct Inner { name: string; ok: u8 }
struct Loose { tags: TagSet; bounded: list<u8> @max(8); fixed: u8[4] }
rpc Say(text: string, inner: Inner) client->server reliable @ratelimit(1/s) @intent(chat);
rpc Shout(text: string @max(64), again: Inner) server->client reliable;
event Seen @audience(owner) { what: Loose }
message Note { body: string @max(4000) }
component Hp replicate(all) { names: list<Name>; server { secret: string } }
struct NotNetwork { free: string }
)",
                         lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const std::string& m = c->messages;
    CHECK(m.find("warning: [size.unbounded] 'Say.text' (string) is network input without @max") != std::string::npos);
    CHECK(m.find("'Inner.name' (string) is reachable from network input") != std::string::npos);
    CHECK(m.find("'Loose.tags' (TagSet)") != std::string::npos);
    CHECK(m.find("'Hp.names' (list<Name>)") != std::string::npos);
    CHECK(m.find("Shout.text") == std::string::npos);   // bounded
    CHECK(m.find("Loose.bounded") == std::string::npos); // bounded
    CHECK(m.find("Loose.fixed") == std::string::npos);   // T[N] is bounded by its type
    CHECK(m.find("Hp.secret") == std::string::npos);     // server {} fields are never replicated
    CHECK(m.find("NotNetwork") == std::string::npos);
    // A struct reached from two rpcs is reported once.
    usize count = 0;
    for (usize at = m.find("'Inner.name'"); at != std::string::npos; at = m.find("'Inner.name'", at + 1)) ++count;
    CHECK(count == 1);
    const std::string* report = c->output("lint.json");
    REQUIRE(report);
    CHECK(report->find(R"({"rule": "size.unbounded", "file": "test/t.hschema", "line": 4, "col": 9, "message": "'Say.text')") != std::string::npos);
    CHECK(report->find(R"({"rpc": "test.Say", "ratelimit": "1/s", "intent": "chat", "reliable": true})") != std::string::npos);
    CHECK(report->find(R"("sec1.client-to-server": 1)") != std::string::npos);
    // --Werror turns the budgets into errors.
    CompileOptions strict = lintOptions();
    strict.warningsAsErrors = true;
    auto failed = compileText("package test;\nrpc Say(text: string) client->server reliable @ratelimit(1/s) @intent(chat);\n", strict);
    CHECK_FALSE(failed->ok());
}

TEST_CASE("lint: an unreliable rpc must fit one netcode payload") {
    auto verdict = [](const std::string& args) {
        return compileText("package test;\nstruct P { a: vec3f; b: quatf }\nrpc R(" + args + ") server->client unreliable;\n", lintOptions())->messages;
    };
    CHECK(verdict("x: u8, p: P, s: string @max(64)").find("size.unreliable") == std::string::npos);
    const std::string big = verdict("s: string @max(1200)");
    CHECK(big.find("[size.unreliable] unreliable rpc 'R' has a worst-case payload of 1203 B (budget 1200 B") != std::string::npos);
    CHECK(verdict("l: list<string> @max(4)").find("worst-case payload of unbounded") != std::string::npos);
    CHECK(verdict("l: list<P> @max(10)").find("size.unreliable") == std::string::npos);
    CHECK(verdict("l: list<P> @max(100)").find("size.unreliable") != std::string::npos);
    // Service rpcs are listed under their service in the SEC-1 table.
    auto c = compileText("package test;\nstruct Req { n: u32 }\nservice Shop { rpc Buy(req: Req) client->server @ratelimit(2/s) @intent(buy); }\n",
                         lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK(c->output("lint.json")->find(R"({"rpc": "test.Shop.Buy", "ratelimit": "2/s", "intent": "buy", "reliable": true})") !=
          std::string::npos);
}

} // namespace
