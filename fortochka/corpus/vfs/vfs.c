/* vfs.exe — corpus rung: the kernel32 file I/O surface, exercised self-contained
 * against the in-memory VFS. Creates a file, writes bytes, reopens it, checks
 * the size, reads the whole thing back and verifies, then seeks and reads a
 * middle slice. Also checks GetFileAttributes for present/absent paths. Exits 42
 * iff every step matches; distinct exit codes pinpoint a failure otherwise.
 */
#include <windows.h>

static const char PAYLOAD[] = "FORTOCHKA-VFS-0123456789"; /* 24 bytes, no NUL */

void start(void)
{
    HANDLE h;
    DWORD n = 0;
    char buf[64];
    int i;
    const DWORD LEN = sizeof(PAYLOAD) - 1;

    /* Create + write. */
    h = CreateFileA("data/test.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) ExitProcess(11);
    if (!WriteFile(h, PAYLOAD, LEN, &n, NULL) || n != LEN) ExitProcess(12);
    CloseHandle(h);

    /* The path now exists; a bogus one does not. */
    if (GetFileAttributesA("data/test.bin") == INVALID_FILE_ATTRIBUTES) ExitProcess(13);
    if (GetFileAttributesA("nope/missing.bin") != INVALID_FILE_ATTRIBUTES) ExitProcess(14);

    /* Reopen (case-insensitive path) and check the size. */
    h = CreateFileA("DATA\\TEST.BIN", GENERIC_READ, 0, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) ExitProcess(15);
    if (GetFileSize(h, NULL) != LEN) ExitProcess(16);

    /* Read the whole file back and compare. */
    n = 0;
    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = 0;
    if (!ReadFile(h, buf, LEN, &n, NULL) || n != LEN) ExitProcess(17);
    for (i = 0; i < (int)LEN; i++)
        if (buf[i] != PAYLOAD[i]) ExitProcess(18);

    /* Seek to offset 10 (FILE_BEGIN) and read 4 bytes; must match PAYLOAD[10..13]. */
    if (SetFilePointer(h, 10, NULL, FILE_BEGIN) != 10) ExitProcess(19);
    n = 0;
    if (!ReadFile(h, buf, 4, &n, NULL) || n != 4) ExitProcess(20);
    for (i = 0; i < 4; i++)
        if (buf[i] != PAYLOAD[10 + i]) ExitProcess(21);

    /* Reading past EOF returns 0 bytes, still success. */
    SetFilePointer(h, 0, NULL, FILE_END);
    n = 0xdead;
    if (!ReadFile(h, buf, 8, &n, NULL) || n != 0) ExitProcess(22);

    CloseHandle(h);
    ExitProcess(42);
}
