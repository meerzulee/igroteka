// RTW interpreter worker (OPFS build). Loads the zhweb WASM, boots RomeTW.exe,
// and runs it in slices. Game assets live in OPFS (imported once via import.html).
//
// The wasm makes SYNCHRONOUS file calls from deep inside the interpreter. OPFS is
// async, so a dedicated fs-worker (fsworker.js) owns OPFS access and we bridge to
// it over a SharedArrayBuffer with Atomics (Approach B). This worker exposes four
// synchronous globals — zhwebFsExists/Stat/Read/Listdir — that block on the SAB;
// the EM_JS shims in zhweb.cpp call them. Requires COOP/COEP (see serve.py) for
// SharedArrayBuffer.
importScripts('fsproto.js'); // -> FSPROTO
const P = FSPROTO;

let booted = false;
const SLICE_M = 40; // million instructions per slice

function say(line) { postMessage({ type: 'log', line }); }

// ---- SAB bridge to the fs-worker --------------------------------------------
const ctrlSAB = new SharedArrayBuffer(P.CTRL_LEN * 4);
const dataSAB = new SharedArrayBuffer(P.DATA_BYTES);
const ctrl = new Int32Array(ctrlSAB);
const data = new Uint8Array(dataSAB);
const enc = new TextEncoder();
let fsw = null;
let fsReady = false;

// Expose the data view for the EM_JS READ/LISTDIR shims to copy out of.
globalThis.zhwebFsData = data;

// Write the request path, signal the fs-worker, block until it responds.
// Returns ctrl[STATUS] (op-specific). Never returns without a response (the
// fs-worker force-RESPs on error), so the interpreter can't hang.
function fsCall(op, path, offset, maxlen) {
  const bytes = enc.encode(path);
  if (bytes.length > P.DATA_BYTES) return -1; // absurd path
  data.set(bytes, 0);
  Atomics.store(ctrl, P.PATH_LEN, bytes.length);
  Atomics.store(ctrl, P.OP, op);
  Atomics.store(ctrl, P.OFFSET, offset | 0);
  Atomics.store(ctrl, P.MAXLEN, maxlen | 0);
  Atomics.store(ctrl, P.SIGNAL, P.REQ);
  fsw.postMessage('go');
  Atomics.wait(ctrl, P.SIGNAL, P.REQ); // -> RESP
  return Atomics.load(ctrl, P.STATUS);
}

// Synchronous bridge API used by the EM_JS shims (see zhweb.cpp).
globalThis.zhwebFsExists = (path) => fsCall(P.OP_EXISTS, path, 0, 0);   // 0|1(file)|2(dir)
globalThis.zhwebFsStat = (path) => fsCall(P.OP_STAT, path, 0, 0);       // size | -1
globalThis.zhwebFsRead = (path, off, len) =>                            // bytes in data[0..n)
  fsCall(P.OP_READ, path, off, Math.min(len, P.DATA_BYTES));
globalThis.zhwebFsListdir = (path) => {                                 // -> count (bytes in EXTRA)
  const count = fsCall(P.OP_LISTDIR, path, 0, 0);
  globalThis.zhwebFsListBytes = Atomics.load(ctrl, P.EXTRA);
  return count;
};
globalThis.zhwebFsChunk = P.DATA_BYTES;

// ---- read a whole OPFS file async (for the exe, before boot) -----------------
async function opfsReadFile(relPath) {
  const root = await navigator.storage.getDirectory();
  let dir = await root.getDirectoryHandle('rtw');
  const segs = relPath.split('/').filter(Boolean);
  const base = segs.pop();
  for (const s of segs) dir = await dir.getDirectoryHandle(s);
  const fh = await dir.getFileHandle(base);
  return new Uint8Array(await (await fh.getFile()).arrayBuffer());
}

// ---- wasm module -------------------------------------------------------------
var Module = {
  onRuntimeInitialized: () => { wasmReady = true; tryBoot(); },
  print: (s) => say(s),
  printErr: (s) => say(s),
  onAbort: (what) => postMessage({ type: 'error', msg: 'WASM abort: ' + what }),
};
let wasmReady = false;

say('worker: spawning fs-worker …');
fsw = new Worker('fsworker.js');
fsw.onmessage = (e) => {
  const m = e.data;
  if (m && m.type === 'ready') { fsReady = true; say('fs-worker: OPFS ready'); tryBoot(); }
  else if (m && m.type === 'error') {
    // Not imported yet (or partial import) — tell the page to show the importer.
    postMessage({ type: 'needimport', msg: m.msg });
  }
};
fsw.onerror = (e) => postMessage({ type: 'error', msg: 'fs-worker error: ' + (e.message || e) });
fsw.postMessage({ type: 'init', ctrl: ctrlSAB, data: dataSAB });

say('worker: loading zhweb.js …');
try { importScripts('zhweb.js'); }
catch (e) { postMessage({ type: 'error', msg: 'importScripts(zhweb.js) failed: ' + e }); }

function cstr(s) {
  const bytes = enc.encode(s + '\0');
  const p = Module._malloc(bytes.length);
  Module.HEAPU8.set(bytes, p);
  return p;
}

let bootStarted = false;
async function tryBoot() {
  if (bootStarted || !fsReady || !wasmReady) return;
  bootStarted = true;
  try {
    postMessage({ type: 'ready' });
    say('worker: reading RomeTW.exe from OPFS …');
    const exe = await opfsReadFile('RomeTW.exe');
    say('worker: RomeTW.exe (' + exe.length + ' bytes), booting …');
    const p = Module._malloc(exe.length);
    Module.HEAPU8.set(exe, p);
    const base = cstr('rtw'); // OPFS root dir name; host_path_for prepends it
    const rc = Module._zhweb_boot(p, exe.length, base, SLICE_M);
    Module._free(p); Module._free(base);
    if (rc < 0) { postMessage({ type: 'error', msg: 'zhweb_boot failed rc=' + rc }); return; }
    booted = true;
    pump();
  } catch (e) {
    postMessage({ type: 'error', msg: 'boot exception: ' + e });
  }
}

function pump() {
  if (!booted) return;
  let status;
  try { status = Module._zhweb_slice(SLICE_M); }
  catch (e) { postMessage({ type: 'error', msg: 'slice crashed: ' + e }); return; }
  const ptr = Module._zhweb_fb_ptr();
  const w = Module._zhweb_fb_width(), h = Module._zhweb_fb_height();
  const icount = Module._zhweb_icount();
  if (ptr && w && h) {
    const frame = Module.HEAPU32.slice(ptr >> 2, (ptr >> 2) + w * h);
    postMessage({ type: 'frame', w, h, icount, buf: frame.buffer }, [frame.buffer]);
  } else {
    postMessage({ type: 'progress', icount });
  }
  if (status === -3) setTimeout(pump, 0);
  else postMessage({ type: 'done', code: status, icount });
}
