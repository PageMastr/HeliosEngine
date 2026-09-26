// Gameplay tags (06 §1.1): registry, compiled queries and containers.
#include "helios/gameplay/tags.h"

#include <algorithm>
#include <format>

#include "helios/core/assert.h"
#include "helios/core/hash.h"

namespace helios::gameplay {
namespace {

bool isSegStart(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool isSegChar(char c) noexcept { return isSegStart(c) || (c >= '0' && c <= '9'); }

enum class Clause : u8 { All, Any, None };

// Parses `all(A, B) any(C) none(D)`; calls onTag(clause, name, offset) for every tag.
template <class F>
Result<void> parseQuery(std::string_view text, F&& onTag) {
    usize i = 0;
    bool seen[3] = {false, false, false};
    auto skipWs = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
    };
    auto readName = [&](usize& start) -> std::string_view {
        start = i;
        while (i < text.size() && (isSegChar(text[i]) || text[i] == '.')) ++i;
        return text.substr(start, i - start);
    };
    while (true) {
        skipWs();
        if (i >= text.size()) return {};
        usize kwStart = 0;
        const std::string_view kw = readName(kwStart);
        Clause clause{};
        if (kw == "all") {
            clause = Clause::All;
        } else if (kw == "any") {
            clause = Clause::Any;
        } else if (kw == "none") {
            clause = Clause::None;
        } else {
            return makeError(ErrorCode::ParseError, "tag query: expected all(, any( or none( at offset {}", kwStart);
        }
        if (seen[static_cast<int>(clause)]) {
            return makeError(ErrorCode::ParseError, "tag query: duplicate {}() clause at offset {}", kw, kwStart);
        }
        seen[static_cast<int>(clause)] = true;
        skipWs();
        if (i >= text.size() || text[i] != '(') {
            return makeError(ErrorCode::ParseError, "tag query: expected '(' at offset {}", i);
        }
        ++i;
        while (true) {
            skipWs();
            usize nameStart = 0;
            const std::string_view name = readName(nameStart);
            if (!isValidTagName(name)) {
                return makeError(ErrorCode::ParseError, "tag query: expected a tag name at offset {}", nameStart);
            }
            HELIOS_TRY(onTag(clause, name, nameStart));
            skipWs();
            if (i < text.size() && text[i] == ',') {
                ++i;
                continue;
            }
            if (i < text.size() && text[i] == ')') {
                ++i;
                break;
            }
            return makeError(ErrorCode::ParseError, "tag query: expected ',' or ')' at offset {}", i);
        }
    }
}

std::string_view parentName(std::string_view name) noexcept {
    const usize dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view() : name.substr(0, dot);
}

} // namespace

bool isValidTagName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 256) return false;
    bool segStart = true;
    for (char c : name) {
        if (segStart) {
            if (!isSegStart(c)) return false;
            segStart = false;
        } else if (c == '.') {
            segStart = true;
        } else if (!isSegChar(c)) {
            return false;
        }
    }
    return !segStart;
}

bool TagBits::none() const noexcept {
    u64 acc = 0;
    for (u64 w : words) acc |= w;
    return acc == 0;
}

bool TagBits::containsAll(const TagBits& mask) const noexcept {
    u64 miss = 0;
    for (u32 i = 0; i < kWords; ++i) miss |= mask.words[i] & ~words[i];
    return miss == 0;
}

bool TagBits::intersects(const TagBits& mask) const noexcept {
    u64 acc = 0;
    for (u32 i = 0; i < kWords; ++i) acc |= mask.words[i] & words[i];
    return acc != 0;
}

TagBits& TagBits::operator|=(const TagBits& o) noexcept {
    for (u32 i = 0; i < kWords; ++i) words[i] |= o.words[i];
    return *this;
}

// ---- builder ---------------------------------------------------------------------------------------

Result<void> TagRegistry::Builder::add(std::string_view tag, Audience replicate) {
    if (!isValidTagName(tag)) return makeError(ErrorCode::InvalidArgument, "invalid tag name '{}'", tag);
    m_decls.push_back(Decl{std::string(tag), replicate}); // duplicates are merged (or rejected) by build()
    return {};
}

Result<void> TagRegistry::Builder::markHot(std::string_view tag) {
    if (!isValidTagName(tag)) return makeError(ErrorCode::InvalidArgument, "invalid tag name '{}'", tag);
    m_hot.emplace_back(tag);
    return {};
}

Result<void> TagRegistry::Builder::markHotFromQuery(std::string_view queryText) {
    return parseQuery(queryText, [&](Clause, std::string_view name, usize) -> Result<void> { return markHot(name); });
}

Result<std::shared_ptr<const TagRegistry>> TagRegistry::Builder::build() const {
    struct Entry {
        std::string name;
        Audience replicate = Audience::Server;
        bool declared = false;
    };
    std::vector<Entry> entries;
    entries.reserve(m_decls.size() * 2);
    for (const Decl& d : m_decls) {
        entries.push_back(Entry{d.name, d.replicate, true});
        for (std::string_view p = parentName(d.name); !p.empty(); p = parentName(p)) {
            entries.push_back(Entry{std::string(p), Audience::Server, false});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.name != b.name) return a.name < b.name;
        if (a.declared != b.declared) return a.declared > b.declared; // the declared entry of a name first
        return a.replicate < b.replicate;
    });
    for (usize i = 1; i < entries.size(); ++i) {
        const Entry& p = entries[i - 1];
        const Entry& c = entries[i];
        if (p.name == c.name && p.declared && c.declared && p.replicate != c.replicate) {
            return makeError(ErrorCode::AlreadyExists, "tag '{}' declared twice with different audiences", c.name);
        }
    }
    entries.erase(std::unique(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name == b.name; }),
                  entries.end());
    if (entries.size() > kMaxTags) return makeError(ErrorCode::LimitExceeded, "more than {} tags", kMaxTags);

    auto reg = std::make_shared<TagRegistry>();
    const usize n = entries.size();
    reg->m_tags.resize(n);
    auto indexOf = [&](std::string_view name) -> TagIndex {
        auto it = std::lower_bound(entries.begin(), entries.end(), name,
                                   [](const Entry& e, std::string_view v) { return std::string_view(e.name) < v; });
        return it != entries.end() && it->name == name ? static_cast<TagIndex>(it - entries.begin()) : kInvalidTag;
    };
    for (usize i = 0; i < n; ++i) {
        TagInfo& t = reg->m_tags[i];
        t.name = entries[i].name;
        t.replicate = entries[i].replicate;
        t.declared = entries[i].declared;
        t.subtreeEnd = static_cast<TagIndex>(i + 1);
        const std::string_view p = parentName(t.name);
        t.parent = p.empty() ? kInvalidTag : indexOf(p);
        t.depth = static_cast<u8>(std::min<usize>(255, static_cast<usize>(std::count(t.name.begin(), t.name.end(), '.'))));
    }
    for (usize i = n; i-- > 0;) {
        const TagIndex p = reg->m_tags[i].parent;
        if (p != kInvalidTag) reg->m_tags[p].subtreeEnd = std::max(reg->m_tags[p].subtreeEnd, reg->m_tags[i].subtreeEnd);
    }
    // Hot set.
    std::vector<TagIndex> hot;
    if (m_allHot) {
        for (usize i = 0; i < n; ++i) hot.push_back(static_cast<TagIndex>(i));
    } else {
        for (const std::string& h : m_hot) {
            const TagIndex t = indexOf(h);
            if (t == kInvalidTag) return makeError(ErrorCode::NotFound, "hot tag '{}' is not declared", h);
            hot.push_back(t);
        }
        std::sort(hot.begin(), hot.end());
        hot.erase(std::unique(hot.begin(), hot.end()), hot.end());
    }
    if (hot.size() > kMaxHotTags) {
        return makeError(ErrorCode::LimitExceeded, "{} hot tags; at most {} tags may be named by queries", hot.size(),
                         kMaxHotTags);
    }
    for (usize h = 0; h < hot.size(); ++h) reg->m_tags[hot[h]].hot = static_cast<u16>(h);
    reg->m_hotCount = static_cast<u32>(hot.size());
    reg->m_hotOffset.resize(n + 1);
    for (usize i = 0; i < n; ++i) {
        reg->m_hotOffset[i] = static_cast<u32>(reg->m_hotList.size());
        for (TagIndex t = static_cast<TagIndex>(i); t != kInvalidTag; t = reg->m_tags[t].parent) {
            if (reg->m_tags[t].hot != kNotHot) reg->m_hotList.push_back(reg->m_tags[t].hot);
        }
    }
    reg->m_hotOffset[n] = static_cast<u32>(reg->m_hotList.size());
    // Content hash: names, audiences, declared flags.
    u64 h = kFnv1a64Offset;
    auto mix = [&](u8 b) {
        h ^= b;
        h *= kFnv1a64Prime;
    };
    for (const TagInfo& t : reg->m_tags) {
        for (char c : t.name) mix(static_cast<u8>(c));
        mix(0);
        mix(static_cast<u8>(t.replicate));
        mix(t.declared ? 1 : 0);
    }
    reg->m_hash = h;
    for (const TagInfo& t : reg->m_tags) {
        mix(static_cast<u8>(t.hot));
        mix(static_cast<u8>(t.hot >> 8));
    }
    reg->m_queryHash = h;
    return std::shared_ptr<const TagRegistry>(std::move(reg));
}

Result<std::shared_ptr<const TagRegistry>> TagRegistry::fromRecords(std::span<const TagDef> defs,
                                                                    std::span<const std::string> queries) {
    Builder b;
    for (const TagDef& d : defs) HELIOS_TRY(b.addRecord(d));
    if (queries.empty()) {
        // No query content yet: every tag is hot when they fit.
        b.markAllHot();
        auto all = b.build();
        if (all || all.errorCode() != ErrorCode::LimitExceeded) return all;
        Builder cold;
        for (const TagDef& d : defs) HELIOS_TRY(cold.addRecord(d));
        return cold.build();
    }
    for (const std::string& q : queries) HELIOS_TRY(b.markHotFromQuery(q));
    return b.build();
}

TagIndex TagRegistry::find(std::string_view name) const noexcept {
    auto it = std::lower_bound(m_tags.begin(), m_tags.end(), name,
                               [](const TagInfo& t, std::string_view v) { return std::string_view(t.name) < v; });
    return it != m_tags.end() && it->name == name ? static_cast<TagIndex>(it - m_tags.begin()) : kInvalidTag;
}

Result<TagQuery> TagRegistry::compileQuery(std::string_view text) const {
    TagQuery q;
    q.registry = m_queryHash;
    auto parsed = parseQuery(text, [&](Clause clause, std::string_view name, usize offset) -> Result<void> {
        const TagIndex t = find(name);
        if (t == kInvalidTag) return makeError(ErrorCode::NotFound, "tag query: unknown tag '{}' at offset {}", name, offset);
        TagBits& mask = clause == Clause::All ? q.allMask : clause == Clause::Any ? q.anyMask : q.noneMask;
        std::vector<TagIndex>& cold = clause == Clause::All ? q.allCold : clause == Clause::Any ? q.anyCold : q.noneCold;
        if (clause == Clause::Any) q.hasAny = true;
        if (m_tags[t].hot != kNotHot) {
            mask.set(m_tags[t].hot);
        } else {
            cold.push_back(t); // deduplicated below (a linear find made hostile queries quadratic)
        }
        return {};
    });
    if (!parsed) return std::move(parsed).error();
    for (auto* v : {&q.allCold, &q.anyCold, &q.noneCold}) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    return q;
}

// ---- queries ----------------------------------------------------------------------------------------

bool TagQuery::matchesEverything() const noexcept {
    return !hasAny && allMask.none() && noneMask.none() && allCold.empty() && noneCold.empty();
}

bool TagQuery::matches(const TagContainer& c) const noexcept {
    HELIOS_ASSERT(registry == 0 || registry == c.registry().queryHash(), "tag query compiled against another registry");
    const TagBits& b = c.bits();
    if (!b.containsAll(allMask) || b.intersects(noneMask)) return false;
    for (TagIndex t : allCold) {
        if (!c.has(t)) return false;
    }
    for (TagIndex t : noneCold) {
        if (c.has(t)) return false;
    }
    if (hasAny && !b.intersects(anyMask)) {
        bool any = false;
        for (TagIndex t : anyCold) {
            if (c.has(t)) {
                any = true;
                break;
            }
        }
        if (!any) return false;
    }
    return true;
}

// ---- container --------------------------------------------------------------------------------------

namespace {
auto lowerBound(std::vector<TagCount>& v, TagIndex tag) {
    return std::lower_bound(v.begin(), v.end(), tag, [](const TagCount& c, TagIndex t) { return c.tag < t; });
}
auto lowerBound(const std::vector<TagCount>& v, TagIndex tag) {
    return std::lower_bound(v.begin(), v.end(), tag, [](const TagCount& c, TagIndex t) { return c.tag < t; });
}
} // namespace

bool TagContainer::add(TagIndex tag, u16 count) {
    HELIOS_ASSERT(tag < m_registry->size(), "tag index out of range");
    if (count == 0 || tag >= m_registry->size()) return false;
    auto it = lowerBound(m_explicit, tag);
    if (it != m_explicit.end() && it->tag == tag) {
        it->count = static_cast<u16>(std::min<u32>(0xFFFFu, u32{it->count} + count));
        return false;
    }
    m_explicit.insert(it, TagCount{tag, count});
    for (u16 h : m_registry->hotAncestors(tag)) m_bits.set(h);
    ++m_version;
    return true;
}

bool TagContainer::remove(TagIndex tag, u16 count) {
    auto it = lowerBound(m_explicit, tag);
    if (it == m_explicit.end() || it->tag != tag || count == 0) return false;
    if (it->count > count) {
        it->count = static_cast<u16>(it->count - count);
        return false;
    }
    m_explicit.erase(it);
    rebuildBits();
    ++m_version;
    return true;
}

void TagContainer::clear() noexcept {
    if (m_explicit.empty()) return;
    m_explicit.clear();
    m_bits.clear();
    ++m_version;
}

u16 TagContainer::count(TagIndex tag) const noexcept {
    auto it = lowerBound(m_explicit, tag);
    return it != m_explicit.end() && it->tag == tag ? it->count : 0;
}

bool TagContainer::has(TagIndex tag) const noexcept {
    if (tag >= m_registry->size()) return false;
    const TagInfo& info = m_registry->info(tag);
    if (info.hot != kNotHot) return m_bits.test(info.hot);
    auto it = lowerBound(m_explicit, tag);
    return it != m_explicit.end() && it->tag < info.subtreeEnd;
}

bool TagContainer::hasAny(std::span<const TagIndex> tags) const noexcept {
    for (TagIndex t : tags) {
        if (has(t)) return true;
    }
    return false;
}

bool TagContainer::hasAll(std::span<const TagIndex> tags) const noexcept {
    for (TagIndex t : tags) {
        if (!has(t)) return false;
    }
    return true;
}

Result<void> TagContainer::assign(std::span<const TagCount> tags) {
    // Replication input is untrusted: validate before touching the container.
    for (const TagCount& t : tags) {
        if (t.tag >= m_registry->size()) {
            return makeError(ErrorCode::OutOfRange, "tag index {} outside the registry ({} tags)", t.tag, m_registry->size());
        }
    }
    std::vector<TagCount> next(tags.begin(), tags.end());
    std::sort(next.begin(), next.end(), [](const TagCount& a, const TagCount& b) { return a.tag < b.tag; });
    std::vector<TagCount> merged;
    merged.reserve(next.size());
    for (const TagCount& t : next) {
        if (t.count == 0) continue;
        if (!merged.empty() && merged.back().tag == t.tag) {
            merged.back().count = static_cast<u16>(std::min<u32>(0xFFFFu, u32{merged.back().count} + t.count));
        } else {
            merged.push_back(t);
        }
    }
    bool sameSet = merged.size() == m_explicit.size();
    for (usize i = 0; sameSet && i < merged.size(); ++i) sameSet = merged[i].tag == m_explicit[i].tag;
    m_explicit = std::move(merged);
    rebuildBits();
    if (!sameSet) ++m_version;
    return {};
}

void TagContainer::replicatedTags(Audience viewer, std::vector<TagCount>& out) const {
    out.clear();
    for (const TagCount& t : m_explicit) {
        const Audience a = m_registry->info(t.tag).replicate;
        const bool send = viewer == Audience::Server || a == Audience::All || (viewer == Audience::Owner && a == Audience::Owner);
        if (send) out.push_back(t);
    }
}

void TagContainer::rebuildBits() noexcept {
    m_bits.clear();
    for (const TagCount& t : m_explicit) {
        for (u16 h : m_registry->hotAncestors(t.tag)) m_bits.set(h);
    }
}

void TagContainer::diff(std::span<const TagCount> before, std::span<const TagCount> now, std::vector<TagIndex>& added,
                        std::vector<TagIndex>& removed) {
    added.clear();
    removed.clear();
    usize i = 0, j = 0;
    while (i < before.size() || j < now.size()) {
        if (j >= now.size() || (i < before.size() && before[i].tag < now[j].tag)) {
            removed.push_back(before[i++].tag);
        } else if (i >= before.size() || now[j].tag < before[i].tag) {
            added.push_back(now[j++].tag);
        } else {
            ++i;
            ++j;
        }
    }
}

} // namespace helios::gameplay
