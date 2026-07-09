/* tri.exe — corpus rung: a real Direct3D 9 triangle. Pre-transformed vertices
 * (D3DFVF_XYZRHW so no matrix/viewport transform needed) with per-vertex color,
 * drawn via DrawPrimitiveUP. Exercises SetFVF + DrawPrimitiveUP → the d9web
 * software rasterizer → backbuffer → canvas.
 */
#include <windows.h>
#include <d3d9.h>

struct Vtx {
    float x, y, z, rhw;
    DWORD color; /* D3DCOLOR 0xAARRGGBB */
};

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;
static struct Vtx g_tri[3] = {
    {320.0f, 80.0f, 0.5f, 1.0f, 0x00FF3030},  /* top,    red   */
    {560.0f, 400.0f, 0.5f, 1.0f, 0x0030FF30}, /* right,  green */
    {80.0f, 400.0f, 0.5f, 1.0f, 0x003030FF},  /* left,   blue  */
};

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
    g_wc.lpszClassName = "fortochka_tri";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_tri", "tri", 0, 0, 0, 640, 480,
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
                           0x00202838, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, g_tri,
                                     sizeof(struct Vtx));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    ExitProcess(0);
}
