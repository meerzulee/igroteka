/* crt2.exe — corpus rung: the kernel32 CRT tail (RTW batch 5). CompareString
 * ordering + ignore-case, MulDiv rounding, IsBad*Ptr probes, GlobalMemoryStatus,
 * the wall-clock family (fixed deterministic date + an exact SystemTime ->
 * FileTime -> SystemTime round trip), and file mapping (named view over a VFS
 * file + a writable anonymous view). Exit 42 = pass.
 */
#include <windows.h>

void start(void)
{
    SYSTEMTIME st, back;
    FILETIME ft;
    MEMORYSTATUS ms;
    HANDLE hf, hm;
    const char *view;
    char *anon;
    DWORD n;

    /* CompareString: ordering, equality, case flag. */
    if (CompareStringA(0, 0, "abc", -1, "abd", -1) != CSTR_LESS_THAN) ExitProcess(11);
    if (CompareStringA(0, 0, "abc", -1, "abc", -1) != CSTR_EQUAL) ExitProcess(12);
    if (CompareStringA(0, 0, "b", -1, "a", -1) != CSTR_GREATER_THAN) ExitProcess(13);
    if (CompareStringA(0, NORM_IGNORECASE, "ABC", -1, "abc", -1) != CSTR_EQUAL)
        ExitProcess(14);
    if (CompareStringA(0, 0, "ab", -1, "abc", -1) != CSTR_LESS_THAN) ExitProcess(15);

    /* MulDiv: rounding to nearest, div-by-zero -> -1. */
    if (MulDiv(10, 3, 2) != 15) ExitProcess(16);
    if (MulDiv(5, 1, 2) != 3) ExitProcess(17);   /* 2.5 rounds to 3 */
    if (MulDiv(1, 1, 0) != -1) ExitProcess(18);

    /* Pointer probes: the stack is good, null/wild are bad. */
    if (IsBadReadPtr(&st, sizeof(st))) ExitProcess(19);
    if (!IsBadReadPtr(NULL, 4)) ExitProcess(20);
    if (!IsBadWritePtr((void *)0xFF000000u, 16)) ExitProcess(21);
    if (IsBadCodePtr((FARPROC)start)) ExitProcess(22);

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    if (ms.dwTotalPhys == 0 || ms.dwAvailPhys == 0) ExitProcess(23);

    /* Wall clock: fixed deterministic date. */
    GetSystemTime(&st);
    if (st.wYear != 2004 || st.wMonth != 6 || st.wDay != 15) ExitProcess(24);

    /* SystemTime -> FileTime -> SystemTime must round-trip exactly. */
    st.wHour = 12; st.wMinute = 34; st.wSecond = 56; st.wMilliseconds = 789;
    if (!SystemTimeToFileTime(&st, &ft)) ExitProcess(25);
    if (!FileTimeToSystemTime(&ft, &back)) ExitProcess(26);
    if (back.wYear != 2004 || back.wMonth != 6 || back.wDay != 15) ExitProcess(27);
    if (back.wHour != 12 || back.wMinute != 34 || back.wSecond != 56 ||
        back.wMilliseconds != 789) ExitProcess(28);

    /* File mapping: a VFS-backed view reads the file bytes. */
    hf = CreateFileA("map.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) ExitProcess(29);
    if (!WriteFile(hf, "MAPDATA!", 8, &n, NULL)) ExitProcess(30);
    hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hm) ExitProcess(31);
    view = (const char *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!view) ExitProcess(32);
    if (view[0] != 'M' || view[7] != '!') ExitProcess(33);
    UnmapViewOfFile((void *)view);
    CloseHandle(hm);
    CloseHandle(hf);

    /* Anonymous mapping: zeroed and writable. */
    hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096,
                            NULL);
    if (!hm) ExitProcess(34);
    anon = (char *)MapViewOfFile(hm, FILE_MAP_WRITE, 0, 0, 0);
    if (!anon) ExitProcess(35);
    if (anon[0] != 0 || anon[4095] != 0) ExitProcess(36);
    anon[100] = 'x';
    if (anon[100] != 'x') ExitProcess(37);

    ExitProcess(42);
}
