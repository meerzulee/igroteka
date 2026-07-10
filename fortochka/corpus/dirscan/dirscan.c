/* dirscan.exe — corpus rung: path/CWD + directory enumeration over the VFS.
 * Creates files across a synthetic data/ tree, checks file-vs-directory
 * attributes, changes the current directory, enumerates with wildcard patterns
 * (both a "*.pack" file glob and a "*" listing that must surface the synthesized
 * subdirectory), then deletes a file and confirms it is gone. Exit 42 on success.
 */
#include <windows.h>

static void mk(const char *p)
{
    DWORD n;
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, "x", 1, &n, NULL);
        CloseHandle(h);
    }
}

void start(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE hf;
    char cwd[260];
    int count;

    mk("data\\a.pack");
    mk("data\\b.pack");
    mk("data\\world\\desc.txt");

    if (GetFileAttributesA("data\\a.pack") != FILE_ATTRIBUTE_NORMAL) ExitProcess(11);
    if (GetFileAttributesA("data\\world") != FILE_ATTRIBUTE_DIRECTORY) ExitProcess(12);
    if (GetFileAttributesA("data\\nope") != INVALID_FILE_ATTRIBUTES) ExitProcess(13);

    if (!SetCurrentDirectoryA("data")) ExitProcess(14);
    if (GetCurrentDirectoryA(260, cwd) == 0) ExitProcess(15);

    /* *.pack in the data dir → a.pack, b.pack (2). */
    count = 0;
    hf = FindFirstFileA("*.pack", &fd);
    if (hf == INVALID_HANDLE_VALUE) ExitProcess(16);
    do { count++; } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    if (count != 2) ExitProcess(20 + count);

    /* * → a.pack, b.pack, and the synthesized "world" directory (3). */
    count = 0;
    hf = FindFirstFileA("*", &fd);
    if (hf == INVALID_HANDLE_VALUE) ExitProcess(30);
    do { count++; } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    if (count != 3) ExitProcess(50 + count);

    DeleteFileA("a.pack");
    if (GetFileAttributesA("a.pack") != INVALID_FILE_ATTRIBUTES) ExitProcess(41);

    /* ".." must normalize back to the parent, not append literally. */
    if (!SetCurrentDirectoryA("..")) ExitProcess(43);
    if (GetFileAttributesA("data\\b.pack") != FILE_ATTRIBUTE_NORMAL) ExitProcess(44);
    if (GetFileAttributesA("data\\world\\..\\b.pack") != FILE_ATTRIBUTE_NORMAL)
        ExitProcess(45);

    ExitProcess(42);
}
