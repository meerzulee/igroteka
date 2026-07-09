/* tex.exe — corpus rung: a textured quad. CreateTexture allocates a surface,
 * LockRect hands back a raw guest pointer we fill with a checkerboard, then two
 * XYZRHW|TEX1 triangles are drawn with UV coords and the texture bound — the
 * rasterizer samples it. Exercises the full texture path: CreateTexture →
 * IDirect3DTexture9::LockRect/UnlockRect → SetTexture → UV sampling.
 */
#include <windows.h>
#include <d3d9.h>

struct Vtx {
    float x, y, z, rhw;
    DWORD color; /* white, so MODULATE shows the texture unchanged */
    float u, v;
};

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;
/* A quad (two triangles) covering a centered 256x256 screen region, UV 0..1. */
static struct Vtx g_quad[6] = {
    {192, 112, 0, 1, 0xFFFFFFFF, 0, 0}, {448, 112, 0, 1, 0xFFFFFFFF, 1, 0},
    {192, 368, 0, 1, 0xFFFFFFFF, 0, 1}, {448, 112, 0, 1, 0xFFFFFFFF, 1, 0},
    {448, 368, 0, 1, 0xFFFFFFFF, 1, 1}, {192, 368, 0, 1, 0xFFFFFFFF, 0, 1},
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
    IDirect3DTexture9 *tex = 0;
    D3DLOCKED_RECT lr;
    unsigned *px;
    int i, j;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_tex";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_tex", "tex", 0, 0, 0, 640, 480,
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

    /* 64x64 checkerboard texture. */
    IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8,
                                   D3DPOOL_MANAGED, &tex, (HANDLE *)0);
    IDirect3DTexture9_LockRect(tex, 0, &lr, (const RECT *)0, 0);
    px = (unsigned *)lr.pBits;
    for (j = 0; j < 64; j++)
        for (i = 0; i < 64; i++)
            px[j * 64 + i] = ((i ^ j) & 8) ? 0xFFFF8020 : 0xFF2050FF;
    IDirect3DTexture9_UnlockRect(tex, 0);

    IDirect3DDevice9_Clear(dev, 0, (const D3DRECT *)0, D3DCLEAR_TARGET,
                           0x00202838, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 2, g_quad,
                                     sizeof(struct Vtx));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);

    IDirect3DTexture9_Release(tex);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    ExitProcess(0);
}
