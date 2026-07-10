/* gpa.exe — corpus rung: runtime GetProcAddress resolution. lstrlenA is forced
 * into the static import table (a direct call), then looked up by name via
 * GetProcAddress and invoked THROUGH the returned pointer — exercising that the
 * returned address is a real callable hostcall thunk. A bogus name must resolve
 * to 0 (graceful "not found"). Exits 42 on success.
 */
#include <windows.h>

typedef int(WINAPI *LSTRLENA)(LPCSTR);

void start(void)
{
    HMODULE h;
    LSTRLENA p;
    int ok = 1;

    /* Direct call → lstrlenA lands in the import table so it is resolvable. */
    if (lstrlenA("ab") != 2) ok = 0;

    h = GetModuleHandleA((const char *)0);
    p = (LSTRLENA)GetProcAddress(h, "lstrlenA");
    if (!p) ok = 0;
    else if (p("hello") != 5) ok = 0; /* call through the resolved pointer */

    /* A function we do not import must come back as NULL, not a bogus thunk. */
    if (GetProcAddress(h, "NoSuchFunc_ZZZ") != (FARPROC)0) ok = 0;

    ExitProcess(ok ? 42 : 1);
}
