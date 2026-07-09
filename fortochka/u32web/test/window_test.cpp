// window_test — drives corpus/bin/window.exe through the full message pump and
// asserts the reverse thunk actually re-entered the guest WndProc.
//
//   window_test path/to/window.exe
//
// Seeds the queue with WM_USER_ADD(10), WM_USER_ADD(20), WM_CLOSE. A correct
// pump reverse-thunks each into WndProc: the two adds make g_acc = 30, WM_CLOSE
// posts quit(30), the loop ends, and the process exits 30. Any break in the
// host→guest→host chain yields a different code or a fault.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime/machine.h"
#include "u32web/u32web.h"

using runtime::Machine;

namespace {
constexpr uint32_t WM_USER_ADD = 0x0400 + 1;
constexpr uint32_t WM_CLOSE = 0x0010;
constexpr uint32_t HWND_TOKEN = 0x00010001;
constexpr int EXPECTED = 30;

std::vector<uint8_t> slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)n);
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { perror(path); exit(2); }
    fclose(f);
    return buf;
}
} // namespace

// k32web is linked in for ExitProcess; declare its installer inline to avoid a
// header dependency churn (it lives in k32web/).
namespace k32web { void install(runtime::Machine& m); }

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: window_test window.exe\n");
        return 2;
    }
    std::vector<uint8_t> exe = slurp(argv[1]);

    Machine m(64u << 20);
    try {
        m.load(exe.data(), exe.size());
    } catch (const peload::LoadError& e) {
        fprintf(stderr, "peload: %s\n", e.what());
        return 2;
    }

    k32web::install(m);
    u32web::install(m);

    // Seed the message script BEFORE run: the pump will drain it.
    u32web::post_message(HWND_TOKEN, WM_USER_ADD, 10, 0);
    u32web::post_message(HWND_TOKEN, WM_USER_ADD, 20, 0);
    u32web::post_message(HWND_TOKEN, WM_CLOSE, 0, 0);

    int code;
    try {
        code = m.run_entry();
    } catch (const runtime::MachineError& e) {
        fprintf(stderr, "FAIL: machine error: %s\n", e.what.c_str());
        return 1;
    }

    printf("exit_code=%d icount=%" PRIu64 "\n", code, m.cpu().icount);
    if (code != EXPECTED) {
        fprintf(stderr, "FAIL: expected exit %d, got %d\n", EXPECTED, code);
        return 1;
    }
    printf("PASS window.exe — reverse thunk round trip (guest WndProc ran)\n");
    return 0;
}
