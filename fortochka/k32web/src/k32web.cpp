#include "k32web/k32web.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

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
    // Interned DLL names for LoadLibrary/GetModuleHandle; the handle is
    // MODULE_TAG + table index, so GetProcAddress can scope a lookup to the
    // module the guest actually asked about.
    std::vector<std::string> modules;
    // In-memory VFS: files a game reads/writes. In the browser this is backed
    // by OPFS; the native harness keeps it in host memory and a guest can
    // populate it itself (write then read back). Paths normalized lowercase.
    struct VFile {
        std::string name;
        std::vector<uint8_t> data;
    };
    std::vector<VFile> vfs;
    // Open file handles: handle = FILE_TAG + index; index into open_files.
    // vfs_index < 0 marks a closed slot (never index-reused in tier 0).
    struct OpenFile {
        int vfs_index = -1;
        uint64_t pos = 0;
    };
    std::vector<OpenFile> open_files;
};
K32 g_k;

// Handle tag for open files, distinct from the module tag and every pseudo-
// handle the runtime hands out. Index stays small, so no range overlap.
constexpr uint32_t FILE_TAG = 0x50000000u;
constexpr uint32_t INVALID_HANDLE = 0xFFFFFFFFu;
// Cap a single file's size and a single read/write span so a guest can't force
// an unbounded host allocation or copy. Also cap the file and open-handle counts
// so a guest looping CreateFileA over distinct names can't grow host memory
// without bound (tier-0 never reclaims slots).
constexpr uint32_t MAX_FILE_BYTES = 64u << 20;
constexpr size_t MAX_FILES = 4096;
constexpr size_t MAX_OPEN_FILES = 4096;

// Normalize a VFS path: lowercase, backslashes → forward slashes.
std::string norm_path(std::string s) {
    for (auto& c : s) {
        c = (char)std::tolower((unsigned char)c);
        if (c == '\\') c = '/';
    }
    return s;
}
int vfs_find(const std::string& name) {
    std::string n = norm_path(name);
    for (size_t i = 0; i < g_k.vfs.size(); i++)
        if (g_k.vfs[i].name == n) return (int)i;
    return -1;
}
int vfs_create(const std::string& name) {
    if (g_k.vfs.size() >= MAX_FILES) return -1; // cap reached
    g_k.vfs.push_back({norm_path(name), {}});
    return (int)g_k.vfs.size() - 1;
}
// Open a VFS file index as a new handle, or INVALID_HANDLE at the handle cap.
uint32_t open_file_handle(int vfs_index) {
    if (vfs_index < 0 || g_k.open_files.size() >= MAX_OPEN_FILES)
        return INVALID_HANDLE;
    g_k.open_files.push_back({vfs_index, 0});
    return FILE_TAG + (uint32_t)(g_k.open_files.size() - 1);
}
// If `h` is a live file handle, return its OpenFile*, else nullptr.
K32::OpenFile* as_open_file(uint32_t h) {
    if (h < FILE_TAG || (h - FILE_TAG) >= g_k.open_files.size()) return nullptr;
    K32::OpenFile* of = &g_k.open_files[h - FILE_TAG];
    return of->vfs_index >= 0 ? of : nullptr;
}
// Copy `n` bytes from host `src` into guest memory at `dst`, bounds-checked per
// byte against the arena (like write32) so a bogus dst is a no-op, not a host
// out-of-bounds write. Returns bytes actually written in-range.
uint32_t guest_write_bytes(Machine& m, uint32_t dst, const uint8_t* src, uint32_t n) {
    uint8_t* base = m.mem();
    uint32_t sz = m.mem_size();
    uint32_t wrote = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t d = (uint64_t)dst + i;
        if (d >= sz) break;
        base[d] = src[i];
        wrote++;
    }
    return wrote;
}

// Synthetic module-handle base. Distinct from the image base (0x400000); index
// stays small so no overlap with the FILE_TAG range or high pseudo-handles.
constexpr uint32_t MODULE_TAG = 0x40000000u;
// Cap the intern table so a guest looping LoadLibrary over distinct names can't
// grow host memory without bound; past the cap we hand back the image base,
// which scopes GetProcAddress to name-only (safe, just unscoped).
constexpr size_t MAX_MODULES = 4096;

// Read a guest UTF-16LE string as narrow ASCII (DLL names/paths are ASCII).
// read32 is bounds-checked (0 out of range), so a bogus pointer just terminates.
std::string read_wstr_narrow(Machine& m, uint32_t p, uint32_t max = 4096) {
    std::string s;
    for (uint32_t i = 0; i < max; i++) {
        uint32_t w = m.read32(p + i * 2) & 0xFFFF; // low 16 bits = the wchar
        if (w == 0) break;
        s.push_back((char)(w & 0xFF));
    }
    return s;
}

// Intern a DLL name (normalized: lowercase, no trailing ".dll") → module handle.
uint32_t module_handle(const std::string& raw) {
    std::string name = raw;
    for (auto& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0)
        name.resize(name.size() - 4);
    for (size_t i = 0; i < g_k.modules.size(); i++)
        if (g_k.modules[i] == name) return MODULE_TAG + (uint32_t)i;
    if (g_k.modules.size() >= MAX_MODULES) return 0x00400000; // cap: name-only scope
    g_k.modules.push_back(name);
    return MODULE_TAG + (uint32_t)(g_k.modules.size() - 1);
}

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
            if (K32::OpenFile* of = as_open_file(h)) {
                // Write into the VFS file at the current position (extending it),
                // capping total size so a guest can't force an unbounded alloc.
                auto& data = g_k.vfs[of->vfs_index].data;
                uint64_t end = of->pos + len;
                if (end > MAX_FILE_BYTES) { m.ret(5, 0); return true; }
                if (data.size() < end) data.resize((size_t)end, 0);
                for (uint32_t i = 0; i < len; i++)
                    data[(size_t)of->pos + i] = m.mem()[buf + i];
                of->pos = end;
                if (p_written) m.write32(p_written, len);
                m.ret(5, 1);
                return true;
            }
            FILE* out = h == H_STDERR ? stderr : stdout;
            std::fwrite(m.mem() + buf, 1, len, out); // exact bytes, NULs included
            std::fflush(out);
            if (p_written) m.write32(p_written, len);
            m.ret(5, 1);
            return true;
        }
        if (name == "CreateFileA") {
            // CreateFileA(name, access, share, sec, disposition, flags, template).
            // disposition: CREATE_NEW=1, CREATE_ALWAYS=2, OPEN_EXISTING=3,
            // OPEN_ALWAYS=4, TRUNCATE_EXISTING=5. Returns a handle or
            // INVALID_HANDLE_VALUE. lpFileName is arg0.
            std::string path = m.read_cstr(m.arg(0));
            uint32_t disp = m.arg(4);
            int idx = vfs_find(path);
            bool exists = idx >= 0;
            // Decide the disposition's intent, then open once. Splitting the
            // "create" step out keeps a cap-hit vfs_create()==-1 from ever
            // indexing g_k.vfs[-1] (the truncate happens only for a valid idx).
            bool open_it = false, create = false, trunc = false;
            switch (disp) {
                case 1: /*CREATE_NEW*/        open_it = create = !exists; break;
                case 2: /*CREATE_ALWAYS*/     open_it = true; create = !exists; trunc = true; break;
                case 3: /*OPEN_EXISTING*/     open_it = exists; break;
                case 4: /*OPEN_ALWAYS*/       open_it = true; create = !exists; break;
                case 5: /*TRUNCATE_EXISTING*/ open_it = trunc = exists; break;
                default:                      open_it = exists; break;
            }
            uint32_t handle = INVALID_HANDLE;
            if (open_it) {
                if (create) idx = vfs_create(path); // -1 if the file cap is hit
                if (idx >= 0) {
                    if (trunc) g_k.vfs[idx].data.clear();
                    handle = open_file_handle(idx); // INVALID if the handle cap is hit
                }
            }
            if (handle == INVALID_HANDLE) g_k.last_error = 2; // ERROR_FILE_NOT_FOUND
            m.ret(7, handle);
            return true;
        }
        if (name == "ReadFile") {
            // ReadFile(hFile, lpBuffer, nBytes, lpBytesRead, lpOverlapped).
            uint32_t h = m.arg(0), buf = m.arg(1), len = m.arg(2), p_read = m.arg(3);
            K32::OpenFile* of = as_open_file(h);
            if (!of) { // console/unknown handle: 0 bytes read, still success
                if (p_read) m.write32(p_read, 0);
                m.ret(5, 1);
                return true;
            }
            const auto& data = g_k.vfs[of->vfs_index].data;
            // Clamp the read cursor to the data end BEFORE forming the source
            // pointer: data.data()+pos with pos>size (a seek past EOF) is UB even
            // when the copy length is 0.
            uint64_t p = of->pos < data.size() ? of->pos : data.size();
            uint32_t avail = (uint32_t)(data.size() - p);
            uint32_t want = len < avail ? len : avail;
            uint32_t got = guest_write_bytes(m, buf, data.data() + p, want);
            of->pos += got;
            if (p_read) m.write32(p_read, got);
            m.ret(5, 1);
            return true;
        }
        if (name == "SetFilePointer") {
            // SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh,
            // dwMoveMethod). FILE_BEGIN=0, FILE_CURRENT=1, FILE_END=2.
            uint32_t h = m.arg(0);
            int32_t dist = (int32_t)m.arg(1);
            uint32_t method = m.arg(3);
            K32::OpenFile* of = as_open_file(h);
            if (!of) { m.ret(4, INVALID_HANDLE); return true; }
            int64_t base = method == 1 ? (int64_t)of->pos
                         : method == 2 ? (int64_t)g_k.vfs[of->vfs_index].data.size()
                                       : 0; // FILE_BEGIN
            int64_t np = base + dist; // base is capped ≤ MAX_FILE_BYTES, so no i64 overflow
            // Reject out-of-range targets. Capping pos at MAX_FILE_BYTES keeps
            // base bounded for the next call (no eventual signed overflow) and
            // matches WriteFile, which rejects any write past that size anyway.
            if (np < 0 || np > (int64_t)MAX_FILE_BYTES) {
                g_k.last_error = 131; // ERROR_NEGATIVE_SEEK
                m.ret(4, INVALID_HANDLE);
                return true;
            }
            of->pos = (uint64_t)np;
            m.ret(4, (uint32_t)of->pos);
            return true;
        }
        if (name == "GetFileSize") {
            // GetFileSize(hFile, lpFileSizeHigh) → low 32 bits (high via ptr).
            uint32_t h = m.arg(0), p_high = m.arg(1);
            K32::OpenFile* of = as_open_file(h);
            if (!of) { m.ret(2, INVALID_HANDLE); return true; }
            uint64_t sz = g_k.vfs[of->vfs_index].data.size();
            if (p_high) m.write32(p_high, (uint32_t)(sz >> 32));
            m.ret(2, (uint32_t)sz);
            return true;
        }
        if (name == "GetFileAttributesA") {
            // 0x80 FILE_ATTRIBUTE_NORMAL if present, else INVALID_FILE_ATTRIBUTES.
            std::string path = m.read_cstr(m.arg(0));
            m.ret(1, vfs_find(path) >= 0 ? 0x80 : INVALID_HANDLE);
            return true;
        }
        if (name == "CloseHandle") {
            // Free a file handle if that's what this is; other handle kinds
            // (heap, pseudo-handles) just succeed.
            if (K32::OpenFile* of = as_open_file(m.arg(0))) of->vfs_index = -1;
            m.ret(1, 1);
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
            if (old) // copy old contents (dword steps); no old-size metadata yet
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
        if (name == "VirtualFree") { m.ret(3, 1); return true; }
        if (name == "VirtualProtect") {
            // VirtualProtect(addr, size, newProtect, lpflOldProtect).
            if (m.arg(3)) m.write32(m.arg(3), 4 /*PAGE_READWRITE*/);
            m.ret(4, 1);
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
        if (name == "GetModuleHandleA") {
            // NULL name = the process image itself (base 0x400000); a named
            // module interns to a distinct handle carrying its DLL identity.
            uint32_t p = m.arg(0);
            m.ret(1, p == 0 ? 0x00400000 : module_handle(m.read_cstr(p)));
            return true;
        }
        if (name == "GetModuleHandleW") {
            // Decode the wide name so W lookups intern and scope like A lookups.
            uint32_t p = m.arg(0);
            m.ret(1, p == 0 ? 0x00400000 : module_handle(read_wstr_narrow(m, p)));
            return true;
        }
        if (name == "GetProcAddress") {
            // GetProcAddress(hModule, lpProcName). An ordinal request (name ptr
            // with a zero high word) is unsupported → 0. With a real interned
            // module handle, scope the name to that DLL so a cross-DLL name
            // collision can't resolve to the wrong ABI's thunk; the image base
            // or an unknown handle falls back to a name-only match. 0 = "not
            // found" so the guest gracefully skips the optional API.
            uint32_t hmod = m.arg(0), proc = m.arg(1), addr = 0;
            if ((proc >> 16) != 0) {
                std::string pname = m.read_cstr(proc);
                if (hmod >= MODULE_TAG && (hmod - MODULE_TAG) < g_k.modules.size())
                    addr = m.resolve_proc(g_k.modules[hmod - MODULE_TAG], pname);
                else
                    addr = m.resolve_proc(pname);
            }
            m.ret(2, addr);
            return true;
        }
        if (name == "LoadLibraryA" || name == "LoadLibraryExA") {
            // Return an interned handle for the named DLL (arg0 for both; ExA's
            // extra args are hFile/flags). GetProcAddress then scopes to it.
            uint32_t p = m.arg(0);
            uint32_t h = p ? module_handle(m.read_cstr(p)) : 0x00400000;
            m.ret(name == "LoadLibraryExA" ? 3 : 1, h);
            return true;
        }
        if (name == "LoadLibraryW" || name == "LoadLibraryExW") {
            // Decode + intern the wide name so W loads scope like A loads.
            uint32_t p = m.arg(0);
            uint32_t h = p ? module_handle(read_wstr_narrow(m, p)) : 0x00400000;
            m.ret(name == "LoadLibraryExW" ? 3 : 1, h);
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
        if (name == "GetCurrentProcess") { m.ret(0, 0xFFFFFFFFu); return true; }
        if (name == "GetCurrentThread") { m.ret(0, 0xFFFFFFFEu); return true; } // (HANDLE)-2
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
            for (uint32_t i = 20; i < 20 + 128; i += 4) m.write32(p + i, 0); // szCSDVersion
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
        if (name == "IsProcessorFeaturePresent") {
            // Report ONLY what zhelezo implements, so a game doesn't select a
            // path we can't run (e.g. SSE2). 2=CMPXCHG_DOUBLE, 6=XMMI(SSE1),
            // 8=RDTSC, 23=CMPXCHG16B(no). SSE2(10)+ report false.
            uint32_t f = m.arg(0);
            m.ret(1, (f == 2 || f == 6 || f == 8) ? 1 : 0);
            return true;
        }
        if (name == "GetACP") { m.ret(0, 1252); return true; }
        if (name == "IsDebuggerPresent") { m.ret(0, 0); return true; }
        if (name == "GetSystemInfo") {
            uint32_t p = m.arg(0); // zero SYSTEM_INFO, set a plausible page size
            for (uint32_t i = 0; i < 36; i += 4) m.write32(p + i, 0);
            m.write32(p + 4, 0x1000);    // dwPageSize
            m.write32(p + 16, 1);        // dwActiveProcessorMask
            m.write32(p + 20, 1);        // dwNumberOfProcessors
            m.write32(p + 28, 0x10000);  // dwAllocationGranularity (64 KB)
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
