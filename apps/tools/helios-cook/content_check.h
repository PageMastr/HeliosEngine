#pragma once
// `helios-cook check` (WP-0.20): the project file and the provenance and layout of the content it names.
//
//   * The project file, `helios.project.jsonc` (09 §2.7.2): every key this version knows is checked, and an
//     unknown key fails (a field the plan adds comes with its check). The product block is checked against
//     08 §2.10.1's lint rules that apply to its stub.
//   * Provenance (01 §5.2; 07 T24): every file under every content root is a content document type that the
//     cook knows (contentTypes()) and has a valid `.meta` sidecar, as engine/assetpipe validates it
//     (scanMetas: GUID, importer, provenance with an allowed licence, case and portability rules). Hidden
//     files fail too, except `.gitattributes` and the ignored cook outputs `.cooked/` and `.cache/` at a
//     root's top: the records cook would read a hidden `.hrec`, so nothing may hide from the scan.
//   * Layout (07 §1.8.2's `collab.scope`; 02 §5.5-5.6): spatial documents (`.hcont`, `.hent`) sit under
//     `zones/<zone>/` of a declared zone and nothing else does; records sit under `records/`; a container's
//     `$container`, `name`, `frame.parent` and `streaming.group` agree with its file and zone, and its
//     entities sit in `<name>.entities/<guid>.hent` with `$entity` equal to the file's GUID. A container's or
//     entity's sidecar carries the document's own GUID, so each has one identity.
//
// Not checked in v0: the rest of a container's fields and an entity's components (the container loader is
// Phase 1's), and the product block's endpoints, branding, signing and keys (not in the stub; WP-2.16a1).
//
// Threading: the functions are stateless and may run concurrently on different inputs.

#include <string>
#include <vector>

#include "helios/assetpipe/importer.h"
#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::cook {

/// One problem: the file it is about ('/'-separated, relative to the project root) and what is wrong.
struct Finding {
    std::string path;
    std::string message;
};

struct ZoneDecl {
    std::string name;  ///< Orchestrator zone name (05 §1.4), also its folder and session (zone-<name>).
    u32 id = 0;        ///< The zone id the cell serves it under.
    u32 tickHz = 0;    ///< 1..60 (ADR-007).
    std::string frame; ///< The zone's root frame, "<kind>:<name>" (02 §5.1).
};

/// What checkProjectFile() read (the parts the content check needs).
struct ProjectFile {
    std::vector<std::string> contentRoots; ///< Relative to the project root, '/'-separated.
    std::vector<ZoneDecl> zones;
};

/// The content document types the cook knows, as v0 importers without a build step (their `.meta` carries
/// provenance; the records cook reads `.hrec`, the container cook of Phase 1 the rest): `hrec` records
/// (02 §3.3), `hcont` object containers and `hent` their entities (02 §5.6), `md` Markdown text.
Result<assetpipe::ImporterRegistry> contentTypes();

/// Reads and validates `projectRoot`/helios.project.jsonc, appending a finding per problem. Returns what it
/// could read, so the content check still runs on a project file with errors.
ProjectFile checkProjectFile(const fs::Path& projectRoot, std::vector<Finding>& findings);

struct ContentStats {
    usize files = 0;     ///< Content documents seen (sidecars and allowed hidden files not counted).
    usize withMeta = 0;  ///< Of those, the ones with a valid sidecar.
    usize containers = 0;
    usize entities = 0;
};

/// Checks provenance and layout under every content root of `project`, appending findings.
ContentStats checkContent(const fs::Path& projectRoot, const ProjectFile& project,
                          const assetpipe::ImporterRegistry& types, std::vector<Finding>& findings);

} // namespace helios::cook
