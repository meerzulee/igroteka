// k32web — kernel32/ntdll-lite for Fortochka. Registers a handler on a Machine;
// grows import by import, stub-log driven. All intelligence lives here, above
// the CPU: zhelezo never sees Windows.
#pragma once

#include "runtime/machine.h"

namespace k32web {

// Install the kernel32 handler set (GetStdHandle/WriteFile/ExitProcess today).
void install(runtime::Machine& m);

} // namespace k32web
