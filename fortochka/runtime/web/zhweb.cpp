// zhweb — Emscripten entry point. Two modes:
//   - zhweb_run(): the original one-shot demo runner (upload a small PE, run it
//     to completion, grab the frame).
//   - zhweb_boot()/zhweb_slice(): RTW mode. A persistent Machine boots RomeTW.exe
//     and runs in slices so the Web Worker can blit a frame + pump input between
//     them (RTW's main loop never returns). Game assets are pulled lazily over
//     HTTP via a synchronous XHR (zhweb_host_fetch, called from k32web::host_load
//     — Workers allow sync XHR).
#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "d9web/d9web.h"
#include "k32web/k32web.h"
#include "runtime/machine.h"
#include "sysweb/sysweb.h"
#include "u32web/u32web.h"

using runtime::Machine;

// Synchronous BYTE-EXACT fetch. In a Web Worker a synchronous XHR may set
// responseType='arraybuffer', which gives the raw bytes regardless of the file's
// apparent encoding (the text path mis-decodes UTF-16/BOM files, halving them).
// Returns 1 + malloc'd bytes on HTTP 200, 0 (len=-1) on a miss.
extern "C" EM_JS(int, zhweb_host_fetch, (const char* url, unsigned char** out, int* len), {
  var path = UTF8ToString(url);
  try {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', path, false); // synchronous — Web Worker only
    try { xhr.responseType = 'arraybuffer'; } catch (e) {}
    xhr.send();
    if (xhr.status !== 200 && xhr.status !== 0) { setValue(len, -1, 'i32'); return 0; }
    var bytes;
    if (xhr.response && xhr.response.byteLength !== undefined) {
      bytes = new Uint8Array(xhr.response);          // byte-exact
    } else {                                          // fallback (no BOM only)
      var s = xhr.responseText, n = s.length;
      bytes = new Uint8Array(n);
      for (var i = 0; i < n; i++) bytes[i] = s.charCodeAt(i) & 0xff;
    }
    var p = _malloc(bytes.length || 1);
    HEAPU8.set(bytes, p);
    setValue(out, p, 'i32');
    setValue(len, bytes.length, 'i32');
    return 1;
  } catch (e) {
    setValue(len, -1, 'i32');
    return 0;
  }
});

namespace {
std::unique_ptr<Machine> g_m;
uint32_t g_fb_w = 0, g_fb_h = 0;
const uint32_t* g_fb = nullptr;
int g_exit = -3; // -3 = still running
} // namespace

extern "C" {

// One-shot demo runner (unchanged behaviour).
EMSCRIPTEN_KEEPALIVE
int zhweb_run(const uint8_t* file, int len, int arena_mb) {
    if (arena_mb <= 0) arena_mb = 64;
    g_fb = nullptr;
    g_fb_w = g_fb_h = 0;
    Machine m((uint32_t)arena_mb << 20);
    try {
        const auto& img = m.load(file, (size_t)len);
        printf("zhweb: base=0x%x entry=0x%x imports=%zu\n", img.base, img.entry,
               img.imports.size());
    } catch (const peload::LoadError& e) {
        printf("peload: %s\n", e.what());
        return -1;
    }
    k32web::install(m);
    u32web::install(m);
    d9web::install(m);
    sysweb::install(m);
    u32web::post_message(0x00010001, 0x000F /*WM_PAINT*/, 0, 0);
    int code;
    try {
        code = m.run_entry();
    } catch (const runtime::MachineError& e) {
        printf("zhweb: %s\n", e.what.c_str());
        code = -2;
    }
    g_fb = d9web::framebuffer(g_fb_w, g_fb_h);
    if (!g_fb) g_fb = u32web::framebuffer(g_fb_w, g_fb_h);
    return code;
}

// RTW mode: boot RomeTW.exe (its bytes are uploaded into WASM memory) and mount
// the asset directory at `url_base` (fetched lazily over HTTP). Runs the first
// slice of `slice_m` million instructions. Returns 0 (booting) or -1 on load
// failure.
EMSCRIPTEN_KEEPALIVE
int zhweb_boot(const uint8_t* exe, int len, const char* url_base, int slice_m) {
    g_fb = nullptr;
    g_fb_w = g_fb_h = 0;
    g_exit = -3;
    // RTW loads its whole menu 3D-background scene (building models, unit db,
    // animations) onto a never-freed bump heap, so it needs a big arena. 1.5GB
    // leaves ~1.45GB of guest heap above the image.
    g_m = std::make_unique<Machine>(1536u << 20);
    try {
        g_m->load(exe, (size_t)len);
    } catch (const peload::LoadError& e) {
        printf("peload: %s\n", e.what());
        g_m.reset();
        return -1;
    }
    k32web::install(*g_m);
    u32web::install(*g_m);
    d9web::install(*g_m);
    sysweb::install(*g_m);
    k32web::mount_host_dir("c:/rtw", url_base ? url_base : "");
    if (slice_m <= 0) slice_m = 100;
    try {
        g_exit = g_m->run_entry((uint64_t)slice_m * 1000000ull);
    } catch (const runtime::MachineError& e) {
        printf("zhweb: %s\n", e.what.c_str());
        g_exit = -2;
    }
    return 0;
}

// Run one more slice. Returns -3 (still running), >=0 (exit code), -2 (fault).
EMSCRIPTEN_KEEPALIVE
int zhweb_slice(int slice_m) {
    if (!g_m || g_exit != -3) return g_exit;
    if (slice_m <= 0) slice_m = 100;
    try {
        g_exit = g_m->run_more((uint64_t)slice_m * 1000000ull);
    } catch (const runtime::MachineError& e) {
        printf("zhweb: %s\n", e.what.c_str());
        g_exit = -2;
    }
    return g_exit;
}

// Latest presented D3D frame (GDI fallback). Call zhweb_fb_ptr first — it
// refreshes the cached width/height.
EMSCRIPTEN_KEEPALIVE const uint32_t* zhweb_fb_ptr() {
    g_fb = d9web::framebuffer(g_fb_w, g_fb_h);
    if (!g_fb) g_fb = u32web::framebuffer(g_fb_w, g_fb_h);
    return g_fb;
}
EMSCRIPTEN_KEEPALIVE int zhweb_fb_width() { return (int)g_fb_w; }
EMSCRIPTEN_KEEPALIVE int zhweb_fb_height() { return (int)g_fb_h; }
EMSCRIPTEN_KEEPALIVE double zhweb_icount() {
    return g_m ? (double)g_m->proc_ticks() : 0.0;
}

} // extern "C"
