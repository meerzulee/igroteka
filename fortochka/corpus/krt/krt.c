/* krt.exe — corpus rung: the CRT-startup kernel32 surface. Exercises the
 * functions a real game's C runtime hits before main: heap, VirtualAlloc, TLS,
 * timing, module/version. Each result feeds a checksum written to stdout via
 * WriteFile so the harness can assert the whole batch worked.
 */
#include <windows.h>

void start(void)
{
    HANDLE heap;
    void *p;
    DWORD tls;
    LARGE_INTEGER freq;
    int ok = 1;

    /* Heap: allocate, write, read back. */
    heap = GetProcessHeap();
    p = HeapAlloc(heap, HEAP_ZERO_MEMORY, 256);
    if (!p) ok = 0;
    ((int *)p)[0] = 0x1234;
    ((int *)p)[10] = 0x5678;
    if (((int *)p)[0] != 0x1234 || ((int *)p)[10] != 0x5678) ok = 0;
    if (((int *)p)[5] != 0) ok = 0; /* HEAP_ZERO_MEMORY */
    HeapFree(heap, 0, p);

    /* VirtualAlloc a page and use it. */
    p = VirtualAlloc((void *)0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) ok = 0;
    ((int *)p)[100] = 0x9abc;
    if (((int *)p)[100] != 0x9abc) ok = 0;

    /* TLS round trip. */
    tls = TlsAlloc();
    if (tls == 0xFFFFFFFF) ok = 0;
    TlsSetValue(tls, (void *)0xC0FFEE);
    if (TlsGetValue(tls) != (void *)0xC0FFEE) ok = 0;
    TlsFree(tls);

    /* Timing + version sanity. */
    if (!QueryPerformanceFrequency(&freq)) ok = 0;
    if (GetModuleHandleA((const char *)0) != (HMODULE)0x400000) ok = 0;
    if ((GetVersion() & 0xFF) < 5) ok = 0; /* major >= 5 */
    if (GetCurrentThreadId() == 0) ok = 0;

    ExitProcess(ok ? 42 : 1);
}
