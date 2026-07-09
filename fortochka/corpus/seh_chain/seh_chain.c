/* seh_chain.exe — proves the fs:[0] WALK, not just a single handler.
 *
 * Installs two SEH frames. The inner one (installed last, called first) returns
 * ExceptionContinueSearch, so the dispatcher must advance down the chain to the
 * outer handler, which handles it. Exit code encodes the order the handlers
 * ran: inner sets bit 1, outer sets bit 2 and records the code. A correct walk
 * exits 0x1234-derived 42; a dispatcher that stops at the first frame or
 * mis-links the chain yields something else.
 */
#include <windows.h>

typedef struct SEH_FRAME {
    struct SEH_FRAME *prev;
    void *handler;
} SEH_FRAME;

static volatile long g_order = 0; /* bit0 inner ran, bit1 outer ran */
static volatile long g_code = 0;

static EXCEPTION_DISPOSITION __cdecl
inner_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp)
{
    (void)rec; (void)frame; (void)ctx; (void)disp;
    g_order |= 1;
    return ExceptionContinueSearch; /* 1: not mine, keep walking */
}

static EXCEPTION_DISPOSITION __cdecl
outer_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp)
{
    (void)frame; (void)ctx; (void)disp;
    g_order |= 2;
    g_code = (long)rec->ExceptionCode;
    return ExceptionContinueExecution; /* 0: handled, resume */
}

static void install(SEH_FRAME *f, void *handler)
{
    void *prev;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(prev));
    f->prev = (SEH_FRAME *)prev;
    f->handler = handler;
    __asm__ volatile("movl %0, %%fs:0" ::"r"(f) : "memory");
}

void start(void)
{
    SEH_FRAME outer, inner;

    install(&outer, (void *)outer_handler);
    install(&inner, (void *)inner_handler); /* inner is now fs:[0] head */

    RaiseException(0x1234, 0, 0, (const ULONG_PTR *)0);

    __asm__ volatile("movl %0, %%fs:0" ::"r"(outer.prev) : "memory");

    /* inner ran (1) AND outer ran (2) AND code seen == 0x1234 → 42. */
    ExitProcess((g_order == 3 && g_code == 0x1234) ? 42 : g_order);
}
