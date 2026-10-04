#pragma once
// OS services of ToolsFramework, implemented in platform/win32 and platform/posix (the IPC
// transport of ipc.h lives there too).

#include <string>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::tf::os {

u32 currentProcessId() noexcept;
/// This machine's name ("" if unknown).
std::string hostName();
bool processIsRunning(u32 pid) noexcept;
/// Environment variable as UTF-8 ("" when unset). Windows reads the UTF-16 environment, so
/// non-ASCII values (a user profile path) survive.
std::string environment(const char* name);
/// Directory for IPC endpoints that are files (POSIX Unix sockets); unused on Windows.
std::string runtimeDirectory();

/// What a directory entry is, looked at without following it (Workspace::confine's link walk).
enum class EntryKind : u8 {
    Missing,    ///< Nothing there (or a parent is missing or not a directory).
    File,       ///< A regular file.
    Directory,
    Link,       ///< POSIX: a symbolic link. Windows: any reparse point (symbolic link, junction, mount point).
    Other,      ///< A device, FIFO or socket.
};
/// lstat() on POSIX, GetFileAttributesW() on Windows. Fails (IoError) when the entry cannot be
/// inspected, e.g. a parent directory is not searchable: callers treat that as a refusal.
Result<EntryKind> entryKind(const fs::Path& path);
/// `path` with every link on it resolved: realpath() on POSIX; on Windows the handle's
/// GetFinalPathNameByHandleW() name, which follows symbolic links, junctions and mount points
/// (a `\\?\` path). Fails when the path (or a link's target) does not exist.
Result<fs::Path> finalPath(const fs::Path& path);

} // namespace helios::tf::os
