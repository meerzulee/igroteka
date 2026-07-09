/* paint.exe — corpus rung: guest paints the client area through GDI. On WM_PAINT
 * it fills the background (FillRect) and draws a 256x128 RGB gradient pixel by
 * pixel (SetPixel), then posts quit. The runtime's framebuffer must hold the
 * result — proving guest->host GDI reaches real pixels a canvas can show.
 */
#include <windows.h>

#define WM_PAINT 0x000F

static WNDCLASSA g_wc;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT rc;
        HBRUSH bg;
        int x, y;
        HDC hdc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        bg = CreateSolidBrush(RGB(20, 20, 40));
        FillRect(hdc, &rc, bg);
        for (y = 0; y < 128; y++)
            for (x = 0; x < 256; x++)
                SetPixel(hdc, 40 + x, 40 + y, RGB(x, y * 2, 128));
        EndPaint(h, &ps);
        DeleteObject(bg);
        PostQuitMessage(7);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

void start(void)
{
    MSG msg;
    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_paint";
    RegisterClassA(&g_wc);
    CreateWindowExA(0, "fortochka_paint", "paint", 0, 0, 0, 640, 480,
                    (HWND)0, (HMENU)0, (HINSTANCE)0, (LPVOID)0);
    while (GetMessageA(&msg, (HWND)0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    ExitProcess(0);
}
