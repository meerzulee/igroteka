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

// Client-area framebuffer the guest painted into (ARGB 0xFFRRGGBB, row-major,
// width*height). Returns nullptr if no window was created. The shell (zhrun PPM
// dump / zhweb canvas blit) reads this after the pump processes WM_PAINT.
const uint32_t* framebuffer(uint32_t& width, uint32_t& height);

} // namespace u32web
