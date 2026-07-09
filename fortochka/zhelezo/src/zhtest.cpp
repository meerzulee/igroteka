// zhtest — flat-binary test runner for the zhelezo tier-0 interpreter.
//
//   zhtest prog.bin [--load 0x1000] [--esp 0xF0000] [--steps N] [--trace]
//
// Loads the image into a 16 MB arena, runs to an exit condition, prints the
// machine state in a stable grep-friendly format the tests/run.py driver
// asserts against.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "zhelezo/zhelezo.h"

using namespace zhelezo;

static const char* exitName(Exit e) {
    switch (e) {
        case Exit::Steps: return "steps";
        case Exit::Hostcall: return "hostcall";
        case Exit::Hlt: return "hlt";
        case Exit::Int3: return "int3";
        case Exit::IntN: return "intn";
        default: return "fault";
    }
}
static const char* faultName(FaultKind k) {
    switch (k) {
        case FaultKind::Ud: return "ud";
        case FaultKind::De: return "de";
        case FaultKind::MemRead: return "memread";
        case FaultKind::MemWrite: return "memwrite";
        case FaultKind::MemExec: return "memexec";
        default: return "none";
    }
}

int main(int argc, char** argv) {
    const char* path = nullptr;
    uint32_t load = 0x1000, esp = 0xF0000;
    uint64_t max_steps = 10'000'000;
    bool trace = false;
    bool nocache = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--load") && i + 1 < argc) load = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--esp") && i + 1 < argc) esp = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) max_steps = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--trace")) trace = true;
        else if (!strcmp(argv[i], "--no-cache")) nocache = true;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: zhtest prog.bin [--load A] [--esp A] [--steps N] [--trace]\n");
        return 2;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    std::vector<uint8_t> arena(16u << 20, 0);
    size_t n = fread(arena.data() + load, 1, arena.size() - load, f);
    fclose(f);
    if (n == 0) {
        fprintf(stderr, "%s: empty image\n", path);
        return 2;
    }

    Bus bus{arena.data(), (uint32_t)arena.size()};
    Cpu cpu;
    cpu.eip = load;
    cpu.gpr[ESP] = esp;

    RunResult r{};
    if (trace) {
        uint64_t steps = 0;
        for (; steps < max_steps; steps++) {
            fprintf(stderr, "eip=%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x\n",
                    cpu.eip, cpu.gpr[EAX], cpu.gpr[ECX], cpu.gpr[EDX], cpu.gpr[EBX],
                    cpu.gpr[ESP]);
            r = step(cpu, bus);
            if (r.exit != Exit::Steps) break;
        }
    } else if (nocache) {
        r = run(cpu, bus, max_steps);
    } else {
        DecodeCache* cache = decode_cache_new();
        r = run(cpu, bus, max_steps, cache);
        decode_cache_free(cache);
    }

    printf("exit=%s fault=%s vector=%u fault_addr=0x%08x icount=%" PRIu64 "\n",
           exitName(r.exit), faultName(r.fault), r.vector, r.fault_addr, cpu.icount);
    printf("eax=0x%08x ecx=0x%08x edx=0x%08x ebx=0x%08x esp=0x%08x ebp=0x%08x "
           "esi=0x%08x edi=0x%08x\n",
           cpu.gpr[EAX], cpu.gpr[ECX], cpu.gpr[EDX], cpu.gpr[EBX], cpu.gpr[ESP],
           cpu.gpr[EBP], cpu.gpr[ESI], cpu.gpr[EDI]);
    printf("eip=0x%08x cf=%d pf=%d af=%d zf=%d sf=%d df=%d of=%d\n", cpu.eip,
           !!(cpu.eflags & FLAG_CF), !!(cpu.eflags & FLAG_PF),
           !!(cpu.eflags & FLAG_AF), !!(cpu.eflags & FLAG_ZF),
           !!(cpu.eflags & FLAG_SF), !!(cpu.eflags & FLAG_DF),
           !!(cpu.eflags & FLAG_OF));
    return r.exit == Exit::Fault ? 1 : 0;
}
