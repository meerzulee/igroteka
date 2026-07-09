/* vb_evil.exe — adversarial rung: CreateVertexBuffer with a near-UINT32_MAX
 * length. The runtime's guest heap allocator must reject it cleanly (the 8-align
 * must not wrap uint32 and silently under-allocate). A clean MachineError is the
 * pass; the harness asserts the runtime aborts rather than corrupting.
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
    IDirect3DVertexBuffer9 *vb = 0;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_vbevil";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_vbevil", "vbe", 0, 0, 0, 640, 480,
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

    /* Absurd length — must be rejected, not wrapped to a tiny allocation. */
    IDirect3DDevice9_CreateVertexBuffer(dev, 0xFFFFFFFF, 0,
                                        D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                        D3DPOOL_DEFAULT, &vb, (HANDLE *)0);
    ExitProcess(0); /* unreachable: allocator must have aborted */
}
