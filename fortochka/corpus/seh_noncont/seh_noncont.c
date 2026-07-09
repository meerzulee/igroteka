/* seh_noncont.exe — a handler that tries to CONTINUE a noncontinuable
 * exception. Windows treats that as an error (raises a new noncontinuable
 * exception); our dispatcher must reject it (MachineError), not return normally.
 * The runtime aborts, so this binary never reaches its ExitProcess — the
 * harness asserts a runtime error, not a clean exit.
 */
#include <windows.h>

#define EXCEPTION_NONCONTINUABLE 0x1

typedef struct SEH_FRAME {
    struct SEH_FRAME *prev;
    void *handler;
} SEH_FRAME;

static EXCEPTION_DISPOSITION __cdecl
bad_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp)
{
    (void)rec; (void)frame; (void)ctx; (void)disp;
    return ExceptionContinueExecution; /* illegal for a noncontinuable exc. */
}

void start(void)
{
    SEH_FRAME frame;
    void *prev;

    __asm__ volatile("movl %%fs:0, %0" : "=r"(prev));
    frame.prev = (SEH_FRAME *)prev;
    frame.handler = (void *)bad_handler;
    __asm__ volatile("movl %0, %%fs:0" ::"r"(&frame) : "memory");

    RaiseException(0x5678, EXCEPTION_NONCONTINUABLE, 0, (const ULONG_PTR *)0);

    ExitProcess(99); /* unreachable: dispatcher must have aborted */
}
