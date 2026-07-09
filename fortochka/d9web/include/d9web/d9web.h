// d9web — Direct3D 9 frontend for Fortochka. Synthesizes the COM objects a
// game drives (IDirect3D9, IDirect3DDevice9) as guest vtable-thunk objects and
// translates the calls into a software backbuffer for now — the backend seam
// (WebGL/WebGPU) swaps in later without the frontend moving. Part of Fortochka
// (MIT). Semantics oracle: WineD3D (read, never copied).
#pragma once

#include <cstdint>

#include "runtime/machine.h"

namespace d9web {

// Install the d3d9 handler (Direct3DCreate9) on a Machine and reset state.
void install(runtime::Machine& m);

// The device backbuffer after the last Present (ARGB 0xFFRRGGBB, row-major).
// Returns nullptr until a device presents a frame. The shell blits this to the
// canvas / dumps it, same path as the GDI framebuffer.
const uint32_t* framebuffer(uint32_t& width, uint32_t& height);

} // namespace d9web
