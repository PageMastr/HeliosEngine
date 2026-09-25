#pragma once
// Dynamic libraries (LoadLibraryExW / dlopen) for plugins and hot-reloadable game code.
// Paths are std::filesystem::path (UTF-16 on Windows) or UTF-8 text via loadUtf8().
//
// Threading: load/unload are thread-safe OS calls; a DynamicLibrary object is owned by one thread.
// symbol() may be called concurrently.

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios {

class DynamicLibrary {
public:
    DynamicLibrary() noexcept = default;
    ~DynamicLibrary();
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    /// Loads a library. Absolute paths also search the library's directory for its dependencies
    /// (Windows). Errors carry the OS message.
    static Result<DynamicLibrary> load(const std::filesystem::path& path);
    static Result<DynamicLibrary> loadUtf8(std::string_view utf8Path);

    void unload() noexcept;
    bool isLoaded() const noexcept { return m_handle != nullptr; }
    const std::filesystem::path& path() const noexcept { return m_path; }

    /// Address of an exported symbol, or nullptr.
    void* symbol(const char* name) const noexcept;

    /// Typed function lookup: `auto fn = lib.function<int (*)(int)>("plugin_add");`
    template <class Fn>
    Fn function(const char* name) const noexcept {
        static_assert(sizeof(Fn) == sizeof(void*), "Fn must be a function pointer type");
        void* p = symbol(name);
        Fn fn = nullptr;
        std::memcpy(&fn, &p, sizeof(fn)); // object->function pointer without a conditionally-supported cast
        return fn;
    }

    /// Platform file extension including the dot (".dll", ".so", ".dylib").
    static std::string_view extension() noexcept;
    /// "foo" -> "foo.dll" (Windows) / "libfoo.so" (Linux).
    static std::string decoratedName(std::string_view baseName);

private:
    void* m_handle = nullptr;
    std::filesystem::path m_path;
};

} // namespace helios
