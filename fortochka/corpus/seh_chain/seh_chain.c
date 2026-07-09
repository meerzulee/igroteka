/* seh_chain.exe — proves the fs:[0] WALK across properly nested frames.
 *
 * Real SEH registration records sit at strictly increasing stack addresses as
 * the chain is walked outward (a deeper call frame installs a lower-addressed
 * record). We mirror that: start() installs the OUTER handler, then calls
 * raise_it() which installs the INNER handler in its own deeper frame and
 * raises. The dispatcher walks inner (lower addr, called first) → outer (higher
 * addr). The inner returns ExceptionContinueSearch, so the walk must advance to
 * the outer, which handles it. Exit 42 iff both ran in order with the right
 * code — validating both chain traversal and the frame-ordering rule.
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
    return ExceptionContinueSearch; /* 1: not mine, keep walking outward */
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

static void unlink_frame(SEH_FRAME *f)
{
    __asm__ volatile("movl %0, %%fs:0" ::"r"(f->prev) : "memory");
}

/* Deeper frame: its registration record is at a lower stack address than
 * start()'s, so it is walked first. noinline keeps it a real call frame — under
 * -Os an inlined body would put both records in start()'s frame at arbitrary
 * order, which the dispatcher's frame-ordering rule (correctly) rejects. */
__attribute__((noinline)) static void raise_it(void)
{
    SEH_FRAME inner;
    install(&inner, (void *)inner_handler);
    RaiseException(0x1234, 0, 0, (const ULONG_PTR *)0);
    unlink_frame(&inner);
}

void start(void)
{
    SEH_FRAME outer;

    install(&outer, (void *)outer_handler);
    raise_it();
    unlink_frame(&outer);

    /* inner ran (1) AND outer ran (2) AND code seen == 0x1234 → 42. */
    ExitProcess((g_order == 3 && g_code == 0x1234) ? 42 : g_order);
}
