/* winmgr.exe — corpus rung: the window-management surface a game touches during
 * startup. Exercises GetSystemMetrics, GetWindowLong/SetWindowLong (including
 * GWL_WNDPROC subclassing, which must redirect where SendMessage dispatches),
 * AdjustWindowRect, and LoadCursor. Distinct-per-step exit codes pinpoint any
 * failure; 42 means all passed.
 */
#include <windows.h>

static WNDCLASSA wc;

static LRESULT CALLBACK Proc1(HWND h, UINT m, WPARAM w, LPARAM l) { return 0x1111; }
static LRESULT CALLBACK Proc2(HWND h, UINT m, WPARAM w, LPARAM l) { return 0x2222; }

void start(void)
{
    HWND hwnd;
    LONG prev;
    RECT r;

    wc.lpfnWndProc = Proc1;
    wc.lpszClassName = "winmgr";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "winmgr", "t", 0, 0, 0, 640, 480, (HWND)0, (HMENU)0,
                           (HINSTANCE)0, (LPVOID)0);

    if (GetSystemMetrics(SM_CXSCREEN) != 1024) ExitProcess(11);
    if (GetSystemMetrics(SM_CYSCREEN) != 768) ExitProcess(12);

    SetWindowLongA(hwnd, GWL_USERDATA, 0xABCD);
    if ((DWORD)GetWindowLongA(hwnd, GWL_USERDATA) != 0xABCD) ExitProcess(13);

    /* The registered WndProc (Proc1) handles messages first. */
    if (SendMessageA(hwnd, WM_USER, 0, 0) != 0x1111) ExitProcess(14);

    /* Subclass via GWL_WNDPROC: SetWindowLong returns the old proc, and a
     * subsequent SendMessage must now reach Proc2. */
    prev = SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)(INT_PTR)Proc2);
    if ((WNDPROC)(INT_PTR)prev != Proc1) ExitProcess(15);
    if (SendMessageA(hwnd, WM_USER, 0, 0) != 0x2222) ExitProcess(16);

    r.left = 0; r.top = 0; r.right = 640; r.bottom = 480;
    if (!AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE)) ExitProcess(17);

    if (!LoadCursorA((HINSTANCE)0, IDC_ARROW)) ExitProcess(18);

    ExitProcess(42);
}
