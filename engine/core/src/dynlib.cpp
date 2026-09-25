#include "helios/core/dynlib.h"

#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "platform/os.h"

namespace helios {

DynamicLibrary::~DynamicLibrary() { unload(); }

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)), m_path(std::move(other.m_path)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        unload();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_path = std::move(other.m_path);
    }
    return *this;
}

Result<DynamicLibrary> DynamicLibrary::load(const std::filesystem::path& path) {
    std::string error;
    void* handle = os::libraryOpen(path, error);
    if (!handle) {
        return makeError(ErrorCode::NotFound, "cannot load library '{}': {}", fs::pathToGenericUtf8(path), error);
    }
    DynamicLibrary lib;
    lib.m_handle = handle;
    lib.m_path = path;
    HELIOS_LOG_DEBUG(LogCore, "Loaded library {}", fs::pathToGenericUtf8(path));
    return lib;
}

Result<DynamicLibrary> DynamicLibrary::loadUtf8(std::string_view utf8Path) { return load(fs::pathFromUtf8(utf8Path)); }

void DynamicLibrary::unload() noexcept {
    if (m_handle) {
        os::libraryClose(m_handle);
        m_handle = nullptr;
    }
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    return m_handle && name ? os::librarySymbol(m_handle, name) : nullptr;
}

std::string_view DynamicLibrary::extension() noexcept { return os::libraryExtension(); }

std::string DynamicLibrary::decoratedName(std::string_view baseName) {
    std::string out(os::libraryPrefix());
    out.append(baseName);
    out.append(os::libraryExtension());
    return out;
}

} // namespace helios
