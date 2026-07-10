#include "u32web/u32web.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace u32web {

using runtime::Machine;

namespace {

// Windows message constants we actually reference.
constexpr uint32_t WM_QUIT = 0x0012;

// Win32 MSG is { HWND, UINT message, WPARAM, LPARAM, DWORD time, POINT pt } —
// 28 bytes on 32-bit. GetMessage/PeekMessage fill a guest buffer in this shape.
struct Msg {
    uint32_t hwnd, message, wparam, lparam;
};

constexpr uint32_t kFakeHwnd = 0x00010001; // one window; a nonzero HWND token.
constexpr uint32_t kFakeHdc = 0x0DC00001;  // one device context token.
constexpr uint32_t kBrushTag = 0xB0000000; // solid-brush handle: tag | RGB.

// COLORREF (0x00BBGGRR) → framebuffer ARGB (0xFFRRGGBB), opaque.
inline uint32_t colorref_to_argb(uint32_t c) {
    uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Single-window state. One process, one message queue, one client framebuffer —
// enough for the pump round-trip and RTW's single top-level window later.
struct State {
    uint32_t wndproc = 0; // guest VA of the registered WNDPROC
    std::deque<Msg> queue;
    bool class_registered = false;
    uint32_t width = 640, height = 480;   // client area
    // Window longs for the single top-level window (GetWindowLong/SetWindowLong).
    // GWL_WNDPROC is stored in `wndproc` above so subclassing redirects dispatch.
    uint32_t userdata = 0, style = 0, exstyle = 0;
    int cursor_show = 0;                   // ShowCursor display counter
    // RegisterWindowMessageA interned names: message id = 0xC000 + index
    // (the real RegisterWindowMessage range), same id for the same name.
    std::vector<std::string> wm_names;
    uint32_t mm_timers = 0; // timeSetEvent ids handed out (timers never fire)
    std::vector<uint32_t> fb;             // ARGB pixels, width*height
    void ensure_fb() {
        if (fb.size() != (size_t)width * height) fb.assign((size_t)width * height, 0xFF000000u);
    }
};
State g_state; // one guest process per Machine in tier 0; a global is fine here.

void write_msg(Machine& m, uint32_t buf, const Msg& msg) {
    m.write32(buf + 0, msg.hwnd);
    m.write32(buf + 4, msg.message);
    m.write32(buf + 8, msg.wparam);
    m.write32(buf + 12, msg.lparam);
    m.write32(buf + 16, 0); // time
    m.write32(buf + 20, 0); // pt.x
    m.write32(buf + 24, 0); // pt.y
}

} // namespace

// Windows caps a thread's message queue (~10k); bound ours so a guest that
// PostMessages without pumping can't grow the host deque without limit.
constexpr size_t kQueueMax = 65536;

void post_message(uint32_t hwnd, uint32_t message, uint32_t wparam,
                  uint32_t lparam) {
    if (g_state.queue.size() >= kQueueMax) return; // drop when full
    g_state.queue.push_back({hwnd, message, wparam, lparam});
}

const uint32_t* framebuffer(uint32_t& width, uint32_t& height) {
    if (g_state.fb.empty()) return nullptr;
    width = g_state.width;
    height = g_state.height;
    return g_state.fb.data();
}

void install(Machine& m) {
    g_state = State{};

    m.add_handler([](Machine& m, const std::string& dll,
                     const std::string& name) -> bool {
        if (dll != "user32.dll" && dll != "gdi32.dll" && dll != "winmm.dll")
            return false;
        State& s = g_state;

        // ---- gdi32: minimal painting into the client framebuffer ----
        if (name == "CreateSolidBrush") { // COLORREF → tagged brush handle
            m.ret(1, kBrushTag | (m.arg(0) & 0x00FFFFFF));
            return true;
        }
        if (name == "SetPixel") { // hdc, x, y, COLORREF
            uint32_t x = m.arg(1), y = m.arg(2);
            s.ensure_fb();
            if (x < s.width && y < s.height)
                s.fb[(size_t)y * s.width + x] = colorref_to_argb(m.arg(3));
            m.ret(4, m.arg(3));
            return true;
        }
        if (name == "GetStockObject") { // return a tagged white/black/etc brush
            uint32_t idx = m.arg(0); // 0 WHITE_BRUSH..4 BLACK_BRUSH (rough)
            uint32_t rgb = idx == 4 ? 0x000000 : 0xFFFFFF;
            m.ret(1, kBrushTag | rgb);
            return true;
        }
        if (name == "DeleteObject" || name == "SelectObject") {
            m.ret(name == "SelectObject" ? 2 : 1, 0);
            return true;
        }

        if (name == "RegisterClassA" || name == "RegisterClassExA") {
            // WNDCLASS(A): lpfnWndProc is the 2nd field (offset 4); WNDCLASSEX
            // has cbSize first, so lpfnWndProc is at offset 8.
            uint32_t p = m.arg(0);
            uint32_t proc_off = name == "RegisterClassExA" ? 8 : 4;
            s.wndproc = m.read32(p + proc_off);
            s.class_registered = true;
            m.ret(1, 0xC001); // nonzero ATOM
            return true;
        }
        if (name == "CreateWindowExA" || name == "CreateWindowA") {
            // args: ...,style,x,y,w,h,... — width/height at indices depending on
            // the ExA prefix (ExA has dwExStyle first). Capture the client size
            // for the framebuffer; ignore CW_USEDEFAULT (0x80000000) and 0.
            bool ex = name == "CreateWindowExA";
            uint32_t w = m.arg(ex ? 6 : 5), h = m.arg(ex ? 7 : 6);
            // Cap dims (≤4096 covers any era resolution) so a guest can't force
            // a >256 MB framebuffer allocation. CW_USEDEFAULT / 0 keep default.
            if (w && w != 0x80000000u && w <= 4096) s.width = w;
            if (h && h != 0x80000000u && h <= 4096) s.height = h;
            s.ensure_fb();
            m.ret(ex ? 12 : 11, kFakeHwnd);
            return true;
        }
        if (name == "GetClientRect") { // hwnd, RECT* {left,top,right,bottom}
            uint32_t r = m.arg(1);
            m.write32(r + 0, 0);
            m.write32(r + 4, 0);
            m.write32(r + 8, s.width);
            m.write32(r + 12, s.height);
            m.ret(2, 1);
            return true;
        }
        if (name == "GetDC" || name == "BeginPaint") {
            // BeginPaint(hwnd, PAINTSTRUCT*) fills ps: hdc(+0), fErase(+4),
            // rcPaint(+8..24). Both return an HDC.
            if (name == "BeginPaint") {
                uint32_t ps = m.arg(1);
                m.write32(ps + 0, kFakeHdc);
                m.write32(ps + 4, 0);
                m.write32(ps + 8, 0);
                m.write32(ps + 12, 0);
                m.write32(ps + 16, s.width);
                m.write32(ps + 20, s.height);
            }
            s.ensure_fb();
            m.ret(name == "BeginPaint" ? 2 : 1, kFakeHdc);
            return true;
        }
        if (name == "EndPaint" || name == "ReleaseDC") {
            m.ret(name == "ReleaseDC" ? 2 : 2, 1);
            return true;
        }
        if (name == "FillRect") { // hdc, RECT*, HBRUSH
            uint32_t r = m.arg(1);
            int32_t l = (int32_t)m.read32(r + 0), t = (int32_t)m.read32(r + 4),
                    ri = (int32_t)m.read32(r + 8), bo = (int32_t)m.read32(r + 12);
            uint32_t argb = colorref_to_argb(m.arg(2) & 0x00FFFFFF);
            s.ensure_fb();
            // Clamp the rect to the framebuffer BEFORE iterating — a negative
            // origin must not turn the fill into a multi-billion-iteration loop.
            if (l < 0) l = 0;
            if (t < 0) t = 0;
            if (ri > (int32_t)s.width) ri = (int32_t)s.width;
            if (bo > (int32_t)s.height) bo = (int32_t)s.height;
            for (int32_t y = t; y < bo; y++)
                for (int32_t x = l; x < ri; x++)
                    s.fb[(size_t)y * s.width + x] = argb;
            m.ret(3, 1);
            return true;
        }
        if (name == "InvalidateRect" || name == "ValidateRect" ||
            name == "UpdateWindow") {
            m.ret(name == "InvalidateRect" ? 3 : name == "ValidateRect" ? 2 : 1, 1);
            return true;
        }
        if (name == "DefWindowProcA") {
            // hwnd, msg, wparam, lparam — default is to return 0 for the
            // messages our corpus sends (real DefWindowProc has per-message
            // behavior we add when a caller needs it).
            m.ret(4, 0);
            return true;
        }
        if (name == "GetMessageA") {
            // BOOL GetMessageA(LPMSG, HWND, UINT min, UINT max). Returns 0 on
            // WM_QUIT (loop terminator), nonzero otherwise, -1 on error.
            // Real GetMessage BLOCKS on an empty queue; tier-0 headless has no
            // event source, and a correct run always posts WM_QUIT before the
            // queue drains. So an empty queue here means a starved pump — error
            // loudly rather than silently synthesizing a quit that could mask a
            // real message-generation bug. F3's rAF pump replaces this with a
            // real yield/block.
            uint32_t lpmsg = m.arg(0);
            if (s.queue.empty())
                throw runtime::MachineError{
                    "GetMessageA on empty queue with no WM_QUIT posted "
                    "(tier-0 headless has no blocking source)"};
            Msg msg = s.queue.front();
            s.queue.pop_front();
            write_msg(m, lpmsg, msg);
            m.ret(4, msg.message == WM_QUIT ? 0 : 1);
            return true;
        }
        if (name == "PeekMessageA") {
            // BOOL PeekMessageA(LPMSG, HWND, min, max, wRemoveMsg).
            uint32_t lpmsg = m.arg(0);
            uint32_t remove = m.arg(4);
            if (s.queue.empty()) {
                m.ret(5, 0);
                return true;
            }
            Msg msg = s.queue.front();
            if (remove & 1 /*PM_REMOVE*/) s.queue.pop_front();
            write_msg(m, lpmsg, msg);
            m.ret(5, 1);
            return true;
        }
        if (name == "TranslateMessage") {
            m.ret(1, 0);
            return true;
        }
        if (name == "DispatchMessageA") {
            // THE REVERSE THUNK. Read the MSG, call the guest wndproc with
            // (hwnd, message, wParam, lParam), return its LRESULT.
            uint32_t lpmsg = m.arg(0);
            uint32_t hwnd = m.read32(lpmsg + 0);
            uint32_t message = m.read32(lpmsg + 4);
            uint32_t wparam = m.read32(lpmsg + 8);
            uint32_t lparam = m.read32(lpmsg + 12);
            uint32_t lresult = 0;
            if (s.wndproc)
                lresult = m.call_guest(s.wndproc, {hwnd, message, wparam, lparam});
            m.ret(1, lresult);
            return true;
        }
        if (name == "PostQuitMessage") {
            uint32_t code = m.arg(0);
            s.queue.push_back({0, WM_QUIT, code, 0});
            m.ret(1, 0);
            return true;
        }
        if (name == "PostMessageA") {
            bool full = s.queue.size() >= kQueueMax;
            if (!full) post_message(m.arg(0), m.arg(1), m.arg(2), m.arg(3));
            m.ret(4, full ? 0 : 1); // Windows: FALSE when the queue is full
            return true;
        }
        if (name == "SendMessageA") {
            // Synchronous: reverse-thunk straight into the wndproc (this is the
            // nested-call_guest path — DispatchMessage's wndproc may SendMessage).
            uint32_t lresult = 0;
            if (s.wndproc)
                lresult = m.call_guest(s.wndproc,
                                       {m.arg(0), m.arg(1), m.arg(2), m.arg(3)});
            m.ret(4, lresult);
            return true;
        }
        // ---- window / system metrics a game reads during startup ----
        if (name == "GetSystemMetrics") {
            // Report a 1024x768 primary display; other indices 0 (a game that
            // needs a specific metric will surface it as a wrong-value bug).
            uint32_t idx = m.arg(0);
            uint32_t v = idx == 0 /*SM_CXSCREEN*/ ? 1024
                       : idx == 1 /*SM_CYSCREEN*/ ? 768
                                                  : 0;
            m.ret(1, v);
            return true;
        }
        if (name == "AdjustWindowRect" || name == "AdjustWindowRectEx") {
            // Client rect == window rect in our HLE (no chrome): leave it as-is.
            m.ret(name == "AdjustWindowRectEx" ? 4 : 3, 1);
            return true;
        }
        if (name == "GetWindowRect") { // hwnd, RECT* → whole window at origin
            uint32_t r = m.arg(1);
            m.write32(r + 0, 0);
            m.write32(r + 4, 0);
            m.write32(r + 8, s.width);
            m.write32(r + 12, s.height);
            m.ret(2, 1);
            return true;
        }
        if (name == "LoadCursorA" || name == "LoadIconA") {
            m.ret(2, 0x0C000001); // a non-null cursor/icon token
            return true;
        }
        if (name == "SetCursor") { m.ret(1, 0); return true; } // prev = none
        if (name == "ShowCursor") {
            s.cursor_show += (m.arg(0) ? 1 : -1);
            m.ret(1, (uint32_t)s.cursor_show);
            return true;
        }
        if (name == "GetKeyState" || name == "GetAsyncKeyState") {
            m.ret(1, 0); // headless: no key held
            return true;
        }
        if (name == "GetCursorPos") { // POINT* → (0,0)
            uint32_t p = m.arg(0);
            m.write32(p + 0, 0);
            m.write32(p + 4, 0);
            m.ret(1, 1);
            return true;
        }
        if (name == "ClientToScreen" || name == "ScreenToClient") {
            m.ret(2, 1); // identity mapping (window at screen origin)
            return true;
        }
        if (name == "GetWindowLongA") {
            // hwnd, index. GWL_WNDPROC=-4, GWL_STYLE=-16, GWL_EXSTYLE=-20,
            // GWL_USERDATA=-21. Unknown indices read as 0.
            int32_t idx = (int32_t)m.arg(1);
            uint32_t v = idx == -4 ? s.wndproc
                       : idx == -16 ? s.style
                       : idx == -20 ? s.exstyle
                       : idx == -21 ? s.userdata
                                    : 0;
            m.ret(2, v);
            return true;
        }
        if (name == "SetWindowLongA") {
            // hwnd, index, newval → returns the previous value. GWL_WNDPROC
            // redirects the dispatch target (window subclassing).
            int32_t idx = (int32_t)m.arg(1);
            uint32_t nv = m.arg(2), old;
            switch (idx) {
                case -4:  old = s.wndproc;  s.wndproc = nv;  break;
                case -16: old = s.style;    s.style = nv;    break;
                case -20: old = s.exstyle;  s.exstyle = nv;  break;
                case -21: old = s.userdata; s.userdata = nv; break;
                default:  old = 0;                           break;
            }
            m.ret(3, old);
            return true;
        }
        if (name == "MessageBoxA") {
            m.ret(4, 1); // IDOK — a game's error dialog "OK"s and continues
            return true;
        }
        if (name == "SetTimer") { m.ret(4, m.arg(1) ? m.arg(1) : 1); return true; }
        if (name == "KillTimer") { m.ret(2, 1); return true; }
        if (name == "GetForegroundWindow" || name == "GetActiveWindow" ||
            name == "GetDesktopWindow") {
            m.ret(0, kFakeHwnd);
            return true;
        }
        if (name == "SetFocus" || name == "SetActiveWindow") {
            m.ret(1, kFakeHwnd); // previous focus/active window
            return true;
        }
        if (name == "SetForegroundWindow" || name == "BringWindowToTop" ||
            name == "SetWindowTextA" || name == "EnableWindow" ||
            name == "MoveWindow" || name == "SetWindowPos") {
            uint32_t nargs = name == "SetWindowPos" ? 7
                           : name == "MoveWindow"   ? 6
                           : name == "SetWindowTextA" || name == "EnableWindow" ? 2
                                                      : 1;
            m.ret(nargs, 1);
            return true;
        }

        // ---- message-pump adjuncts (RTW batch 4) ----
        if (name == "GetQueueStatus") {
            // (flags) → high word: kinds currently in queue; low: since last
            // call. Report QS_ALLEVENTS when anything is queued.
            uint32_t qs = s.queue.empty() ? 0 : 0x04BF;
            m.ret(1, (qs << 16) | qs);
            return true;
        }
        if (name == "PostThreadMessageA") {
            // (idThread, Msg, wParam, lParam): single thread → the one queue.
            bool full = s.queue.size() >= kQueueMax;
            if (!full) post_message(0, m.arg(1), m.arg(2), m.arg(3));
            m.ret(4, full ? 0 : 1);
            return true;
        }
        if (name == "MsgWaitForMultipleObjects") {
            // (nCount, pHandles, fWaitAll, dwMilliseconds, dwWakeMask).
            // Headless tier 0: a queued message reports WAIT_OBJECT_0+nCount;
            // finite timeout with nothing queued reports WAIT_TIMEOUT; INFINITE
            // with an empty queue would block forever — error loudly (same
            // starved-pump policy as GetMessageA).
            uint32_t n = m.arg(0), timeout = m.arg(3);
            if (!s.queue.empty()) { m.ret(5, n); return true; }
            if (timeout != 0xFFFFFFFFu) { m.ret(5, 0x102 /*WAIT_TIMEOUT*/); return true; }
            throw runtime::MachineError{
                "MsgWaitForMultipleObjects INFINITE on empty queue "
                "(tier-0 headless has no event source)"};
        }
        if (name == "RegisterWindowMessageA") {
            // Intern the name: the same string always yields the same id in the
            // real RegisterWindowMessage range (0xC000..).
            std::string nm = m.read_cstr(m.arg(0));
            for (size_t i = 0; i < s.wm_names.size(); i++)
                if (s.wm_names[i] == nm) { m.ret(1, 0xC000 + (uint32_t)i); return true; }
            if (s.wm_names.size() >= 0x3FFF) { m.ret(1, 0); return true; } // range cap
            s.wm_names.push_back(nm);
            m.ret(1, 0xC000 + (uint32_t)(s.wm_names.size() - 1));
            return true;
        }
        if (name == "GetKeyboardState") {
            // (PBYTE lpKeyState[256]): headless — no key down.
            uint32_t p = m.arg(0);
            for (uint32_t off = 0; off < 256; off += 4) m.write32(p + off, 0);
            m.ret(1, 1);
            return true;
        }
        if (name == "SetKeyboardState") { m.ret(1, 1); return true; }
        if (name == "SetCursorPos") { m.ret(2, 1); return true; }
        if (name == "GetDoubleClickTime") { m.ret(0, 500); return true; }

        // ---- gdi32: display caps ----
        if (name == "GetDeviceCaps") {
            // (hdc, index): the display metrics an era game gates on.
            uint32_t idx = m.arg(1), v = 0;
            switch (idx) {
                case 8:  v = 1024; break; // HORZRES
                case 10: v = 768; break;  // VERTRES
                case 12: v = 32; break;   // BITSPIXEL
                case 14: v = 1; break;    // PLANES
                case 88: v = 96; break;   // LOGPIXELSX
                case 90: v = 96; break;   // LOGPIXELSY
                case 116: v = 60; break;  // VREFRESH
                default: break;
            }
            m.ret(2, v);
            return true;
        }

        // ---- winmm: multimedia timing ----
        if (name == "timeGetTime") {
            m.ret(0, (uint32_t)(m.cpu().icount / 1000)); // same clock as GetTickCount
            return true;
        }
        if (name == "timeBeginPeriod" || name == "timeEndPeriod") {
            m.ret(1, 0); // TIMERR_NOERROR
            return true;
        }
        if (name == "timeSetEvent") {
            // (uDelay, uResolution, lpTimeProc, dwUser, fuEvent) → a nonzero
            // timer id. Timers never fire in headless tier 0; the rAF pump work
            // (F3) gives them a real clock. If a game hangs waiting on one, the
            // stub log will say so.
            m.ret(5, ++s.mm_timers);
            return true;
        }
        if (name == "timeKillEvent") { m.ret(1, 0); return true; }

        // Cosmetic no-ops the corpus/RTW call during window setup.
        if (name == "ShowWindow" || name == "UpdateWindow" ||
            name == "DestroyWindow") {
            m.ret(name == "ShowWindow" ? 2 : 1, 1);
            return true;
        }
        return false;
    });
}

} // namespace u32web
