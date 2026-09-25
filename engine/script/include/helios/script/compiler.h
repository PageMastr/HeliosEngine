#pragma once
// Luau compilation (luau_compile) and the shared bytecode cache.
//
// Bytecode is immutable and shared between VMs of one content version (02 §7.4), so the cache is
// keyed by the XXH3-128 of the source plus the compile-option fingerprint, never by module name.
// Every compile passes the sandbox's wrapped builtins as `disabledBuiltins`, so no FASTCALL can
// bypass a fuel-charging wrapper (02 §7.4, §8.3), plus the builtins whose FASTCALL fallback depends
// on GC pacing, so fuel counts are a function of the script and its inputs only.
//
// Threading: compile() is thread-safe (pure). BytecodeCache is thread-safe (internal mutex) and
// may be shared by VMs on different threads.

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "helios/core/hash.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::script {

struct CompileOptions {
    /// 0 = none, 1 = baseline (keeps debuggability), 2 = inlining/unrolling (hurts debugging).
    int optimizationLevel = 1;
    /// 0 = none, 1 = line info + function names (tracebacks), 2 = locals/upvalues (debugger).
    int debugLevel = 1;
    /// 0 = type info for native modules only, 1 = for all modules (guides native codegen).
    int typeInfoLevel = 0;
    /// 0 = off, 1 = statement coverage, 2 = statement + expression coverage.
    int coverageLevel = 0;

    /// Stable hash of every field (part of the cache key).
    u64 fingerprint() const noexcept;
    friend bool operator==(const CompileOptions&, const CompileOptions&) = default;
};

/// Immutable compiled chunk.
struct Bytecode {
    Hash128 sourceHash;
    u64 optionsFingerprint = 0;
    std::string data; ///< Luau bytecode blob for luau_load.
};
using BytecodePtr = std::shared_ptr<const Bytecode>;

/// Compiles Luau source. On a syntax/compile error returns Error{ParseError,
/// "<chunkName>:<line>: <message>"}.
Result<BytecodePtr> compile(std::string_view source, const CompileOptions& options = {},
                            std::string_view chunkName = "chunk");

/// Library functions the sandbox replaces with fuel-charging (or determinism) wrappers, as
/// "lib.name" or "name" for globals.
std::span<const char* const> wrappedBuiltins() noexcept;
/// The wrapped builtins that keep their FASTCALL because the fast path only covers an O(1) case and
/// falls back to calling the charging wrapper otherwise (table.insert: append vs positional insert).
std::span<const char* const> fastcallWrappedBuiltins() noexcept;
/// Everything passed to luau_compile as `disabledBuiltins`: the wrapped builtins (except
/// fastcallWrappedBuiltins) plus those whose FASTCALL path depends on GC pacing (tostring,
/// string.char, string.sub), so fuel counts never depend on the VM's allocation history.
std::span<const char* const> disabledBuiltins() noexcept;

/// Thread-safe cache of compiled bytecode keyed by (source hash, options fingerprint).
class BytecodeCache {
public:
    explicit BytecodeCache(usize maxEntries = 4096) : m_maxEntries(maxEntries) {}

    /// Returns cached bytecode or compiles (and caches) it. Compile errors are not cached.
    Result<BytecodePtr> getOrCompile(std::string_view source, const CompileOptions& options = {},
                                     std::string_view chunkName = "chunk");

    usize size() const;
    u64 hits() const;
    u64 misses() const;
    void clear();

private:
    struct Key {
        Hash128 source;
        u64 options = 0;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        usize operator()(const Key& k) const noexcept {
            return static_cast<usize>(k.source.low ^ (k.source.high * 0x9e3779b97f4a7c15ull) ^ k.options);
        }
    };

    mutable std::mutex m_mutex;
    std::unordered_map<Key, BytecodePtr, KeyHash> m_entries;
    usize m_maxEntries;
    u64 m_hits = 0;
    u64 m_misses = 0;
};

} // namespace helios::script
