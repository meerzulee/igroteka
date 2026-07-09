// k32web — kernel32/ntdll-lite for Fortochka. Grows import by import,
// stub-log driven; today it holds exactly what the corpus needs.
// All intelligence lives here, above the CPU: zhelezo never sees Windows.
#pragma once

#include <cstdint>
#include <string>

#include "zhelezo/zhelezo.h"

namespace k32web {

struct Process {
    zhelezo::Bus bus;
    bool exited = false;
    uint32_t exit_code = 0;
};

// Handle one hostcall for `dll!name`: lift stdcall args from the guest stack,
// perform the effect, set EAX, pop args, and resume at the return address.
// Returns false when the import is not implemented (caller logs and stops).
bool dispatch(const std::string& dll, const std::string& name,
              zhelezo::Cpu& cpu, Process& p);

} // namespace k32web
