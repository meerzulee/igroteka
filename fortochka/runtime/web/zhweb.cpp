// zhweb — Emscripten entry point. Same peload→zhelezo→k32web pipeline as
// zhrun, exposed as one C function the page calls with an uploaded PE's bytes.
// k32web's WriteFile already goes to stdout; Emscripten routes stdout to a
// JS callback, so the guest's console output lands in the browser.
#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "k32web/k32web.h"
#include "peload/peload.h"
#include "zhelezo/zhelezo.h"

using namespace zhelezo;

namespace {
constexpr uint32_t SENTINEL_SLOT = 0;
constexpr uint32_t STACK_TOP = 0x00380000;
} // namespace

extern "C" {

// Run a PE32 image. Returns the process exit code, or a negative error:
//   -1 peload failure   -2 unimplemented import   -3 fault   -4 runaway
EMSCRIPTEN_KEEPALIVE
int zhweb_run(const uint8_t* file, int len, int arena_mb) {
    if (arena_mb <= 0) arena_mb = 64;
    std::vector<uint8_t> arena((size_t)arena_mb << 20, 0);

    uint32_t next_slot = SENTINEL_SLOT + 1;
    peload::Image img;
    try {
        img = peload::load(arena.data(), (uint32_t)arena.size(), file,
                           (size_t)len, HOSTCALL_BASE, next_slot);
    } catch (const peload::LoadError& e) {
        printf("peload: %s\n", e.what());
        return -1;
    }
    printf("zhweb: base=0x%x entry=0x%x imports=%zu\n", img.base, img.entry,
           img.imports.size());

    std::vector<const peload::Import*> slots(next_slot, nullptr);
    for (const auto& imp : img.imports) slots[imp.slot] = &imp;

    k32web::Process proc;
    proc.bus = Bus{arena.data(), (uint32_t)arena.size()};

    Cpu cpu;
    cpu.eip = img.entry;
    cpu.gpr[ESP] = STACK_TOP - 4;
    const uint32_t sentinel = HOSTCALL_BASE + SENTINEL_SLOT * 16;
    for (int i = 0; i < 4; i++) arena[cpu.gpr[ESP] + i] = (sentinel >> (8 * i)) & 0xFF;

    const uint64_t budget = 200'000'000;
    while (!proc.exited) {
        RunResult r = run(cpu, proc.bus, budget);
        if (r.exit == Exit::Hostcall) {
            const uint32_t slot = (cpu.eip - HOSTCALL_BASE) / 16;
            if (slot == SENTINEL_SLOT) {
                proc.exited = true;
                proc.exit_code = cpu.gpr[EAX];
                break;
            }
            const peload::Import* imp = slot < slots.size() ? slots[slot] : nullptr;
            if (!imp || !k32web::dispatch(imp->dll, imp->name, cpu, proc)) {
                printf("zhweb: unimplemented import %s\n",
                       imp ? (imp->dll + "!" + imp->name).c_str() : "<bad slot>");
                return -2;
            }
        } else if (r.exit == Exit::Fault) {
            printf("zhweb: FAULT kind=%d eip=0x%08x addr=0x%08x\n", (int)r.fault,
                   cpu.eip, r.fault_addr);
            return -3;
        } else if (r.exit == Exit::Steps) {
            printf("zhweb: runaway (icount=%llu)\n", (unsigned long long)cpu.icount);
            return -4;
        } else {
            printf("zhweb: halted early (%d) at eip=0x%08x\n", (int)r.exit, cpu.eip);
            return -4;
        }
    }
    printf("zhweb: exit=%u icount=%llu\n", proc.exit_code,
           (unsigned long long)cpu.icount);
    return (int)proc.exit_code;
}

} // extern "C"
