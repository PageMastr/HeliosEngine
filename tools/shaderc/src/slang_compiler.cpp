#include "slang_compiler.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

#include "helios/core/dynlib.h"
#include "helios/core/fs.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <slang-com-ptr.h>
#include <slang.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#ifndef HELIOS_SHADERC_DEFAULT_SLANG_ROOT
#define HELIOS_SLANG_DEFAULT_ROOT_TEXT ""
#else
#define HELIOS_SLANG_DEFAULT_ROOT_TEXT HELIOS_SHADERC_DEFAULT_SLANG_ROOT
#endif

namespace helios::shaderc {

namespace {

using CreateGlobalSessionFn = SlangResult (*)(SlangInt apiVersion, slang::IGlobalSession** outGlobalSession);
using GetBuildTagFn = const char* (*)();

std::string blobText(slang::IBlob* blob) {
    if (!blob) return {};
    return std::string(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
}

std::string escapeMake(const std::string& path) {
    std::string out;
    for (char c : path) {
        if (c == ' ' || c == '#') out += '\\';
        if (c == '$') out += '$';
        out += c;
    }
    return out;
}

} // namespace

struct SlangCompiler::Impl {
    DynamicLibrary library;
    Slang::ComPtr<slang::IGlobalSession> global;
    GetBuildTagFn buildTag = nullptr;
};

SlangCompiler::SlangCompiler() : m_impl(std::make_unique<Impl>()) {}

SlangCompiler::~SlangCompiler() {
    // Release the session before the library that implements it is unloaded.
    if (m_impl) m_impl->global = nullptr;
}

const std::filesystem::path& SlangCompiler::libraryPath() const noexcept { return m_impl->library.path(); }

std::string SlangCompiler::version() const { return m_impl->buildTag ? std::string(m_impl->buildTag()) : std::string(); }

Result<std::unique_ptr<SlangCompiler>> SlangCompiler::load(const std::filesystem::path& slangRoot) {
    std::vector<std::filesystem::path> roots;
    if (!slangRoot.empty()) roots.push_back(slangRoot);
    if (const char* env = std::getenv("HELIOS_SLANG_ROOT"); env && *env) roots.push_back(fs::pathFromUtf8(env));
    if (auto exe = fs::executablePath(); exe.ok()) {
        roots.push_back(exe.value().parent_path());
        roots.push_back(exe.value().parent_path().parent_path());
    }
    if (std::strlen(HELIOS_SLANG_DEFAULT_ROOT_TEXT) != 0) roots.push_back(fs::pathFromUtf8(HELIOS_SLANG_DEFAULT_ROOT_TEXT));

    std::vector<std::string> names;
    for (const char* base : {"slang-compiler", "slang"}) names.push_back(DynamicLibrary::decoratedName(base));

    std::unique_ptr<SlangCompiler> compiler(new SlangCompiler());
    std::string tried;
    bool loaded = false;
    for (const std::filesystem::path& root : roots) {
        for (const char* sub : {"lib", "bin", ""}) {
            for (const std::string& name : names) {
                const std::filesystem::path candidate = std::string(sub).empty() ? root / name : root / sub / name;
                if (!fs::isFile(candidate)) continue;
                auto lib = DynamicLibrary::load(candidate);
                if (lib.ok()) {
                    compiler->m_impl->library = std::move(lib).value();
                    loaded = true;
                    break;
                }
                tried += std::format("\n  {}: {}", fs::pathToUtf8(candidate), lib.error().message);
            }
            if (loaded) break;
        }
        if (loaded) break;
    }
    if (!loaded) {
        for (const std::string& name : names) {
            auto lib = DynamicLibrary::load(name);  // system search path
            if (lib.ok()) {
                compiler->m_impl->library = std::move(lib).value();
                loaded = true;
                break;
            }
        }
    }
    if (!loaded) {
        std::string searched;
        for (const auto& r : roots) searched += "\n  " + fs::pathToUtf8(r);
        return Error{ErrorCode::NotFound, std::format("Slang compiler library not found (set --slang-root or "
                                                      "HELIOS_SLANG_ROOT); searched:{}{}",
                                                      searched, tried)};
    }
    Impl& impl = *compiler->m_impl;
    auto create = impl.library.function<CreateGlobalSessionFn>("slang_createGlobalSession");
    if (!create) {
        return Error{ErrorCode::Unsupported, std::format("{} does not export slang_createGlobalSession",
                                                        fs::pathToUtf8(impl.library.path()))};
    }
    impl.buildTag = impl.library.function<GetBuildTagFn>("spGetBuildTagString");
    if (SLANG_FAILED(create(SLANG_API_VERSION, impl.global.writeRef())) || !impl.global) {
        return Error{ErrorCode::Unknown, "slang_createGlobalSession failed"};
    }
    // Slang loads its SPIR-V back ends (glslang, spirv-opt, spirv-dis, spirv-link, all in slang-glslang)
    // lazily by bare name. Windows resolves that against the executable's directory, not the Slang DLL's,
    // so point Slang at the directory it was loaded from.
    const std::filesystem::path libraryDir = impl.library.path().parent_path();
    if (!libraryDir.empty()) {
        const std::string dir = fs::pathToUtf8(libraryDir);
        for (SlangPassThrough backEnd : {SLANG_PASS_THROUGH_GLSLANG, SLANG_PASS_THROUGH_SPIRV_OPT,
                                         SLANG_PASS_THROUGH_SPIRV_DIS, SLANG_PASS_THROUGH_SPIRV_LINK}) {
            impl.global->setDownstreamCompilerPath(backEnd, dir.c_str());
        }
    }
    // Without spirv-opt Slang only reports a diagnostic and emits unoptimized SPIR-V, which silently differs
    // from what the build's slangc produces. Refuse to run instead.
    if (SLANG_FAILED(impl.global->checkPassThroughSupport(SLANG_PASS_THROUGH_SPIRV_OPT))) {
        return Error{ErrorCode::NotFound,
                     std::format("Slang's SPIR-V optimizer (slang-glslang) could not be loaded next to {}",
                                 fs::pathToUtf8(impl.library.path()))};
    }
    return compiler;
}

Result<CompileOutput> SlangCompiler::compile(const CompileRequest& request) {
    Impl& impl = *m_impl;
    HELIOS_TRY_ASSIGN(std::string source, fs::readTextFile(request.source));
    const std::string sourcePath = fs::pathToUtf8(std::filesystem::absolute(request.source));

    slang::TargetDesc target;
    target.format = SLANG_SPIRV;
    target.profile = impl.global->findProfile("spirv_1_6");

    std::vector<std::string> searchStorage;
    for (const auto& dir : request.includeDirs) searchStorage.push_back(fs::pathToUtf8(std::filesystem::absolute(dir)));
    std::vector<const char*> searchPaths;
    for (const std::string& s : searchStorage) searchPaths.push_back(s.c_str());
    std::vector<slang::PreprocessorMacroDesc> macros;
    for (const auto& [name, value] : request.defines) macros.push_back({name.c_str(), value.c_str()});

    auto intOption = [](slang::CompilerOptionName name, int value) {
        slang::CompilerOptionEntry e;
        e.name = name;
        e.value.kind = slang::CompilerOptionValueKind::Int;
        e.value.intValue0 = value;
        return e;
    };
    std::vector<slang::CompilerOptionEntry> options;
    options.push_back(intOption(slang::CompilerOptionName::VulkanUseEntryPointName, 1));
    options.push_back(intOption(slang::CompilerOptionName::GLSLForceScalarLayout, 1));
    options.push_back(intOption(slang::CompilerOptionName::MatrixLayoutColumn, 1));
    options.push_back(intOption(slang::CompilerOptionName::Optimization,
                                request.debugInfo ? SLANG_OPTIMIZATION_LEVEL_NONE : SLANG_OPTIMIZATION_LEVEL_HIGH));
    options.push_back(intOption(slang::CompilerOptionName::DebugInformation,
                                request.debugInfo ? SLANG_DEBUG_INFO_LEVEL_STANDARD : SLANG_DEBUG_INFO_LEVEL_NONE));
    slang::CompilerOptionEntry warnings;
    warnings.name = slang::CompilerOptionName::DisableWarnings;
    warnings.value.kind = slang::CompilerOptionValueKind::String;
    warnings.value.stringValue0 = "41012,39001";
    options.push_back(warnings);

    // Layout, entry-point naming, optimization and debug info are per-target options in Slang;
    // they are given to the target (and the session) so the result matches slangc exactly.
    target.forceGLSLScalarBufferLayout = true;
    target.compilerOptionEntries = options.data();
    target.compilerOptionEntryCount = static_cast<u32>(options.size());

    slang::SessionDesc desc;
    desc.targets = &target;
    desc.targetCount = 1;
    desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    desc.searchPaths = searchPaths.data();
    desc.searchPathCount = static_cast<SlangInt>(searchPaths.size());
    desc.preprocessorMacros = macros.data();
    desc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    desc.compilerOptionEntries = options.data();
    desc.compilerOptionEntryCount = static_cast<u32>(options.size());

    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(impl.global->createSession(desc, session.writeRef())) || !session) {
        return Error{ErrorCode::Unknown, "Slang: createSession failed"};
    }
    CompileOutput out;
    Slang::ComPtr<slang::IBlob> diagnostics;
    // UTF-8 (path::string() converts to the ANSI code page on Windows and throws for names it
    // cannot represent).
    const std::string moduleName = fs::pathToUtf8(request.source.stem());
    slang::IModule* module =
        session->loadModuleFromSourceString(moduleName.c_str(), sourcePath.c_str(), source.c_str(), diagnostics.writeRef());
    std::string diag = blobText(diagnostics);
    if (!module) return Error{ErrorCode::ParseError, diag.empty() ? std::string("Slang: module failed to load") : diag};

    std::vector<Slang::ComPtr<slang::IEntryPoint>> entries;
    if (request.entryPoints.empty()) {
        const SlangInt32 count = module->getDefinedEntryPointCount();
        for (SlangInt32 i = 0; i < count; ++i) {
            Slang::ComPtr<slang::IEntryPoint> ep;
            if (SLANG_SUCCEEDED(module->getDefinedEntryPoint(i, ep.writeRef())) && ep) entries.push_back(ep);
        }
    } else {
        for (const std::string& name : request.entryPoints) {
            Slang::ComPtr<slang::IEntryPoint> ep;
            if (SLANG_FAILED(module->findEntryPointByName(name.c_str(), ep.writeRef())) || !ep) {
                return Error{ErrorCode::NotFound, std::format("{}: no entry point '{}' (declare it with [shader(...)])",
                                                              sourcePath, name)};
            }
            entries.push_back(ep);
        }
    }
    if (entries.empty()) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("{}: no entry points (declare them with [shader(\"...\")])", sourcePath)};
    }
    std::vector<slang::IComponentType*> parts{module};
    for (auto& ep : entries) parts.push_back(ep.get());
    Slang::ComPtr<slang::IComponentType> composite;
    diagnostics = nullptr;
    if (SLANG_FAILED(session->createCompositeComponentType(parts.data(), static_cast<SlangInt>(parts.size()),
                                                           composite.writeRef(), diagnostics.writeRef()))) {
        return Error{ErrorCode::ParseError, diag + blobText(diagnostics)};
    }
    diag += blobText(diagnostics);
    Slang::ComPtr<slang::IComponentType> linked;
    diagnostics = nullptr;
    if (SLANG_FAILED(composite->link(linked.writeRef(), diagnostics.writeRef()))) {
        return Error{ErrorCode::ParseError, diag + blobText(diagnostics)};
    }
    diag += blobText(diagnostics);
    Slang::ComPtr<slang::IBlob> code;
    diagnostics = nullptr;
    if (SLANG_FAILED(linked->getTargetCode(0, code.writeRef(), diagnostics.writeRef())) || !code) {
        return Error{ErrorCode::ParseError, diag + blobText(diagnostics)};
    }
    diag += blobText(diagnostics);
    const usize bytes = code->getBufferSize();
    if (bytes == 0 || bytes % 4 != 0) return Error{ErrorCode::Corrupt, "Slang produced no SPIR-V"};
    out.spirv.resize(bytes / 4);
    std::memcpy(out.spirv.data(), code->getBufferPointer(), bytes);
    out.diagnostics = std::move(diag);

    out.dependencies.push_back(std::filesystem::absolute(request.source));
    const SlangInt32 deps = module->getDependencyFileCount();
    for (SlangInt32 i = 0; i < deps; ++i) {
        const char* path = module->getDependencyFilePath(i);
        if (!path || !*path) continue;
        std::filesystem::path p = fs::pathFromUtf8(path);
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) continue;  // builtin modules have virtual paths
        p = std::filesystem::absolute(p);
        if (std::find(out.dependencies.begin(), out.dependencies.end(), p) == out.dependencies.end()) {
            out.dependencies.push_back(p);
        }
    }
    return out;
}

std::string makeDepfile(const std::filesystem::path& target, const std::vector<std::filesystem::path>& deps) {
    std::string out = escapeMake(fs::pathToGenericUtf8(target)) + ":";
    for (const auto& d : deps) out += " \\\n  " + escapeMake(fs::pathToGenericUtf8(d));
    out += '\n';
    return out;
}

} // namespace helios::shaderc
