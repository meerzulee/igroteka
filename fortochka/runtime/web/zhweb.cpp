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

// The three host-FS shims below are serviced by the fs-worker over a
// SharedArrayBuffer (see worker.js + fsworker.js). Each reads the C path
// byte-by-byte (paths are ASCII; avoids UTF8ToString -> TextDecoder, which
// throws on ALLOW_MEMORY_GROWTH's resizable buffer) and calls the synchronous
// bridge globals defined in worker.js. The bridge blocks on Atomics until the
// fs-worker (which owns async OPFS) answers, so these return synchronously.

// Read a whole OPFS file. Returns 1 + malloc'd bytes (caller frees) via *out/*len,
// or 0 with *len<0 on a miss. Large files are pulled in DATA_BYTES chunks.
extern "C" EM_JS(int, zhweb_host_fetch, (const char* url, unsigned char** out, int* len), {
  var path = '';
  for (var k = url; HEAPU8[k] !== 0; k++) path += String.fromCharCode(HEAPU8[k]);
  var size = zhwebFsStat(path);
  if (size < 0) { setValue(len, -1, 'i32'); return 0; }
  var p = _malloc(size || 1);
  var off = 0;
  while (off < size) {
    var n = zhwebFsRead(path, off, Math.min(zhwebFsChunk, size - off));
    if (n < 0) { _free(p); setValue(len, -1, 'i32'); return 0; }
    if (n === 0) break; // EOF short read
    HEAPU8.set(zhwebFsData.subarray(0, n), p + off);
    off += n;
  }
  setValue(out, p, 'i32');
  setValue(len, off, 'i32');
  return 1;
});

// Existence probe. Returns 0 (absent), 1 (a file), or 2 (a directory).
extern "C" EM_JS(int, zhweb_host_exists, (const char* url), {
  var path = '';
  for (var k = url; HEAPU8[k] !== 0; k++) path += String.fromCharCode(HEAPU8[k]);
  return zhwebFsExists(path);
});

// List a directory. Returns the entry count (or -1 if absent) and, via *out/*len,
// a malloc'd buffer of packed entries (caller frees). Packed format per entry
// (little-endian): u8 type(1=file,2=dir), u32 size, u16 nameLen, u8[nameLen] name.
extern "C" EM_JS(int, zhweb_host_listdir, (const char* url, unsigned char** out, int* len), {
  var path = '';
  for (var k = url; HEAPU8[k] !== 0; k++) path += String.fromCharCode(HEAPU8[k]);
  var count = zhwebFsListdir(path);
  if (count < 0) { setValue(len, -1, 'i32'); return -1; }
  var bytes = zhwebFsListBytes;
  var p = _malloc(bytes || 1);
  if (bytes > 0) HEAPU8.set(zhwebFsData.subarray(0, bytes), p);
  setValue(out, p, 'i32');
  setValue(len, bytes, 'i32');
  return count;
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
    // leaves ~1.45GB of guest heap above the image; memory grows to fit it plus
    // the ~724MB asset VFS.
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
