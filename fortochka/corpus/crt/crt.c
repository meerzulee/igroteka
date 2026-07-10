/* crt.exe — corpus rung: the CRT pre-main kernel32 surface (the subset the MSVC
 * __tmainCRTStartup calls before WinMain). Exercises the real-logic ones —
 * Interlocked atomics, MultiByte<->WideChar round-trip, GetCPInfo, LCMapString
 * case mapping, GetStringType classification, GetModuleFileName, GetFileType —
 * so a regression in any is caught. Distinct exit codes pinpoint failures; 42 =
 * all passed.
 */
#include <windows.h>

/* NOTE: InterlockedIncrement/Decrement/Exchange are HLE'd in k32web (RTW imports
 * them), but GCC always inlines them to `lock xadd`/`xchg`, so a C rung can't
 * call the import. They're covered by a zhelezo CPU rung, not here. */

void start(void)
{
    char mb[64], back[64];
    WCHAR wc[64];
    CPINFO cpi;
    WORD ctype[8];
    int n, i;

    /* MultiByte -> Wide -> MultiByte round trip (-1 length includes the NUL). */
    n = MultiByteToWideChar(CP_ACP, 0, "Hello", -1, wc, 64);
    if (n != 6) ExitProcess(15);
    if (wc[0] != 'H' || wc[4] != 'o' || wc[5] != 0) ExitProcess(16);
    n = WideCharToMultiByte(CP_ACP, 0, wc, -1, back, 64, NULL, NULL);
    if (n != 6) ExitProcess(17);
    for (i = 0; i < 6; i++)
        if (back[i] != "Hello"[i]) ExitProcess(18);

    if (!GetCPInfo(CP_ACP, &cpi)) ExitProcess(19);
    if (cpi.MaxCharSize != 1) ExitProcess(20);

    /* LCMapString uppercase. */
    n = LCMapStringA(0, LCMAP_UPPERCASE, "abc", 3, mb, 64);
    if (n != 3 || mb[0] != 'A' || mb[1] != 'B' || mb[2] != 'C') ExitProcess(21);

    /* GetStringType: '5' classifies as a digit. */
    if (!GetStringTypeA(0, CT_CTYPE1, "5", 1, ctype)) ExitProcess(22);
    if (!(ctype[0] & C1_DIGIT)) ExitProcess(23);

    /* Module path + std handle type. */
    n = GetModuleFileNameA(NULL, mb, 64);
    if (n == 0) ExitProcess(24);
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR) ExitProcess(25);

    if (!IsValidCodePage(1252)) ExitProcess(26);

    ExitProcess(42);
}
