/* sysinit.exe — corpus rung: the init-gate stub DLLs (RTW batch 6). Exercises
 * ole32 (CoInitialize/CoTaskMemAlloc/CoUninitialize), wsock32's real byte-order
 * helpers + WSAStartup, and d3d8 (Direct3DCreate8 -> a non-null fake IDirect3D8:
 * RTW REQUIRES a working D3D8 stack at startup — its "DirectX 9" self-check
 * creates + enumerates a D3D8 device and exits if that fails). steam_api has no
 * import lib to link against, so it is covered when RTW itself boots, not here.
 * Exit 42 = pass.
 */
#include <windows.h>
#include <winsock.h>

/* d3d8 is not in the standard headers here; declare the one export. */
extern void *__stdcall Direct3DCreate8(unsigned int SDKVersion);

void start(void)
{
    WSADATA wsa;
    void *p;

    if (CoInitialize(NULL) != S_OK) ExitProcess(11);

    /* Real byte-order swaps (guest is little-endian → network order). */
    if (htonl(0x11223344u) != 0x44332211u) ExitProcess(12);
    if (htons(0x1122) != 0x2211) ExitProcess(13);
    if (ntohl(0xAABBCCDDu) != 0xDDCCBBAAu) ExitProcess(14);

    if (WSAStartup(0x0202, &wsa) != 0) ExitProcess(15);
    if (wsa.wVersion != 0x0202) ExitProcess(16);

    p = CoTaskMemAlloc(64);
    if (!p) ExitProcess(17);
    CoTaskMemFree(p);

    /* d3d8 hands back a non-null IDirect3D8 (RTW's startup requires it). */
    if (Direct3DCreate8(220) == NULL) ExitProcess(18);

    WSACleanup();
    CoUninitialize();
    ExitProcess(42);
}
