/* gpa.exe — corpus rung: runtime GetProcAddress resolution + module scoping.
 * lstrlenA (kernel32) and MessageBoxA (user32) are forced into the static
 * import table by direct calls, then looked up by name via GetProcAddress and,
 * for lstrlenA, invoked THROUGH the returned pointer — exercising that the
 * returned address is a real callable hostcall thunk. A bogus name resolves to
 * 0. Module scoping is checked at the boundary: a kernel32 handle resolves
 * lstrlenA but NOT MessageBoxA (a user32 name), while a user32 handle does.
 * Exits 42 on success.
 */
#include <windows.h>

typedef int(WINAPI *LSTRLENA)(LPCSTR);

void start(void)
{
    HMODULE h, hk, hu;
    LSTRLENA p;
    int ok = 1;

    /* Direct calls → both land in the import table so they are resolvable. */
    if (lstrlenA("ab") != 2) ok = 0;
    MessageBoxA((HWND)0, "x", "y", MB_OK); /* our HLE returns IDOK, no UI */

    h = GetModuleHandleA((const char *)0);
    p = (LSTRLENA)GetProcAddress(h, "lstrlenA");
    if (!p) ok = 0;
    else if (p("hello") != 5) ok = 0; /* call through the resolved pointer */

    /* A function we do not import must come back as NULL, not a bogus thunk. */
    if (GetProcAddress(h, "NoSuchFunc_ZZZ") != (FARPROC)0) ok = 0;

    /* Module scoping: MessageBoxA IS imported, but from user32 — so a kernel32
     * handle must not resolve it, while a user32 handle must. */
    hk = LoadLibraryA("kernel32.dll");
    hu = LoadLibraryA("user32.dll");
    if (!GetProcAddress(hk, "lstrlenA")) ok = 0;    /* kernel32 import via kernel32 */
    if (GetProcAddress(hk, "MessageBoxA")) ok = 0;  /* user32 name scoped out of kernel32 */
    if (!GetProcAddress(hu, "MessageBoxA")) ok = 0; /* user32 import via user32 */

    ExitProcess(ok ? 42 : 1);
}
