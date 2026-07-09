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
#include <cstring>
#include <vector>

#include "d9web/d9web.h"
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

constexpr uint32_t WM_SELF = 0x0400 + 7;

// recurse.exe mode: a self-SendMessaging WndProc must trip the depth guard and
// raise a MachineError instead of overflowing the host stack.
static int run_recurse_guard(std::vector<uint8_t>& exe) {
    Machine m(64u << 20);
    m.load(exe.data(), exe.size());
    k32web::install(m);
    u32web::install(m);
    u32web::post_message(HWND_TOKEN, WM_SELF, 0, 0);
    try {
        m.run_entry();
    } catch (const runtime::MachineError& e) {
        printf("PASS recurse.exe — depth guard tripped cleanly: %s\n",
               e.what.c_str());
        return 0;
    }
    fprintf(stderr, "FAIL: unbounded recursion did not raise MachineError\n");
    return 1;
}

constexpr uint32_t WM_PAINT = 0x000F;

// paint mode: seed WM_PAINT, run, then dump the client framebuffer as a binary
// PPM (P6) so the pixels the guest drew can be inspected/screenshotted.
static int run_paint(std::vector<uint8_t>& exe, const char* ppm_out) {
    Machine m(64u << 20);
    m.load(exe.data(), exe.size());
    k32web::install(m);
    u32web::install(m);
    u32web::post_message(HWND_TOKEN, WM_PAINT, 0, 0);
    m.run_entry();

    uint32_t w = 0, h = 0;
    const uint32_t* fb = u32web::framebuffer(w, h);
    if (!fb) {
        fprintf(stderr, "FAIL: no framebuffer (window never created)\n");
        return 1;
    }
    if (ppm_out) {
        FILE* f = fopen(ppm_out, "wb");
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t p = fb[i];
            uint8_t rgb[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    // Spot-check a couple of gradient pixels: (40,40)=RGB(0,0,128) blue,
    // (40+255,40+127)=RGB(255,254,128).
    uint32_t p0 = fb[40u * w + 40], p1 = fb[(40u + 127) * w + (40 + 255)];
    printf("fb %ux%u  px(40,40)=%06x px(295,167)=%06x\n", w, h, p0 & 0xFFFFFF,
           p1 & 0xFFFFFF);
    bool ok = (p0 & 0xFFFFFF) == 0x000080 && (p1 & 0xFFFFFF) == 0xFFFE80;
    printf("%s paint.exe — guest GDI reached the framebuffer\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// d3d mode: run a D3D9 program (no message pump needed) and inspect the device
// backbuffer d9web presented. Spot-checks the clear color (RGB 0x336699).
static int run_d3d(std::vector<uint8_t>& exe, const char* ppm_out) {
    Machine m(64u << 20);
    m.load(exe.data(), exe.size());
    k32web::install(m);
    u32web::install(m);
    d9web::install(m);
    m.run_entry();

    uint32_t w = 0, h = 0;
    const uint32_t* fb = d9web::framebuffer(w, h);
    if (!fb) {
        fprintf(stderr, "FAIL: no presented D3D frame\n");
        return 1;
    }
    if (ppm_out) {
        FILE* f = fopen(ppm_out, "wb");
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t p = fb[i];
            uint8_t rgb[3] = {(uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
    uint32_t px = fb[0] & 0xFFFFFF;
    printf("d3d %ux%u clear=%06x\n", w, h, px);
    bool ok = px == 0x336699;
    printf("%s d3dclear.exe — D3D9 COM Clear reached the backbuffer\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    bool recurse = false, paint = false, d3d = false;
    const char* paint_out = nullptr;
    const char* path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--recurse-guard")) recurse = true;
        else if (!strcmp(argv[i], "--paint")) paint = true;
        else if (!strcmp(argv[i], "--d3d")) d3d = true;
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) paint_out = argv[++i];
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: window_test [--recurse-guard|--paint|--d3d [--ppm F]] exe\n");
        return 2;
    }
    std::vector<uint8_t> exe = slurp(path);

    if (recurse) return run_recurse_guard(exe);
    if (paint) return run_paint(exe, paint_out);
    if (d3d) return run_d3d(exe, paint_out);

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
