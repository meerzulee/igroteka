/* recurse.exe — adversarial rung: a WndProc that re-enters itself without
 * bound via SendMessage. Validates the reverse-thunk depth guard: the runtime
 * must abort cleanly (bounded host recursion), never overflow the native stack.
 *
 * On the first message, WndProc SendMessages itself another message → the host
 * reverse-thunks in again → repeat. A correct runtime caps the nesting and
 * raises a MachineError; window_test asserts a graceful error, not a crash.
 */
#include <windows.h>

#define WM_SELF (WM_USER + 7)

static WNDCLASSA g_wc;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SELF) {
        /* Unbounded self re-entry: each call nests one more host frame. */
        return SendMessageA(h, WM_SELF, wp + 1, 0);
    }
    return DefWindowProcA(h, msg, wp, lp);
}

void start(void)
{
    MSG msg;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_recurse";
    RegisterClassA(&g_wc);
    CreateWindowExA(0, "fortochka_recurse", "r", 0, 0, 0, 0, 0,
                    (HWND)0, (HMENU)0, (HINSTANCE)0, (LPVOID)0);

    while (GetMessageA(&msg, (HWND)0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    ExitProcess(0);
}
