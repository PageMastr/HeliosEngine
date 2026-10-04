#pragma once
// Shared helpers of the engine/records tests.

#include <doctest/doctest.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gameplay/tags.gen.h"
#include "helios/records/records.h"
#include "helios/reflect/reflect.h"
#include "records_test.gen.h"

namespace helios::records::test {

/// The test types (test.records) plus the gameplay kernel's TagDef, in a registry of their own.
inline const refl::TypeRegistry& registry() {
    static refl::TypeRegistry* reg = [] {
        auto* r = new refl::TypeRegistry();
        REQUIRE(::test::records::registerRecordsTestTypes(*r).ok());
        REQUIRE(gameplay::registerTagsTypes(*r).ok());
        return r;
    }();
    return *reg;
}

inline const refl::TypeInfo& type(std::string_view name) {
    const refl::TypeInfo* t = registry().find(name);
    REQUIRE_MESSAGE(t != nullptr, name);
    return *t;
}

inline std::vector<SourceRecord> projectSources() {
    auto sources = collectSources(fs::pathFromUtf8(HELIOS_RECORDS_TEST_PROJECT), registry());
    REQUIRE_MESSAGE(sources.ok(), (sources.ok() ? "" : sources.error().message));
    return std::move(*sources);
}

inline CookOutput cookProject() {
    const std::vector<SourceRecord> sources = projectSources();
    auto out = cook(sources);
    REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
    return std::move(*out);
}

inline RecordDb open(std::span<const u8> bytes, const refl::TypeRegistry& reg = registry()) {
    auto db = RecordDb::openBytes(bytes, reg);
    REQUIRE_MESSAGE(db.ok(), (db.ok() ? "" : db.error().message));
    return std::move(*db);
}

inline SourceRecord source(std::string path, const refl::TypeInfo& t, std::string text) {
    return SourceRecord{std::move(path), &t, std::move(text)};
}

/// Cooks and expects failure; returns the diagnostics.
inline std::vector<CookDiagnostic> cookErrors(std::span<const SourceRecord> sources, const CookOptions& options = {}) {
    std::vector<CookDiagnostic> diags;
    auto out = cook(sources, options, &diags);
    REQUIRE_FALSE(out.ok());
    REQUIRE(!diags.empty());
    return diags;
}

inline bool hasDiag(const std::vector<CookDiagnostic>& diags, std::string_view path, std::string_view fragment) {
    return std::any_of(diags.begin(), diags.end(), [&](const CookDiagnostic& d) {
        return d.path == path && d.message.find(fragment) != std::string::npos;
    });
}

inline std::string dump(const std::vector<CookDiagnostic>& diags) {
    std::string s;
    for (const CookDiagnostic& d : diags) s += d.path + ": " + d.message + "\n";
    return s;
}

inline bool containsBytes(std::span<const u8> hay, std::span<const u8> needle) {
    return !needle.empty() && std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}
inline bool containsText(std::span<const u8> hay, std::string_view needle) {
    return containsBytes(hay, std::span<const u8>(reinterpret_cast<const u8*>(needle.data()), needle.size()));
}
template <class T>
bool containsValue(std::span<const u8> hay, const T& v) {
    return containsBytes(hay, std::span<const u8>(reinterpret_cast<const u8*>(&v), sizeof(T)));
}

/// Decodes a record of a DB into a new object of its type.
inline refl::Value decoded(const RecordDb& db, const RecordView& r) {
    REQUIRE(r);
    refl::Value v(*r.type);
    REQUIRE(db.decode(r, v.data()).ok());
    return v;
}

/// Resets the top-level fields `audience` drops to their defaults (what the cook leaves out).
inline void strip(const refl::TypeInfo& t, void* obj, CookAudience audience) {
    refl::Value fresh(t);
    for (const refl::FieldInfo& f : t.fields) {
        if (!keepsField(f, audience)) f.type().ops->copy(f.ptr(obj), f.ptr(fresh.data()));
    }
}

} // namespace helios::records::test
