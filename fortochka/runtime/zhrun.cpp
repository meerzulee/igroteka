// zhrun — native Fortochka runtime shell: Machine + HLE modules.
//
//   zhrun game.exe [--arena MB] [--steps N]
//
// The browser build (runtime/web) wraps the same Machine; only the shell
// differs. HLE modules register handlers; Machine drives the guest and
// reverse-thunks callbacks.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "k32web/k32web.h"
#include "runtime/machine.h"
#include "u32web/u32web.h"

using runtime::Machine;

static std::vector<uint8_t> slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)n);
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { perror(path); exit(2); }
    fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    const char* path = nullptr;
    uint32_t arena_mb = 64;
    uint64_t max_steps = 200'000'000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--arena") && i + 1 < argc) arena_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) max_steps = strtoull(argv[++i], nullptr, 0);
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: zhrun game.exe [--arena MB] [--steps N]\n");
        return 2;
    }

    std::vector<uint8_t> exe = slurp(path);
    Machine m((uint64_t)arena_mb << 20 > 0xF0000000u ? 0xE0000000u
                                                      : (arena_mb << 20));
    try {
        const auto& img = m.load(exe.data(), exe.size());
        fprintf(stderr, "zhrun: %s base=0x%x entry=0x%x imports=%zu\n", path,
                img.base, img.entry, img.imports.size());
    } catch (const peload::LoadError& e) {
        fprintf(stderr, "peload: %s\n", e.what());
        return 2;
    }

    k32web::install(m);
    u32web::install(m);

    int code;
    try {
        code = m.run_entry(max_steps);
    } catch (const runtime::MachineError& e) {
        fprintf(stderr, "zhrun: %s\n", e.what.c_str());
        return 1;
    }
    fprintf(stderr, "zhrun: process exited code=%d icount=%" PRIu64 "\n", code,
            m.cpu().icount);
    return code;
}
