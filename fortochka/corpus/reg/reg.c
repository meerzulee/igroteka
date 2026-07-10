/* reg.exe — corpus rung: the advapi32 registry shim. Creates a key, sets a
 * REG_SZ and a REG_DWORD, reopens the key by path, queries both back (including
 * the size-probe and too-small-buffer protocols), and confirms the miss paths:
 * a missing value and a missing key both come back ERROR_FILE_NOT_FOUND — the
 * fallback RTW's install-path lookup must survive. Exit 42 on success.
 */
#include <windows.h>

void start(void)
{
    HKEY hk, hk2;
    DWORD disp, type, cb, dv;
    char buf[64];

    /* Create + set. */
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Fortochka", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hk, &disp) != ERROR_SUCCESS)
        ExitProcess(11);
    if (disp != REG_CREATED_NEW_KEY) ExitProcess(12);
    if (RegSetValueExA(hk, "Greeting", 0, REG_SZ, (const BYTE *)"hello", 6)
        != ERROR_SUCCESS) ExitProcess(13);
    dv = 0xC0FFEE;
    if (RegSetValueExA(hk, "Num", 0, REG_DWORD, (const BYTE *)&dv, 4)
        != ERROR_SUCCESS) ExitProcess(14);
    RegCloseKey(hk);

    /* Reopen by path (case-insensitive) and query back. */
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\FORTOCHKA", 0, KEY_READ,
                      &hk2) != ERROR_SUCCESS) ExitProcess(15);

    /* Size probe: lpData NULL → size lands in cb. */
    cb = 0;
    if (RegQueryValueExA(hk2, "Greeting", NULL, &type, NULL, &cb)
        != ERROR_SUCCESS) ExitProcess(16);
    if (type != REG_SZ || cb != 6) ExitProcess(17);

    /* Too-small buffer → ERROR_MORE_DATA, cb = required. */
    cb = 2;
    if (RegQueryValueExA(hk2, "Greeting", NULL, &type, (BYTE *)buf, &cb)
        != ERROR_MORE_DATA) ExitProcess(18);
    if (cb != 6) ExitProcess(19);

    /* Full read. */
    cb = 64;
    if (RegQueryValueExA(hk2, "Greeting", NULL, &type, (BYTE *)buf, &cb)
        != ERROR_SUCCESS) ExitProcess(20);
    if (cb != 6 || buf[0] != 'h' || buf[4] != 'o' || buf[5] != 0) ExitProcess(21);

    /* REG_DWORD round trip. */
    cb = 4; dv = 0;
    if (RegQueryValueExA(hk2, "Num", NULL, &type, (BYTE *)&dv, &cb)
        != ERROR_SUCCESS) ExitProcess(22);
    if (type != REG_DWORD || dv != 0xC0FFEE) ExitProcess(23);

    /* Miss paths: missing value, then missing key. */
    cb = 64;
    if (RegQueryValueExA(hk2, "NoSuchValue", NULL, &type, (BYTE *)buf, &cb)
        != ERROR_FILE_NOT_FOUND) ExitProcess(24);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\NoSuchVendor\\NoSuchApp", 0,
                      KEY_READ, &hk) == ERROR_SUCCESS) ExitProcess(25);

    /* Reopening an existing key must report OPENED_EXISTING via Create too. */
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Fortochka", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hk, &disp) != ERROR_SUCCESS)
        ExitProcess(26);
    if (disp != REG_OPENED_EXISTING_KEY) ExitProcess(27);

    ExitProcess(42);
}
