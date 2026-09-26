// Gameplay tags (06 §1.1): deterministic interning, hierarchy, hot-set bits vs the cold path, queries,
// refcounts and replication helpers.
#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "helios/core/assert.h"
#include "helios/gameplay/tags.h"

using namespace helios;
using namespace helios::gameplay;

namespace {

const std::vector<std::string> kTags = {
    "State.Debuff.Stun", "State.Debuff.Slow", "State.Buff.Haste", "State.Combat", "Ship.Class.Frigate",
    "Ship.Class.Frigate.Assault", "Ship.Class.Cruiser", "Ship.Size.Small", "Item.Weapon.Hybrid.Rail",
    "Item.Weapon.Laser", "Item.Plug.Barrel", "Faction.Pilots", "Faction.Syndicate", "Cue.Hit.Shield",
    "A", "A_B", "A.B", "A.B0", "A0",
};

std::shared_ptr<const TagRegistry> makeRegistry(const std::vector<std::string>& tags, bool allHot) {
    TagRegistry::Builder b;
    for (const auto& t : tags) REQUIRE(b.add(t).ok());
    if (allHot) b.markAllHot();
    auto r = b.build();
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return *r;
}

// Brute-force reference: does the entity (a set of names) have `tag` or a descendant?
bool refHas(const std::set<std::string>& held, const std::string& tag) {
    for (const auto& h : held) {
        if (h == tag || (h.size() > tag.size() && h.compare(0, tag.size(), tag) == 0 && h[tag.size()] == '.')) return true;
    }
    return false;
}

} // namespace

TEST_CASE("tags: names are validated") {
    CHECK(isValidTagName("State.Debuff.Stun"));
    CHECK(isValidTagName("A_1.b2"));
    CHECK_FALSE(isValidTagName(""));
    CHECK_FALSE(isValidTagName("A..B"));
    CHECK_FALSE(isValidTagName(".A"));
    CHECK_FALSE(isValidTagName("A."));
    CHECK_FALSE(isValidTagName("1A"));
    CHECK_FALSE(isValidTagName("A-B"));
    CHECK_FALSE(isValidTagName(std::string(257, 'a')));
    TagRegistry::Builder b;
    CHECK_FALSE(b.add("bad name").ok());
    REQUIRE(b.add("X.Y", Audience::All).ok());
    CHECK(b.add("X.Y", Audience::All).ok()); // same declaration twice is fine
    CHECK(b.build().ok());
    REQUIRE(b.add("X.Y", Audience::Owner).ok());
    CHECK(b.build().errorCode() == ErrorCode::AlreadyExists); // conflicting audiences
    // An implied ancestor may also be declared explicitly, with any audience.
    TagRegistry::Builder c;
    REQUIRE(c.add("X.Y.Z").ok());
    REQUIRE(c.add("X", Audience::All).ok());
    auto rc = c.build();
    REQUIRE(rc.ok());
    CHECK((*rc)->info((*rc)->find("X")).declared);
    CHECK((*rc)->info((*rc)->find("X")).replicate == Audience::All);
}

TEST_CASE("tags: ids are deterministic, sorted and independent of declaration order") {
    auto a = makeRegistry(kTags, true);
    std::vector<std::string> shuffled = kTags;
    std::mt19937 rng(7);
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    auto b = makeRegistry(shuffled, true);
    REQUIRE(a->size() == b->size());
    for (usize i = 0; i < a->size(); ++i) CHECK(a->name(static_cast<TagIndex>(i)) == b->name(static_cast<TagIndex>(i)));
    CHECK(a->hash() == b->hash());
    // Byte-wise order equals segment-wise order because '.' sorts before every segment character.
    for (usize i = 1; i < a->size(); ++i) CHECK(a->name(static_cast<TagIndex>(i - 1)) < a->name(static_cast<TagIndex>(i)));
    // Implied ancestors exist but are not declared.
    const TagIndex debuff = a->find("State.Debuff");
    REQUIRE(debuff != kInvalidTag);
    CHECK_FALSE(a->info(debuff).declared);
    CHECK(a->info(a->find("State.Debuff.Stun")).declared);
    CHECK(a->info(a->find("State.Debuff.Stun")).parent == debuff);
    CHECK(a->info(a->find("State.Debuff.Stun")).depth == 2);
    CHECK(a->info(a->find("State")).parent == kInvalidTag);
    CHECK(a->find("State.Deb") == kInvalidTag);
    // The audience is part of the content hash.
    TagRegistry::Builder c;
    for (const auto& t : kTags) REQUIRE(c.add(t, t == "State.Combat" ? Audience::All : Audience::Server).ok());
    c.markAllHot();
    CHECK((*c.build())->hash() != a->hash());
}

TEST_CASE("tags: descendants form contiguous ranges") {
    auto r = makeRegistry(kTags, false);
    for (usize t = 0; t < r->size(); ++t) {
        for (usize u = 0; u < r->size(); ++u) {
            const std::string& tn = r->info(static_cast<TagIndex>(t)).name;
            const std::string& un = r->info(static_cast<TagIndex>(u)).name;
            const bool ref = un == tn || (un.size() > tn.size() && un.compare(0, tn.size(), tn) == 0 && un[tn.size()] == '.');
            CHECK_MESSAGE(r->isSelfOrDescendant(static_cast<TagIndex>(u), static_cast<TagIndex>(t)) == ref, un << " vs " << tn);
        }
    }
    // "A" does not own "A_B" or "A0" (different segments).
    CHECK_FALSE(r->isSelfOrDescendant(r->find("A_B"), r->find("A")));
    CHECK(r->isSelfOrDescendant(r->find("A.B0"), r->find("A")));
    CHECK_FALSE(r->isSelfOrDescendant(r->find("A.B0"), r->find("A.B")));
}

TEST_CASE("tags: containers count references and answer exact and parent-of queries") {
    for (bool hot : {true, false}) {
        CAPTURE(hot);
        auto r = makeRegistry(kTags, hot);
        TagContainer c(*r);
        const TagIndex stun = r->find("State.Debuff.Stun");
        const TagIndex debuff = r->find("State.Debuff");
        const TagIndex state = r->find("State");
        CHECK(c.add(stun));
        CHECK_FALSE(c.add(stun)); // second grant: refcount only
        CHECK(c.count(stun) == 2);
        CHECK(c.hasExact(stun));
        CHECK_FALSE(c.hasExact(debuff));
        CHECK(c.has(debuff));
        CHECK(c.has(state));
        CHECK_FALSE(c.has(r->find("State.Buff")));
        const u32 v = c.version();
        CHECK_FALSE(c.remove(stun));
        CHECK(c.version() == v); // refcount change only
        CHECK(c.has(debuff));
        CHECK(c.remove(stun));
        CHECK(c.version() == v + 1);
        CHECK_FALSE(c.has(debuff));
        CHECK_FALSE(c.remove(stun));
        // Two children share an ancestor: removing one keeps the ancestor.
        c.add(stun);
        c.add(r->find("State.Debuff.Slow"));
        c.remove(stun);
        CHECK(c.has(debuff));
        const TagIndex both[] = {debuff, r->find("Ship")};
        CHECK(c.hasAny(both));
        CHECK_FALSE(c.hasAll(both));
        c.clear();
        CHECK(c.explicitTags().empty());
        CHECK(c.bits().none());
    }
}

TEST_CASE("tags: queries (all/any/none), hot bits and the cold path agree with a reference") {
    auto hot = makeRegistry(kTags, true);
    auto cold = makeRegistry(kTags, false);
    REQUIRE(hot->hotCount() == hot->size());
    REQUIRE(cold->hotCount() == 0);
    std::vector<std::string> all;
    for (usize i = 0; i < hot->size(); ++i) all.emplace_back(hot->name(static_cast<TagIndex>(i)));
    std::mt19937 rng(42);
    auto pick = [&] { return all[rng() % all.size()]; };
    usize matched = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        std::set<std::string> held;
        const int n = static_cast<int>(rng() % 5);
        for (int k = 0; k < n; ++k) held.insert(pick());
        TagContainer ch(*hot), cc(*cold);
        for (const auto& h : held) {
            ch.add(hot->find(h));
            cc.add(cold->find(h));
        }
        std::vector<std::string> allT, anyT, noneT;
        for (int k = static_cast<int>(rng() % 3); k > 0; --k) allT.push_back(pick());
        for (int k = static_cast<int>(rng() % 3); k > 0; --k) anyT.push_back(pick());
        for (int k = static_cast<int>(rng() % 2); k > 0; --k) noneT.push_back(pick());
        auto clause = [](const char* kw, const std::vector<std::string>& v) {
            if (v.empty()) return std::string();
            std::string s = std::string(kw) + "(";
            for (usize i = 0; i < v.size(); ++i) s += (i ? ", " : "") + v[i];
            return s + ") ";
        };
        const std::string text = clause("none", noneT) + clause("all", allT) + clause("any", anyT);
        auto qh = hot->compileQuery(text);
        auto qc = cold->compileQuery(text);
        REQUIRE_MESSAGE(qh.ok(), text);
        REQUIRE(qc.ok());
        bool ref = true;
        for (const auto& t : allT) ref = ref && refHas(held, t);
        for (const auto& t : noneT) ref = ref && !refHas(held, t);
        if (!anyT.empty()) {
            bool any = false;
            for (const auto& t : anyT) any = any || refHas(held, t);
            ref = ref && any;
        }
        CHECK_MESSAGE(qh->matches(ch) == ref, text);
        CHECK_MESSAGE(qc->matches(cc) == ref, text);
        for (const auto& t : all) CHECK(ch.has(hot->find(t)) == refHas(held, t));
        for (const auto& t : all) CHECK(cc.has(cold->find(t)) == refHas(held, t));
        matched += ref ? 1 : 0;
    }
    CHECK(matched > 100); // the sweep exercises both outcomes
    CHECK(matched < 2900);
}

TEST_CASE("tags: query syntax and hot-set marking") {
    TagRegistry::Builder b;
    for (const auto& t : kTags) REQUIRE(b.add(t).ok());
    REQUIRE(b.markHotFromQuery("all(State.Debuff) none(State.Buff.Haste)").ok());
    REQUIRE(b.markHot("Ship.Class").ok());
    auto r = *b.build();
    CHECK(r->hotCount() == 3);
    CHECK(r->info(r->find("State.Debuff")).hot != kNotHot);
    CHECK(r->info(r->find("State.Debuff.Stun")).hot == kNotHot);
    // Adding a cold child sets its hot ancestor's bit.
    TagContainer c(*r);
    c.add(r->find("State.Debuff.Stun"));
    CHECK(c.bits().test(r->info(r->find("State.Debuff")).hot));
    auto q = r->compileQuery("all(State.Debuff) none(State.Buff.Haste)");
    REQUIRE(q.ok());
    CHECK(q->allCold.empty());
    CHECK(q->matches(c));
    c.add(r->find("State.Buff.Haste"));
    CHECK_FALSE(q->matches(c));
    auto empty = r->compileQuery("  ");
    REQUIRE(empty.ok());
    CHECK(empty->matchesEverything());
    CHECK(empty->matches(c));
    CHECK(r->compileQuery("all(State.Nope)").errorCode() == ErrorCode::NotFound);
    CHECK(r->compileQuery("most(State)").errorCode() == ErrorCode::ParseError);
    CHECK(r->compileQuery("all(State").errorCode() == ErrorCode::ParseError);
    CHECK(r->compileQuery("all()").errorCode() == ErrorCode::ParseError);
    CHECK(r->compileQuery("all(State) all(Ship)").errorCode() == ErrorCode::ParseError);
    CHECK(r->compileQuery("all(State,, Ship)").errorCode() == ErrorCode::ParseError);
    // Unknown hot tags fail the build.
    TagRegistry::Builder bad;
    REQUIRE(bad.add("X.Y").ok());
    REQUIRE(bad.markHot("X.Z").ok());
    CHECK(bad.build().errorCode() == ErrorCode::NotFound);
}

TEST_CASE("tags: the hot set is limited to 1,024 tags") {
    TagRegistry::Builder b;
    for (int i = 0; i < 1100; ++i) REQUIRE(b.add("T.N" + std::to_string(i)).ok());
    b.markAllHot();
    CHECK(b.build().errorCode() == ErrorCode::LimitExceeded);
    // fromRecords without queries falls back to a cold registry when everything does not fit.
    std::vector<TagDef> defs(1100);
    for (int i = 0; i < 1100; ++i) defs[i].tag = Name("T.N" + std::to_string(i));
    auto r = TagRegistry::fromRecords(defs);
    REQUIRE(r.ok());
    CHECK((*r)->hotCount() == 0);
    CHECK((*r)->size() == 1101);
    // With queries, only their tags are hot.
    const std::string queries[] = {"any(T.N5, T.N7)"};
    auto q = TagRegistry::fromRecords(defs, queries);
    REQUIRE(q.ok());
    CHECK((*q)->hotCount() == 2);
}

TEST_CASE("tags: replication helpers (assign, diff, version)") {
    auto r = makeRegistry(kTags, true);
    TagContainer c(*r);
    const TagIndex a = r->find("State.Combat"), b = r->find("Ship.Class.Frigate"), d = r->find("Faction.Pilots");
    c.add(a);
    c.add(b, 3);
    const std::vector<TagCount> before(c.explicitTags().begin(), c.explicitTags().end());
    TagContainer receiver(*r);
    REQUIRE(receiver.assign(before).ok());
    CHECK(receiver.bits() == c.bits());
    CHECK(receiver.count(b) == 3);
    c.remove(a);
    c.add(d);
    std::vector<TagIndex> added, removed;
    TagContainer::diff(before, c.explicitTags(), added, removed);
    CHECK(added == std::vector<TagIndex>{d});
    CHECK(removed == std::vector<TagIndex>{a});
    // assign merges duplicates and drops zero counts.
    const TagCount raw[] = {{d, 1}, {b, 0}, {d, 2}};
    const u32 v = receiver.version();
    REQUIRE(receiver.assign(raw).ok());
    CHECK(receiver.count(d) == 3);
    CHECK_FALSE(receiver.hasExact(b));
    CHECK(receiver.version() == v + 1);
    REQUIRE(receiver.assign(receiver.explicitTags().empty() ? std::span<const TagCount>{} : std::span<const TagCount>(raw, 1)).ok());
    CHECK(receiver.version() == v + 1); // same set of tags: no version bump
}

TEST_CASE("tags: registry from TagDef records") {
    std::vector<TagDef> defs(3);
    defs[0].tag = Name("Ship.Class.Frigate");
    defs[0].replicate = Audience::All;
    defs[1].tag = Name("State.Debuff.Stun");
    defs[1].replicate = Audience::Owner;
    defs[2].tag = Name("Ship.Size.Small");
    auto r = TagRegistry::fromRecords(defs);
    REQUIRE(r.ok());
    CHECK((*r)->size() == 8); // plus Ship, Ship.Class, Ship.Size, State, State.Debuff
    CHECK((*r)->info((*r)->find("Ship.Class.Frigate")).replicate == Audience::All);
    CHECK((*r)->info((*r)->find("State.Debuff.Stun")).replicate == Audience::Owner);
    defs[2].tag = Name("bad..tag");
    CHECK_FALSE(TagRegistry::fromRecords(defs).ok());
}

namespace {
int g_tagAsserts = 0;
AssertAction countTagAssert(const AssertInfo&) {
    ++g_tagAsserts;
    return AssertAction::Continue;
}
} // namespace

TEST_CASE("tags: untrusted replication input and foreign indices are rejected safely (review regression)") {
    // assign() used to trust indices from the wire (only a debug assert): an out-of-range index was
    // read out of bounds in release builds. has() likewise indexed the registry unchecked.
    auto r = makeRegistry(kTags, true);
    TagContainer c(*r);
    const TagIndex combat = r->find("State.Combat");
    REQUIRE(c.add(combat));
    const u32 v = c.version();
    const TagBits bitsBefore = c.bits();
    const TagCount bad[] = {{combat, 1}, {static_cast<TagIndex>(r->size()), 1}};
    auto res = c.assign(bad);
    REQUIRE_FALSE(res.ok());
    CHECK(res.errorCode() == ErrorCode::OutOfRange);
    CHECK(c.version() == v); // unchanged
    CHECK(c.bits() == bitsBefore);
    CHECK(c.count(combat) == 1);
    const TagCount invalid[] = {{kInvalidTag, 1}};
    CHECK_FALSE(c.assign(invalid).ok());
    CHECK_FALSE(c.has(kInvalidTag));
    CHECK_FALSE(c.has(static_cast<TagIndex>(r->size())));
    CHECK_FALSE(c.hasExact(static_cast<TagIndex>(r->size())));
    // A query compiled against a larger (cold) registry names indices this registry lacks.
    std::vector<std::string> more = kTags;
    for (int i = 0; i < 40; ++i) more.push_back("Zz.T" + std::to_string(i));
    auto big = makeRegistry(more, false);
    auto q = big->compileQuery("none(Zz.T39)");
    REQUIRE(q.ok());
    REQUIRE(q->noneCold.size() == 1);
    REQUIRE(q->noneCold[0] >= r->size());
    // Matching it is a caller bug (asserted, since WP-0.19's review: TagQuery records its registry),
    // but release builds must stay safe: the foreign tag is simply not held.
    CHECK(q->registry == big->queryHash());
    CHECK(q->registry != r->queryHash());
    g_tagAsserts = 0;
    const AssertHandler previous = setAssertHandler(&countTagAssert);
    CHECK(q->matches(c));
    setAssertHandler(previous);
#if HELIOS_ENABLE_ASSERTS
    CHECK(g_tagAsserts == 1);
#endif
}

TEST_CASE("tags: replicatedTags filters explicit tags by declared audience (06 §1.6)") {
    TagRegistry::Builder b;
    REQUIRE(b.add("Pub.Visible", Audience::All).ok());
    REQUIRE(b.add("Own.Cooldown", Audience::Owner).ok());
    REQUIRE(b.add("Srv.Internal").ok()); // Server: never sent to clients
    auto r = *b.build();
    TagContainer c(*r);
    const TagIndex pub = r->find("Pub.Visible"), own = r->find("Own.Cooldown"), srv = r->find("Srv.Internal");
    c.add(pub);
    c.add(own, 2);
    c.add(srv);
    std::vector<TagCount> out;
    c.replicatedTags(Audience::All, out);
    CHECK(out == std::vector<TagCount>{{pub, 1}});
    c.replicatedTags(Audience::Owner, out);
    std::vector<TagCount> owner{{own, 2}, {pub, 1}};
    std::sort(owner.begin(), owner.end(), [](const TagCount& x, const TagCount& y) { return x.tag < y.tag; });
    CHECK(out == owner);
    c.replicatedTags(Audience::Server, out);
    CHECK(out.size() == 3);
    // Deltas for other clients never mention server-only or owner-only tags.
    std::vector<TagCount> before;
    c.replicatedTags(Audience::All, before);
    c.remove(srv);
    c.remove(own, 2);
    std::vector<TagCount> now;
    c.replicatedTags(Audience::All, now);
    std::vector<TagIndex> added, removed;
    TagContainer::diff(before, now, added, removed);
    CHECK(added.empty());
    CHECK(removed.empty());
}
