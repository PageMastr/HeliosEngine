// `--emit lint` (02 §3.5): the size-budget lints and the lint report. The rule lints (SEC-1, SEC-4,
// ledger/persist, keyed lists, naming, fuel) fail every compilation and are tested in test_sema.cpp;
// here the report must list what they checked and classify every client->server rpc.

#include <chrono>
#include <format>

#include "generators.h"
#include "helios/net/wire.h"
#include "test_util.h"

using namespace schemac_test;

namespace {

// size.unreliable's budget is the largest message engine/net sends unfragmented on every unreliable
// channel (LATEST and EVENT_R: 1,186 B; EVENT_U: 1,188 B), not the 1,200 B netcode payload.
namespace wire = helios::net::wire;
static_assert(helios::schemac::kLintUnreliableBudget == wire::maxPayloadFor(helios::net::Channel::Latest, wire::kMaxPacketPayload));
static_assert(helios::schemac::kLintUnreliableBudget <= wire::maxPayloadFor(helios::net::Channel::EventUnreliable, wire::kMaxPacketPayload));

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
    // A struct reached from two rpcs is reported once, and its fields are counted once: Say.text,
    // Inner.name, Shout.text, Loose.tags, .bounded and .fixed, Note.body and Hp.names.
    usize count = 0;
    for (usize at = m.find("'Inner.name'"); at != std::string::npos; at = m.find("'Inner.name'", at + 1)) ++count;
    CHECK(count == 1);
    const std::string* report = c->output("lint.json");
    REQUIRE(report);
    CHECK(report->find(R"("size.unbounded-checked": 8)") != std::string::npos);
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
    CHECK(big.find("[size.unreliable] unreliable rpc 'R' has a worst-case payload of 1203 B (budget 1186 B") != std::string::npos);
    // The budget's edge: a lone string's tag (1 B) and length (2 B) plus @max bytes. The round-1 probe
    // (@max(1197), 1,200 B) passed against the netcode payload although the string alone is too big.
    CHECK(verdict("s: string @max(1183)").find("size.unreliable") == std::string::npos);
    CHECK(verdict("s: string @max(1184)").find("worst-case payload of 1187 B") != std::string::npos);
    CHECK(verdict("s: string @max(1197)").find("size.unreliable") != std::string::npos);
    // The worst case saturates: 10 + (2^64 - 1) wrapped to 9 B and passed.
    CHECK(verdict("s: string @max(18446744073709551615)").find("worst-case payload of 18446744073709551615 B") != std::string::npos);
    CHECK(verdict("l: list<P> @max(18446744073709551615)").find("size.unreliable") != std::string::npos);
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

TEST_CASE("lint: rpc results are network input") {
    // The round-1 probe: an rpc's `-> T` was never walked. A server->client rpc's result is the client's
    // reply, which the server decodes.
    auto c = compileText(R"(package test;
struct Resp { s: string }
struct Ok { n: u8 }
rpc Ask(q: u8) -> Resp server->client reliable;
rpc Label(q: u8) -> string server->client reliable;
rpc Fine(q: u8) -> Ok server->client reliable;
service Shop { rpc Buy(n: u32) -> Resp; }
)",
                         lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const std::string& m = c->messages;
    INFO(m);
    CHECK(m.find("'Resp.s' (string) is reachable from network input without @max") != std::string::npos);
    CHECK(m.find("'Label' result (string) is network input and cannot carry @max: return a struct with a bounded field") != std::string::npos);
    CHECK(m.find("Fine") == std::string::npos);
    CHECK(m.find("Ok.n") == std::string::npos);
    // Resp.s is counted once although Ask and Shop.Buy both return it; the string result counts too.
    CHECK(c->output("lint.json")->find(R"("size.unbounded-checked": 2)") != std::string::npos);
    // An unreliable rpc's result is a message within the budget too.
    auto unreliable = compileText("package test;\nstruct Big { s: string @max(2000) }\nrpc R(q: u8) -> Big server->client unreliable;\n",
                                  lintOptions());
    CHECK(unreliable->messages.find("unreliable rpc 'R' result has a worst-case payload of 2003 B") != std::string::npos);
}

TEST_CASE("lint: text builtins take @max like string") {
    // The round-1 probe: size.unbounded asked for @max on LocString, TagQuery and HxlExpr, which sema
    // rejected, so the finding had no remedy. They are strings on the wire and now take a byte bound.
    auto loose = compileText("package test;\nevent Notice @audience(owner) { text: LocString; q: TagQuery; e: HxlExpr }\n", lintOptions());
    REQUIRE_MESSAGE(loose->ok(), loose->messages);
    CHECK(loose->messages.find("'Notice.text' (LocString) is network input without @max") != std::string::npos);
    auto bounded = compileText("package test;\nevent Notice @audience(owner) { text: LocString @max(64); q: TagQuery @max(256); e: HxlExpr? @max(512) }\n"
                               "rpc Cue(t: LocString @max(64)) server->client unreliable;\n",
                               lintOptions());
    REQUIRE_MESSAGE(bounded->ok(), bounded->messages);
    CHECK(bounded->messages.find("size.") == std::string::npos);
    // The Luau glue checks a text builtin's bytes like a string's, in arguments and in result fields.
    CompileOptions luau;
    luau.emitLuau = true;
    luau.cppOut = "cpp";
    auto glue = compileText("package test;\nstruct Line { text: LocString @max(8); q: TagQuery @max(4) }\n"
                            "scriptlib Lib @realm(server) { fn say(l: LocString @max(16)) -> Line @script(cost=1); }\n",
                            luau);
    REQUIRE_MESSAGE(glue->ok(), glue->messages);
    const std::string* cpp = glue->output("t.luau.gen.cpp");
    REQUIRE(cpp);
    CHECK(cpp->find("if (v.text.key.size() > 8u)") != std::string::npos);
    CHECK(cpp->find("if (v.q.text.size() > 4u)") != std::string::npos);
    CHECK(cpp->find("checkMax(c, 1, 16u, \"l\");") != std::string::npos);
}

TEST_CASE("lint: findings and the SEC-1 table sort by what they print") {
    // clientToServer sorted by the service rpc's argument struct (test.ShopBuyRequest) printed
    // test.ShopA before test.Shop.Buy; findings sorted by the physical path, not the printed one.
    auto c = compileText("package test;\nrpc ShopA(n: u8) client->server reliable @ratelimit(1/s) @intent(chat);\n"
                         "service Shop { rpc Buy(n: u32) client->server @ratelimit(2/s) @intent(buy); }\n",
                         lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    const std::string* report = c->output("lint.json");
    REQUIRE(report);
    CHECK(report->find("\"test.Shop.Buy\"") < report->find("\"test.ShopA\""));
    CompileOptions two = lintOptions();
    two.includeDirs = {"r1/inc", "r2"};
    two.files = {"r1/inc/b.hschema", "r2/a.hschema"};
    auto files = compileFiles({{"r1/inc/b.hschema", "package b;\nrpc Rb(s: string) server->client reliable;\n"},
                               {"r2/a.hschema", "package a;\nrpc Ra(s: string) server->client reliable;\n"}},
                              two);
    REQUIRE_MESSAGE(files->ok(), files->messages);
    const std::string* both = files->output("lint.json");
    REQUIRE(both);
    INFO(*both);
    CHECK(both->find(R"("file": "a.hschema")") < both->find(R"("file": "b.hschema")"));
}

TEST_CASE("lint: an @unreliable event (EVENT_U) must fit one netcode payload") {
    // The round-2 probe: an @unreliable event goes on EVENT_U (fire-and-forget FX), which engine/net
    // never fragments (wire::maxPayloadFor(EventUnreliable) = 1,188 B), and it was never checked.
    auto c = compileText("package test;\nevent Impact @audience(relevant) @unreliable { fx: string @max(4000) }\n", lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK(c->messages.find("size.unreliable") != std::string::npos);
    CHECK_MESSAGE(c->messages.find("[size.unreliable] unreliable event 'Impact' has a worst-case payload of 4003 B (budget 1186 B") !=
                      std::string::npos,
                  c->messages);
    CHECK(c->output("lint.json")->find(R"("size.unreliable-events": 1)") != std::string::npos);
    // The same edge as an rpc's: a lone string's tag (1 B) and length (2 B) plus @max bytes.
    auto edge = [](u32 max) {
        return compileText(std::format("package test;\nevent Impact @audience(relevant) @unreliable {{ fx: string @max({}) }}\n", max),
                           lintOptions())
            ->messages;
    };
    CHECK(edge(1183).find("size.unreliable") == std::string::npos);
    CHECK(edge(1184).find("unreliable event 'Impact' has a worst-case payload of 1187 B") != std::string::npos);
    // --Werror makes it an error, like an rpc's.
    CompileOptions strict = lintOptions();
    strict.warningsAsErrors = true;
    CHECK_FALSE(compileText("package test;\nevent Impact @audience(relevant) @unreliable { fx: string @max(4000) }\n", strict)->ok());
    // A reliable event (EVENT_R) is WP-1.10's to check, with reliable rpcs.
    auto reliable = compileText("package test;\nevent Big @audience(relevant) { fx: string @max(4000) }\n", lintOptions());
    REQUIRE_MESSAGE(reliable->ok(), reliable->messages);
    CHECK(reliable->messages.find("size.unreliable") == std::string::npos);
    CHECK(reliable->output("lint.json")->find(R"("size.unreliable-events": 0)") != std::string::npos);
}

TEST_CASE("lint: a type reached as a network type and as a field type is reported once") {
    // The round-2 probe: a message used as an event's field type was walked twice, at depth 0 and 1, and
    // 'Note.body' was reported as network input and again as reachable from it. Either declaration
    // order reports it once, worded for the shallower depth.
    for (const char* text : {"package test;\nmessage Note { body: string }\nevent E @audience(owner) { n: Note }\n",
                             "package test;\nevent E @audience(owner) { n: Note }\nmessage Note { body: string }\n"}) {
        auto c = compileText(text, lintOptions());
        REQUIRE_MESSAGE(c->ok(), c->messages);
        const std::string& m = c->messages;
        INFO(m);
        usize count = 0;
        for (usize at = m.find("'Note.body'"); at != std::string::npos; at = m.find("'Note.body'", at + 1)) ++count;
        CHECK(count == 1);
        CHECK(m.find("'Note.body' (string) is network input without @max") != std::string::npos);
        const std::string* report = c->output("lint.json");
        REQUIRE(report);
        usize inReport = 0;
        for (usize at = report->find("'Note.body'"); at != std::string::npos; at = report->find("'Note.body'", at + 1)) ++inReport;
        CHECK(inReport == 1);
    }
    // A field with its own finding and its elements' keeps both.
    auto two = compileText("package test;\nmessage M { xs: list<string> }\nevent E @audience(owner) { m: M }\n", lintOptions());
    REQUIRE_MESSAGE(two->ok(), two->messages);
    CHECK(two->messages.find("'M.xs' (list<string>) is network input without @max") != std::string::npos);
    CHECK(two->messages.find("'M.xs' (list<string>) is network input with elements of type string") != std::string::npos);
}

TEST_CASE("lint: service rpc arguments are named after the rpc") {
    // The round-2 probe: arguments were named after the synthesized request struct (BankBalanceRequest).
    auto c = compileText("package test;\nservice Bank { rpc Balance(currency: string); }\n", lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK_MESSAGE(c->messages.find("'Bank.Balance.currency' (string) is network input without @max") != std::string::npos, c->messages);
    CHECK(c->messages.find("BalanceRequest") == std::string::npos);
}

TEST_CASE("lint: @max on a TagSet bounds its encoded tags in bytes") {
    // A TagSet is a LEN message of tags on the wire; @max(n) bounds those n bytes (each tag's field tag,
    // length and text), as it bounds a string's, so the worst case is exact: 1 B tag and 2 B length.
    auto verdict = [](const std::string& max) {
        return compileText("package test;\nrpc R(t: TagSet @max(" + max + ")) server->client unreliable;\n", lintOptions())->messages;
    };
    CHECK(verdict("4").find("size.") == std::string::npos);
    CHECK(verdict("1183").find("size.unreliable") == std::string::npos);
    CHECK(verdict("1184").find("unreliable rpc 'R' has a worst-case payload of 1187 B") != std::string::npos);
}

TEST_CASE("lint: a keyed list entry's tag and length are counted") {
    // PR #35's round 3: a flat 22 B per entry under-counted a keyed list once its field id is 16 or more
    // (2-byte tag) and an entry 128 B or more (2-byte length). Here `items` is field 16 and each entry
    // holds a 123-byte P: 146 B on the wire, counted as 145. The generated codec encodes the maximal value
    // in 1,191 B, and the lint passed it at 1,186 B. Counted now as 7 * (5 + 2 + 18 + 1 + 123) more.
    auto c = compileText("package test;\nstruct P { s: string @max(120) }\n"
                         "event E @audience(relevant) @unreliable {\n"
                         "  a1: f32; a2: f32; a3: f32; a4: f32; a5: f32; a6: f32; a7: f32; a8: f32\n"
                         "  a9: f32; a10: f32; a11: f32; a12: f32; a13: f32; a14: f32; a15: f32\n"
                         "  items: list<P> @keyed @max(7)\n  pad: string @max(91)\n}\n",
                         lintOptions());
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK_MESSAGE(c->messages.find("unreliable event 'E' has a worst-case payload of 1214 B") != std::string::npos, c->messages);
    // A keyed list with a 1-byte tag and 1-byte entry lengths stays within its count.
    auto small = compileText("package test;\nstruct P { s: string @max(8) }\n"
                             "event E @audience(relevant) @unreliable { items: list<P> @keyed @max(4) }\n",
                             lintOptions());
    REQUIRE_MESSAGE(small->ok(), small->messages);
    CHECK(small->messages.find("size.unreliable") == std::string::npos);
}

TEST_CASE("lint: a --lint-out path that another output uses is an error") {
    // #33's round 6 made two outputs at one path an error. The check runs after the lint pass, so the
    // report is one of the outputs it compares.
    CompileOptions options = lintOptions();
    options.emitCpp = true;
    options.cppOut = "cpp";
    options.lintOut = "cpp/test/t.gen.h";
    auto c = compileText("package test;\nstruct S { a: u8 }\n", options);
    CHECK_FALSE(c->ok());
    CHECK_MESSAGE(c->messages.find("two outputs would be written to 'cpp/test/t.gen.h': give each its own path") != std::string::npos,
                  c->messages);
    options.lintOut = "lint.json";
    auto apart = compileText("package test;\nstruct S { a: u8 }\n", options);
    CHECK_MESSAGE(apart->ok(), apart->messages);
}

TEST_CASE("perf: --emit lint is O(n log n) in its findings") {
    // PR #35's round 3: add() scanned every earlier finding, so the lint was quadratic in its findings
    // (20,000 took 1.07 s, 40,000 took 4.1 s). Budget: 40,000 findings, the frontend included, in <= 1 s
    // (about 0.2 s here; the quadratic scan took 3 s and more).
    std::string text = "package test;\nevent E @audience(relevant) {\n";
    for (int i = 0; i < 40000; ++i) text += std::format("  s{}: string\n", i);
    text += "}\n";
    auto timed = [&](const CompileOptions& options) {
        const auto start = std::chrono::steady_clock::now();
        auto c = compileText(text, options);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        REQUIRE_MESSAGE(c->ok(), c->messages.substr(0, 2000));
        return std::pair(elapsed, std::move(c));
    };
    const auto [plain, plainResult] = timed(CompileOptions{});
    const auto [lint, lintResult] = timed(lintOptions());
    usize findings = 0;
    for (usize at = lintResult->messages.find("[size.unbounded]"); at != std::string::npos; at = lintResult->messages.find("[size.unbounded]", at + 1))
        ++findings;
    CHECK(findings == 40000);
    MESSAGE(std::format("40,000 findings: {:.3f} s with --emit lint, {:.3f} s without", lint, plain));
#if defined(NDEBUG) && !defined(HELIOS_SANITIZERS_ENABLED) && !defined(__SANITIZE_ADDRESS__)
    CHECK(lint <= 1.0);
#endif
}

TEST_CASE("perf: --emit lint is linear in a shared struct graph") {
    // struct S_i { a: S_{i-1}; b: S_{i-1} } doubles the paths per level: without a per-type memo the
    // worst case took 0.73 s at 24 levels and 8.5 s at 28. Budget: 30 levels in <= 0.25 s.
    std::string text = "package test;\nstruct S0 { a: u8 }\n";
    for (int i = 1; i <= 30; ++i) text += std::format("struct S{0} {{ a: S{1}; b: S{1} }}\n", i, i - 1);
    text += "rpc R(s: S30) server->client unreliable;\n";
    const auto start = std::chrono::steady_clock::now();
    auto c = compileText(text, lintOptions());
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK(c->messages.find("size.unreliable") != std::string::npos); // 2^30 * 11 B and more
    MESSAGE(std::format("30 levels in {:.3f} s", elapsed));
#if defined(NDEBUG) && !defined(HELIOS_SANITIZERS_ENABLED) && !defined(__SANITIZE_ADDRESS__)
    CHECK(elapsed <= 0.25);
#endif
}

} // namespace
