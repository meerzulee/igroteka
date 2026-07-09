// zhrun — native Fortochka runtime shell: arena + peload + zhelezo + k32web.
// Runs a real PE32 in the interpreter with imports served by the host.
//
//   zhrun game.exe [--arena MB] [--steps N]
//
// This is the F1 pipeline exactly as it will exist in the browser; only the
// shell (CLI vs worker) and the VFS behind k32web differ there.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "k32web/k32web.h"
#include "peload/peload.h"
#include "zhelezo/zhelezo.h"

using namespace zhelezo;

namespace {

// Slot 0 is not an import: it is the return address we push before entry, so
// a `ret` from the entry point exits cleanly with EAX as the process code.
constexpr uint32_t SENTINEL_SLOT = 0;
constexpr uint32_t STACK_TOP = 0x00380000; // below the 0x400000 image base

std::vector<uint8_t> slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)n);
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        fprintf(stderr, "%s: short read\n", path);
        exit(2);
    }
    fclose(f);
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    uint32_t arena_mb = 64;
    uint64_t max_steps = ~0ull;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--arena") && i + 1 < argc) arena_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) max_steps = strtoull(argv[++i], nullptr, 0);
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: zhrun game.exe [--arena MB] [--steps N]\n");
        return 2;
    }

    std::vector<uint8_t> arena((size_t)arena_mb << 20, 0);
    std::vector<uint8_t> file = slurp(path);

    uint32_t next_slot = SENTINEL_SLOT + 1;
    peload::Image img;
    try {
        img = peload::load(arena.data(), (uint32_t)arena.size(), file.data(),
                           file.size(), HOSTCALL_BASE, next_slot);
    } catch (const peload::LoadError& e) {
        fprintf(stderr, "peload: %s\n", e.what());
        return 2;
    }
    fprintf(stderr, "zhrun: %s base=0x%x entry=0x%x imports=%zu\n", path,
            img.base, img.entry, img.imports.size());

    std::vector<const peload::Import*> slots(next_slot, nullptr);
    for (const auto& imp : img.imports) slots[imp.slot] = &imp;

    k32web::Process proc;
    proc.bus = Bus{arena.data(), (uint32_t)arena.size()};

    Cpu cpu;
    cpu.eip = img.entry;
    cpu.gpr[ESP] = STACK_TOP;
    // Entry return address: sentinel hostcall slot.
    cpu.gpr[ESP] -= 4;
    const uint32_t sentinel = HOSTCALL_BASE + SENTINEL_SLOT * 16;
    memcpy(arena.data() + cpu.gpr[ESP], &sentinel, 4);

    if (!img.tls_callbacks.empty())
        fprintf(stderr, "zhrun: warning: %zu TLS callbacks not run yet\n",
                img.tls_callbacks.size());

    while (!proc.exited) {
        RunResult r = run(cpu, proc.bus, max_steps);
        switch (r.exit) {
            case Exit::Hostcall: {
                const uint32_t slot = (cpu.eip - HOSTCALL_BASE) / 16;
                if (slot == SENTINEL_SLOT) {
                    proc.exited = true;
                    proc.exit_code = cpu.gpr[EAX];
                    break;
                }
                const peload::Import* imp =
                    slot < slots.size() ? slots[slot] : nullptr;
                if (!imp) {
                    fprintf(stderr, "zhrun: hostcall to unknown slot %u\n", slot);
                    return 3;
                }
                if (!k32web::dispatch(imp->dll, imp->name, cpu, proc)) {
                    fprintf(stderr, "zhrun: unimplemented import %s!%s\n",
                            imp->dll.c_str(), imp->name.c_str());
                    return 3;
                }
                break;
            }
            case Exit::Steps:
                fprintf(stderr, "zhrun: step budget exhausted (icount=%" PRIu64 ")\n",
                        cpu.icount);
                return 4;
            case Exit::Hlt:
            case Exit::Int3:
            case Exit::IntN:
                fprintf(stderr, "zhrun: unexpected %s at eip=0x%08x\n",
                        r.exit == Exit::Hlt ? "hlt" : "int", cpu.eip);
                return 4;
            case Exit::Fault:
                fprintf(stderr,
                        "zhrun: FAULT kind=%d eip=0x%08x addr=0x%08x "
                        "icount=%" PRIu64 "\n",
                        (int)r.fault, cpu.eip, r.fault_addr, cpu.icount);
                fprintf(stderr,
                        "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x "
                        "ebp=%08x esi=%08x edi=%08x\n",
                        cpu.gpr[EAX], cpu.gpr[ECX], cpu.gpr[EDX], cpu.gpr[EBX],
                        cpu.gpr[ESP], cpu.gpr[EBP], cpu.gpr[ESI], cpu.gpr[EDI]);
                return 1;
        }
    }

    fprintf(stderr, "zhrun: process exited code=%u icount=%" PRIu64 "\n",
            proc.exit_code, cpu.icount);
    return (int)proc.exit_code;
}
