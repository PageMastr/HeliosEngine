#pragma once
// Gameplay tags (06 §1.1): a hierarchical registry ("State.Debuff.Stun") interned to dense u16
// TagIndex values, hot-set bitsets for O(1) queries, and per-entity TagContainers with refcounts.
//
// Deterministic ids: the registry sorts every tag (declared ones plus their implied ancestors) by
// byte-wise name order and numbers them in that order. Segments use [A-Za-z0-9_], all of which sort
// after '.', so a tag's descendants form the contiguous range (index, subtreeEnd): "is X a descendant
// of T" is two integer compares, and ids depend only on the set of names (never on load order), so
// the client, the cell and the Go services agree. hash() covers names and audiences and is part of
// the content version (06 §1.1).
//
// Hot set: tags named by TagQuery content (markHot / markHotFromQuery; at most 1,024) get a bit in
// TagBits (128 B). Adding tag X sets the bits of X and of its hot ancestors, so a compiled query is
// `(b & all) == all && (any == 0 || (b & any) != 0) && !(b & none)` — 16-word operations. Tags
// outside the hot set fall back to a binary search of the container's sorted explicit tags.
//
// Threading: a built TagRegistry is immutable and safe to share across threads (hold it by
// shared_ptr; containers keep a pointer to it). A TagContainer is a per-entity value with no
// internal locking: one writer, or many readers.

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gameplay/tags.gen.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::gameplay {

using TagIndex = u16;
inline constexpr TagIndex kInvalidTag = 0xFFFF;
inline constexpr u32 kMaxTags = 0xFFFE;
inline constexpr u32 kMaxHotTags = 1024;
inline constexpr u16 kNotHot = 0xFFFF;

/// True for dotted names of [A-Za-z_][A-Za-z0-9_]* segments (at most 256 bytes).
bool isValidTagName(std::string_view name) noexcept;

/// The hot-set bitset (06 §1.1): 1,024 bits, 128 bytes.
struct TagBits {
    static constexpr u32 kWords = kMaxHotTags / 64;
    std::array<u64, kWords> words{};

    void set(u32 hot) noexcept { words[hot >> 6] |= u64{1} << (hot & 63); }
    bool test(u32 hot) const noexcept { return (words[hot >> 6] >> (hot & 63)) & 1; }
    void clear() noexcept { words.fill(0); }
    bool none() const noexcept;
    /// (this & mask) == mask.
    bool containsAll(const TagBits& mask) const noexcept;
    /// (this & mask) != 0.
    bool intersects(const TagBits& mask) const noexcept;
    TagBits& operator|=(const TagBits& o) noexcept;
    friend bool operator==(const TagBits&, const TagBits&) = default;
};

/// Registry entry of one tag.
struct TagInfo {
    std::string name;
    TagIndex parent = kInvalidTag;
    TagIndex subtreeEnd = 0;     ///< exclusive end of the descendant range (index, subtreeEnd)
    u16 hot = kNotHot;           ///< hot-set bit, or kNotHot
    u8 depth = 0;                ///< 0 for a root tag
    bool declared = false;       ///< declared by a TagDef (not only implied as an ancestor)
    Audience replicate = Audience::Server;
};

class TagContainer;

/// A compiled TagQuery {all, any, none} (06 §1.1). Text form: `all(A.B, C) any(D, E) none(F)`;
/// each clause is optional and appears at most once; the empty query matches everything. Matching
/// is hierarchical: `all(State.Debuff)` matches an entity holding State.Debuff.Stun.
struct TagQuery {
    TagBits allMask;
    TagBits anyMask;
    TagBits noneMask;
    std::vector<TagIndex> allCold; ///< tags outside the hot set (checked by search)
    std::vector<TagIndex> anyCold;
    std::vector<TagIndex> noneCold;
    bool hasAny = false;           ///< the query has an any() clause
    /// TagRegistry::queryHash() of the registry that compiled the query: the masks are hot-set bits
    /// of that registry and mean nothing in another (0 for a query built by hand).
    u64 registry = 0;

    /// True for the empty query (no clause).
    bool matchesEverything() const noexcept;
    /// Evaluates the query; `tags` must use the registry that compiled it.
    bool matches(const TagContainer& tags) const noexcept;
};

/// Immutable tag registry. Build it with TagRegistry::Builder or fromRecords().
class TagRegistry {
public:
    class Builder {
    public:
        /// Declares a tag (and implicitly its ancestors). Declaring it again with another audience makes
        /// build() fail.
        Result<void> add(std::string_view tag, Audience replicate = Audience::Server);
        /// Declares the tag of a TagDef record.
        Result<void> addRecord(const TagDef& def) { return add(def.tag.view(), def.replicate); }
        /// Puts a tag into the hot set (it must be declared by build()).
        Result<void> markHot(std::string_view tag);
        /// Marks every tag named by a TagQuery text as hot.
        Result<void> markHotFromQuery(std::string_view queryText);
        /// Makes every tag hot (fails in build() if there are more than kMaxHotTags).
        void markAllHot() noexcept { m_allHot = true; }
        /// Assigns deterministic ids and computes subtree ranges, hot bits and the hash.
        Result<std::shared_ptr<const TagRegistry>> build() const;

    private:
        struct Decl {
            std::string name;
            Audience replicate;
        };
        std::vector<Decl> m_decls;
        std::vector<std::string> m_hot;
        bool m_allHot = false;
    };

    /// Builds a registry from TagDef records. `queries` are TagQuery texts found in content; their
    /// tags become the hot set. With no queries, every tag is hot when there are at most 1,024.
    static Result<std::shared_ptr<const TagRegistry>> fromRecords(std::span<const TagDef> defs,
                                                                  std::span<const std::string> queries = {});

    /// Number of tags (declared ones and their implied ancestors); valid indices are [0, size()).
    usize size() const noexcept { return m_tags.size(); }
    /// Number of tags in the hot set (at most kMaxHotTags).
    u32 hotCount() const noexcept { return m_hotCount; }
    /// kInvalidTag if the name is unknown. O(log n).
    TagIndex find(std::string_view name) const noexcept;
    /// Registry entry of a valid index (< size(); unchecked, like the other index accessors).
    const TagInfo& info(TagIndex tag) const noexcept { return m_tags[tag]; }
    std::string_view name(TagIndex tag) const noexcept { return m_tags[tag].name; }
    /// True if `tag` is `ancestor` or one of its descendants.
    bool isSelfOrDescendant(TagIndex tag, TagIndex ancestor) const noexcept {
        return ancestor <= tag && tag < m_tags[ancestor].subtreeEnd;
    }
    /// Hot-set bits of the tag and of its hot ancestors (what adding the tag sets in TagBits).
    std::span<const u16> hotAncestors(TagIndex tag) const noexcept {
        return std::span<const u16>(m_hotList).subspan(m_hotOffset[tag], m_hotOffset[tag + 1] - m_hotOffset[tag]);
    }
    /// Compiles a TagQuery text (see TagQuery). Unknown tags or bad syntax fail with ParseError /
    /// NotFound naming the byte offset.
    Result<TagQuery> compileQuery(std::string_view text) const;
    /// FNV-1a 64 over the sorted names and audiences: part of the content version.
    u64 hash() const noexcept { return m_hash; }
    /// hash() plus the hot-set assignment: equal only for registries on which compiled TagQuery
    /// masks mean the same thing (a compiled query records it).
    u64 queryHash() const noexcept { return m_queryHash; }

private:
    std::vector<TagInfo> m_tags;
    std::vector<u32> m_hotOffset; // size() + 1 entries into m_hotList
    std::vector<u16> m_hotList;
    u32 m_hotCount = 0;
    u64 m_hash = 0;
    u64 m_queryHash = 0;
};

/// A tag and how many sources grant it (06 §1.1 TagCounts).
struct TagCount {
    TagIndex tag = kInvalidTag;
    u16 count = 0;
    friend bool operator==(const TagCount&, const TagCount&) = default;
};

/// Per-entity tags: sorted (TagIndex, refcount) explicit tags plus the derived hot-set bits.
class TagContainer {
public:
    explicit TagContainer(const TagRegistry& registry) noexcept : m_registry(&registry) {}

    const TagRegistry& registry() const noexcept { return *m_registry; }

    /// Adds `count` references; returns true if the tag was absent before. An index outside the
    /// registry is a caller bug (asserted) and is ignored (returns false).
    bool add(TagIndex tag, u16 count = 1);
    /// Removes up to `count` references; returns true if this call removed the tag (it was held and
    /// its last reference is gone).
    bool remove(TagIndex tag, u16 count = 1);
    void clear() noexcept;

    /// References held for exactly this tag.
    u16 count(TagIndex tag) const noexcept;
    /// Exact match: the tag itself is held.
    bool hasExact(TagIndex tag) const noexcept { return count(tag) != 0; }
    /// Parent-of match: the tag or one of its descendants is held (GAS HasTag). False for an index
    /// outside the registry (e.g. a query compiled against another registry).
    bool has(TagIndex tag) const noexcept;
    bool hasAny(std::span<const TagIndex> tags) const noexcept;
    bool hasAll(std::span<const TagIndex> tags) const noexcept;
    bool matches(const TagQuery& query) const noexcept { return query.matches(*this); }

    const TagBits& bits() const noexcept { return m_bits; }
    /// Explicit tags sorted by TagIndex (what replicates, as index deltas).
    std::span<const TagCount> explicitTags() const noexcept { return m_explicit; }
    /// Replaces the explicit tags (replication receive) and rebuilds the bits. Entries are sorted and
    /// merged (counts saturate at 0xFFFF); zero counts are dropped. The input is untrusted: an index
    /// outside the registry fails with OutOfRange and leaves the container unchanged.
    Result<void> assign(std::span<const TagCount> tags);
    /// The explicit tags a viewer may receive (06 §1.6: explicit tags replicate per declaration),
    /// sorted by tag: Audience::All selects tags declared All (every client), Audience::Owner those
    /// declared Owner or All (the owning client), Audience::Server all of them (server to server).
    /// Feed the result to diff() to produce the index deltas.
    void replicatedTags(Audience viewer, std::vector<TagCount>& out) const;
    /// Incremented whenever the set of held tags changes (not on refcount-only changes).
    u32 version() const noexcept { return m_version; }

    /// Index deltas between two explicit sets (replication): tags present in `now` but not in
    /// `before`, and the reverse. Both inputs sorted by tag.
    static void diff(std::span<const TagCount> before, std::span<const TagCount> now, std::vector<TagIndex>& added,
                     std::vector<TagIndex>& removed);

private:
    void rebuildBits() noexcept;

    const TagRegistry* m_registry;
    std::vector<TagCount> m_explicit;
    TagBits m_bits;
    u32 m_version = 0;
};

} // namespace helios::gameplay
