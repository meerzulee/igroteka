/* tex_evil.exe — adversarial rung: a textured triangle with a huge (finite) UV
 * coordinate. u*tw is then far outside int range; the naive (int) cast is UB.
 * The runtime must wrap the UV in float first. Run under UBSan: a clean exit
 * with the triangle drawn (not the clear color) proves the guard holds.
 */
#include <windows.h>
#include <d3d9.h>

struct Vtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;
/* Huge UVs on every vertex — must not reach a bad float→int cast. */
static struct Vtx g_tri[3] = {
    {320, 100, 0, 1, 0xFFFFFFFF, 1e30f, 1e30f},
    {520, 380, 0, 1, 0xFFFFFFFF, 1e30f, 1e30f},
    {120, 380, 0, 1, 0xFFFFFFFF, 1e30f, 1e30f},
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
    int i;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_texevil";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_texevil", "te", 0, 0, 0, 640, 480,
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

    IDirect3DDevice9_CreateTexture(dev, 8, 8, 1, 0, D3DFMT_A8R8G8B8,
                                   D3DPOOL_MANAGED, &tex, (HANDLE *)0);
    IDirect3DTexture9_LockRect(tex, 0, &lr, (const RECT *)0, 0);
    px = (unsigned *)lr.pBits;
    for (i = 0; i < 64; i++) px[i] = 0xFF00FFFF; /* solid cyan */
    IDirect3DTexture9_UnlockRect(tex, 0);

    IDirect3DDevice9_Clear(dev, 0, (const D3DRECT *)0, D3DCLEAR_TARGET,
                           0x00202838, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, g_tri,
                                     sizeof(struct Vtx));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);
    ExitProcess(0);
}
