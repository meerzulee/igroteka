/* itri.exe — corpus rung: indexed drawing. A quad from 4 shared vertices + 6
 * indices (two triangles), exercising CreateIndexBuffer → Lock → SetIndices →
 * DrawIndexedPrimitive: the index buffer selects vertices from the bound stream.
 */
#include <windows.h>
#include <d3d9.h>

struct Vtx {
    float x, y, z, rhw;
    DWORD color;
};

static WNDCLASSA g_wc;
static D3DPRESENT_PARAMETERS g_pp;
static struct Vtx g_verts[4] = {
    {200, 120, 0, 1, 0x00FF3030}, /* TL red    */
    {440, 120, 0, 1, 0x0030FF30}, /* TR green  */
    {440, 360, 0, 1, 0x003030FF}, /* BR blue   */
    {200, 360, 0, 1, 0x00FFFF30}, /* BL yellow */
};
static unsigned short g_idx[6] = {0, 1, 2, 0, 2, 3};

static void copy(void *d, const void *s, unsigned n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
}

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
    IDirect3DIndexBuffer9 *ib = 0;
    void *p = 0;

    g_wc.lpfnWndProc = WndProc;
    g_wc.lpszClassName = "fortochka_itri";
    RegisterClassA(&g_wc);
    hwnd = CreateWindowExA(0, "fortochka_itri", "itri", 0, 0, 0, 640, 480,
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

    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(g_verts), 0,
                                        D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                        D3DPOOL_DEFAULT, &vb, (HANDLE *)0);
    IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(g_verts), &p, 0);
    copy(p, g_verts, sizeof(g_verts));
    IDirect3DVertexBuffer9_Unlock(vb);

    IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(g_idx), 0, D3DFMT_INDEX16,
                                       D3DPOOL_DEFAULT, &ib, (HANDLE *)0);
    IDirect3DIndexBuffer9_Lock(ib, 0, sizeof(g_idx), &p, 0);
    copy(p, g_idx, sizeof(g_idx));
    IDirect3DIndexBuffer9_Unlock(ib);

    IDirect3DDevice9_Clear(dev, 0, (const D3DRECT *)0, D3DCLEAR_TARGET,
                           0x00202838, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(struct Vtx));
    IDirect3DDevice9_SetIndices(dev, ib);
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, 0, 0, (HWND)0, 0);

    IDirect3DIndexBuffer9_Release(ib);
    IDirect3DVertexBuffer9_Release(vb);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    ExitProcess(0);
}
