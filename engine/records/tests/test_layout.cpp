// Cooked layouts (layout.h): sizes, alignment, the audience filter and order-independent hashes.
#include <array>

#include "test_util.h"

using namespace helios;
using namespace helios::records;
using namespace helios::records::test;

namespace {

TEST_CASE("layout: every field is naturally aligned and every size is a multiple of its alignment") {
    LayoutCache cache(CookAudience::Server);
    auto l = cache.get(type("test.records.ShipDef"));
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? "" : l.error().message));
    const CookedLayout& ship = **l;
    CHECK(ship.size % ship.align == 0);
    u32 end = 0;
    for (const CookedLayout::Field& f : ship.fields) {
        INFO(f.info->name);
        CHECK(f.offset % f.layout->align == 0);
        CHECK(f.offset >= end); // schema order, no overlap
        end = f.offset + f.layout->size;
        CHECK(f.layout->size % f.layout->align == 0);
    }
    CHECK(end <= ship.size);
    auto field = [&](std::string_view n) -> const CookedLayout& {
        const CookedLayout::Field* f = ship.field(n);
        REQUIRE_MESSAGE(f != nullptr, n);
        return *f->layout;
    };
    CHECK(field("grade").size == 1);
    CHECK(field("perms").size == 2);
    CHECK(field("u64v").size == 8);
    CHECK(field("label").enc == Enc::Text);
    CHECK(field("label").size == 8);
    CHECK(field("formula").size == 16);
    CHECK(field("guid").size == 16);
    CHECK(field("pos").size == 24);
    CHECK(field("rot").size == 16);
    CHECK(field("mounts").enc == Enc::KeyedList);
    CHECK(field("mounts").size == 16);
    CHECK(field("slots").enc == Enc::List);
    CHECK(field("maybe").size == 8);          // u8 present, 3 pad, f32
    CHECK(field("maybe").payloadOffset == 4);
    CHECK(field("nums").size == 6);           // u16[3]
    CHECK(field("attrs").entrySize == 16);    // Name span (8) + f64
    CHECK(field("attrs").payloadOffset == 8);
    const CookedLayout& mode = field("mode");
    CHECK(mode.enc == Enc::Variant);
    CHECK(mode.payloadOffset == 8);           // u32 index, then the 8-aligned largest alternative:
    CHECK(mode.size == 24);                   // Warp {target: Name span, spool: Duration}
    // Recursive types go through lists.
    auto tree = cache.get(type("test.records.TreeDef"));
    REQUIRE(tree.ok());
    CHECK((*tree)->field("root")->layout->field("kids")->layout->element == (*tree)->field("root")->layout);
}

TEST_CASE("layout: the client drops server {} fields, the server drops client {} fields") {
    LayoutCache client(CookAudience::Client);
    LayoutCache server(CookAudience::Server);
    const refl::TypeInfo& shipT = type("test.records.ShipDef");
    auto c = client.get(shipT);
    auto s = server.get(shipT);
    REQUIRE(c.ok());
    REQUIRE(s.ok());
    for (const char* f : {"loot", "aiNotes", "aiHints", "serverTags", "threat", "secretCode"}) {
        CHECK_MESSAGE((*c)->field(f) == nullptr, f);
        CHECK_MESSAGE((*s)->field(f) != nullptr, f);
    }
    for (const char* f : {"skin", "hudColor"}) {
        CHECK_MESSAGE((*c)->field(f) != nullptr, f);
        CHECK_MESSAGE((*s)->field(f) == nullptr, f);
    }
    CHECK((*c)->hash != (*s)->hash);
    // A record type of the other side cannot be laid out at all.
    CHECK_FALSE(client.get(type("test.records.LootDef")).ok());
    CHECK_FALSE(server.get(type("test.records.SkinDef")).ok());
    CHECK(client.get(type("test.records.SkinDef")).ok());
}

TEST_CASE("layout: hashes do not depend on the order layouts are built in") {
    const char* names[] = {"test.records.ShipDef", "test.records.TreeDef", "test.records.PartDef", "helios.gameplay.TagDef"};
    LayoutCache a(CookAudience::Server);
    LayoutCache b(CookAudience::Server);
    std::vector<u64> ha;
    for (const char* n : names) ha.push_back((*a.get(type(n)))->hash);
    std::vector<u64> hb(4);
    for (int i = 3; i >= 0; --i) hb[static_cast<usize>(i)] = (*b.get(type(names[i])))->hash;
    CHECK(ha == hb);
    CHECK(ha[0] != ha[1]);
    // The recursive node, asked for directly, gets the same hash as when it was reached through TreeDef.
    LayoutCache c(CookAudience::Server);
    const u64 direct = (*c.get(type("test.records.TreeNode")))->hash;
    CHECK(direct == (*a.get(type("test.records.TreeNode")))->hash);
}

TEST_CASE("layout: recursion through a list of structs that hold the type by value works in any build order") {
    // DlgNode { choices: list<DlgChoice> }, DlgChoice { next: DlgNode? }: DlgChoice holds DlgNode by
    // value, DlgNode holds DlgChoice only through a span. Each of the three types first, in a fresh cache
    // per audience, gives the same layouts and hashes.
    const char* names[] = {"test.records.DlgNode", "test.records.DlgChoice", "test.records.DlgDef"};
    for (const CookAudience audience : {CookAudience::Client, CookAudience::Server}) {
        std::vector<std::vector<u64>> hashes;
        for (const char* first : names) {
            INFO(first);
            LayoutCache cache(audience);
            auto f = cache.get(type(first));
            REQUIRE_MESSAGE(f.ok(), (f.ok() ? "" : f.error().message));
            std::vector<u64> h;
            for (const char* n : names) {
                auto l = cache.get(type(n));
                REQUIRE_MESSAGE(l.ok(), (l.ok() ? "" : l.error().message));
                h.push_back((*l)->hash);
            }
            auto node = cache.get(type("test.records.DlgNode"));
            auto choice = cache.get(type("test.records.DlgChoice"));
            CHECK((*node)->field("choices")->layout->element == *choice);
            CHECK((*choice)->field("next")->layout->element == *node);
            CHECK((*choice)->size == 8 + (*choice)->field("next")->layout->size); // label span + DlgNode? inline
            CHECK((*node)->size == 16);                                           // two spans
            hashes.push_back(std::move(h));
        }
        CHECK(hashes[0] == hashes[1]);
        CHECK(hashes[0] == hashes[2]);
    }
}

// Hand-made TypeInfos of types that contain themselves by value: no C++ type or schema can (they would
// have infinite size), but TypeInfos are plain data (dynamic packages later), and a layout must never be
// sized from a half-built one. Each loop is struct `loop.S { v: <wrapper> }` with the wrapper holding S.
const refl::TypeInfo& loopStruct(int wrapper) noexcept;
template <int W>
const refl::TypeInfo& loopStructFn() noexcept {
    return loopStruct(W);
}
template <int W>
const refl::TypeInfo& loopWrapper() noexcept {
    static const refl::VariantAlt alt{"S", 1, &loopStructFn<W>};
    static const refl::TypeInfo t = [] {
        refl::TypeInfo i;
        i.qualifiedName = W == 0 ? "loop.S?" : W == 1 ? "loop.S[2]" : "loop.V";
        i.kind = W == 0 ? refl::Kind::Optional : W == 1 ? refl::Kind::Array : refl::Kind::Variant;
        if (W == 2) {
            i.alternatives = std::span<const refl::VariantAlt>(&alt, 1);
        } else {
            i.elementFn = &loopStructFn<W>;
            i.arraySize = 2;
        }
        return i;
    }();
    return t;
}
const refl::TypeInfo& loopStruct(int wrapper) noexcept {
    static const refl::FieldInfo fields[3] = {{"v", 1, 0, &loopWrapper<0>}, {"v", 1, 0, &loopWrapper<1>}, {"v", 1, 0, &loopWrapper<2>}};
    static const std::array<refl::TypeInfo, 3> types = [] {
        std::array<refl::TypeInfo, 3> t{};
        for (int w = 0; w < 3; ++w) {
            t[static_cast<usize>(w)].qualifiedName = "loop.S";
            t[static_cast<usize>(w)].kind = refl::Kind::Struct;
            t[static_cast<usize>(w)].fields = std::span<const refl::FieldInfo>(&fields[w], 1);
        }
        return t;
    }();
    return types[static_cast<usize>(wrapper)];
}

const refl::TypeInfo& emptyArray() noexcept {
    static const refl::TypeInfo t = [] {
        refl::TypeInfo i;
        i.qualifiedName = "i32[0]";
        i.kind = refl::Kind::Array;
        i.elementFn = &refl::typeOf<i32>;
        i.arraySize = 0;
        return i;
    }();
    return t;
}

TEST_CASE("layout: a zero-length array is refused (its lists would have stride 0)") {
    // schemac allows sizes 1..65536; a builder std::array<T, 0> would otherwise make a list whose count
    // the loader cannot bound by the bytes it occupies.
    LayoutCache cache(CookAudience::Server);
    auto l = cache.get(emptyArray());
    REQUIRE_FALSE(l.ok());
    CHECK(l.error().code == ErrorCode::Unsupported);
    CHECK(l.error().message == "array type 'i32[0]' has no elements");
}

TEST_CASE("layout: a type that contains itself by value is refused through an optional, array or variant, whichever comes first") {
    for (const refl::TypeInfo* wrapper : {&loopWrapper<0>(), &loopWrapper<1>(), &loopWrapper<2>()}) {
        INFO(wrapper->qualifiedName);
        for (const bool wrapperFirst : {true, false}) {
            LayoutCache cache(CookAudience::Server);
            auto l = cache.get(wrapperFirst ? *wrapper : wrapper->kind == refl::Kind::Variant ? wrapper->alternatives[0].type()
                                                                                                : wrapper->element());
            REQUIRE_FALSE(l.ok());
            CHECK(l.error().code == ErrorCode::InvalidArgument);
            CHECK(l.error().message.find("contains itself by value") != std::string::npos);
        }
    }
}

} // namespace
