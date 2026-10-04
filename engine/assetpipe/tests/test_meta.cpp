// `.meta` sidecars v0 (02 §6.1, 07 T24): the importer registry, the licence and path rules, settings
// through reflect, canonical text, precise errors, create-on-first-import, GUID-stable moves and the scan.

#include <algorithm>
#include <string>
#include <vector>

#include "assetpipe_test_util.h"
#include "helios/core/platform.h"

namespace {

using namespace assetpipe_test;

const Guid kGuid = Guid::parse("8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18").value();

AssetMeta sampleMeta() {
    AssetMeta m;
    m.guid = kGuid;
    m.importer = "png";
    m.importerVersion = 2;
    m.settings = R"({"mips":false,"lods":[0,1]})";
    m.labels = {"hull", "scout"};
    m.source = "art/src/scout/hull.blend";
    m.provenance = original();
    m.provenance.notes = "Blocked out for the Phase 1 slice.";
    return m;
}

/// The sample's canonical text: the layout meta.h documents.
constexpr std::string_view kSampleText = R"({
  "$meta": 0,
  "guid": "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18",
  "importer": "png",
  "importerVersion": 2,
  "settings": {
    "mips": false,
    "lods": [0, 1]
  },
  "labels": ["hull", "scout"],
  "source": "art/src/scout/hull.blend",
  "provenance": {
    "origin": "original",
    "author": "Owner",
    "licence": "MIT",
    "notes": "Blocked out for the Phase 1 slice."
  }
}
)";

std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
    std::string out(text);
    const usize at = out.find(from);
    REQUIRE(at != std::string::npos);
    out.replace(at, from.size(), to);
    return out;
}

TEST_CASE("meta: the importer registry checks ids, versions, extensions and settings types") {
    ImporterRegistry r = makeRegistry();
    CHECK(r.size() == 2);
    CHECK(r.find("png")->version == 2);
    CHECK(r.find("PNG") == nullptr);
    CHECK(r.forFile("art/Hull.PNG")->id == "png");
    CHECK(r.forFile("a.tga")->id == "png");
    CHECK(r.forFile("fonts/ui.ttf")->id == "font");
    CHECK(r.forFile("a.png.meta") == nullptr);
    CHECK(r.forFile(".png") == nullptr); // a dotfile has no extension
    CHECK(r.forFile("png") == nullptr);

    ImporterInfo bad = textureImporter();
    bad.id = "png";
    CHECK(r.add(bad).error().code == ErrorCode::AlreadyExists);
    for (const char* id : {"", "Png", "-x", ".x", "a b", "a/b",
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        CAPTURE(id);
        bad.id = id;
        CHECK(r.add(bad).error().code == ErrorCode::InvalidArgument);
    }
    bad = textureImporter();
    bad.id = "tex2";
    bad.extensions = {".tga"};
    CHECK(r.add(bad).error().code == ErrorCode::AlreadyExists); // ".tga" is png's
    for (const char* ext : {"tga", ".TGA", ".", ".a-b", ".abcdefghijklmnopq"}) {
        CAPTURE(ext);
        bad.extensions = {ext};
        CHECK(r.add(bad).error().code == ErrorCode::InvalidArgument);
    }
    bad.extensions = {".dds"};
    bad.version = 0;
    CHECK(r.add(bad).error().code == ErrorCode::InvalidArgument);
    bad.version = 1;
    bad.settings = &refl::typeOf<u32>();
    CHECK(r.add(bad).error().code == ErrorCode::InvalidArgument);
    bad.settings = nullptr;
    CHECK(r.add(bad));
    CHECK(r.forFile("x.dds")->id == "tex2");
}

TEST_CASE("meta: licences fail closed on anything off 01 §5.2's list (OFL-1.1 for fonts only)") {
    CHECK(allowedLicences().size() == 9);
    for (const std::string_view id : allowedLicences()) {
        CAPTURE(id);
        CHECK(checkLicence(id, false));
    }
    CHECK(checkLicence("OFL-1.1", true));
    CHECK(checkLicence("OFL-1.1", false).error().message.find("font assets only") != std::string::npos);
    CHECK(checkLicence("mit", false).error().message.find("write the SPDX id 'MIT'") != std::string::npos);
    CHECK(checkLicence("cc0-1.0", false).error().message.find("'CC0-1.0'") != std::string::npos);
    for (const char* bad :
         {"", "CC-BY-4.0", "CC-BY-NC-4.0", "GPL-3.0-only", "AGPL-3.0", "LGPL-2.1", "MPL-2.0", "public domain",
          "Unlicense", "proprietary", "MIT OR GPL-3.0", " MIT"}) {
        CAPTURE(bad);
        const auto r = checkLicence(bad, true);
        REQUIRE(!r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("meta: project paths follow the Windows-first rule") {
    for (const char* ok : {"a.png", "art/ships/Scout Hull.png", "a/b/c.d.e", "con10.png", "nullable/x.png",
                           "\xC3\xA9t\xC3\xA9/caf\xC3\xA9.png"}) {
        CAPTURE(ok);
        CHECK(checkProjectPath(ok));
    }
    for (const char* bad : {"",
                            "/abs.png",
                            "dir/",
                            "a//b.png",
                            "./a.png",
                            "a/../b.png",
                            "..",
                            "a\\b.png",
                            "C:/x.png",
                            "c:x.png",
                            "a?.png",
                            "a*.png",
                            "a<b.png",
                            "a|b.png",
                            "a\"b.png",
                            "con.png",
                            "art/NUL",
                            "Com1.tga",
                            "lpt9.png",
                            "conout$.png",
                            "trailing./x.png",
                            "space /x.png",
                            "x.png ",
                            "tab\t.png",
                            "nl\n.png",
                            "c1\xC2\x85.png"}) {
        CAPTURE(bad);
        const auto r = checkProjectPath(bad);
        REQUIRE(!r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    CHECK(checkProjectPath("a/" + std::string(255, 'n') + "/b.png"));
    CHECK(!checkProjectPath("a/" + std::string(256, 'n') + "/b.png"));
}

TEST_CASE("meta: settings resolve through the importer's reflected type, canonical and strict") {
    const ImporterRegistry r = makeRegistry();
    const ImporterInfo& png = *r.find("png");
    CHECK(resolveSettings(png, "{}").value() == "{}");
    CHECK(resolveSettings(png, R"({"mips": true, "maxSize": 2048})").value() == "{}"); // defaults are omitted
    // Schema order, whatever order they were written in; comments and trailing commas are JSONC.
    CHECK(resolveSettings(png, R"({"format": "bc5", /* note */ "mips": false,})").value() ==
          R"({"mips":false,"format":"bc5"})");
    CHECK(resolveSettings(png, R"({"lods": [2, 1]})").value() == R"({"lods":[2,1]})");

    const auto unknown = resolveSettings(png, R"({"mipz": false})");
    REQUIRE(!unknown);
    CHECK(unknown.error().message.find("settings.mipz: unknown field") != std::string::npos);
    const auto wrongType = resolveSettings(png, R"({"maxSize": "big"})");
    REQUIRE(!wrongType);
    CHECK(wrongType.error().message.find("settings.maxSize") != std::string::npos);
    CHECK(!resolveSettings(png, R"({"mips": false, "mips": true})"));
    CHECK(!resolveSettings(png, "[]"));
    CHECK(!resolveSettings(png, "{"));

    const ImporterInfo& font = *r.find("font");
    CHECK(resolveSettings(font, "{}").value() == "{}");
    CHECK(resolveSettings(font, R"({"x": 1})").error().message.find("takes no settings") !=
          std::string::npos);
}

TEST_CASE("meta: canonical text, and a rewrite of it is byte-identical") {
    const ImporterRegistry r = makeRegistry();
    const AssetMeta meta = sampleMeta();
    const std::string text = writeMeta(meta, r).value();
    CHECK(text == kSampleText);
    const AssetMeta parsed = parseMeta(text, r).value();
    CHECK(parsed == meta);
    CHECK(writeMeta(parsed, r).value() == text);

    // Any spelling of the same sidecar parses to the same value and rewrites to the canonical text.
    const std::string messy = R"(// hand-edited
{
  "provenance": {"licence": "MIT", "notes": "Blocked out for the Phase 1 slice.",
                 "author": "Owner", "origin": "original"},
  "labels": ["scout", "hull", "scout"],
  "settings": {"lods": [0, 1], "mips": false, "maxSize": 2048},
  "source": "art/src/scout/hull.blend",
  "importerVersion": 2, "importer": "png",
  "guid": "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18", "$meta": 0,
})";
    const AssetMeta fromMessy = parseMeta(messy, r).value();
    CHECK(fromMessy == meta);
    CHECK(writeMeta(fromMessy, r).value() == text);

    // Minimal sidecar: defaults are omitted, and an AI-assisted one keeps its record in order.
    AssetMeta minimal;
    minimal.guid = kGuid;
    minimal.importer = "font";
    minimal.importerVersion = 1;
    minimal.provenance.origin = Origin::AiAssisted;
    minimal.provenance.author = "Owner";
    minimal.provenance.licence = "OFL-1.1";
    minimal.provenance.ai = AiProvenance{"Gen", "gen-2", "a \"hull\" glyph\nsecond line", {}};
    const std::string minimalText = writeMeta(minimal, r).value();
    CHECK(minimalText == R"({
  "$meta": 0,
  "guid": "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18",
  "importer": "font",
  "importerVersion": 1,
  "provenance": {
    "origin": "ai-assisted",
    "author": "Owner",
    "licence": "OFL-1.1",
    "ai": {
      "tool": "Gen",
      "model": "gen-2",
      "prompt": "a \"hull\" glyph\nsecond line"
    }
  }
}
)");
    CHECK(parseMeta(minimalText, r).value() == minimal);
}

TEST_CASE("meta: parse errors name the file, the field and the problem") {
    const ImporterRegistry r = makeRegistry();
    struct Case {
        std::string text;
        ErrorCode code;
        std::string_view needle;
    };
    const std::string t(kSampleText);
    const std::vector<Case> cases = {
        {replaced(t, R"(  "guid": "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18",
)",
                  ""),
         ErrorCode::ParseError, "a.png.meta: missing required field 'guid'"},
        {replaced(t, "8c0f4d1e", "8C0F4D1E"), ErrorCode::ParseError,
         "guid: '8C0F4D1E-2b7a-4c39-9e51-0d6a3f2b7c18' is not in canonical form"},
        {replaced(t, "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18", "{8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18}"),
         ErrorCode::ParseError, "not in canonical form"},
        {replaced(t, "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18", "not-a-guid"), ErrorCode::ParseError,
         "guid: 'not-a-guid' is not a GUID"},
        {replaced(t, "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18", "00000000-0000-0000-0000-000000000000"),
         ErrorCode::InvalidArgument, "guid: the nil GUID"},
        {replaced(t, R"("guid": "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18")", R"("guid": 7)"),
         ErrorCode::ParseError, "guid: expected string"},
        {replaced(t, R"("importer": "png")", R"("importer": "gltf")"), ErrorCode::NotFound,
         "importer: 'gltf' is not a registered importer"},
        {replaced(t, R"("importerVersion": 2)", R"("importerVersion": 3)"), ErrorCode::VersionMismatch,
         "written for 'png' version 3, but this build has version 2"},
        {replaced(t, R"("importerVersion": 2)", R"("importerVersion": 0)"), ErrorCode::InvalidArgument,
         "importerVersion: versions start at 1"},
        {replaced(t, R"("importerVersion": 2)", R"("importerVersion": -1)"), ErrorCode::ParseError,
         "importerVersion: expected non-negative integer"},
        {replaced(t, R"("importerVersion": 2)", R"("importerVersion": 4294967296)"), ErrorCode::ParseError,
         "importerVersion: above 4294967295"},
        {replaced(t, R"("$meta": 0)", R"("$meta": 1)"), ErrorCode::VersionMismatch,
         "$meta: sidecar format 1"},
        {replaced(t, R"(  "$meta": 0,
)",
                  ""),
         ErrorCode::ParseError, "missing required field '$meta'"},
        {replaced(t, R"("mips": false)", R"("mips": "no")"), ErrorCode::ParseError, "settings.mips"},
        {replaced(t, R"("mips": false)", R"("mipz": false)"), ErrorCode::ParseError,
         "settings.mipz: unknown field"},
        {replaced(t, R"("labels": ["hull", "scout"])", R"("labels": "hull")"), ErrorCode::ParseError,
         "labels: expected array of strings"},
        {replaced(t, R"("labels": ["hull", "scout"])", R"("labels": ["hull", 3])"), ErrorCode::ParseError,
         "labels[1]: expected string"},
        {replaced(t, R"("labels": ["hull", "scout"])", R"("labels": [""])"), ErrorCode::InvalidArgument,
         "labels[0]"},
        {replaced(t, "art/src/scout/hull.blend", "art\\\\src\\\\hull.blend"), ErrorCode::InvalidArgument,
         "source: 'art\\src\\hull.blend': '\\' is not a separator"},
        {replaced(t, "art/src/scout/hull.blend", "C:/art/hull.blend"), ErrorCode::InvalidArgument,
         "source: 'C:/art/hull.blend'"},
        {replaced(t, "art/src/scout/hull.blend", "art/aux.blend"), ErrorCode::InvalidArgument,
         "Windows device name"},
        {replaced(t, R"("source")", R"("sources")"), ErrorCode::ParseError, "sources: unknown field"},
        {replaced(t, R"("labels")", R"("guid": "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18", "labels")"),
         ErrorCode::ParseError, "duplicate key 'guid'"},
        {replaced(t, R"(    "origin": "original",
)",
                  ""),
         ErrorCode::ParseError, "provenance: missing required field 'origin'"},
        {replaced(t, R"("origin": "original")", R"("origin": "found-online")"), ErrorCode::ParseError,
         "provenance.origin: 'found-online' is not an origin"},
        {replaced(t, R"("author": "Owner")", R"("author": "  ")"), ErrorCode::InvalidArgument,
         "provenance.author: must not be empty"},
        {replaced(t, R"("licence": "MIT")", R"("licence": "CC-BY-4.0")"), ErrorCode::InvalidArgument,
         "provenance.licence: 'CC-BY-4.0' is not an allowed licence"},
        {replaced(t, R"("licence": "MIT")", R"("licence": "OFL-1.1")"), ErrorCode::InvalidArgument,
         "font assets only"},
        {replaced(t, R"("licence": "MIT",)", R"("license": "MIT",)"), ErrorCode::ParseError,
         "provenance.license: unknown field"},
        {replaced(t, R"("origin": "original")", R"("origin": "cc0")"), ErrorCode::InvalidArgument,
         "provenance.url: a cc0 asset records where it was obtained"},
        {replaced(t, R"("origin": "original")", R"("origin": "cc0", "url": "https://example.org/hull")"),
         ErrorCode::InvalidArgument, "a cc0 asset keeps licence 'CC0-1.0'"},
        {replaced(t, R"("origin": "original")", R"("origin": "commissioned")"), ErrorCode::InvalidArgument,
         "provenance.rights"},
        {replaced(t, R"("origin": "original")", R"("origin": "ai-assisted")"), ErrorCode::InvalidArgument,
         "provenance.ai: an ai-assisted asset records"},
        {replaced(t, R"("origin": "original",)",
                  R"("origin": "original", "ai": {"tool": "t", "model": "m", "prompt": "p"},)"),
         ErrorCode::InvalidArgument, "only an ai-assisted asset has 'ai'"},
        {replaced(t, R"("origin": "original",)",
                  R"("origin": "ai-assisted", "ai": {"tool": "t", "model": "m"},)"),
         ErrorCode::ParseError, "provenance.ai: missing required field 'prompt'"},
        {replaced(t, R"("origin": "original",)",
                  R"("origin": "ai-assisted", "ai": {"tool": "t", "model": "m", "prompt": "p", "seed": 1},)"),
         ErrorCode::ParseError, "provenance.ai.seed: unknown field"},
        {replaced(t, R"(  "provenance": {
    "origin": "original",
    "author": "Owner",
    "licence": "MIT",
    "notes": "Blocked out for the Phase 1 slice."
  }
)",
                  R"(  "provenance": null
)"),
         ErrorCode::ParseError, "provenance: expected object"},
        {"[]", ErrorCode::ParseError, "expected object"},
        {"{", ErrorCode::ParseError, "a.png.meta:"},
        {"", ErrorCode::ParseError, "a.png.meta"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.text);
        const auto r2 = parseMeta(c.text, r, "a.png.meta");
        REQUIRE(!r2);
        CHECK(r2.error().code == c.code);
        CAPTURE(r2.error().message);
        CHECK(r2.error().message.find(c.needle) != std::string::npos);
        CHECK(r2.error().message.starts_with("a.png.meta"));
    }
    // A missing provenance is refused outright (01 §5.2: every asset records it).
    const usize at = t.find(R"(,
  "provenance")");
    const std::string noProvenance = t.substr(0, at) + "\n}\n";
    CHECK(parseMeta(noProvenance, r).error().message.find("missing required field 'provenance'") !=
          std::string::npos);
}

TEST_CASE("meta: hostile sidecars fail cleanly") {
    const ImporterRegistry r = makeRegistry();
    // Too large: refused before parsing.
    std::string big(kMaxMetaBytes + 1, ' ');
    CHECK(parseMeta(big, r).error().code == ErrorCode::LimitExceeded);
    // Deep nesting inside the settings: bounded by reflect's JSON depth limit.
    std::string deep = std::string(kSampleText);
    deep = replaced(deep, R"("mips": false)", R"("lods": )" + std::string(300, '[') + std::string(300, ']'));
    CHECK(parseMeta(deep, r).error().code == ErrorCode::LimitExceeded);
    // Invalid UTF-8 and NULs inside strings.
    CHECK(!parseMeta(replaced(kSampleText, "Owner", "Ow\xFF\xFEner"), r));
    const auto nul = parseMeta(replaced(kSampleText, "art/src/scout/hull.blend", "art/x\\u0000.blend"), r);
    REQUIRE(!nul);
    CHECK(nul.error().message.find("control character") != std::string::npos);
    CHECK(parseMeta(replaced(kSampleText, R"(["hull", "scout"])", "[\"" + std::string(65, 'x') + "\"]"), r)
              .error()
              .message.find("labels[0]") != std::string::npos);
    std::string many = "[";
    for (int i = 0; i < 65; ++i) many += (i ? ",\"l" : "\"l") + std::to_string(i) + "\"";
    many += "]";
    CHECK(parseMeta(replaced(kSampleText, R"(["hull", "scout"])", many), r).error().code ==
          ErrorCode::LimitExceeded);
}

TEST_CASE("meta: create on first import mints the GUID once") {
    TempDir dir;
    const ImporterRegistry r = makeRegistry();
    writeText(dir.path, "art/hull.png", "png bytes");
    NewMeta init = newMeta(R"({"mips": false})");
    init.labels = {"b", "a", "b"};
    const EnsuredMeta first = ensureMeta(dir.path, "art/hull.png", init, r).value();
    CHECK(first.created);
    CHECK(!first.meta.guid.isNil());
    CHECK(first.meta.guid.version() == 4);
    CHECK(first.meta.importer == "png");
    CHECK(first.meta.importerVersion == 2);
    CHECK(first.meta.settings == R"({"mips":false})");
    CHECK(first.meta.labels == std::vector<std::string>{"a", "b"});
    const std::string text = readText(dir.path, "art/hull.png.meta");
    CHECK(text == writeMeta(first.meta, r).value());

    // Again: the same sidecar, untouched, whatever `init` says now.
    init.settings = R"({"mips": true})";
    const EnsuredMeta second = ensureMeta(dir.path, "art/hull.png", init, r).value();
    CHECK(!second.created);
    CHECK(second.meta == first.meta);
    CHECK(readText(dir.path, "art/hull.png.meta") == text);
    CHECK(loadMeta(dir.path, "art/hull.png", r).value() == first.meta);

    // Refusals write nothing.
    CHECK(ensureMeta(dir.path, "art/missing.png", newMeta(), r).error().code == ErrorCode::NotFound);
    writeText(dir.path, "notes.txt", "x");
    CHECK(ensureMeta(dir.path, "notes.txt", newMeta(), r).error().code == ErrorCode::NotFound);
    NewMeta wrongImporter = newMeta();
    wrongImporter.importer = "font";
    CHECK(ensureMeta(dir.path, "art/hull.png", wrongImporter, r)); // exists: returned as is
    writeText(dir.path, "art/deck.png", "png bytes");
    CHECK(ensureMeta(dir.path, "art/deck.png", wrongImporter, r).error().code == ErrorCode::InvalidArgument);
    NewMeta badLicence = newMeta();
    badLicence.provenance.licence = "CC-BY-4.0";
    CHECK(ensureMeta(dir.path, "art/deck.png", badLicence, r).error().message.find("provenance.licence") !=
          std::string::npos);
    NewMeta badSettings = newMeta(R"({"nope": 1})");
    CHECK(!ensureMeta(dir.path, "art/deck.png", badSettings, r));
    CHECK(!fileExists(dir.path, "art/deck.png.meta"));
    CHECK(ensureMeta(dir.path, "art/../deck.png", newMeta(), r).error().code == ErrorCode::InvalidArgument);

    // A sidecar spelled in another case is not a reason to mint a second GUID: where names differ by case
    // it is refused, and where they do not (Windows) it is the file's sidecar.
    writeText(dir.path, "art/cased.png", "png bytes");
    writeText(dir.path, "art/CASED.PNG.meta", writeMeta(sampleMeta(), r).value());
    const auto cased = ensureMeta(dir.path, "art/cased.png", newMeta(), r);
    if (caseSensitive(dir.path)) {
        CHECK(cased.error().code == ErrorCode::InvalidState);
    } else {
        CHECK(!cased.value().created);
        CHECK(cased.value().meta.guid == kGuid);
    }
}

TEST_CASE("meta: saveMeta never changes a GUID and skips identical rewrites") {
    TempDir dir;
    const ImporterRegistry r = makeRegistry();
    writeText(dir.path, "a.png", "png");
    AssetMeta meta = ensureMeta(dir.path, "a.png", newMeta(), r).value().meta;
    const auto stamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(5);
    REQUIRE(fs::setLastWriteTime(dir.path / "a.png.meta", stamp));
    CHECK(saveMeta(dir.path, "a.png", meta, r));
    CHECK(fs::lastWriteTime(dir.path / "a.png.meta").value() == stamp); // identical bytes: not rewritten

    meta.labels = {"edited"};
    meta.settings = R"({"maxSize":1024})";
    CHECK(saveMeta(dir.path, "a.png", meta, r));
    CHECK(loadMeta(dir.path, "a.png", r).value() == meta);

    AssetMeta other = meta;
    other.guid = Guid::generate();
    CHECK(saveMeta(dir.path, "a.png", other, r).error().code == ErrorCode::InvalidState);
    CHECK(loadMeta(dir.path, "a.png", r).value().guid == meta.guid);
    // Non-canonical settings or an invalid record are never written.
    other = meta;
    other.settings = R"({"maxSize": 1024})";
    CHECK(saveMeta(dir.path, "a.png", other, r).error().message.find("not canonical") != std::string::npos);
    other = meta;
    other.provenance.licence = "GPL-3.0-only";
    CHECK(!saveMeta(dir.path, "a.png", other, r));
    CHECK(loadMeta(dir.path, "a.png", r).value() == meta);
    // A broken sidecar whose GUID cannot be read is not overwritten.
    writeText(dir.path, "a.png.meta", "{ not json");
    CHECK(saveMeta(dir.path, "a.png", meta, r).error().code == ErrorCode::InvalidState);
    // ...but one whose GUID matches is repaired.
    writeText(dir.path, "a.png.meta", R"({"guid": ")" + meta.guid.toString() + R"(", "licence": "?"})");
    CHECK(saveMeta(dir.path, "a.png", meta, r));
    CHECK(loadMeta(dir.path, "a.png", r).value() == meta);
}

TEST_CASE("meta: moves and renames keep the GUID (the GUID follows the file)") {
    TempDir dir;
    const ImporterRegistry r = makeRegistry();
    writeText(dir.path, "art/hull.png", "hull");
    const AssetMeta meta =
        ensureMeta(dir.path, "art/hull.png", newMeta(R"({"mips": false})"), r).value().meta;
    const std::string sidecar = readText(dir.path, "art/hull.png.meta");

    REQUIRE(moveAsset(dir.path, "art/hull.png", "ships/scout/hull.tga", r));
    CHECK(!fileExists(dir.path, "art/hull.png"));
    CHECK(!fileExists(dir.path, "art/hull.png.meta"));
    CHECK(readText(dir.path, "ships/scout/hull.tga") == "hull");
    CHECK(readText(dir.path, "ships/scout/hull.tga.meta") == sidecar); // moved, not rewritten
    CHECK(loadMeta(dir.path, "ships/scout/hull.tga", r).value() == meta);

    // Case-only rename (one file on Windows): through a temporary name, GUID kept.
    REQUIRE(moveAsset(dir.path, "ships/scout/hull.tga", "ships/scout/Hull.tga", r));
    CHECK(loadMeta(dir.path, "ships/scout/Hull.tga", r).value().guid == meta.guid);
    const auto listing = fs::listDirectory(dir.path / "ships" / "scout").value();
    REQUIRE(listing.size() == 2);
    CHECK(listing[0].relativePath == "Hull.tga");
    CHECK(listing[1].relativePath == "Hull.tga.meta");
    CHECK(moveAsset(dir.path, "ships/scout/Hull.tga", "ships/scout/Hull.tga", r)); // no-op

    // Refusals move nothing.
    writeText(dir.path, "ships/deck.png", "deck");
    REQUIRE(ensureMeta(dir.path, "ships/deck.png", newMeta(), r));
    CHECK(moveAsset(dir.path, "ships/scout/Hull.tga", "ships/deck.png", r).error().code ==
          ErrorCode::AlreadyExists);
    CHECK(moveAsset(dir.path, "ships/scout/Hull.tga", "ships/DECK.PNG", r).error().code ==
          ErrorCode::AlreadyExists);
    writeText(dir.path, "ships/orphan.png.meta", "{}");
    CHECK(moveAsset(dir.path, "ships/scout/Hull.tga", "ships/Orphan.png", r).error().code ==
          ErrorCode::AlreadyExists);
    CHECK(moveAsset(dir.path, "ships/scout/Hull.tga", "ships/hull.ttf", r).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(moveAsset(dir.path, "ships/scout/Hull.tga", "ships/con.png", r).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(moveAsset(dir.path, "ships/scout/Hull.tga", "../out.png", r).error().code ==
          ErrorCode::InvalidArgument);
    writeText(dir.path, "loose.png", "no sidecar");
    CHECK(moveAsset(dir.path, "loose.png", "loose2.png", r).error().code == ErrorCode::NotFound);
    CHECK(fileExists(dir.path, "loose.png"));
    CHECK(loadMeta(dir.path, "ships/scout/Hull.tga", r).value().guid == meta.guid);
    CHECK(readText(dir.path, "ships/deck.png") == "deck");
}

TEST_CASE("meta: the scan reports every missing, orphan, mis-cased, duplicate and colliding sidecar") {
    TempDir dir;
    const ImporterRegistry r = makeRegistry();
    // Good assets.
    writeText(dir.path, "a/good.png", "1");
    writeText(dir.path, "a/font.ttf", "2");
    const Guid goodGuid = ensureMeta(dir.path, "a/good.png", newMeta(), r).value().meta.guid;
    REQUIRE(ensureMeta(dir.path, "a/font.ttf", newMeta(), r));
    // Not assets: ignored.
    writeText(dir.path, "a/readme.txt", "x");
    writeText(dir.path, ".git/objects/x.png", "hidden");
    writeText(dir.path, "a/.cache/y.png", "hidden");
    // Problems.
    writeText(dir.path, "b/nometa.png", "3");
    writeText(dir.path, "b/orphan.png.meta", writeMeta(sampleMeta(), r).value());
    writeText(dir.path, "b/Cased.png", "4");
    writeText(dir.path, "b/cased.png.meta", writeMeta(sampleMeta(), r).value());
    writeText(dir.path, "b/upper.png", "5");
    writeText(dir.path, "b/upper.png.META", "{}");
    writeText(dir.path, "c/copy.png", "6");
    writeText(dir.path, "c/copy.png.meta", readText(dir.path, "a/good.png.meta")); // a copied sidecar
    writeText(dir.path, "c/broken.png", "7");
    writeText(dir.path, "c/broken.png.meta",
              replaced(readText(dir.path, "a/good.png.meta"), "\"MIT\"", "\"CC-BY-4.0\""));
    writeText(dir.path, "c/wrong.tga", "8");
    AssetMeta fontMeta = sampleMeta();
    fontMeta.guid = Guid::generate();
    fontMeta.importer = "font";
    fontMeta.importerVersion = 1;
    fontMeta.settings = "{}";
    writeText(dir.path, "c/wrong.tga.meta", writeMeta(fontMeta, r).value()); // font importer on a .tga
    // Names Windows cannot hold: two that differ only in case, and a device name.
    const bool sensitive = caseSensitive(dir.path);
    if (sensitive) {
        writeText(dir.path, "d/twin.png", "9");
        writeText(dir.path, "d/TWIN.png", "10");
    }
    if constexpr (!platform::kIsWindows) writeText(dir.path, "d/nul.png", "11");

    const MetaScan scan = scanMetas(dir.path, r).value();
    std::vector<std::string> assets;
    for (const ScannedAsset& a : scan.assets) assets.push_back(a.path);
    CHECK(assets == std::vector<std::string>{"a/font.ttf", "a/good.png"});
    CHECK(scan.assets[1].meta.guid == goodGuid);

    std::vector<std::string> report;
    for (const MetaProblem& p : scan.problems) report.push_back(p.path + " | " + p.message);
    CAPTURE(report);
    const auto has = [&](std::string_view path, std::string_view needle) {
        return std::any_of(scan.problems.begin(), scan.problems.end(), [&](const MetaProblem& p) {
            return p.path == path && p.message.find(needle) != std::string::npos;
        });
    };
    CHECK(has("b/nometa.png", "no sidecar"));
    CHECK(has("b/orphan.png.meta", "orphan sidecar"));
    CHECK(has("b/Cased.png", "its sidecar is spelled 'b/cased.png.meta'"));
    CHECK(has("b/upper.png.META", "'.meta' in lower case"));
    CHECK(has("b/upper.png", "its sidecar is spelled 'b/upper.png.META'"));
    CHECK(has("c/copy.png.meta", "is also the GUID of 'a/good.png.meta'"));
    CHECK(has("c/broken.png.meta", "provenance.licence"));
    CHECK(has("c/wrong.tga.meta", "importer 'font' does not import 'c/wrong.tga'"));
    if (sensitive) CHECK(has("d/twin.png", "differ only in case"));
    if constexpr (!platform::kIsWindows) CHECK(has("d/nul.png", "Windows device name"));
    CHECK(std::is_sorted(scan.problems.begin(), scan.problems.end(),
                         [](const MetaProblem& a, const MetaProblem& b) { return a.path < b.path; }));
    for (const MetaProblem& p : scan.problems) CHECK(p.path.find(".git") == std::string::npos);

    // A fold collision (02 §6.1) between two different GUIDs, with a deliberately weak fold.
    TempDir dir2;
    writeText(dir2.path, "x.png", "x");
    writeText(dir2.path, "y.png", "y");
    REQUIRE(ensureMeta(dir2.path, "x.png", newMeta(), r));
    REQUIRE(ensureMeta(dir2.path, "y.png", newMeta(), r));
    const MetaScan folded = scanMetas(dir2.path, r, [](const Guid&) { return asset::AssetId{42}; }).value();
    REQUIRE(folded.problems.size() == 1);
    CHECK(folded.problems[0].path == "y.png.meta");
    CHECK(folded.problems[0].message.find("AssetId fold collision") != std::string::npos);
    CHECK(scanMetas(dir2.path, r).value().problems.empty());
    CHECK(scanMetas(dir.path / "missing", r).error().code == ErrorCode::NotFound);
}

} // namespace
