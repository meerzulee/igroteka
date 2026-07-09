// zhweb — Emscripten entry point. Same Machine + HLE pipeline as zhrun, exposed
// as one C function the page calls with an uploaded PE's bytes. Emscripten
// routes stdout to a JS callback, so guest console output lands in the browser.
#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "k32web/k32web.h"
#include "runtime/machine.h"
#include "u32web/u32web.h"

using runtime::Machine;

extern "C" {

// Run a PE32 image. Returns the process exit code, or a negative error:
//   -1 peload failure   -2 machine error (fault/unimplemented import/runaway)
EMSCRIPTEN_KEEPALIVE
int zhweb_run(const uint8_t* file, int len, int arena_mb) {
    if (arena_mb <= 0) arena_mb = 64;
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

    try {
        int code = m.run_entry();
        printf("zhweb: exit=%d icount=%llu\n", code,
               (unsigned long long)m.cpu().icount);
        return code;
    } catch (const runtime::MachineError& e) {
        printf("zhweb: %s\n", e.what.c_str());
        return -2;
    }
}

} // extern "C"
