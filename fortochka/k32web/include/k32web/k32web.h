// k32web — kernel32/ntdll-lite for Fortochka. Registers a handler on a Machine;
// grows import by import, stub-log driven. All intelligence lives here, above
// the CPU: zhelezo never sees Windows.
#pragma once

#include <string>

#include "runtime/machine.h"

namespace k32web {

// Install the kernel32 handler set (GetStdHandle/WriteFile/ExitProcess today).
void install(runtime::Machine& m);

// Mount a host directory as the backing store for the in-memory VFS. A guest
// path under `guest_prefix` (normalized, e.g. "c:/rtw") that misses the in-
// memory VFS is lazily loaded from `<host_root>/<remainder>` on first open, and
// directory enumeration (FindFirstFile) falls back to scanning the host dir.
// Lets a native boot read the real game tree without preloading gigabytes.
void mount_host_dir(const std::string& guest_prefix, const std::string& host_root);

} // namespace k32web
