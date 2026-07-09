#include "k32web/k32web.h"

#include <cstdio>
#include <string>

namespace k32web {

using runtime::Machine;

namespace {
// Windows std-handle pseudo-values; any distinct nonzero constants work.
constexpr uint32_t H_STDOUT = 0x12, H_STDERR = 0x13, H_STDIN = 0x11;
} // namespace

void install(Machine& m) {
    m.add_handler([](Machine& m, const std::string& dll,
                     const std::string& name) -> bool {
        if (dll != "kernel32.dll") return false;

        if (name == "GetStdHandle") {
            uint32_t n = m.arg(0); // -10 in, -11 out, -12 err
            uint32_t h = 0xFFFFFFFFu;
            if (n == (uint32_t)-10) h = H_STDIN;
            else if (n == (uint32_t)-11) h = H_STDOUT;
            else if (n == (uint32_t)-12) h = H_STDERR;
            m.ret(1, h);
            return true;
        }
        if (name == "WriteFile") {
            uint32_t h = m.arg(0), buf = m.arg(1), len = m.arg(2),
                     p_written = m.arg(3);
            if (buf >= m.mem_size() || m.mem_size() - buf < len) {
                m.ret(5, 0); // buffer outside guest memory: fail the call
                return true;
            }
            FILE* out = h == H_STDERR ? stderr : stdout;
            std::fwrite(m.mem() + buf, 1, len, out); // exact bytes, NULs included
            std::fflush(out);
            if (p_written) m.write32(p_written, len);
            m.ret(5, 1);
            return true;
        }
        if (name == "ExitProcess") {
            m.exited = true;
            m.exit_code = m.arg(0);
            return true; // no ret: nothing resumes
        }
        return false;
    });
}

} // namespace k32web
