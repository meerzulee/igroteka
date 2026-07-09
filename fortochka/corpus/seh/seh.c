/* seh.exe — corpus rung: the fs:[0] structured-exception round trip.
 *
 * MinGW/GCC on i686 uses DWARF/SjLj EH, not Win32 fs:[0] SEH, so __try/__except
 * would not exercise our dispatcher. Instead we hand-roll the OS-level
 * mechanism: install an EXCEPTION_REGISTRATION_RECORD on the stack, point
 * fs:[0] at it, and RaiseException. The runtime's dispatcher must walk fs:[0]
 * and reverse-thunk into WndProc-style guest handler code, exactly the path a
 * real SEH-based CRT and C++ throw travel.
 *
 * The handler records the raised code and returns ExceptionContinueExecution,
 * so RaiseException returns and start() exits 42 iff the handler saw 0x1234 —
 * provable only if host walked the chain and re-entered the guest handler.
 */
#include <windows.h>

typedef struct SEH_FRAME {
    struct SEH_FRAME *prev; /* offset 0: next record (0xFFFFFFFF = end) */
    void *handler;          /* offset 4: EXCEPTION_HANDLER */
} SEH_FRAME;

static volatile long g_code = 0;

static EXCEPTION_DISPOSITION __cdecl
seh_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp)
{
    (void)frame; (void)ctx; (void)disp;
    g_code = (long)rec->ExceptionCode;
    return ExceptionContinueExecution; /* 0: stop the walk, resume */
}

void start(void)
{
    SEH_FRAME frame;
    void *prev;

    __asm__ volatile("movl %%fs:0, %0" : "=r"(prev));
    frame.prev = (SEH_FRAME *)prev;
    frame.handler = (void *)seh_handler;
    __asm__ volatile("movl %0, %%fs:0" ::"r"(&frame) : "memory");

    RaiseException(0x1234, 0, 0, (const ULONG_PTR *)0);

    /* Unlink our frame before leaving. */
    __asm__ volatile("movl %0, %%fs:0" ::"r"(frame.prev) : "memory");

    ExitProcess(g_code == 0x1234 ? 42 : 1);
}
