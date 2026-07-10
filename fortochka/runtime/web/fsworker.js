// fsworker.js — OPFS side of the synchronous filesystem bridge.
//
// The interpreter worker (worker.js) makes blocking file calls from inside the
// wasm guest. It writes a request into two SharedArrayBuffers (see fsproto.js),
// posts 'go' to this worker, and parks in Atomics.wait. This worker owns all
// OPFS access: it does the async work, writes the result back into the shared
// buffers, then Atomics.store(RESP) + Atomics.notify to wake the caller.
//
// Data lives under the OPFS directory "rtw", populated by import.html. OPFS
// entry names preserve the game folder's EXACT case; incoming request paths
// are lowercase-normalized (Win32 semantics), so every path segment is matched
// case-insensitively against a lazily built per-directory index.
//
// This file must be loaded as a CLASSIC worker (no {type:'module'}) so that
// importScripts can pull in the shared protocol constants.

'use strict';

importScripts('fsproto.js'); // defines FSPROTO (single source of truth)

const {
  SIGNAL, OP, PATH_LEN, OFFSET, MAXLEN, STATUS, EXTRA,
  REQ, RESP,
  OP_EXISTS, OP_STAT, OP_READ, OP_LISTDIR,
  DATA_BYTES,
} = FSPROTO;

const OPFS_ROOT_NAME = 'rtw';
const MARKER_NAME = '.import-complete';

// ---- shared state ---------------------------------------------------------

let ctrl = null; // Int32Array over the control SAB
let data = null; // Uint8Array over the data SAB
let dv = null;   // DataView over the data SAB (for LE u16/u32 writes)

let rtwRoot = null; // FileSystemDirectoryHandle of the "rtw" OPFS directory

const utf8dec = new TextDecoder();
const utf8enc = new TextEncoder();

// Directory index cache. 42k+ files: each directory is listed AT MOST ONCE.
//   key:   "/"-joined lowercase path of the directory below rtw ("" = rtw root)
//   value: { handle, entries: Map<lowercaseName,
//            { name /*exact case*/, handle, kind, size, file }> }
const dirCache = new Map();

// ---- OPFS resolution ------------------------------------------------------

async function ensureRoot() {
  if (rtwRoot) return rtwRoot;
  const opfsRoot = await navigator.storage.getDirectory();
  rtwRoot = await opfsRoot.getDirectoryHandle(OPFS_ROOT_NAME); // throws if absent
  return rtwRoot;
}

// Split a request path into segments below the rtw root.
// Accepts "rtw/data/foo.cas" or "data/foo.cas"; returns null for hostile paths.
function pathSegments(raw) {
  const segs = raw.split(/[\\/]+/).filter((s) => s.length > 0 && s !== '.');
  if (segs.includes('..')) return null; // never escape the rtw root
  if (segs.length > 0 && segs[0].toLowerCase() === OPFS_ROOT_NAME) segs.shift();
  return segs;
}

// Return the cached index for a directory handle, listing it on first touch.
async function indexOf(dirHandle, key) {
  let idx = dirCache.get(key);
  if (idx) return idx;
  const entries = new Map();
  for await (const [name, handle] of dirHandle.entries()) {
    entries.set(name.toLowerCase(), {
      name,               // exact-case basename, as stored by import.html
      handle,
      kind: handle.kind,  // 'file' | 'directory'
      size: undefined,    // lazily fetched via getFile()
      file: undefined,    // cached File snapshot (read-only store, safe)
    });
  }
  idx = { handle: dirHandle, entries };
  dirCache.set(key, idx);
  return idx;
}

// Resolve lowercase segments to a DIRECTORY index below rtw, or null.
async function resolveDirIndex(segs) {
  const root = await ensureRoot();
  let idx = await indexOf(root, '');
  let key = '';
  for (const rawSeg of segs) {
    const seg = rawSeg.toLowerCase();
    const rec = idx.entries.get(seg);
    if (!rec || rec.kind !== 'directory') return null;
    key = key ? key + '/' + seg : seg;
    idx = await indexOf(rec.handle, key);
  }
  return idx;
}

// Resolve segments to an entry record (file or dir), the rtw root itself
// (kind:'directory') for an empty path, or null if absent.
async function resolveEntry(segs) {
  if (segs.length === 0) return { kind: 'directory', root: true };
  const parent = await resolveDirIndex(segs.slice(0, -1));
  if (!parent) return null;
  return parent.entries.get(segs[segs.length - 1].toLowerCase()) || null;
}

// Fetch (once) and cache the File snapshot + size for a file entry.
async function fileOf(rec) {
  if (!rec.file) {
    rec.file = await rec.handle.getFile();
    rec.size = rec.file.size;
  }
  return rec.file;
}

// ---- ops --------------------------------------------------------------------

// OP_EXISTS -> 1 file, 2 directory, 0 absent
async function doExists(segs) {
  const rec = await resolveEntry(segs);
  if (!rec) return 0;
  return rec.kind === 'file' ? 1 : 2;
}

// OP_STAT -> file size in bytes, -1 if absent or a directory
async function doStat(segs) {
  const rec = await resolveEntry(segs);
  if (!rec || rec.kind !== 'file') return -1;
  if (rec.size === undefined) await fileOf(rec);
  let size = rec.size;
  if (size > 0x7fffffff) {
    console.warn('fsworker: STAT size > int32 for "' + rec.name + '" (' + size + '), clamping');
    size = 0x7fffffff;
  }
  return size;
}

// OP_READ -> bytes copied into data[0..n), -1 if absent/error; n < maxLen at EOF
async function doRead(segs, offset, maxLen) {
  const rec = await resolveEntry(segs);
  if (!rec || rec.kind !== 'file') return -1;
  const file = await fileOf(rec);
  const off = Math.max(0, offset);
  const len = Math.min(Math.max(0, maxLen), DATA_BYTES);
  const buf = await file.slice(off, off + len).arrayBuffer();
  const bytes = new Uint8Array(buf);
  data.set(bytes, 0);
  return bytes.length;
}

// OP_LISTDIR -> { count, bytes }: entries packed into data[0..bytes) as
//   u8 type (1=file, 2=dir) | u32 size LE (0 for dirs) | u16 nameLen LE | name
// count = -1 if the path is not a directory.
async function doListdir(segs) {
  const idx = await resolveDirIndex(segs);
  if (!idx) return { count: -1, bytes: 0 };

  // Hide the importer's completion marker from the game-root listing —
  // it is runtime metadata, not game data.
  const hideMarker = segs.length === 0;
  const recs = [...idx.entries.values()].filter(
    (r) => !(hideMarker && r.name === MARKER_NAME)
  );

  // Fill in missing file sizes in parallel (a few hundred entries max per dir).
  await Promise.all(recs.map(async (r) => {
    if (r.kind === 'file' && r.size === undefined) {
      try {
        await fileOf(r);
      } catch (e) {
        r.size = 0;
        console.warn('fsworker: size fetch failed for "' + r.name + '": ' + (e && e.message || e));
      }
    }
  }));

  let pos = 0;
  let count = 0;
  for (const r of recs) {
    const nameBytes = utf8enc.encode(r.name); // EXACT-CASE basename
    const need = 1 + 4 + 2 + nameBytes.length;
    if (pos + need > DATA_BYTES) {
      console.warn('fsworker: LISTDIR result exceeds ' + DATA_BYTES +
                   ' bytes, truncated to ' + count + ' of ' + recs.length + ' entries');
      break;
    }
    data[pos] = r.kind === 'file' ? 1 : 2;
    dv.setUint32(pos + 1, r.kind === 'file' ? (r.size || 0) : 0, true);
    dv.setUint16(pos + 5, nameBytes.length, true);
    data.set(nameBytes, pos + 7);
    pos += need;
    count++;
  }
  return { count, bytes: pos };
}

// ---- request dispatch -------------------------------------------------------

// Service one request. ALWAYS ends in RESP + notify so the caller never hangs.
async function handleGo() {
  if (!ctrl) {
    console.error("fsworker: 'go' received before init — cannot respond");
    return;
  }
  if (Atomics.load(ctrl, SIGNAL) !== REQ) {
    console.warn("fsworker: 'go' without SIGNAL=REQ, ignoring");
    return;
  }

  const op = ctrl[OP];
  let status = op === OP_EXISTS ? 0 : -1; // per-op error default
  let extra = 0;

  try {
    // data is SAB-backed; TextDecoder rejects shared views, so slice() first
    // (slice on a typed array always copies into a fresh non-shared buffer).
    const raw = utf8dec.decode(data.slice(0, ctrl[PATH_LEN]));
    const segs = pathSegments(raw);
    if (segs !== null) {
      switch (op) {
        case OP_EXISTS:
          status = await doExists(segs);
          break;
        case OP_STAT:
          status = await doStat(segs);
          break;
        case OP_READ:
          status = await doRead(segs, ctrl[OFFSET], ctrl[MAXLEN]);
          break;
        case OP_LISTDIR: {
          const r = await doListdir(segs);
          status = r.count;
          extra = r.bytes;
          break;
        }
        default:
          console.warn('fsworker: unknown op ' + op);
      }
    }
  } catch (e) {
    // NotFoundError etc. — fall through to the error default already in status
    console.warn('fsworker: op ' + op + ' failed: ' + (e && e.message || e));
  }

  ctrl[STATUS] = status;
  ctrl[EXTRA] = extra;
  Atomics.store(ctrl, SIGNAL, RESP); // release: publishes data + STATUS writes
  Atomics.notify(ctrl, SIGNAL, 1);
}

// ---- init -------------------------------------------------------------------

async function handleInit(msg) {
  // Accept either raw SharedArrayBuffers or pre-built views.
  ctrl = msg.ctrl instanceof Int32Array ? msg.ctrl : new Int32Array(msg.ctrl);
  data = msg.data instanceof Uint8Array ? msg.data : new Uint8Array(msg.data);
  dv = new DataView(data.buffer, data.byteOffset, data.byteLength);

  try {
    await ensureRoot();
  } catch (e) {
    postMessage({
      type: 'error',
      msg: 'OPFS directory "' + OPFS_ROOT_NAME + '" not found — game data has not been ' +
           'imported yet. Open import.html and run the import first. (' + (e && e.message || e) + ')',
    });
    return;
  }

  // The importer writes rtw/.import-complete last; without it the tree is partial.
  try {
    await rtwRoot.getFileHandle(MARKER_NAME);
  } catch (e) {
    postMessage({
      type: 'error',
      msg: '"' + OPFS_ROOT_NAME + '" exists but "' + MARKER_NAME + '" is missing — a previous ' +
           'import did not finish. Clear OPFS in import.html and re-import.',
    });
    return;
  }

  postMessage({ type: 'ready' });
}

// ---- message pump -----------------------------------------------------------

// Requests are serialized by the (blocking) caller, but chain on a promise
// anyway so a slow await can never interleave two messages.
let chain = Promise.resolve();

self.onmessage = (e) => {
  const msg = e.data;
  chain = chain
    .then(() => {
      if (msg && msg.type === 'init') return handleInit(msg);
      if (msg === 'go' || (msg && msg.type === 'go')) return handleGo();
      console.warn('fsworker: unknown message', msg);
    })
    .catch((err) => {
      console.error('fsworker: unhandled error in message pump:', err);
      // Last-ditch: if a request is still pending, fail it so the caller
      // parked in Atomics.wait is guaranteed to wake.
      if (ctrl && Atomics.load(ctrl, SIGNAL) === REQ) {
        ctrl[STATUS] = -1;
        ctrl[EXTRA] = 0;
        Atomics.store(ctrl, SIGNAL, RESP);
        Atomics.notify(ctrl, SIGNAL, 1);
      }
    });
};
