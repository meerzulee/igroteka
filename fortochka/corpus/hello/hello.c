/* hello.exe — corpus rung 1.
 *
 * Smallest possible real PE: no CRT, imports exactly three kernel32
 * functions (GetStdHandle, WriteFile, ExitProcess). This is the Phase F1
 * exit criterion: zhelezo + peload + k32web must run this binary and
 * produce the line below on the host console.
 */
#include <windows.h>

void start(void)
{
    static const char msg[] = "hello from fortochka corpus\n";
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteFile(out, msg, sizeof(msg) - 1, &written, NULL);
    ExitProcess(0);
}
