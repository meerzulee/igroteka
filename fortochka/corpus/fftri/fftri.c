/* fftri.exe — corpus rung: a 3D triangle through the fixed-function transform.
 * Untransformed D3DFVF_XYZ vertices (model space) go through
 * World*View*Projection + the viewport map to reach the screen — the pipeline
 * real games use (not the pre-transformed XYZRHW shortcut). World/View are
 * identity, Projection scales by 0.8, so a correct pipeline yields a centered
 * triangle a bit smaller than the viewport.
 */
#include <windows.h>
#include <d3d9.h>

struct Vtx {
    float x, y, z; /* D3DFVF_XYZ (model space) */
    DWORD color;   /* D3DFVF_DIFFUSE */
};

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;
static float g_ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static float g_proj[16] = {0.8f, 0, 0, 0, 0, 0.8f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static struct Vtx g_tri[3] = {
    {0.0f, 0.8f, 0.0f, 0x00FF3030},   /* top */
    {0.8f, -0.8f, 0.0f, 0x0030FF30},  /* right */
    {-0.8f, -0.8f, 0.0f, 0x003030FF}, /* left */
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
    g_wc.lpszClassName = "fortochka_ff";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_ff", "ff", 0, 0, 0, 640, 480,
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
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, (const D3DMATRIX *)g_ident);
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, (const D3DMATRIX *)g_ident);
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, (const D3DMATRIX *)g_proj);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, g_tri,
                                     sizeof(struct Vtx));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    ExitProcess(0);
}
