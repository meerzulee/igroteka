/* tri_evil.exe — adversarial rung: a triangle with a NaN vertex coordinate.
 * Guest vertex floats are attacker-controlled; a NaN reaching the rasterizer's
 * float→int bounding-box cast is UB. The runtime must reject non-finite coords
 * and skip the triangle — leaving the cleared background. Run under UBSan, a
 * clean exit + unchanged clear color proves the guard holds.
 */
#include <windows.h>
#include <d3d9.h>

struct Vtx {
    float x, y, z, rhw;
    DWORD color;
};

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;
static struct Vtx g_tri[3];

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

void start(void)
{
    HWND hwnd;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = 0;
    union { unsigned u; float f; } qnan;
    qnan.u = 0x7FC00000; /* quiet NaN bit pattern */

    /* A NaN in vertex 0's x — everything else finite. */
    g_tri[0].x = qnan.f; g_tri[0].y = 100.0f; g_tri[0].rhw = 1.0f; g_tri[0].color = 0x00FF0000;
    g_tri[1].x = 500.0f; g_tri[1].y = 400.0f; g_tri[1].rhw = 1.0f; g_tri[1].color = 0x0000FF00;
    g_tri[2].x = 100.0f; g_tri[2].y = 400.0f; g_tri[2].rhw = 1.0f; g_tri[2].color = 0x000000FF;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_evil";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_evil", "evil", 0, 0, 0, 640, 480,
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
                           0x00123456, 1.0f, 0);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, g_tri,
                                     sizeof(struct Vtx));
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);
    ExitProcess(0);
}
