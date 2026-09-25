// Luau compilation through luau_compile and the shared bytecode cache.

#include "helios/script/compiler.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

#include "luacode.h"

namespace helios::script {
namespace {

// Wrapped or replaced by the sandbox (stdlib.cpp) and compiled as disabled builtins. Keep in sync
// with kBuiltinWraps there; a unit test asserts that every wrapper is either compiled as a disabled
// builtin or listed in kFastcallWrappedBuiltins, so no FASTCALL can bypass a charge (02 §8.3).
constexpr const char* kDisabledWrappedBuiltins[] = {
    "string.rep",      "string.format",     "string.gsub",     "string.find",        "string.match",
    "string.gmatch",   "string.split",      "string.sub",      "string.upper",       "string.lower",
    "string.reverse",  "string.pack",       "string.unpack",   "table.concat",       "table.sort",
    "table.move",      "table.create",      "table.clone",     "table.find",         "table.remove",
    "table.clear",     "table.maxn",        "buffer.fill",     "buffer.copy",        "buffer.create",
    "buffer.fromstring", "buffer.tostring", "buffer.readstring", "buffer.writestring", "utf8.len",
    "utf8.offset",     "setmetatable",      "math.random",
};

// Wrapped builtins that keep their FASTCALL: the fast path handles only an O(1) case and falls back
// to a regular call of the (charging) wrapper otherwise, independently of GC state
// (luauF_tinsert: two-argument append only; a positional insert calls the wrapper).
constexpr const char* kFastcallWrappedBuiltins[] = {"table.insert"};

// Builtins whose FASTCALL path bails out to a regular (safepoint-counting) call when a GC step is
// due (luaC_needsGC in VM/src/lbuiltins.cpp). Left as fastcalls, their fuel would depend on GC
// pacing, i.e. on the VM's allocation history; compiled as plain calls, they always cost one
// safepoint, so fuel counts depend only on the script and its inputs (04 §10.2).
constexpr const char* kGcSensitiveBuiltins[] = {"tostring", "string.char", "string.sub"};

// luau_compile wants a null-terminated array (string.sub is both wrapped and GC-sensitive: listed
// once).
struct DisabledBuiltinList {
    const char* names[std::size(kDisabledWrappedBuiltins) + std::size(kGcSensitiveBuiltins) + 1];
    usize count = 0;
    constexpr DisabledBuiltinList() : names{} {
        for (const char* name : kDisabledWrappedBuiltins) names[count++] = name;
        for (const char* name : kGcSensitiveBuiltins) {
            if (!contains(name)) names[count++] = name;
        }
        names[count] = nullptr;
    }
    constexpr bool contains(const char* name) const {
        for (usize i = 0; i < count; ++i) {
            if (std::string_view(names[i]) == std::string_view(name)) return true;
        }
        return false;
    }
};
constexpr DisabledBuiltinList kDisabledBuiltins{};

struct WrappedBuiltinList {
    const char* names[std::size(kDisabledWrappedBuiltins) + std::size(kFastcallWrappedBuiltins)];
    constexpr WrappedBuiltinList() : names{} {
        usize n = 0;
        for (const char* name : kDisabledWrappedBuiltins) names[n++] = name;
        for (const char* name : kFastcallWrappedBuiltins) names[n++] = name;
    }
};
constexpr WrappedBuiltinList kWrappedBuiltins{};

struct MallocDeleter {
    void operator()(char* p) const noexcept { std::free(p); }
};

} // namespace

std::span<const char* const> wrappedBuiltins() noexcept {
    return std::span<const char* const>(kWrappedBuiltins.names, std::size(kWrappedBuiltins.names));
}

std::span<const char* const> fastcallWrappedBuiltins() noexcept {
    return std::span<const char* const>(kFastcallWrappedBuiltins, std::size(kFastcallWrappedBuiltins));
}

std::span<const char* const> disabledBuiltins() noexcept {
    return std::span<const char* const>(kDisabledBuiltins.names, kDisabledBuiltins.count);
}

u64 CompileOptions::fingerprint() const noexcept {
    // Includes the disabled-builtin list: changing the wrapper set invalidates cached bytecode.
    u64 h = hashValue(static_cast<i32>(optimizationLevel), 0x5c01);
    h = hashValue(static_cast<i32>(debugLevel), h);
    h = hashValue(static_cast<i32>(typeInfoLevel), h);
    h = hashValue(static_cast<i32>(coverageLevel), h);
    for (const char* name : disabledBuiltins()) h = hash64(std::string_view(name), h);
    return h;
}

Result<BytecodePtr> compile(std::string_view source, const CompileOptions& options,
                            std::string_view chunkName) {
    if (options.optimizationLevel < 0 || options.optimizationLevel > 2 || options.debugLevel < 0 ||
        options.debugLevel > 2 || options.typeInfoLevel < 0 || options.typeInfoLevel > 1 ||
        options.coverageLevel < 0 || options.coverageLevel > 2) {
        return Error{ErrorCode::InvalidArgument, "CompileOptions out of range"};
    }

    lua_CompileOptions opts{};
    opts.optimizationLevel = options.optimizationLevel;
    opts.debugLevel = options.debugLevel;
    opts.typeInfoLevel = options.typeInfoLevel;
    opts.coverageLevel = options.coverageLevel;
    opts.disabledBuiltins = kDisabledBuiltins.names;

    usize size = 0;
    std::unique_ptr<char, MallocDeleter> blob(luau_compile(source.data(), source.size(), &opts, &size));
    if (!blob) return Error{ErrorCode::OutOfMemory, "luau_compile failed to allocate"};

    // An error blob starts with 0 followed by ":<line>: <message>".
    if (size == 0 || blob.get()[0] == 0) {
        std::string message(chunkName);
        if (size > 1) message.append(blob.get() + 1, size - 1);
        return Error{ErrorCode::ParseError, std::move(message)};
    }

    auto bytecode = std::make_shared<Bytecode>();
    bytecode->sourceHash = hash128(source);
    bytecode->optionsFingerprint = options.fingerprint();
    bytecode->data.assign(blob.get(), size);
    return BytecodePtr(std::move(bytecode));
}

Result<BytecodePtr> BytecodeCache::getOrCompile(std::string_view source, const CompileOptions& options,
                                                std::string_view chunkName) {
    const Key key{hash128(source), options.fingerprint()};
    {
        std::lock_guard lock(m_mutex);
        if (auto it = m_entries.find(key); it != m_entries.end()) {
            ++m_hits;
            return it->second;
        }
        ++m_misses;
    }
    // Compile outside the lock; a concurrent duplicate compile is harmless (same bytes).
    HELIOS_TRY_ASSIGN(BytecodePtr bytecode, compile(source, options, chunkName));
    std::lock_guard lock(m_mutex);
    if (m_entries.size() >= m_maxEntries) {
        // Drop entries no VM references any more; bytecode in use stays shared.
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            it = it->second.use_count() == 1 ? m_entries.erase(it) : std::next(it);
        }
    }
    auto [it, inserted] = m_entries.emplace(key, bytecode);
    return it->second;
}

usize BytecodeCache::size() const {
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

u64 BytecodeCache::hits() const {
    std::lock_guard lock(m_mutex);
    return m_hits;
}

u64 BytecodeCache::misses() const {
    std::lock_guard lock(m_mutex);
    return m_misses;
}

void BytecodeCache::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
}

} // namespace helios::script
