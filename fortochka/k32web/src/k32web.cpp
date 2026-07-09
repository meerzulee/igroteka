#include "k32web/k32web.h"

#include <cstdio>
#include <cstring>

namespace k32web {

namespace {

using zhelezo::Bus;
using zhelezo::Cpu;
using zhelezo::EAX;
using zhelezo::ESP;

uint32_t rd32(const Bus& b, uint32_t a) {
    if (a >= b.size || b.size - a < 4) return 0; // host side: never throw
    uint32_t v;
    std::memcpy(&v, b.base + a, 4);
    return v;
}
void wr32(const Bus& b, uint32_t a, uint32_t v) {
    if (a >= b.size || b.size - a < 4) return;
    std::memcpy(b.base + a, &v, 4);
}

// stdcall frame at hostcall time: [esp] = return address, [esp+4+4i] = arg i.
uint32_t arg(const Cpu& c, const Bus& b, unsigned i) {
    return rd32(b, c.gpr[ESP] + 4 + 4 * i);
}
// Complete a stdcall: set EAX, pop return address + nargs arguments, resume.
void ret(Cpu& c, const Bus& b, unsigned nargs, uint32_t eax) {
    c.gpr[EAX] = eax;
    c.eip = rd32(b, c.gpr[ESP]);
    c.gpr[ESP] += 4 + 4 * nargs;
}

// Windows std-handle pseudo-values; any distinct nonzero constants work.
constexpr uint32_t H_STDIN = 0x11, H_STDOUT = 0x12, H_STDERR = 0x13;

} // namespace

bool dispatch(const std::string& dll, const std::string& name, Cpu& cpu,
              Process& p) {
    const Bus& b = p.bus;
    if (dll != "kernel32.dll") return false;

    if (name == "GetStdHandle") {
        const uint32_t n = arg(cpu, b, 0); // -10 in, -11 out, -12 err
        uint32_t h = 0xFFFFFFFFu;          // INVALID_HANDLE_VALUE
        if (n == (uint32_t)-10) h = H_STDIN;
        else if (n == (uint32_t)-11) h = H_STDOUT;
        else if (n == (uint32_t)-12) h = H_STDERR;
        ret(cpu, b, 1, h);
        return true;
    }
    if (name == "WriteFile") {
        const uint32_t h = arg(cpu, b, 0), buf = arg(cpu, b, 1),
                       len = arg(cpu, b, 2), p_written = arg(cpu, b, 3);
        if (buf >= b.size || b.size - buf < len) {
            ret(cpu, b, 5, 0); // buffer outside guest memory: fail the call
            return true;
        }
        FILE* out = h == H_STDERR ? stderr : stdout;
        std::fwrite(b.base + buf, 1, len, out);
        std::fflush(out);
        if (p_written) wr32(b, p_written, len);
        ret(cpu, b, 5, 1);
        return true;
    }
    if (name == "ExitProcess") {
        p.exited = true;
        p.exit_code = arg(cpu, b, 0);
        return true; // no ret: nothing resumes
    }
    return false;
}

} // namespace k32web
