/* threads.exe — corpus rung: the cooperative thread scheduler (RTW batch 8).
 * Exercises the load-bearing paths: a worker thread + an auto-reset event
 * handoff (main blocks, scheduler runs the worker, worker signals, main wakes),
 * thread exit codes, a mutex handed between threads, and — the reason
 * preemption exists — a spinner that busy-waits on a memory flag with NO wait
 * API, so only the preemptive time-slice lets the main thread run and release
 * it. Exit 42 = pass.
 */
#include <windows.h>

static volatile LONG g_flag;
static volatile LONG g_spin;
static HANDLE g_mtx;
static volatile LONG g_mtx_saw;

static DWORD WINAPI worker(LPVOID p)
{
    g_flag = 0x1234;
    SetEvent((HANDLE)p); /* wake the main thread */
    return 7;
}

static DWORD WINAPI spinner(LPVOID p)
{
    (void)p;
    while (g_spin == 0) { /* busy-wait: no wait API → needs preemption */ }
    return 9;
}

static DWORD WINAPI mutex_worker(LPVOID p)
{
    (void)p;
    /* Blocks until the main thread releases the mutex, then records that it
     * acquired it. */
    if (WaitForSingleObject(g_mtx, INFINITE) != WAIT_OBJECT_0) return 0;
    g_mtx_saw = 0x55;
    ReleaseMutex(g_mtx);
    return 1;
}

void start(void)
{
    HANDLE ev, t1, t2, t3;
    DWORD code;

    /* 1. Event handoff. */
    ev = CreateEventA(NULL, FALSE, FALSE, NULL); /* auto-reset, unsignaled */
    if (!ev) ExitProcess(11);
    t1 = CreateThread(NULL, 0, worker, ev, 0, NULL);
    if (!t1) ExitProcess(12);
    if (WaitForSingleObject(ev, INFINITE) != WAIT_OBJECT_0) ExitProcess(13);
    if (g_flag != 0x1234) ExitProcess(14);
    if (WaitForSingleObject(t1, INFINITE) != WAIT_OBJECT_0) ExitProcess(15);
    if (!GetExitCodeThread(t1, &code) || code != 7) ExitProcess(16);

    /* 2. Busy-wait preemption. */
    t2 = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
    if (!t2) ExitProcess(17);
    g_spin = 1; /* the spinner exits once the scheduler slices it back in */
    if (WaitForSingleObject(t2, INFINITE) != WAIT_OBJECT_0) ExitProcess(18);
    if (!GetExitCodeThread(t2, &code) || code != 9) ExitProcess(19);

    /* 3. Mutex handed across threads: main owns it, worker blocks, main
     * releases, worker acquires. */
    g_mtx = CreateMutexA(NULL, TRUE, NULL); /* main owns it initially */
    if (!g_mtx) ExitProcess(20);
    t3 = CreateThread(NULL, 0, mutex_worker, NULL, 0, NULL);
    if (!t3) ExitProcess(21);
    if (g_mtx_saw != 0) ExitProcess(22); /* worker must still be blocked */
    ReleaseMutex(g_mtx);
    if (WaitForSingleObject(t3, INFINITE) != WAIT_OBJECT_0) ExitProcess(23);
    if (g_mtx_saw != 0x55) ExitProcess(24);
    if (!GetExitCodeThread(t3, &code) || code != 1) ExitProcess(25);

    ExitProcess(42);
}
