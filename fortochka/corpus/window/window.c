/* window.exe — corpus rung 2: the reverse-thunk round trip.
 *
 * Registers a window class, creates a window, and runs a real message pump.
 * DispatchMessageA must re-enter THIS binary's WndProc (host→guest reverse
 * thunk) for each message. WndProc accumulates WM_USER wParams into g_acc and,
 * on WM_CLOSE, posts a quit with the accumulator as exit code. The harness
 * seeds the queue with WM_USER(10), WM_USER(20), WM_CLOSE, so a correct pump
 * exits with code 30 — provable only if the guest WndProc actually ran.
 *
 * F2 exit criterion binary. No CRT: static storage avoids a memset import.
 */
#include <windows.h>

#define WM_USER_ADD (WM_USER + 1)

static long g_acc = 0;
static WNDCLASSA g_wc; /* static => zero-initialized, no memset */

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_USER_ADD) {
        g_acc += (long)wp;
        return 0;
    }
    if (msg == WM_CLOSE) {
        PostQuitMessage((int)g_acc);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

void start(void)
{
    MSG msg;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_window";
    RegisterClassA(&g_wc);

    CreateWindowExA(0, "fortochka_window", "corpus", 0, 0, 0, 0, 0,
                    (HWND)0, (HMENU)0, (HINSTANCE)0, (LPVOID)0);

    while (GetMessageA(&msg, (HWND)0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    ExitProcess((UINT)g_acc);
}
