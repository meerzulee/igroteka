// u32web — user32-enough: one window, a message queue, and the message pump.
// The load-bearing piece is DispatchMessageA, which reverse-thunks into the
// guest's wndproc via Machine::call_guest. Part of Fortochka (MIT).
#pragma once

#include <cstdint>

#include "runtime/machine.h"

namespace u32web {

// Install the user32 handler set on a Machine. `seed_messages`, if set, is
// called once when the first window is created to enqueue an initial message
// script (the corpus harness uses this to drive the pump deterministically
// without a real event source).
void install(runtime::Machine& m);

// Enqueue a message onto the single window's queue (used by the harness and by
// guest PostMessage/PostQuitMessage). msg fields are guest ABI values.
void post_message(uint32_t hwnd, uint32_t message, uint32_t wparam,
                  uint32_t lparam);

} // namespace u32web
