// AAA-SEC-4 (01 §3.8; 02 §3.3, §6.5): no server-only field, type or record reaches a client cook,
// and a shared field may reference a server-only record only when it is @opaque. The reverse direction
// (no client-only data in the server cook) is checked the same way.
#include <format>

#include "helios/hxl/hxl.h"
#include "test_util.h"

using namespace helios;
using namespace helios::records;
using namespace helios::records::test;

// helios-lint: outside-anon-namespace begin (hand-built sec4t types and their TypeOf<> specializations)
namespace sec4t {
/// Hand-built types: schemac already refuses these schemas, so the cooker's own check is what stops
/// them (generated code is not the only source of TypeInfos: builder types, and dynamic packages later).
struct Secret {
    i32 code = 0;
};
struct Visible {
    i32 code = 0;
};
enum class Mood : u8 { Calm, Angry };
struct RefPlain {
    refl::RecordRef<Secret> secret;
};
struct RefOpaque {
    std::vector<refl::RecordRef<Secret>> secrets;
};
struct Embeds {
    Secret inner;
};
struct UsesMood {
    Mood mood = Mood::Calm;
};
struct RefVisible {
    refl::RecordRef<Visible> visible;
};
} // namespace sec4t

HELIOS_REFLECT_TYPE(sec4t::Secret);
HELIOS_REFLECT_TYPE(sec4t::Visible);
HELIOS_REFLECT_ENUM(sec4t::Mood);
HELIOS_REFLECT_TYPE(sec4t::RefPlain);
HELIOS_REFLECT_TYPE(sec4t::RefOpaque);
HELIOS_REFLECT_TYPE(sec4t::Embeds);
HELIOS_REFLECT_TYPE(sec4t::UsesMood);
HELIOS_REFLECT_TYPE(sec4t::RefVisible);

namespace {
using refl::DeclKind;
using refl::FieldFlags;
using refl::StructBuilder;
using refl::TypeFlags;
using refl::TypeInfo;

/// A builder TypeInfo with extra type flags (the builders have no API for @server_only / @client_only).
const TypeInfo& flagged(const TypeInfo* built, TypeFlags flags) {
    auto* t = new TypeInfo(*built); // process lifetime, like every TypeInfo
    t->flags |= flags;
    return *t;
}
} // namespace

const TypeInfo& helios::refl::TypeOf<sec4t::Secret>::get() noexcept {
    static const TypeInfo& info = flagged(StructBuilder<sec4t::Secret>("sec4.Secret", DeclKind::Record).field("code", &sec4t::Secret::code).build(),
                                          TypeFlags::ServerOnly);
    return info;
}
const TypeInfo& helios::refl::TypeOf<sec4t::Visible>::get() noexcept {
    static const TypeInfo& info = flagged(StructBuilder<sec4t::Visible>("sec4.Visible", DeclKind::Record).field("code", &sec4t::Visible::code).build(),
                                          TypeFlags::ClientOnly);
    return info;
}
const TypeInfo& helios::refl::TypeOf<sec4t::Mood>::get() noexcept {
    static const TypeInfo& info =
        flagged(refl::EnumBuilder<sec4t::Mood>("sec4.Mood").value("Calm", sec4t::Mood::Calm).value("Angry", sec4t::Mood::Angry).build(),
                TypeFlags::ServerOnly);
    return info;
}
const TypeInfo& helios::refl::TypeOf<sec4t::RefPlain>::get() noexcept {
    static const TypeInfo* info = StructBuilder<sec4t::RefPlain>("sec4.RefPlain", DeclKind::Record).field("secret", &sec4t::RefPlain::secret).build();
    return *info;
}
const TypeInfo& helios::refl::TypeOf<sec4t::RefOpaque>::get() noexcept {
    static const TypeInfo* info = StructBuilder<sec4t::RefOpaque>("sec4.RefOpaque", DeclKind::Record)
                                      .field("secrets", &sec4t::RefOpaque::secrets, {.flags = FieldFlags::Opaque})
                                      .build();
    return *info;
}
const TypeInfo& helios::refl::TypeOf<sec4t::Embeds>::get() noexcept {
    static const TypeInfo* info = StructBuilder<sec4t::Embeds>("sec4.Embeds", DeclKind::Record)
                                      .field("inner", &sec4t::Embeds::inner, {.flags = FieldFlags::Opaque})
                                      .build();
    return *info;
}
const TypeInfo& helios::refl::TypeOf<sec4t::UsesMood>::get() noexcept {
    static const TypeInfo* info = StructBuilder<sec4t::UsesMood>("sec4.UsesMood", DeclKind::Record).field("mood", &sec4t::UsesMood::mood).build();
    return *info;
}
const TypeInfo& helios::refl::TypeOf<sec4t::RefVisible>::get() noexcept {
    static const TypeInfo* info =
        StructBuilder<sec4t::RefVisible>("sec4.RefVisible", DeclKind::Record).field("visible", &sec4t::RefVisible::visible).build();
    return *info;
}
// helios-lint: outside-anon-namespace end

namespace {

/// The hand-built types, in a registry of their own (the loader looks record types up by id).
const refl::TypeRegistry& sec4Registry() {
    static refl::TypeRegistry* reg = [] {
        auto* r = new refl::TypeRegistry();
        for (const TypeInfo* t : {&refl::typeOf<sec4t::Secret>(), &refl::typeOf<sec4t::Visible>(), &refl::typeOf<sec4t::RefPlain>(),
                                  &refl::typeOf<sec4t::RefOpaque>(), &refl::typeOf<sec4t::Embeds>(), &refl::typeOf<sec4t::UsesMood>(),
                                  &refl::typeOf<sec4t::RefVisible>()}) {
            REQUIRE(r->add(*t).ok());
        }
        return r;
    }();
    return *reg;
}

constexpr refl::RecordId kBase = 4390381077356775243ull;
constexpr refl::RecordId kLoot = 2531117344111056078ull;
constexpr refl::RecordId kSkin = 1759060513237310278ull;
constexpr u64 kSecretCode = 0x5EC4'5EC4'5EC4'5EC4ull;

TEST_CASE("AAA-SEC-4: the client cook of the test records holds no server-only field, record, tag name or sentinel") {
    const CookOutput out = cookProject();
    const std::span<const u8> client(out.client);
    const std::span<const u8> server(out.server);
    // Planted sentinels: each is in the server cook (so the search can find it) and never in the client's.
    const auto threat = hxl::compile("attr(self, Sec4SentinelThreat) * 31337.25");
    REQUIRE(threat.ok());
    const f64 threatConst = 31337.25;
    for (const std::string_view s : {"SEC4-SENTINEL", "Sec4Sentinel", "SEC4-SENTINEL-ai-notes", "Sec4SentinelHint",
                                     "SEC4-SENTINEL-loot-secret", "Sec4SentinelLootEntry", "Sec4Sentinel.Server.Tag", "31337.25"}) {
        CHECK_MESSAGE(containsText(server, s), s);
        CHECK_MESSAGE(!containsText(client, s), s);
    }
    CHECK(containsValue(server, kSecretCode));
    CHECK_FALSE(containsValue(client, kSecretCode));
    CHECK(containsValue(server, threatConst)); // the server formula's constant, in its bytecode
    CHECK_FALSE(containsValue(client, threatConst));
    CHECK(containsBytes(server, threat->encode()));
    CHECK_FALSE(containsBytes(client, threat->encode()));

    // Structure: no server-only record or type, no server field in the client layout.
    const RecordDb c = open(client);
    const RecordDb s = open(server);
    CHECK_FALSE(c.find(kLoot));
    CHECK(s.find(kLoot));
    for (usize i = 0; i < c.recordCount(); ++i) CHECK_FALSE(excludesType(*c.record(i).type, CookAudience::Client));
    const ValueView ship = c.find(kBase).value;
    for (const refl::FieldInfo& f : ship.type().fields) {
        const bool kept = ship.layout().field(f.name) != nullptr;
        CHECK_MESSAGE(kept == !refl::hasFlag(f.flags, FieldFlags::ServerOnly), f.name);
    }
    // The @opaque shared reference cooks as the bare id of a record the client does not have.
    REQUIRE(ship.field("drop").hasValue());
    CHECK(ship.field("drop").value().asRecordId() == kLoot);
    // Tag names that only server-only data uses are withheld; the indices still agree with the server's.
    REQUIRE(c.tagCount() == s.tagCount());
    usize withheld = 0;
    for (usize i = 0; i < c.tagCount(); ++i) {
        const TagView ct = c.tag(static_cast<TagIndex>(i));
        const TagView st = s.tag(static_cast<TagIndex>(i));
        CHECK(ct.parent == st.parent);
        CHECK(ct.subtreeEnd == st.subtreeEnd);
        if (ct.withheld) {
            ++withheld;
            CHECK(ct.name.empty());
            CHECK(st.name.starts_with("Sec4Sentinel"));
            CHECK(c.findTag(st.name) == kNoTag);
        } else {
            CHECK(ct.name == st.name);
        }
    }
    CHECK(withheld == 3); // Sec4Sentinel, Sec4Sentinel.Server, Sec4Sentinel.Server.Tag
    CHECK(withheld == out.stats.withheldTags);
    CHECK(c.findTag("Ship.Role.Escort") != kNoTag); // used by shared data too

    // The reverse direction: the server cook holds no client-only record or field.
    CHECK_FALSE(s.find(kSkin));
    CHECK(c.find(kSkin));
    const ValueView sship = s.find(kBase).value;
    CHECK_FALSE(sship.field("skin").isValid());
    CHECK_FALSE(sship.field("hudColor").isValid());
    CHECK(sship.field("aiNotes").asText() == "SEC4-SENTINEL-ai-notes");
}

TEST_CASE("AAA-SEC-4: a shared reference to a server-only record without @opaque is refused with a precise error") {
    const std::vector<SourceRecord> s = {
        source("secret.hrec", refl::typeOf<sec4t::Secret>(), R"({"$rid": 100, "$name": "s/secret", "code": 7})"),
        source("plain.hrec", refl::typeOf<sec4t::RefPlain>(), R"({"$rid": 101, "$name": "s/plain", "secret": 100})"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(s);
    INFO(dump(d));
    REQUIRE(d.size() == 1);
    CHECK(d[0].message == "client cook: record type 'sec4.RefPlain': field 'sec4.RefPlain.secret' references server-only record "
                          "type 'sec4.Secret' in a client cook; mark it @opaque (the reference then cooks as a bare RecordId) "
                          "or move it into a server {} block (AAA-SEC-4)");
}

TEST_CASE("AAA-SEC-4: an @opaque reference cooks as the bare RecordId; the record stays server-side") {
    const std::vector<SourceRecord> s = {
        source("secret.hrec", refl::typeOf<sec4t::Secret>(), R"({"$rid": 110, "$name": "s/secret", "code": 1234567})"),
        source("opaque.hrec", refl::typeOf<sec4t::RefOpaque>(), R"({"$rid": 111, "$name": "s/opaque", "secrets": [110]})"),
    };
    auto out = cook(s);
    REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
    const RecordDb c = open(out->client, sec4Registry());
    const RecordDb sv = open(out->server, sec4Registry());
    CHECK_FALSE(c.find(110));
    CHECK(sv.find(110));
    CHECK(c.find(111).value.field("secrets")[0].asRecordId() == 110);
    CHECK_FALSE(containsValue(std::span<const u8>(out->client), i32{1234567}));
    CHECK(containsValue(std::span<const u8>(out->server), i32{1234567}));
}

TEST_CASE("AAA-SEC-4: a server-only struct or enum inside shared data is refused, @opaque or not") {
    const std::vector<SourceRecord> embeds = {source("embeds.hrec", refl::typeOf<sec4t::Embeds>(), R"({"$rid": 120, "$name": "s/e"})")};
    std::vector<CookDiagnostic> d = cookErrors(embeds);
    INFO(dump(d));
    REQUIRE(d.size() == 1);
    CHECK(d[0].message == "client cook: record type 'sec4.Embeds': field 'sec4.Embeds.inner': server-only type 'sec4.Secret' "
                          "cannot be part of a client cook (AAA-SEC-4)");
    const std::vector<SourceRecord> mood = {source("mood.hrec", refl::typeOf<sec4t::UsesMood>(), R"({"$rid": 121, "$name": "s/m"})")};
    d = cookErrors(mood);
    REQUIRE(d.size() == 1);
    CHECK(d[0].message.find("server-only type 'sec4.Mood' cannot be part of a client cook (AAA-SEC-4)") != std::string::npos);
}

TEST_CASE("AAA-SEC-4: the reverse direction refuses a client-only record referenced by server-cooked data") {
    const std::vector<SourceRecord> s = {
        source("visible.hrec", refl::typeOf<sec4t::Visible>(), R"({"$rid": 130, "$name": "s/v"})"),
        source("ref.hrec", refl::typeOf<sec4t::RefVisible>(), R"({"$rid": 131, "$name": "s/r", "visible": 130})"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(s);
    INFO(dump(d));
    REQUIRE(d.size() == 1);
    CHECK(d[0].message.find("server cook: record type 'sec4.RefVisible': field 'sec4.RefVisible.visible' references client-only "
                            "record type 'sec4.Visible' in a server cook") != std::string::npos);
}

TEST_CASE("AAA-SEC-4: a loader refuses a client cook that claims server-side content") {
    const CookOutput out = cookProject();
    // The server cook relabelled as a client cook (and resealed, so only the content checks can catch it).
    std::vector<u8> forged = out.server;
    forged[6] = static_cast<u8>(CookAudience::Client);
    hrdb::seal(forged);
    auto db = RecordDb::openBytes(forged, registry());
    REQUIRE_FALSE(db.ok());
    INFO(db.error().message);
    CHECK((db.error().code == ErrorCode::Corrupt || db.error().code == ErrorCode::VersionMismatch));
}

} // namespace
