/* vbtri.exe — corpus rung: a triangle drawn from a real vertex buffer. Exercises
 * the identity-mapped resource path: CreateVertexBuffer allocates guest memory,
 * Lock returns a raw guest pointer we memcpy vertices into (no marshalling),
 * Unlock commits, SetStreamSource binds it, DrawPrimitive draws from it.
 */
#include <windows.h>
#include <d3d9.h>

struct Vtx {
    float x, y, z, rhw;
    DWORD color;
};

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;
static struct Vtx g_src[3] = {
    {320.0f, 80.0f, 0.5f, 1.0f, 0x00FF3030},
    {560.0f, 400.0f, 0.5f, 1.0f, 0x0030FF30},
    {80.0f, 400.0f, 0.5f, 1.0f, 0x003030FF},
};

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

/* minimal memcpy (no CRT) */
static void copy(void *d, const void *s, unsigned n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
}

void start(void)
{
    HWND hwnd;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = 0;
    IDirect3DVertexBuffer9 *vb = 0;
    void *ptr = 0;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_vb";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_vb", "vb", 0, 0, 0, 640, 480,
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

    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(g_src), 0,
                                        D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                        D3DPOOL_DEFAULT, &vb, (HANDLE *)0);
    IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(g_src), &ptr, 0);
    copy(ptr, g_src, sizeof(g_src)); /* write through the identity-mapped pointer */
    IDirect3DVertexBuffer9_Unlock(vb);

    IDirect3DDevice9_Clear(dev, 0, (const D3DRECT *)0, D3DCLEAR_TARGET,
                           0x00202838, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vtx));
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);

    IDirect3DVertexBuffer9_Release(vb);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    ExitProcess(0);
}
