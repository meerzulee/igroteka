/* d3dinit.exe — corpus rung: the D3D9 device-init / enumeration path a real
 * game walks before it draws. Queries adapter count, display mode, and device
 * caps, checks a device type, creates a device, resets it, and clears — all via
 * the forged single-adapter software-FFP surface. Reading caps through the real
 * D3DCAPS9 struct fields also validates the offsets fill_caps() writes to.
 * Exits 42 iff every step returns the expected value.
 */
#include <windows.h>
#include <d3d9.h>

static D3DPRESENT_PARAMETERS g_pp;

void start(void)
{
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = 0;
    D3DCAPS9 caps;
    D3DDISPLAYMODE mode;
    HRESULT hr;
    int ok = 1;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) ExitProcess(10);

    if (IDirect3D9_GetAdapterCount(d3d) != 1) ExitProcess(11);

    if (IDirect3D9_GetAdapterDisplayMode(d3d, 0, &mode) != D3D_OK) ExitProcess(12);
    if (mode.Width == 0 || mode.Height == 0) ExitProcess(13);

    if (IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps) != D3D_OK) ExitProcess(14);
    if (caps.VertexShaderVersion != 0) ExitProcess(15); /* FFP persona: no shaders */
    if (caps.PixelShaderVersion != 0) ExitProcess(16);
    if (caps.MaxTextureWidth != 4096) ExitProcess(17);  /* validates the caps offset */

    if (IDirect3D9_CheckDeviceType(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                   D3DFMT_X8R8G8B8, TRUE) != D3D_OK) ExitProcess(18);

    /* CreateDevice ignores the device window in our HLE, so skip the window. */
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferWidth = 640;
    g_pp.BackBufferHeight = 480;
    g_pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    g_pp.hDeviceWindow = (HWND)0;
    hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, (HWND)0,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_pp, &dev);
    if (hr != D3D_OK || !dev) ExitProcess(19);

    if (IDirect3DDevice9_GetDeviceCaps(dev, &caps) != D3D_OK) ExitProcess(20);
    if (caps.VertexShaderVersion != 0) ExitProcess(21);
    /* Reset must succeed and leave the device usable for a subsequent clear. */
    if (IDirect3DDevice9_Reset(dev, &g_pp) != D3D_OK) ExitProcess(22);
    if (IDirect3DDevice9_Clear(dev, 0, (const D3DRECT *)0, D3DCLEAR_TARGET,
                               0x00112233, 1.0f, 0) != D3D_OK) ExitProcess(23);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    (void)ok;
    ExitProcess(42);
}
