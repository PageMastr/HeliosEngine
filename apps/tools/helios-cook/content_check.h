#pragma once
// `helios-cook check` (WP-0.20): the project file and the provenance and layout of the content it names.
//
//   * The project file, `helios.project.jsonc` (09 §2.7.2): every key this version knows is checked, and an
//     unknown key fails (a field the plan adds comes with its check). The product block is checked against
//     08 §2.10.1's lint rules that apply to its stub.
//   * Provenance (01 §5.2; 07 T24): every file under every content root is a content document type that the
//     cook knows (contentTypes()) and has a valid `.meta` sidecar, as engine/assetpipe validates it
//     (scanMetas: GUID, importer, provenance with an allowed licence, case and portability rules). Hidden
//     files fail too, except at a root's top only: `.gitattributes` and the ignored cook outputs `.cooked/`
//     and `.cache/`. The records cook would read a hidden `.hrec`, so nothing may hide from the scan; nor may
//     a symbolic link, junction or other special file (the scan does not follow one into a directory).
//     Extensions are lower case, since the root's `.gitattributes` patterns are case-sensitive on Linux.
//   * Text documents (records, containers, entities, Markdown) are UTF-8 without NUL bytes, so a binary file
//     under a text extension cannot bypass Git LFS and ASSETS.md (both go by extension).
//   * Binary sources (07 §1.7; `.glb`, `.png`, `.exr`): each file is a Git LFS pointer (a checkout without
//     the LFS objects, as in CI) or starts like a file of its type, so a renamed file of another kind fails.
//   * Git (07 §1.7), when the content root lies in a git work tree: no tracked file under `.cooked/` or
//     `.cache/` (output, exempt from the scan above only because git ignores it), no tracked symbolic link
//     or submodule, and every tracked binary source stored as a Git LFS pointer, never as a plain blob.
//     ContentOptions::requireGit turns "not a git work tree" (or no git on PATH) into a finding.
//
// Not checked in v0: the rest of a container's fields and an entity's components (the container loader is
// Phase 1's), and the product block's endpoints, branding, signing and keys (not in the stub; WP-2.16a1).
//
// Threading: the functions are stateless and may run concurrently on different inputs. checkContent() runs
// `git` (rev-parse, ls-files, cat-file) as child processes and blocks until they exit.

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
/// (02 §3.3), `hcont` object containers and `hent` their entities (02 §5.6), `md` Markdown text, and the
/// binary sources `glb` (glTF 2.0 binary), `png` and `exr` (07 T24's MVP formats; their importers, which
/// keep these ids, are WP-1.4's).
Result<assetpipe::ImporterRegistry> contentTypes();

/// Reads and validates `projectRoot`/helios.project.jsonc, appending a finding per problem. Returns what it
/// could read, so the content check still runs on a project file with errors.
ProjectFile checkProjectFile(const fs::Path& projectRoot, std::vector<Finding>& findings);

struct ContentStats {
    usize files = 0;    ///< Content documents seen (sidecars and allowed hidden files not counted).
    usize withMeta = 0; ///< Of those, the ones with a valid sidecar.
    usize containers = 0;
    usize entities = 0;
    usize binaries = 0;    ///< Binary sources (`.glb`, `.png`, `.exr`) among the documents.
    usize lfsPointers = 0; ///< Of those, the ones whose file is a Git LFS pointer (LFS objects not fetched).
    usize gitRoots = 0;    ///< Content roots checked against git (in a work tree, with git on PATH).
    usize gitLfs = 0;      ///< Tracked binary sources that git stores as Git LFS pointers.
};

struct ContentOptions {
    /// A content root outside a git work tree, or no `git` on PATH, is a finding instead of skipping the git
    /// rules (the repository's lint passes it, so CI cannot skip them unnoticed).
    bool requireGit = false;
};

/// Checks provenance and layout under every content root of `project`, appending findings.
ContentStats checkContent(const fs::Path& projectRoot, const ProjectFile& project,
                          const assetpipe::ImporterRegistry& types, std::vector<Finding>& findings,
                          const ContentOptions& options = {});

} // namespace helios::cook
