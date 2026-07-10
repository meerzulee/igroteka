// RTW Web Worker: loads the zhweb WASM, boots RomeTW.exe, and runs it in slices.
// Game assets are pulled lazily over HTTP by the WASM (synchronous XHR — allowed
// here in a Worker). Frames are transferred to the main thread for the canvas.
let booted = false;
const SLICE_M = 40; // million instructions per slice (keeps the worker responsive)

function say(line) { postMessage({ type: 'log', line }); }

// Live asset log: every game file RTW pulls goes through a synchronous XHR here,
// so wrap open() to surface it (the guest's own stderr routing is unreliable in
// the -O2 WASM). One line per fetch, with the byte size once it completes.
const _open = XMLHttpRequest.prototype.open;
const _send = XMLHttpRequest.prototype.send;
XMLHttpRequest.prototype.open = function (method, url) {
  this.__url = url;
  return _open.apply(this, arguments);
};
XMLHttpRequest.prototype.send = function () {
  const r = _send.apply(this, arguments);
  const u = (this.__url || '').replace('/game-data/RomeTW/', '');
  if (this.status === 200 || this.status === 0) {
    const n = this.response && this.response.byteLength !== undefined ? this.response.byteLength : 0;
    say('load ' + u + (n ? '  (' + (n / 1024 | 0) + ' KB)' : ''));
  } else if (this.status === 404) {
    say('miss ' + u);
  }
  return r;
};

// Predefine Module BEFORE loading the WASM glue so the hooks are registered.
var Module = {
  onRuntimeInitialized: () => postMessage({ type: 'ready' }),
  print: (s) => say(s),
  printErr: (s) => say(s),
  onAbort: (what) => postMessage({ type: 'error', msg: 'WASM abort: ' + what }),
  instantiateWasm: undefined,
};

say('worker: loading zhweb.js …');
try {
  importScripts('zhweb.js');
  say('worker: zhweb.js loaded, initializing WASM …');
} catch (e) {
  postMessage({ type: 'error', msg: 'importScripts(zhweb.js) failed: ' + e });
}

function cstr(s) {
  const bytes = new TextEncoder().encode(s + '\0');
  const p = Module._malloc(bytes.length);
  Module.HEAPU8.set(bytes, p);
  return p;
}

function pump() {
  if (!booted) return;
  let status;
  try {
    status = Module._zhweb_slice(SLICE_M);
  } catch (e) {
    postMessage({ type: 'error', msg: 'slice crashed: ' + e });
    return;
  }
  const ptr = Module._zhweb_fb_ptr();
  const w = Module._zhweb_fb_width(), h = Module._zhweb_fb_height();
  const icount = Module._zhweb_icount();
  if (ptr && w && h) {
    const frame = Module.HEAPU32.slice(ptr >> 2, (ptr >> 2) + w * h);
    postMessage({ type: 'frame', w, h, icount, buf: frame.buffer }, [frame.buffer]);
  } else {
    postMessage({ type: 'progress', icount });
  }
  if (status === -3) setTimeout(pump, 0);          // still running (Machine::RUNNING)
  else postMessage({ type: 'done', code: status, icount });
}

onmessage = async (e) => {
  const msg = e.data;
  if (msg.type !== 'boot') return;
  try {
    postMessage({ type: 'progress', icount: 0, note: 'fetching RomeTW.exe' });
    const resp = await fetch(msg.exeURL);
    if (!resp.ok) { postMessage({ type: 'error', msg: 'RomeTW.exe HTTP ' + resp.status + ' at ' + msg.exeURL }); return; }
    const exe = new Uint8Array(await resp.arrayBuffer());
    say('worker: RomeTW.exe fetched (' + exe.length + ' bytes), booting …');
    const p = Module._malloc(exe.length);
    Module.HEAPU8.set(exe, p);
    const base = cstr(msg.assetBase);
    const rc = Module._zhweb_boot(p, exe.length, base, SLICE_M);
    Module._free(p); Module._free(base);
    if (rc < 0) { postMessage({ type: 'error', msg: 'zhweb_boot failed rc=' + rc + ' (PE load)' }); return; }
    booted = true;
    pump();
  } catch (e) {
    postMessage({ type: 'error', msg: 'boot exception: ' + e });
  }
};
