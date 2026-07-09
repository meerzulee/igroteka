#include "k32web/k32web.h"

#include <cstdio>
#include <string>

namespace k32web {

using runtime::Machine;

namespace {
// Windows std-handle pseudo-values; any distinct nonzero constants work.
constexpr uint32_t H_STDOUT = 0x12, H_STDERR = 0x13, H_STDIN = 0x11;

// EXCEPTION_DISPOSITION
constexpr uint32_t ExceptionContinueExecution = 0;
constexpr uint32_t ExceptionContinueSearch = 1;

// Dispatch a synthetic exception through the guest's fs:[0] SEH chain. Builds an
// EXCEPTION_RECORD in TIB scratch and reverse-thunks each registered handler
// with (record, establisher_frame, context, dispatcher). Honors the handler's
// disposition: ContinueExecution stops the walk (caller resumes), everything
// else advances to the next frame. Returns true if a handler took it, false if
// the chain was exhausted (unhandled).
//
// Reference: ReactOS/Wine RtlDispatchException — the chain walk and disposition
// semantics; read, not copied.
bool dispatch_seh(Machine& m, uint32_t code, uint32_t flags, uint32_t except_addr,
                  uint32_t nparams, uint32_t params_ptr) {
    const uint32_t tib = m.tib_addr();
    const uint32_t rec = tib + Machine::TIB_SCRATCH_RECORD;
    const uint32_t ctx = tib + Machine::TIB_SCRATCH_CONTEXT;

    // EXCEPTION_RECORD { code, flags, *nested, address, nparams, info[15] }.
    m.write32(rec + 0, code);
    m.write32(rec + 4, flags);
    m.write32(rec + 8, 0);
    m.write32(rec + 12, except_addr);
    uint32_t n = nparams > 15 ? 15 : nparams;
    m.write32(rec + 16, n);
    for (uint32_t i = 0; i < n; i++)
        m.write32(rec + 20 + 4 * i, params_ptr ? m.read32(params_ptr + 4 * i) : 0);

    uint32_t node = m.read32(m.cpu().fs_base); // fs:[0] = ExceptionList head
    while (node != Machine::SEH_CHAIN_END && node != 0) {
        uint32_t handler = m.read32(node + 4); // record: { *next, handler }
        // cdecl handler(ExceptionRecord*, EstablisherFrame, Context*, Dispatcher).
        uint32_t disp = m.call_guest(handler, {rec, node, ctx, 0});
        if (disp == ExceptionContinueExecution) return true;
        if (disp == ExceptionContinueSearch) {
            node = m.read32(node); // advance to *next
            continue;
        }
        node = m.read32(node); // nested/collided: keep searching (tier-0)
    }
    return false;
}
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
        if (name == "RaiseException") {
            // void RaiseException(code, flags, nNumberOfArguments, lpArguments).
            uint32_t code = m.arg(0), flags = m.arg(1), nargs = m.arg(2),
                     lpargs = m.arg(3);
            uint32_t except_addr = m.read32(m.cpu().gpr[zhelezo::ESP]); // call site
            bool handled = dispatch_seh(m, code, flags, except_addr, nargs, lpargs);
            if (!handled)
                throw runtime::MachineError{"unhandled SEH exception code=" +
                                            std::to_string(code)};
            // Handler chose ContinueExecution: RaiseException returns normally
            // and the guest resumes after the call site.
            m.ret(4, 0);
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
