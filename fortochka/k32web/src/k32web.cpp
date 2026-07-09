#include "k32web/k32web.h"

#include <cstdio>
#include <string>

namespace k32web {

using runtime::Machine;

namespace {
// Windows std-handle pseudo-values; any distinct nonzero constants work.
constexpr uint32_t H_STDOUT = 0x12, H_STDERR = 0x13, H_STDIN = 0x11;

// Host-side process state for the CRT-startup kernel32 surface (single guest
// thread in tier 0). Reset per install().
struct K32 {
    uint32_t last_error = 0;
    uint32_t tls[64] = {};   // Tls{Alloc,Get,Set,Free} slots
    bool tls_used[64] = {};
    uint32_t cmdline = 0;    // cached GetCommandLineA guest string
    uint32_t heap_handle = 0x00E70001; // one process heap pseudo-handle
};
K32 g_k;

// Write a NUL-terminated string into freshly-allocated guest memory, return VA.
uint32_t guest_strdup(Machine& m, const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    uint32_t p = m.alloc(n + 1);
    for (uint32_t i = 0; i <= n; i++) m.mem()[p + i] = (uint8_t)s[i];
    return p;
}

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
    g_k = K32{};
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

        // ---- guest heap ----
        if (name == "GetProcessHeap") { m.ret(0, g_k.heap_handle); return true; }
        if (name == "HeapCreate") { m.ret(3, g_k.heap_handle); return true; }
        if (name == "HeapDestroy") { m.ret(1, 1); return true; }
        if (name == "HeapAlloc") {
            // HeapAlloc(hHeap, dwFlags, dwBytes). HEAP_ZERO_MEMORY = 0x8.
            uint32_t flags = m.arg(1), bytes = m.arg(2);
            uint32_t p = m.alloc(bytes ? bytes : 1);
            if (flags & 0x8)
                for (uint32_t i = 0; i < bytes; i++) m.mem()[p + i] = 0;
            m.ret(3, p);
            return true;
        }
        if (name == "HeapReAlloc") {
            // HeapReAlloc(hHeap, dwFlags, lpMem, dwBytes) — bump-alloc a fresh
            // block and copy dwBytes across (no old-size tracking; over-copy is
            // bounds-clamped). Leaks the old block until the free path lands.
            uint32_t old = m.arg(2), bytes = m.arg(3);
            uint32_t p = m.alloc(bytes ? bytes : 1);
            for (uint32_t i = 0; i < bytes; i++) m.write32(p + i, 0); // clear
            if (old)
                for (uint32_t i = 0; i + 4 <= bytes; i += 4)
                    m.write32(p + i, m.read32(old + i));
            m.ret(4, p);
            return true;
        }
        if (name == "HeapFree" || name == "HeapSize" ||
            name == "HeapValidate") {
            m.ret(3, name == "HeapFree" ? 1 : 0); // no free (bump allocator)
            return true;
        }

        // ---- virtual memory ----
        if (name == "VirtualAlloc") {
            // VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect).
            uint32_t addr = m.arg(0), size = m.arg(1);
            m.ret(4, addr ? addr : m.alloc(size ? size : 1));
            return true;
        }
        if (name == "VirtualFree" || name == "VirtualProtect") {
            m.ret(name == "VirtualProtect" ? 4 : 3, 1);
            return true;
        }

        // ---- TLS (single thread) ----
        if (name == "TlsAlloc") {
            for (uint32_t i = 0; i < 64; i++)
                if (!g_k.tls_used[i]) {
                    g_k.tls_used[i] = true;
                    g_k.tls[i] = 0;
                    m.ret(0, i);
                    return true;
                }
            m.ret(0, 0xFFFFFFFFu); // TLS_OUT_OF_INDEXES
            return true;
        }
        if (name == "TlsFree") {
            uint32_t i = m.arg(0);
            if (i < 64) g_k.tls_used[i] = false;
            m.ret(1, 1);
            return true;
        }
        if (name == "TlsGetValue") {
            uint32_t i = m.arg(0);
            m.ret(1, i < 64 ? g_k.tls[i] : 0);
            return true;
        }
        if (name == "TlsSetValue") {
            uint32_t i = m.arg(0);
            if (i < 64) g_k.tls[i] = m.arg(1);
            m.ret(2, 1);
            return true;
        }

        // ---- critical sections (single-threaded no-ops) ----
        if (name == "InitializeCriticalSection" ||
            name == "EnterCriticalSection" || name == "LeaveCriticalSection" ||
            name == "DeleteCriticalSection") {
            m.ret(1, 0);
            return true;
        }
        if (name == "InitializeCriticalSectionAndSpinCount") {
            m.ret(2, 1);
            return true;
        }

        // ---- timing (deterministic: derived from retired instruction count) ----
        if (name == "GetTickCount") {
            m.ret(0, (uint32_t)(m.cpu().icount / 1000)); // ~1 tick per 1k insns
            return true;
        }
        if (name == "QueryPerformanceCounter") {
            uint32_t p = m.arg(0);
            m.write32(p, (uint32_t)m.cpu().icount);
            m.write32(p + 4, (uint32_t)(m.cpu().icount >> 32));
            m.ret(1, 1);
            return true;
        }
        if (name == "QueryPerformanceFrequency") {
            uint32_t p = m.arg(0);
            m.write32(p, 1000000); // 1 MHz virtual timer
            m.write32(p + 4, 0);
            m.ret(1, 1);
            return true;
        }
        if (name == "GetSystemTimeAsFileTime") {
            uint32_t p = m.arg(0); // 64-bit FILETIME; a fixed 2004-ish stamp
            m.write32(p, 0xD0000000);
            m.write32(p + 4, 0x01C40000);
            m.ret(1, 0);
            return true;
        }
        if (name == "Sleep") { m.ret(1, 0); return true; }

        // ---- module / process info ----
        if (name == "GetModuleHandleA" || name == "GetModuleHandleW") {
            // NULL name = the process image itself (base 0x400000).
            m.ret(1, m.arg(0) == 0 ? 0x00400000 : 0x00400000);
            return true;
        }
        if (name == "GetProcAddress") { m.ret(2, 0); return true; } // not resolvable yet
        if (name == "LoadLibraryA" || name == "LoadLibraryW" ||
            name == "LoadLibraryExA") {
            m.ret(name == "LoadLibraryExA" ? 3 : 1, 0x10000000); // fake module
            return true;
        }
        if (name == "FreeLibrary") { m.ret(1, 1); return true; }
        if (name == "GetCommandLineA") {
            if (!g_k.cmdline) g_k.cmdline = guest_strdup(m, "fortochka.exe");
            m.ret(0, g_k.cmdline);
            return true;
        }
        if (name == "GetCurrentThreadId" || name == "GetCurrentProcessId") {
            m.ret(0, 1);
            return true;
        }
        if (name == "GetCurrentProcess" || name == "GetCurrentThread") {
            m.ret(0, 0xFFFFFFFFu); // pseudo-handle
            return true;
        }
        if (name == "GetVersion") {
            m.ret(0, 0x0A280105); // build 0x0A28, Windows 5.1 (XP)
            return true;
        }
        if (name == "GetVersionExA" || name == "GetVersionExW") {
            uint32_t p = m.arg(0); // OSVERSIONINFO: keep dwOSVersionInfoSize
            m.write32(p + 4, 5);   // dwMajorVersion
            m.write32(p + 8, 1);   // dwMinorVersion
            m.write32(p + 12, 2600); // dwBuildNumber
            m.write32(p + 16, 2);  // dwPlatformId = VER_PLATFORM_WIN32_NT
            m.ret(1, 1);
            return true;
        }
        if (name == "GetStartupInfoA" || name == "GetStartupInfoW") {
            uint32_t p = m.arg(0); // zero STARTUPINFO (68 bytes), set cb
            for (uint32_t i = 0; i < 68; i += 4) m.write32(p + i, 0);
            m.write32(p, 68);
            m.ret(1, 0);
            return true;
        }

        // ---- error / misc ----
        if (name == "SetLastError") { g_k.last_error = m.arg(0); m.ret(1, 0); return true; }
        if (name == "GetLastError") { m.ret(0, g_k.last_error); return true; }
        if (name == "SetUnhandledExceptionFilter") { m.ret(1, 0); return true; }
        if (name == "OutputDebugStringA") {
            fprintf(stderr, "[guest] %s", m.read_cstr(m.arg(0)).c_str());
            m.ret(1, 0);
            return true;
        }
        if (name == "IsProcessorFeaturePresent") { m.ret(1, 1); return true; }
        if (name == "GetACP") { m.ret(0, 1252); return true; }
        if (name == "IsDebuggerPresent") { m.ret(0, 0); return true; }
        if (name == "GetSystemInfo") {
            uint32_t p = m.arg(0); // zero SYSTEM_INFO, set a plausible page size
            for (uint32_t i = 0; i < 36; i += 4) m.write32(p + i, 0);
            m.write32(p + 4, 0x1000);  // dwPageSize
            m.write32(p + 20, 1);      // dwNumberOfProcessors
            m.ret(1, 0);
            return true;
        }
        if (name == "lstrlenA") {
            m.ret(1, (uint32_t)m.read_cstr(m.arg(0)).size());
            return true;
        }
        return false;
    });
}

} // namespace k32web
