// Win32 child processes and pipes (process.h): CreateProcessW with STARTUPINFOEXW and
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so the child inherits exactly its standard handles and the
// whitelisted ones, never another thread's pipe. UTF-16 command line (quoted per the MSVC CRT rules)
// and a sorted UTF-16 environment block (CREATE_UNICODE_ENVIRONMENT).

#include "platform/win32/win32_common.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "helios/core/process.h"
#include "platform/os.h"

namespace helios::os {

namespace {

// Closes the handles it holds on every exit path of processSpawn.
struct HandleSet {
    std::vector<HANDLE> handles;
    ~HandleSet() {
        for (HANDLE h : handles) {
            if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
    HANDLE add(HANDLE h) {
        handles.push_back(h);
        return h;
    }
    HANDLE take(HANDLE h) {
        for (HANDLE& x : handles) {
            if (x == h) x = nullptr;
        }
        return h;
    }
};

// Restores the inherit flag of the caller's whitelisted handles after the spawn.
struct InheritRestore {
    std::vector<std::pair<HANDLE, DWORD>> saved;
    ~InheritRestore() {
        for (auto [h, flags] : saved) SetHandleInformation(h, HANDLE_FLAG_INHERIT, flags & HANDLE_FLAG_INHERIT);
    }
};

bool envNameEquals(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

std::wstring_view envName(std::wstring_view entry) {
    // Hidden per-drive entries ("=C:=C:\\dir") start with '='; their name runs to the second '='.
    const usize eq = entry.find(L'=', entry.empty() || entry[0] != L'=' ? 0 : 1);
    return eq == std::wstring_view::npos ? entry : entry.substr(0, eq);
}

std::wstring buildEnvironmentBlock(const ProcessDesc& desc) {
    std::vector<std::wstring> env;
    if (desc.inheritEnvironment) {
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* p = block; *p; p += wcslen(p) + 1) env.emplace_back(p);
            FreeEnvironmentStringsW(block);
        }
    }
    auto removeName = [&](std::wstring_view name) {
        std::erase_if(env, [&](const std::wstring& e) { return envNameEquals(envName(e), name); });
    };
    for (const std::string& name : desc.unsetEnvironment) removeName(win32::widen(name));
    for (const auto& [name, value] : desc.environment) {
        const std::wstring wname = win32::widen(name);
        removeName(wname);
        env.push_back(wname + L"=" + win32::widen(value));
    }
    // CreateProcess requires the block sorted by name, case-insensitively, in ordinal order.
    std::stable_sort(env.begin(), env.end(), [](const std::wstring& a, const std::wstring& b) {
        const std::wstring_view na = envName(a), nb = envName(b);
        return CompareStringOrdinal(na.data(), static_cast<int>(na.size()), nb.data(), static_cast<int>(nb.size()),
                                    TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const std::wstring& e : env) {
        block += e;
        block.push_back(L'\0');
    }
    if (env.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

Result<HANDLE> openNul(bool write) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return lastError("CreateFileW(NUL)");
    return h;
}

// An inheritable duplicate of one of the parent's standard handles (nullptr if it has none).
HANDLE inheritableStdHandle(DWORD which, HandleSet& owned) {
    HANDLE h = GetStdHandle(which);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE dup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return owned.add(dup);
}

// CreateProcessW starts .bat and .cmd files through cmd.exe, which re-parses the command line with
// its own rules (%VAR% expansion, & | < > ^ and quote handling that differs from the MSVC CRT), so
// no quoting of untrusted arguments is safe ("BatBadBut", CVE-2024-24576 and relatives). Windows
// ignores trailing dots and spaces in file names ("run.bat. " opens run.bat), so strip them first.
bool isBatchFile(const fs::Path& executable) {
    std::wstring name = executable.filename().wstring();
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.size() < 4) return false;
    std::wstring ext = name.substr(name.size() - 4);
    for (wchar_t& c : ext) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return ext == L".bat" || ext == L".cmd";
}

} // namespace

// ---- pipes ------------------------------------------------------------------------------------

Result<void> pipeCreate(NativeHandle& readEnd, NativeHandle& writeEnd) {
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, nullptr, 0)) return lastError("CreatePipe");
    readEnd = win32::fromHandle(r);
    writeEnd = win32::fromHandle(w);
    return {};
}

void handleClose(NativeHandle handle) noexcept {
    if (handle != kInvalidNativeHandle && handle != 0) CloseHandle(win32::toHandle(handle));
}

Result<usize> pipeRead(NativeHandle handle, void* buffer, usize size) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<usize>(size, 1u << 30));
    if (!ReadFile(win32::toHandle(handle), buffer, want, &got, nullptr)) {
        const DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) return usize{0};
        return win32::makeWin32Error(e, "ReadFile(pipe)");
    }
    return static_cast<usize>(got);
}

Result<usize> pipeWrite(NativeHandle handle, const void* data, usize size) {
    DWORD wrote = 0;
    const DWORD want = static_cast<DWORD>(std::min<usize>(size, 1u << 30));
    if (!WriteFile(win32::toHandle(handle), data, want, &wrote, nullptr)) return lastError("WriteFile(pipe)");
    return static_cast<usize>(wrote);
}

// ---- processes ----------------------------------------------------------------------------------

Result<SpawnedProcess> processSpawn(const ProcessDesc& desc) {
    if (isBatchFile(desc.executable)) {
        return Error{ErrorCode::InvalidArgument,
                     "Process::spawn: batch files (.bat, .cmd) run through cmd.exe and cannot receive arguments "
                     "safely; start cmd.exe explicitly if that is really intended"};
    }
    HandleSet owned;
    InheritRestore restore;
    SpawnedProcess out;

    // Standard streams.
    HANDLE childIn = nullptr, childOut = nullptr, childErr = nullptr;
    HANDLE parentIn = nullptr, parentOut = nullptr, parentErr = nullptr;
    auto makePipe = [&](HANDLE& parentEnd, HANDLE& childEnd, bool parentReads) -> Result<void> {
        HANDLE r = nullptr, w = nullptr;
        if (!CreatePipe(&r, &w, nullptr, 0)) return lastError("CreatePipe");
        owned.add(r);
        owned.add(w);
        parentEnd = parentReads ? r : w;
        childEnd = parentReads ? w : r;
        if (!SetHandleInformation(childEnd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            return lastError("SetHandleInformation");
        }
        return {};
    };
    switch (desc.stdinMode) {
    case StdioMode::Pipe:
        if (auto r = makePipe(parentIn, childIn, false); !r) return r.error();
        break;
    case StdioMode::Null: {
        Result<HANDLE> h = openNul(false);
        if (!h) return h.error();
        childIn = owned.add(*h);
        break;
    }
    case StdioMode::Inherit: childIn = inheritableStdHandle(STD_INPUT_HANDLE, owned); break;
    }
    switch (desc.stdoutMode) {
    case StdioMode::Pipe:
        if (auto r = makePipe(parentOut, childOut, true); !r) return r.error();
        break;
    case StdioMode::Null: {
        Result<HANDLE> h = openNul(true);
        if (!h) return h.error();
        childOut = owned.add(*h);
        break;
    }
    case StdioMode::Inherit: childOut = inheritableStdHandle(STD_OUTPUT_HANDLE, owned); break;
    }
    if (desc.mergeStderrIntoStdout) {
        childErr = childOut;
    } else {
        switch (desc.stderrMode) {
        case StdioMode::Pipe:
            if (auto r = makePipe(parentErr, childErr, true); !r) return r.error();
            break;
        case StdioMode::Null: {
            Result<HANDLE> h = openNul(true);
            if (!h) return h.error();
            childErr = owned.add(*h);
            break;
        }
        case StdioMode::Inherit: childErr = inheritableStdHandle(STD_ERROR_HANDLE, owned); break;
        }
    }

    // The whitelist: standard handles plus the caller's handles, each inheritable for this spawn.
    std::vector<HANDLE> inherit;
    auto addInherit = [&](HANDLE h) {
        if (h != nullptr && h != INVALID_HANDLE_VALUE && std::find(inherit.begin(), inherit.end(), h) == inherit.end()) {
            inherit.push_back(h);
        }
    };
    addInherit(childIn);
    addInherit(childOut);
    addInherit(childErr);
    for (NativeHandle nh : desc.inheritHandles) {
        HANDLE h = win32::toHandle(nh);
        DWORD flags = 0;
        if (h == nullptr || h == INVALID_HANDLE_VALUE || !GetHandleInformation(h, &flags)) {
            return Error{ErrorCode::InvalidArgument, "Process::spawn: invalid handle in inheritHandles"};
        }
        restore.saved.emplace_back(h, flags);
        if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) return lastError("SetHandleInformation");
        addInherit(h);
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = childIn;
    si.StartupInfo.hStdOutput = childOut;
    si.StartupInfo.hStdError = childErr;

    std::vector<u8> attrStorage;
    LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
    if (!inherit.empty()) {
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        attrStorage.resize(attrSize);
        attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
        if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
            return lastError("InitializeProcThreadAttributeList");
        }
        if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                       inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const Error e = lastError("UpdateProcThreadAttribute");
            DeleteProcThreadAttributeList(attrList);
            return e;
        }
        si.lpAttributeList = attrList;
    }

    const std::string programUtf8 = fs::pathToUtf8(desc.executable);
    std::wstring commandLine = win32::widen(buildWindowsCommandLine(programUtf8, desc.args));
    const std::wstring application = desc.executable.wstring();
    const bool customEnv = !desc.inheritEnvironment || !desc.environment.empty() || !desc.unsetEnvironment.empty();
    std::wstring envBlock;
    if (customEnv) envBlock = buildEnvironmentBlock(desc);
    const std::wstring cwd = desc.workingDirectory.empty() ? std::wstring() : desc.workingDirectory.wstring();

    PROCESS_INFORMATION pi{};
    const BOOL created =
        CreateProcessW(desc.searchPath ? nullptr : application.c_str(), commandLine.data(), nullptr, nullptr,
                       inherit.empty() ? FALSE : TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                       customEnv ? static_cast<LPVOID>(envBlock.data()) : nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                       &si.StartupInfo, &pi);
    const DWORD createError = GetLastError();
    if (attrList) DeleteProcThreadAttributeList(attrList);
    if (!created) return win32::makeWin32Error(createError, std::string("CreateProcessW '") + programUtf8 + "'");

    CloseHandle(pi.hThread);
    out.process = win32::fromHandle(pi.hProcess);
    out.pid = static_cast<u32>(pi.dwProcessId);
    if (parentIn) out.stdinWrite = win32::fromHandle(owned.take(parentIn));
    if (parentOut) out.stdoutRead = win32::fromHandle(owned.take(parentOut));
    if (parentErr) out.stderrRead = win32::fromHandle(owned.take(parentErr));
    return out; // `owned` closes the child ends, NUL handles and std-handle duplicates
}

Result<std::optional<i32>> processWait(NativeHandle process, u32, i64 timeoutMs) {
    // WaitForSingleObject can time out up to one scheduler tick (~15.6 ms) before the requested interval;
    // wait again for the remainder so a timed-out wait() always lasted at least `timeoutMs`.
    using Clock = std::chrono::steady_clock;
    const i64 cappedMs = std::min<i64>(timeoutMs, INFINITE - 1);
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max<i64>(cappedMs, 0));
    DWORD r = WAIT_TIMEOUT;
    for (;;) {
        DWORD ms = INFINITE;
        if (timeoutMs >= 0) {
            const i64 left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            ms = static_cast<DWORD>(std::clamp<i64>(left, 0, INFINITE - 1));
        }
        r = WaitForSingleObject(win32::toHandle(process), ms);
        if (r != WAIT_TIMEOUT || Clock::now() >= deadline) break;
    }
    if (r == WAIT_TIMEOUT) return std::optional<i32>();
    if (r != WAIT_OBJECT_0) return lastError("WaitForSingleObject(process)");
    DWORD code = 0;
    if (!GetExitCodeProcess(win32::toHandle(process), &code)) return lastError("GetExitCodeProcess");
    return std::optional<i32>(static_cast<i32>(code));
}

Result<void> processKill(NativeHandle process, u32) {
    HANDLE h = win32::toHandle(process);
    if (!TerminateProcess(h, static_cast<UINT>(kKilledExitCode))) {
        const DWORD e = GetLastError();
        // Terminating a process that already exited fails with access denied; that is fine.
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) return {};
        return win32::makeWin32Error(e, "TerminateProcess");
    }
    return {};
}

void processClose(NativeHandle process) noexcept {
    if (process != kInvalidNativeHandle && process != 0) CloseHandle(win32::toHandle(process));
}

bool activeCodePageIsUtf8() noexcept { return GetACP() == CP_UTF8; }

NativeHandle standardHandle(int which) noexcept {
    const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    if (which < 0 || which > 2) return kInvalidNativeHandle;
    HANDLE h = GetStdHandle(ids[which]);
    return (h == nullptr || h == INVALID_HANDLE_VALUE) ? kInvalidNativeHandle : win32::fromHandle(h);
}

} // namespace helios::os
