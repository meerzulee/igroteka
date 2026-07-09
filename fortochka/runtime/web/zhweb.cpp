// zhweb — Emscripten entry point. Same Machine + HLE pipeline as zhrun, exposed
// as one C function the page calls with an uploaded PE's bytes. Emscripten
// routes stdout to a JS callback, so guest console output lands in the browser.
#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "d9web/d9web.h"
#include "k32web/k32web.h"
#include "runtime/machine.h"
#include "u32web/u32web.h"

using runtime::Machine;

namespace {
uint32_t g_fb_w = 0, g_fb_h = 0;
const uint32_t* g_fb = nullptr;
} // namespace

extern "C" {

// Run a PE32 image. Returns the process exit code, or a negative error:
//   -1 peload failure   -2 machine error (fault/unimplemented import/runaway)
EMSCRIPTEN_KEEPALIVE
int zhweb_run(const uint8_t* file, int len, int arena_mb) {
    if (arena_mb <= 0) arena_mb = 64;
    g_fb = nullptr;
    g_fb_w = g_fb_h = 0;
    Machine m((uint32_t)arena_mb << 20);
    try {
        const auto& img = m.load(file, (size_t)len);
        printf("zhweb: base=0x%x entry=0x%x imports=%zu\n", img.base, img.entry,
               img.imports.size());
    } catch (const peload::LoadError& e) {
        printf("peload: %s\n", e.what());
        return -1;
    }

    k32web::install(m);
    u32web::install(m);
    d9web::install(m);
    // Seed one WM_PAINT so a windowed exe paints once; a console exe never pumps
    // and leaves it unconsumed.
    u32web::post_message(0x00010001, 0x000F /*WM_PAINT*/, 0, 0);

    int code;
    try {
        code = m.run_entry();
        printf("zhweb: exit=%d icount=%llu\n", code,
               (unsigned long long)m.cpu().icount);
    } catch (const runtime::MachineError& e) {
        printf("zhweb: %s\n", e.what.c_str());
        code = -2;
    }
    // Prefer a presented D3D frame; fall back to the GDI paint framebuffer.
    g_fb = d9web::framebuffer(g_fb_w, g_fb_h);
    if (!g_fb) g_fb = u32web::framebuffer(g_fb_w, g_fb_h);
    return code;
}

// Framebuffer accessors for the page's canvas blit (ARGB 0xFFRRGGBB, row-major).
EMSCRIPTEN_KEEPALIVE int zhweb_fb_width() { return (int)g_fb_w; }
EMSCRIPTEN_KEEPALIVE int zhweb_fb_height() { return (int)g_fb_h; }
EMSCRIPTEN_KEEPALIVE const uint32_t* zhweb_fb_ptr() { return g_fb; }

} // extern "C"
