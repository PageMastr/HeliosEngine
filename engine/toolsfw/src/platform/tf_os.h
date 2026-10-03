#pragma once
// OS services of ToolsFramework, implemented in platform/win32 and platform/posix (the IPC
// transport of ipc.h lives there too).

#include <string>

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

} // namespace helios::tf::os
