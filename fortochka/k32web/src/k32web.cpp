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
constexpr uint32_t EXCEPTION_NONCONTINUABLE = 0x1;

constexpr uint32_t EXC_RECORD_SIZE = 0x50;  // { code,flags,*rec,addr,n,info[15] }
constexpr uint32_t CONTEXT_SIZE = 0x2CC;    // i386 CONTEXT
constexpr uint32_t CTX_FLAGS = 0x00010007;  // CONTEXT_i386|INTEGER|CONTROL

// Fill an i386 CONTEXT at guest address `ctx` from the current CPU state — so a
// handler reading ContextRecord->Esp/Ebp/Eip/registers sees sane values. This
// is the state at the exception point (for RaiseException, the call site).
void write_context(Machine& m, uint32_t ctx, uint32_t except_addr) {
    for (uint32_t i = 0; i < CONTEXT_SIZE; i += 4) m.write32(ctx + i, 0);
    const auto& c = m.cpu();
    m.write32(ctx + 0x00, CTX_FLAGS);
    m.write32(ctx + 0x9C, c.gpr[zhelezo::EDI]);
    m.write32(ctx + 0xA0, c.gpr[zhelezo::ESI]);
    m.write32(ctx + 0xA4, c.gpr[zhelezo::EBX]);
    m.write32(ctx + 0xA8, c.gpr[zhelezo::EDX]);
    m.write32(ctx + 0xAC, c.gpr[zhelezo::ECX]);
    m.write32(ctx + 0xB0, c.gpr[zhelezo::EAX]);
    m.write32(ctx + 0xB4, c.gpr[zhelezo::EBP]);
    m.write32(ctx + 0xB8, except_addr);      // Eip
    m.write32(ctx + 0xC0, c.eflags);         // EFlags
    m.write32(ctx + 0xC4, c.gpr[zhelezo::ESP]); // Esp
}

// Dispatch a synthetic exception through the guest's fs:[0] SEH chain, reverse-
// thunking each registered handler with (record, establisher, context,
// dispatcher). The EXCEPTION_RECORD and CONTEXT live on the guest stack below
// the current ESP — one frame per dispatch, so a handler that raises a nested
// exception cannot clobber the outer record. Honors dispositions:
// ContinueExecution stops the walk (caller resumes), ContinueSearch advances;
// anything else is an invalid disposition. The chain must climb strictly toward
// higher stack addresses (real SEH's frame-order rule), which also rejects
// cyclic/corrupt chains. Returns true if handled, false if the chain was
// exhausted unhandled.
//
// Reference: ReactOS/Wine RtlDispatchException — walk, ordering, and disposition
// semantics; read, not copied.
//
// KNOWN GAPS (need feature work, not fixes here):
//  - Fault→SEH: dispatching a real CPU fault must first snapshot ALL guest
//    registers into CONTEXT and, on ContinueExecution, restore the (possibly
//    handler-edited) CONTEXT — call_guest only preserves ESP/EIP, so the fault
//    path will need its own register save/apply around this call.
//  - RtlUnwind / __except transfer: a handler that unwinds sets guest ESP/EIP
//    to an __except block instead of resuming at the exception point. That is
//    incompatible with call_guest's unconditional ESP/EIP restore; it needs a
//    distinct "transfer, do not return to dispatcher" path.
//  - CONTEXT here reflects the RaiseException call site only (integer+control),
//    not FP/debug/extended state.
bool dispatch_seh(Machine& m, uint32_t code, uint32_t flags, uint32_t except_addr,
                  uint32_t nparams, uint32_t params_ptr) {
    const uint32_t saved_esp = m.cpu().gpr[zhelezo::ESP];
    // Reserve record + context on the guest stack; run handlers below them.
    const uint32_t rec = (saved_esp - EXC_RECORD_SIZE) & ~0xFu;
    const uint32_t ctx = (rec - CONTEXT_SIZE) & ~0xFu;
    if (ctx < 0x2000 || saved_esp >= m.mem_size()) // sanity: stack sane?
        throw runtime::MachineError{"SEH dispatch: stack pointer out of range"};

    // EXCEPTION_RECORD { code, flags, *nested, address, nparams, info[15] }.
    m.write32(rec + 0, code);
    m.write32(rec + 4, flags);
    m.write32(rec + 8, 0);
    m.write32(rec + 12, except_addr);
    uint32_t n = nparams > 15 ? 15 : nparams;
    m.write32(rec + 16, n);
    for (uint32_t i = 0; i < n; i++)
        m.write32(rec + 20 + 4 * i, params_ptr ? m.read32(params_ptr + 4 * i) : 0);
    write_context(m, ctx, except_addr);

    // Handlers push below the reserved region; restore ESP when done.
    m.cpu().gpr[zhelezo::ESP] = ctx - 0x20;

    const uint32_t stack_base = m.tib_addr(); // records live below the TIB/stack top
    uint32_t node = m.read32(m.cpu().fs_base); // fs:[0] = ExceptionList head
    uint32_t prev = 0; // enforce strictly increasing addresses (no cycles)
    bool handled = false;
    for (unsigned steps = 0; node != Machine::SEH_CHAIN_END && node != 0; steps++) {
        // Validate: in bounds, aligned, room for the record, strictly above the
        // previous frame. A bad chain is corruption — stop rather than spin.
        if (steps > 4096 || node <= prev || (node & 3) || node + 8 > stack_base ||
            node + 8 > m.mem_size())
            break;
        prev = node;
        uint32_t handler = m.read32(node + 4); // record: { *next, handler }
        // cdecl handler(ExceptionRecord*, EstablisherFrame, Context*, Dispatcher).
        uint32_t disp = m.call_guest(handler, {rec, node, ctx, 0});
        if (disp == ExceptionContinueExecution) {
            if (flags & EXCEPTION_NONCONTINUABLE)
                throw runtime::MachineError{
                    "handler continued a noncontinuable exception"};
            handled = true;
            break;
        }
        if (disp != ExceptionContinueSearch)
            throw runtime::MachineError{"invalid SEH disposition " +
                                        std::to_string(disp)};
        node = m.read32(node); // advance to *next
    }

    m.cpu().gpr[zhelezo::ESP] = saved_esp;
    return handled;
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
