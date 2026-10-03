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
    CHECK(m.find("Loose.fixed") == std::string::npos);   // T[N] of bounded elements needs no @max
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

TEST_CASE("lint: elements that cannot carry @max are size.unbounded findings") {
    // @max bounds a field's own length or count; a string, Name, TagSet or container *inside* a list,
    // set, map or T[N] (map keys included) has no bound the language can express.
    auto c = compileText(R"(package test;
struct Bag {
  keys: map<string, u8> @max(4)
  values: map<u8, list<u8>> @max(4)
  history: list<list<i16>> @max(4)
  tags: list<TagSet> @max(2)
  ok: list<u8> @max(4)
  okMap: map<u8, f32> @max(4)
  okFixed: f32[3][2]
}
struct Holder { inner: variant { Empty; Text { s: string } } }
rpc Strings(xs: string[4], ys: list<string> @max(4), maybe: string?, bag: Bag, h: Holder) client->server reliable @ratelimit(1/s) @intent(chat);
)",
                         lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const std::string& m = c->messages;
    INFO(m);
    CHECK(m.find("'Strings.xs' (string[4]) is network input with elements of type string, which cannot carry @max") != std::string::npos);
    CHECK(m.find("'Strings.ys' (list<string>) is network input with elements of type string") != std::string::npos);
    CHECK(m.find("'Strings.maybe' (string?) is network input without @max") != std::string::npos);
    CHECK(m.find("'Bag.keys' (map<string,u8>) is reachable from network input with elements of type string") != std::string::npos);
    CHECK(m.find("'Bag.values' (map<u8,list<u8>>) is reachable from network input with elements of type list<u8>") != std::string::npos);
    CHECK(m.find("'Bag.history' (list<list<i16>>) is reachable from network input with elements of type list<i16>") != std::string::npos);
    CHECK(m.find("'Bag.tags' (list<TagSet>) is reachable from network input with elements of type TagSet") != std::string::npos);
    CHECK(m.find("'Text.s' (string) is reachable from network input without @max") != std::string::npos); // a variant alternative
    CHECK(m.find("Bag.ok'") == std::string::npos);
    CHECK(m.find("Bag.okMap") == std::string::npos);
    CHECK(m.find("Bag.okFixed") == std::string::npos);
    CHECK(m.find("Strings.bag") == std::string::npos); // a struct is bounded by its fields
    CHECK(c->output("lint.json")->find(R"("size.unbounded-checked": 11)") != std::string::npos);
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
    // Each entry counts its tag, length and a keyed list's key (22 B) on top of P's 33 B: 21 entries
    // fit (1 + 21 * 55 = 1,156 B), 22 do not (1,211 B). Without the overhead both would fit.
    CHECK(verdict("l: list<P> @max(21)").find("size.unreliable") == std::string::npos);
    CHECK(verdict("l: list<P> @max(22)").find("worst-case payload of 1211 B") != std::string::npos);
    // Service rpcs are listed under their service in the SEC-1 table.
    auto c = compileText("package test;\nstruct Req { n: u32 }\nservice Shop { rpc Buy(req: Req) client->server @ratelimit(2/s) @intent(buy); }\n",
                         lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK(c->output("lint.json")->find(R"({"rpc": "test.Shop.Buy", "ratelimit": "2/s", "intent": "buy", "reliable": true})") !=
          std::string::npos);
}

} // namespace
