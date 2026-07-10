// RTW Web Worker: loads the zhweb WASM, boots RomeTW.exe, and runs it in slices.
// Game assets are pulled lazily over HTTP by the WASM (synchronous XHR — allowed
// here in a Worker). Frames are transferred to the main thread for the canvas.
let booted = false;
const SLICE_M = 40; // million instructions per slice (keeps the worker responsive)

// Predefine Module BEFORE loading the WASM glue so the ready hook is registered.
var Module = {
  onRuntimeInitialized: () => postMessage({ type: 'ready' }),
  print: (s) => postMessage({ type: 'log', line: s }),
  printErr: (s) => postMessage({ type: 'log', line: s }),
};
importScripts('zhweb.js');

function cstr(s) {
  const bytes = new TextEncoder().encode(s + '\0');
  const p = Module._malloc(bytes.length);
  Module.HEAPU8.set(bytes, p);
  return p;
}

function pump() {
  if (!booted) return;
  const status = Module._zhweb_slice(SLICE_M);
  const ptr = Module._zhweb_fb_ptr();
  const w = Module._zhweb_fb_width(), h = Module._zhweb_fb_height();
  const icount = Module._zhweb_icount();
  if (ptr && w && h) {
    // Copy the ARGB framebuffer out of WASM memory (its heap may move on growth).
    const frame = Module.HEAPU32.slice(ptr >> 2, (ptr >> 2) + w * h);
    postMessage({ type: 'frame', w, h, icount, buf: frame.buffer }, [frame.buffer]);
  } else {
    postMessage({ type: 'progress', icount });
  }
  if (status === -3) setTimeout(pump, 0);      // still running
  else postMessage({ type: 'done', code: status });
}

onmessage = async (e) => {
  const msg = e.data;
  if (msg.type === 'boot') {
    postMessage({ type: 'progress', icount: 0, note: 'fetching RomeTW.exe' });
    const exe = new Uint8Array(await (await fetch(msg.exeURL)).arrayBuffer());
    const p = Module._malloc(exe.length);
    Module.HEAPU8.set(exe, p);
    const base = cstr(msg.assetBase);
    const rc = Module._zhweb_boot(p, exe.length, base, SLICE_M);
    Module._free(p); Module._free(base);
    if (rc < 0) { postMessage({ type: 'error', msg: 'RomeTW.exe failed to load (rc=' + rc + ')' }); return; }
    booted = true;
    pump();
  }
};
