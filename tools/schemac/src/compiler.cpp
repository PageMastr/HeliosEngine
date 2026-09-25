#include "compiler.h"

#include <filesystem>
#include <format>
#include <functional>
#include <set>
#include <unordered_map>

#include "helios/core/fs.h"
#include "generators.h"
#include "lock.h"
#include "parser.h"
#include "sema.h"
#include "text.h"

namespace helios::schemac {

std::string normalizePath(const std::string& path) {
    std::string p = std::filesystem::path(path).lexically_normal().generic_string();
    if (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::optional<std::string> RealFileSystem::read(const std::string& path) {
    auto text = fs::readTextFile(fs::pathFromUtf8(path));
    if (!text) return std::nullopt;
    return std::move(*text);
}

std::optional<std::string> MemoryFileSystem::read(const std::string& path) {
    auto it = files.find(normalizePath(path));
    if (it == files.end()) return std::nullopt;
    return it->second;
}

std::string cppHeaderPath(const std::string& logicalPath) {
    const usize dot = logicalPath.rfind(".hschema");
    return (dot == std::string::npos ? logicalPath : logicalPath.substr(0, dot)) + ".gen.h";
}

std::string cppSourcePath(const std::string& logicalPath) {
    const usize dot = logicalPath.rfind(".hschema");
    return (dot == std::string::npos ? logicalPath : logicalPath.substr(0, dot)) + ".gen.cpp";
}

namespace {

/// Import chains deeper than this are rejected (the loader recurses per level).
constexpr usize kMaxImportDepth = 64;

std::string dirOf(const std::string& path) {
    const usize slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string joinPath(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return normalizePath(rel);
    return normalizePath(dir + "/" + rel);
}

/// Path relative to the first include root that contains it, else the file name.
std::string logicalPathFor(const std::string& path, const std::vector<std::string>& roots) {
    for (const std::string& root : roots) {
        const std::filesystem::path rel = std::filesystem::path(path).lexically_relative(root);
        const std::string r = rel.generic_string();
        if (!r.empty() && r != "." && !r.starts_with("..")) return r;
    }
    return std::filesystem::path(path).filename().generic_string();
}

std::string stemOf(const std::string& logical) {
    std::string name = std::filesystem::path(logical).filename().generic_string();
    const usize dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

/// Declarations whose layout is part of `t`'s layout (by value or as container elements; record
/// refs are plain ids and do not count).
void layoutDeps(const Type* t, std::vector<const Decl*>& out) {
    if (!t) return;
    switch (t->kind) {
    case TypeKind::Enum:
    case TypeKind::Flags:
    case TypeKind::Struct:
    case TypeKind::Variant: out.push_back(t->decl); break;
    default: break;
    }
    layoutDeps(t->element, out);
    layoutDeps(t->key, out);
}

/// Layout hash (the cooked-data DDC key, 02 §3.4): the type's own ids, names and field types plus
/// the local layouts of every type reachable from it, so a change in a nested struct changes every
/// type that embeds or lists it. Reachable types are combined as a set (sorted by name), which
/// keeps the hash independent of declaration and file order, and cycles through lists are fine.
void computeLayoutHashes(Schema& s) {
    // Reachable types contribute names, field types and enum values but not lock ids: imported types
    // get ids only when their own file is compiled, and the hash must not depend on the file set.
    std::unordered_map<const Decl*, u64> local;
    auto localHash = [&](const Decl* d, bool withIds) -> u64 {
        if (!withIds) {
            if (auto it = local.find(d); it != local.end()) return it->second;
        }
        std::string text = d->qualifiedName + ";";
        for (const Field& f : d->fields) {
            text += withIds ? std::format("{}:", f.id) : std::string();
            text += std::format("{}:{};", f.name, f.type ? f.type->signature : "?");
        }
        for (const EnumVal& v : d->values) text += std::format("{}={};", v.name, v.value);
        for (const Alternative& a : d->alternatives) text += withIds ? std::format("{}:{};", a.id, a.name) : a.name + ";";
        const u64 h = fnv1a64(text);
        if (!withIds) local.emplace(d, h);
        return h;
    };
    auto directDeps = [](const Decl* d) {
        std::vector<const Decl*> deps;
        for (const Field& f : d->fields) layoutDeps(f.type, deps);
        for (const Alternative& a : d->alternatives) deps.push_back(a.type);
        return deps;
    };
    for (Decl* d : s.decls) {
        if (!d->isLockable()) continue;
        std::set<const Decl*> seen{d};
        std::vector<const Decl*> todo = directDeps(d);
        std::map<std::string, u64> reachable; // qualified name -> local hash (sorted, order-independent)
        while (!todo.empty()) {
            const Decl* x = todo.back();
            todo.pop_back();
            if (!seen.insert(x).second) continue;
            reachable.emplace(x->qualifiedName, localHash(x, false));
            for (const Decl* y : directDeps(x)) todo.push_back(y);
        }
        std::string text = std::format("{:016x}", localHash(d, true));
        for (const auto& [name, h] : reachable) text += std::format(";{}={:016x}", name, h);
        d->layoutHash = fnv1a64(text);
    }
}

/// Ids for files compiled without a lock: stable within one run only.
void assignEphemeralIds(Schema& s) {
    std::set<u32> used;
    for (Decl* d : s.decls) {
        if (!d->isLockable()) continue;
        u32 id = fnv1a32(d->qualifiedName);
        for (u32 salt = 1; id == 0 || used.contains(id); ++salt) id = fnv1a32(d->qualifiedName + "#" + std::to_string(salt));
        used.insert(id);
        d->typeId = id;
        u32 next = 1;
        for (Field& f : d->fields) f.id = next++;
        next = 1;
        for (Alternative& a : d->alternatives) a.id = next++;
    }
}

} // namespace

CompileResult compile(const CompileOptions& options, SourceProvider& fsys, DiagnosticEngine& diags) {
    diags.setWarningsAsErrors(options.warningsAsErrors);
    CompileResult result;
    result.schema = std::make_unique<Schema>();
    Schema& S = *result.schema;

    std::vector<std::string> roots;
    for (const std::string& dir : options.includeDirs) roots.push_back(normalizePath(dir));

    enum class State { Loading, Done };
    std::map<std::string, SourceFile*> byPath;
    std::map<std::string, State> state;
    std::vector<std::string> stack;

    std::function<SourceFile*(const std::string&, const std::string&, bool, SourceLoc)> load =
        [&](const std::string& path, const std::string& logical, bool generate, SourceLoc from) -> SourceFile* {
        if (auto it = byPath.find(path); it != byPath.end()) {
            if (generate) it->second->generate = true;
            if (state[path] == State::Loading) {
                std::string chain;
                bool on = false;
                for (const std::string& p : stack) {
                    if (p == path) on = true;
                    if (on) chain += p + " -> ";
                }
                diags.error(from, std::format("import cycle: {}{}", chain, path));
                return nullptr;
            }
            return it->second;
        }
        if (stack.size() >= kMaxImportDepth) {
            diags.error(from, std::format("imports nest more than {} files deep (at '{}')", kMaxImportDepth, path));
            return nullptr;
        }
        auto text = fsys.read(path);
        if (!text) {
            diags.error(from, std::format("cannot read schema file '{}'", path));
            return nullptr;
        }
        auto file = std::make_unique<SourceFile>();
        SourceFile* f = file.get();
        f->path = path;
        f->logicalPath = logical;
        f->stem = stemOf(logical);
        f->generate = generate;
        f->diagIndex = diags.addFile(path, *text);
        f->ast = parseFile(diags.fileText(f->diagIndex), f->diagIndex, diags);
        S.files.push_back(std::move(file));
        byPath[path] = f;
        result.inputs.push_back(path);
        state[path] = State::Loading;
        stack.push_back(path);
        for (const ImportAst& imp : f->ast.imports) {
            std::string found;
            std::string foundLogical;
            const std::string local = joinPath(dirOf(path), imp.path);
            if (byPath.contains(local) || fsys.read(local)) {
                found = local;
                foundLogical = normalizePath(dirOf(logical).empty() ? imp.path : dirOf(logical) + "/" + imp.path);
            } else {
                for (const std::string& root : roots) {
                    const std::string candidate = joinPath(root, imp.path);
                    if (byPath.contains(candidate) || fsys.read(candidate)) {
                        found = candidate;
                        foundLogical = normalizePath(imp.path);
                        break;
                    }
                }
            }
            if (found.empty()) {
                diags.error(imp.loc, std::format("cannot find imported schema '{}' (searched next to '{}' and in {} include director{})",
                                                 imp.path, path, roots.size(), roots.size() == 1 ? "y" : "ies"));
                continue;
            }
            if (SourceFile* child = load(found, foundLogical, false, imp.loc)) {
                if (std::find(f->imports.begin(), f->imports.end(), child) == f->imports.end()) f->imports.push_back(child);
            }
        }
        stack.pop_back();
        state[path] = State::Done;
        return f;
    };

    if (options.files.empty()) {
        diags.error({}, "no input files");
        return result;
    }
    for (const std::string& file : options.files) {
        const std::string path = normalizePath(file);
        load(path, logicalPathFor(path, roots), true, {});
    }
    if (diags.hasErrors()) return result;

    // Output stems must be unique per C++ output directory / Go package.
    {
        std::map<std::string, const SourceFile*> stems;
        std::map<std::string, const SourceFile*> logicals;
        for (const auto& f : S.files) {
            if (!f->generate) continue;
            if (auto [it, ok] = logicals.emplace(f->logicalPath, f.get()); !ok)
                diags.error({}, std::format("'{}' and '{}' map to the same output '{}'", it->second->path, f->path, f->logicalPath));
            if (options.emitGo) {
                if (auto [it, ok] = stems.emplace(f->stem, f.get()); !ok)
                    diags.error({}, std::format("'{}' and '{}' would both generate {}.go", it->second->path, f->path, f->stem));
            }
        }
    }

    if (!analyze(S, diags, SemaOptions{options.namingLints})) return result;

    // Stable ids from the lock.
    if (!options.lockPath.empty()) {
        Lock lock;
        std::string oldText;
        bool exists = false;
        if (auto text = fsys.read(options.lockPath)) {
            exists = true;
            oldText = std::move(*text);
            const u32 lockFile = diags.addFile(options.lockPath, oldText);
            result.inputs.push_back(normalizePath(options.lockPath));
            if (!loadLock(diags.fileText(lockFile), lockFile, lock, diags)) return result;
        } else if (options.checkLock) {
            diags.error({}, std::format("schema lock '{}' does not exist (run helios-schemac without --check-lock and commit it)",
                                        options.lockPath));
            return result;
        }
        LockOptions lo;
        lo.allowDefaultChange = options.allowDefaultChange;
        if (!applyLock(S, lock, diags, lo, result.lockChanges)) return result;
        result.lockText = writeLock(lock);
        result.lockChanged = !exists || result.lockText != oldText;
        if (options.checkLock && result.lockChanged) {
            diags.error({}, std::format("schema lock '{}' is out of date; run helios-schemac without --check-lock and commit the lock",
                                        options.lockPath));
            constexpr usize kMaxNotes = 20;
            for (usize i = 0; i < result.lockChanges.size() && i < kMaxNotes; ++i) diags.note({}, result.lockChanges[i]);
            if (result.lockChanges.size() > kMaxNotes) diags.note({}, std::format("... and {} more changes", result.lockChanges.size() - kMaxNotes));
            if (result.lockChanges.empty()) diags.note({}, "the lock file is not in canonical form");
            return result;
        }
    } else {
        if (options.warnWithoutLock) diags.warning({}, "no --lock file given: type and field ids are assigned per run and are NOT stable");
        assignEphemeralIds(S);
    }
    computeLayoutHashes(S);

    if (options.emitCpp) {
        for (OutputFile& o : generateCpp(S, options)) result.outputs.push_back(std::move(o));
    }
    if (options.emitGo) {
        for (OutputFile& o : generateGo(S, options, diags)) result.outputs.push_back(std::move(o));
    }
    if (options.emitJson) result.outputs.push_back(OutputFile{options.jsonOut, generateSchemaJson(S)});
    result.ok = !diags.hasErrors();
    return result;
}

} // namespace helios::schemac
