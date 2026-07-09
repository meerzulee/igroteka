/* d3dclear.exe — corpus rung: a real Direct3D 9 program. Creates a device and
 * Clears the backbuffer to a color, then Presents. Exercises the COM path end to
 * end: Direct3DCreate9 (import) → IDirect3D9::CreateDevice (vtable thunk) →
 * IDirect3DDevice9::Clear/Present (vtable thunks) → software backbuffer → canvas.
 *
 * Uses the d3d9.h C COM macros (IDirect3D9_CreateDevice = obj->lpVtbl->...),
 * so the guest issues genuine indirect vtable calls the runtime must dispatch.
 * Static structs avoid a memset import under -nostdlib.
 */
#include <windows.h>
#include <d3d9.h>

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

void start(void)
{
    HWND hwnd;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = 0;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_d3d";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_d3d", "d3d", 0, 0, 0, 640, 480,
                           (HWND)0, (HMENU)0, (HINSTANCE)0, (LPVOID)0);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);

    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferWidth = 640;
    g_pp.BackBufferHeight = 480;
    g_pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    g_pp.hDeviceWindow = hwnd;

    IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd,
                            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &dev);

    IDirect3DDevice9_Clear(dev, 0, (const D3DRECT *)0, D3DCLEAR_TARGET,
                           0x00336699, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    ExitProcess(0);
}
