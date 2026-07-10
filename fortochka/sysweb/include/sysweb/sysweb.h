// sysweb — the init-gate stub DLLs for Fortochka: the subsystem imports a game
// resolves at startup that we satisfy just enough to get past initialization.
// steam_api (run without Steam), ole32 (COM apartment), ddraw/dinput8/d3d8
// (declined so the game takes the d3d9 path we implement), msvfw32 (video
// skipped), wsock32 (no network). Growth is stub-log-driven. Part of Fortochka
// (MIT). No code from any real DLL is copied — these are behavioral stubs.
#pragma once

#include "runtime/machine.h"

namespace sysweb {
void install(runtime::Machine& m);
} // namespace sysweb
