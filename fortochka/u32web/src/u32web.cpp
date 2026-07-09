#include "u32web/u32web.h"

#include <deque>
#include <string>

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

// Single-window state. One process, one message queue — enough for the pump
// round-trip and for RTW's single top-level window later.
struct State {
    uint32_t wndproc = 0; // guest VA of the registered WNDPROC
    std::deque<Msg> queue;
    bool class_registered = false;
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

void post_message(uint32_t hwnd, uint32_t message, uint32_t wparam,
                  uint32_t lparam) {
    g_state.queue.push_back({hwnd, message, wparam, lparam});
}

void install(Machine& m) {
    g_state = State{};

    m.add_handler([](Machine& m, const std::string& dll,
                     const std::string& name) -> bool {
        if (dll != "user32.dll") return false;
        State& s = g_state;

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
            // Return our single HWND. A real event source would start here;
            // the harness seeds the queue via post_message before run.
            unsigned nargs = name == "CreateWindowExA" ? 12 : 11;
            m.ret(nargs, kFakeHwnd);
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
            uint32_t lpmsg = m.arg(0);
            if (s.queue.empty()) {
                // No real blocking source in tier 0: treat empty as quit.
                write_msg(m, lpmsg, {kFakeHwnd, WM_QUIT, 0, 0});
                m.ret(4, 0);
                return true;
            }
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
            post_message(m.arg(0), m.arg(1), m.arg(2), m.arg(3));
            m.ret(4, 1);
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
