/* pump.exe — corpus rung: message-pump adjuncts + winmm timing + display caps
 * (RTW batch 4). Exercises GetQueueStatus empty/non-empty, PostThreadMessageA
 * feeding the queue, MsgWaitForMultipleObjects (timeout on empty, ready after a
 * post), RegisterWindowMessageA interning (same name -> same id, distinct names
 * -> distinct ids in 0xC000+), GetKeyboardState (all zeros), GetDeviceCaps
 * (BITSPIXEL 32), and timeGetTime/timeBeginPeriod/timeSetEvent. Exit 42 = pass.
 */
#include <windows.h>

void start(void)
{
    MSG msg;
    BYTE keys[256];
    UINT a, b, c;
    DWORD t1, t2;
    int i;

    /* Empty queue: no status bits, MsgWait with finite timeout times out. */
    if (GetQueueStatus(QS_ALLEVENTS) != 0) ExitProcess(11);
    if (MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT)
        != WAIT_TIMEOUT) ExitProcess(12);

    /* Post → status reports pending, MsgWait reports message-ready (index n). */
    if (!PostThreadMessageA(0, WM_USER + 7, 11, 22)) ExitProcess(13);
    if (GetQueueStatus(QS_ALLEVENTS) == 0) ExitProcess(14);
    if (MsgWaitForMultipleObjects(0, NULL, FALSE, 5, QS_ALLINPUT)
        != WAIT_OBJECT_0) ExitProcess(15);
    if (!PeekMessageA(&msg, (HWND)0, 0, 0, PM_REMOVE)) ExitProcess(16);
    if (msg.message != WM_USER + 7 || msg.wParam != 11) ExitProcess(17);

    /* RegisterWindowMessage: interned ids in the real range. */
    a = RegisterWindowMessageA("FORTOCHKA_MSG_A");
    b = RegisterWindowMessageA("FORTOCHKA_MSG_B");
    c = RegisterWindowMessageA("FORTOCHKA_MSG_A");
    if (a < 0xC000 || b < 0xC000) ExitProcess(18);
    if (a != c || a == b) ExitProcess(19);

    /* Headless keyboard: success, all zeros. */
    keys[0] = keys[255] = 0xAA;
    if (!GetKeyboardState(keys)) ExitProcess(20);
    for (i = 0; i < 256; i++)
        if (keys[i] != 0) ExitProcess(21);

    if (GetDoubleClickTime() != 500) ExitProcess(22);
    if (GetDeviceCaps((HDC)0, BITSPIXEL) != 32) ExitProcess(23);

    /* winmm clock: monotonic non-decreasing; period calls succeed; a timer id. */
    if (timeBeginPeriod(1) != TIMERR_NOERROR) ExitProcess(24);
    t1 = timeGetTime();
    t2 = timeGetTime();
    if (t2 < t1) ExitProcess(25);
    if (timeSetEvent(16, 1, (LPTIMECALLBACK)0, 0, TIME_PERIODIC) == 0) ExitProcess(26);
    if (timeEndPeriod(1) != TIMERR_NOERROR) ExitProcess(27);

    ExitProcess(42);
}
