/* rstest.exe — corpus rung: render states (depth test + z-write). Draws two
 * full-screen triangles of identical winding that differ only in depth, color,
 * and draw order: a NEAR blue one first, then a FAR red one. With ZENABLE the
 * far triangle fails the D3DCMP_LESSEQUAL depth test and is rejected, so the
 * center must stay blue. Without depth testing (the pre-render-state behaviour)
 * the later red triangle would overwrite it — so this rung distinguishes a real
 * depth buffer from a plain painter's-order rasterizer.
 *
 * Pre-transformed XYZRHW vertices: the .z field feeds the depth buffer directly.
 */
#include <windows.h>
#include <d3d9.h>

struct Vtx {
    float x, y, z, rhw;
    DWORD color; /* D3DCOLOR 0xAARRGGBB */
};

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;

/* A big triangle covering the whole 640x480 backbuffer (front-facing: negative
 * signed area, so the default D3DCULL_CCW keeps it). Same geometry for both
 * draws; only z + color change. */
static struct Vtx g_near[3] = {
    {-100.0f, -100.0f, 0.2f, 1.0f, 0x000000FF}, /* blue, near (z=0.2) */
    { 900.0f, -100.0f, 0.2f, 1.0f, 0x000000FF},
    { 320.0f,  900.0f, 0.2f, 1.0f, 0x000000FF},
};
static struct Vtx g_far[3] = {
    {-100.0f, -100.0f, 0.8f, 1.0f, 0x00FF0000}, /* red, far (z=0.8) */
    { 900.0f, -100.0f, 0.8f, 1.0f, 0x00FF0000},
    { 320.0f,  900.0f, 0.8f, 1.0f, 0x00FF0000},
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
    g_wc.lpszClassName = "fortochka_rstest";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_rstest", "rstest", 0, 0, 0, 640, 480,
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

    /* Clear color AND depth (Z=1.0 = far plane). */
    IDirect3DDevice9_Clear(dev, 0, (const D3DRECT *)0,
                           D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                           0x00202838, 1.0f, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);

    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    /* Near first (writes z=0.2), then far (z=0.8 > 0.2 → depth-rejected). */
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, g_near,
                                     sizeof(struct Vtx));
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, g_far,
                                     sizeof(struct Vtx));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    ExitProcess(0);
}
