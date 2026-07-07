// Igroteka shared: asset manifest, OPFS store, zip extraction.
// Rule: game bytes flow device/cloud -> browser OPFS. Never through our servers.

// Versions — single source of truth. The About window shows both; the play
// page cache-busts engine fetches with ENGINE_BUILD (bump it whenever new
// GeneralsXZH.js/.wasm binaries are staged, or browsers replay the old ones).
export const SITE_VERSION = '0.1.0';
export const ENGINE_BUILD = 22;

export const ZH_MANIFEST = {
  // Zero Hour expansion archives (from the ZH install dir)
  zh: [
    'INIZH.big', 'EnglishZH.big', 'WindowZH.big', 'gensecZH.big',
    'TexturesZH.big', 'W3DZH.big', 'TerrainZH.big', 'MapsZH.big',
  ],
  // Base-game archives (same dir on Steam installs, or a separate Generals dir)
  base: ['Textures.big', 'W3D.big', 'Terrain.big', 'Window.big'],
  // Loose files under Data/Scripts — the skirmish AI lives here
  scripts: ['SkirmishScripts.scb', 'MultiplayerScripts.scb', 'Scripts.ini'],
};

// The 34 cursor files Mouse.ini's Texture entries resolve to. Canonical
// casing matters: the wasm FS is case-sensitive and the engine asks for
// exactly Data/Cursors/<Texture>.ani (install dirs mix the case freely).
export const ZH_CURSORS = [
  'SCCAttMov.ani', 'SCCAttack.ani', 'SCCCashHack.ani', 'SCCEnter.ani',
  'SCCExit.ani', 'SCCFriendly.ani', 'SCCHostile.ani', 'SCCHostile2.ani',
  'SCCHostile3.ani', 'SCCKnifeAttack.ani', 'SCCMove.ani', 'SCCNoAction.ani',
  'SCCNoBomb.ani', 'SCCNoKnife.ani', 'SCCPlaceBeacon.ani', 'SCCPointer.ani',
  'SCCRallyPnt.ani', 'SCCRemoteChg.ani', 'SCCRepair.ani', 'SCCResumeC.ani',
  'SCCSDIUplink.ani', 'SCCScroll0.ani', 'SCCScroll1.ani', 'SCCScroll2.ani',
  'SCCScroll3.ani', 'SCCScroll4.ani', 'SCCScroll5.ani', 'SCCScroll6.ani',
  'SCCScroll7.ani', 'SCCSelect.ani', 'SCCSniper.ani', 'SCCTNTAttack.ani',
  'SCCTimedChg.ani', 'SCCWaypoint.ani',
];

// Nice-to-have files: imported when present, never block installation.
// The game's own icon comes from the USER'S copy — we never ship EA art.
export const ZH_OPTIONAL = {
  icons: ['GeneralsZH.ico', 'Generals.ico'],
  // The native install/loading splash — a plain 800x600 BMP the browser can
  // display directly. From the user's own files (never shipped by us).
  loading: ['Install_Final.bmp'],
  // Native in-game mouse cursors (Data/Cursors). Optional so installs made
  // before this bucket existed keep working — they just get the web cursor.
  cursors: ZH_CURSORS,
  // Sound. Optional: ~900MB total, and the game is fully playable silent.
  // ZH archives mount at /game, base-game ones at /game-base.
  'audio-zh': ['AudioZH.big', 'AudioEnglishZH.big', 'SpeechZH.big',
               'SpeechEnglishZH.big', 'MusicZH.big'],
  'audio-base': ['Audio.big', 'AudioEnglish.big', 'Speech.big',
                 'SpeechEnglish.big', 'Music.big'],
};

const norm = (name) => name.toLowerCase();

const zhSet = new Set(ZH_MANIFEST.zh.map(norm));
const baseSet = new Set(ZH_MANIFEST.base.map(norm));
const scriptSet = new Set(ZH_MANIFEST.scripts.map(norm));
const iconSet = new Set(ZH_OPTIONAL.icons.map(norm));
const loadingSet = new Set(ZH_OPTIONAL.loading.map(norm));
const cursorFileSet = new Set(ZH_OPTIONAL.cursors.map(norm));
const audioZhSet = new Set(ZH_OPTIONAL['audio-zh'].map(norm));
const audioBaseSet = new Set(ZH_OPTIONAL['audio-base'].map(norm));

// Classify a filename (any path) into its OPFS bucket, or null if not needed.
export function classify(path) {
  const name = norm(path.split('/').pop());
  if (zhSet.has(name)) return { dir: 'zh', name: canonical(name) };
  if (baseSet.has(name)) return { dir: 'base', name: canonical(name) };
  if (scriptSet.has(name)) return { dir: 'scripts', name: canonical(name) };
  if (iconSet.has(name)) return { dir: 'icons', name: canonical(name) };
  if (loadingSet.has(name)) return { dir: 'loading', name: canonical(name) };
  if (cursorFileSet.has(name)) return { dir: 'cursors', name: canonical(name) };
  if (audioZhSet.has(name)) return { dir: 'audio-zh', name: canonical(name) };
  if (audioBaseSet.has(name)) return { dir: 'audio-base', name: canonical(name) };
  return null;
}

// The Steam ZH depot ships DUPLICATE archives that classify to the same
// target: an older INIZH.big under Data/INI/ (engine boots to a black shell
// with it) and the base game's smaller SkirmishScripts.scb under
// ZH_Generals/Data/Scripts/ (broken skirmish AI). Import order would decide
// which copy wins — instead keep the shallowest path per target, which is
// the real ZH file in every known layout.
export function dedupeImports(entries, relOf) {
  const byTarget = new Map();
  for (const e of entries) {
    const rel = relOf(e);
    const c = classify(rel);
    if (!c) continue;
    const key = c.dir + '/' + c.name;
    const depth = rel.split(/[\\/]/).length;
    const prev = byTarget.get(key);
    if (!prev || depth < prev.depth) byTarget.set(key, { e, depth });
  }
  return [...byTarget.values()].map((x) => x.e);
}

function canonical(lower) {
  for (const list of [...Object.values(ZH_MANIFEST), ...Object.values(ZH_OPTIONAL)]) {
    for (const f of list) if (norm(f) === lower) return f;
  }
  return lower;
}

// Blob URL for the native loading splash if the user's install provided one.
export async function loadingImageURL() {
  for (const name of ZH_OPTIONAL.loading) {
    try {
      const f = await opfsReadFile('loading', name);
      return URL.createObjectURL(f);
    } catch { /* try next */ }
  }
  return null;
}

// Blob URL for the game's own icon if the user's install provided one.
export async function gameIconURL() {
  for (const name of ZH_OPTIONAL.icons) {
    try {
      const f = await opfsReadFile('icons', name);
      return URL.createObjectURL(f);
    } catch { /* try next */ }
  }
  return null;
}

// Use the game's own .ico as the tab favicon. From the user's install in
// OPFS (never shipped by us); no-op until they've imported the icons bucket.
export async function setGameFavicon() {
  const url = await gameIconURL();
  if (!url) return;
  let link = document.querySelector('link[rel="icon"]');
  if (!link) {
    link = document.createElement('link');
    link.rel = 'icon';
    document.head.appendChild(link);
  }
  link.type = 'image/x-icon';
  link.href = url;
}

export async function opfsRoot() {
  const root = await navigator.storage.getDirectory();
  return root;
}

export async function opfsWrite(dir, name, blobOrBuffer, onProgress) {
  const root = await opfsRoot();
  const d = await root.getDirectoryHandle(dir, { create: true });
  const fh = await d.getFileHandle(name, { create: true });
  const w = await fh.createWritable();
  if (blobOrBuffer instanceof Blob && blobOrBuffer.stream && onProgress) {
    const reader = blobOrBuffer.stream().getReader();
    let written = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      await w.write(value);
      written += value.byteLength;
      onProgress(written, blobOrBuffer.size);
    }
  } else {
    await w.write(blobOrBuffer);
  }
  await w.close();
}

// Inventory of what's already imported. Returns {zh:[names], base:[], scripts:[]}
export async function opfsInventory() {
  const out = { zh: [], base: [], scripts: [], icons: [], loading: [],
                cursors: [], 'audio-zh': [], 'audio-base': [] };
  const root = await opfsRoot();
  for (const dir of Object.keys(out)) {
    try {
      const d = await root.getDirectoryHandle(dir);
      for await (const [name, handle] of d.entries()) {
        if (handle.kind === 'file') out[dir].push(name);
      }
    } catch { /* dir absent */ }
  }
  return out;
}

export function missingFiles(inv) {
  const missing = [];
  for (const [dir, files] of Object.entries(ZH_MANIFEST)) {
    const have = new Set((inv[dir] || []).map(norm));
    for (const f of files) if (!have.has(norm(f))) missing.push(`${dir}/${f}`);
  }
  return missing;
}

export async function opfsReadFile(dir, name) {
  const root = await opfsRoot();
  const d = await root.getDirectoryHandle(dir);
  const fh = await d.getFileHandle(name);
  return fh.getFile();
}

export async function requestPersistence() {
  if (navigator.storage && navigator.storage.persist) {
    try { return await navigator.storage.persist(); } catch { return false; }
  }
  return false;
}

// Probe that OPFS actually accepts writes before a multi-GB import. Safari
// Private Browsing (and some locked-down profiles) block or stall persistent
// storage — without this the copy loop hangs with no error. The 5s race
// catches the stall case, not just the reject case.
export async function opfsWritable() {
  const probe = (async () => {
    const root = await opfsRoot();
    const fh = await root.getFileHandle('.igroteka-probe', { create: true });
    const w = await fh.createWritable();
    await w.write(new Uint8Array([1]));
    await w.close();
    await root.removeEntry('.igroteka-probe');
    return true;
  })();
  const timeout = new Promise((res) => setTimeout(() => res(false), 5000));
  try { return await Promise.race([probe, timeout]); } catch { return false; }
}

// ---- Native loading-bar art -------------------------------------------------
// The game's own loading bar lives in EnglishZH.big (already imported for
// play) as slices of one UI atlas. We read just the BIG index plus that one
// ~1MB entry — never the whole archive. Slice coordinates come from
// Data/INI/MappedImages/TextureSize_512/SCSmShellUserInterface512.INI; the
// bar layout matches the engine's W3DGadgetProgressBarImageDraw.

const BAR_ATLAS = 'scsmshelluserinterface512_001.tga';

// Atlas slices as [x, y, w, h]
export const NATIVE_BAR = {
  frameL: [303, 451, 20, 20],  // LoadingBar_L
  frameR: [259, 451, 20, 20],  // LoadingBar_R
  frameC: [155, 471, 10, 20],  // LoadingBar_C (repeating)
  fill:   [446, 489, 3, 11],   // LoadingBar_ProgressCenter0 — the gold segment
  empty:  [508, 69, 3, 11],    // LoadingBar_DePowered
};

// Parse a BIG archive index from a Blob/File without reading the payload.
async function bigIndex(file) {
  const head = new DataView(await file.slice(0, 16).arrayBuffer());
  const magic = String.fromCharCode(...new Uint8Array(head.buffer, 0, 4));
  if (magic !== 'BIGF' && magic !== 'BIG4') throw new Error('not a BIG archive');
  const count = head.getUint32(8);       // big-endian
  // Header field 3 is the data start (header + index size); pad by 16 in case
  // an archive stores the index size alone. Over-reading is harmless — the
  // parse loop stops after `count` entries.
  const indexEnd = head.getUint32(12) + 16;
  const buf = new Uint8Array(await file.slice(16, indexEnd).arrayBuffer());
  const dv = new DataView(buf.buffer);
  const entries = [];
  let p = 0;
  for (let i = 0; i < count && p + 8 < buf.length; i++) {
    const offset = dv.getUint32(p), size = dv.getUint32(p + 4);
    p += 8;
    let name = '';
    while (p < buf.length && buf[p]) name += String.fromCharCode(buf[p++]);
    p++;
    entries.push({ name, offset, size });
  }
  return entries;
}

async function bigExtract(file, suffix) {
  const want = suffix.toLowerCase();
  const e = (await bigIndex(file)).find(x => x.name.toLowerCase().endsWith(want));
  if (!e) return null;
  return new Uint8Array(await file.slice(e.offset, e.offset + e.size).arrayBuffer());
}

// Uncompressed TGA (type 2, 24/32bpp) -> canvas.
function tgaToCanvas(bytes) {
  const idLen = bytes[0], type = bytes[2];
  if (type !== 2) throw new Error('unsupported TGA type ' + type);
  const w = bytes[12] | (bytes[13] << 8), h = bytes[14] | (bytes[15] << 8);
  const bpp = bytes[16] >> 3, topDown = !!(bytes[17] & 0x20);
  const img = new ImageData(w, h);
  let p = 18 + idLen;
  for (let row = 0; row < h; row++) {
    let q = (topDown ? row : h - 1 - row) * w * 4;
    for (let x = 0; x < w; x++, p += bpp, q += 4) {
      img.data[q] = bytes[p + 2];
      img.data[q + 1] = bytes[p + 1];
      img.data[q + 2] = bytes[p];
      img.data[q + 3] = bpp === 4 ? bytes[p + 3] : 255;
    }
  }
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  c.getContext('2d').putImageData(img, 0, 0);
  return c;
}

// Canvas holding the shell UI atlas, or null when unavailable.
// Pass a Blob to read from (dev server); defaults to the OPFS copy.
export async function nativeBarAtlas(srcBlob = null) {
  try {
    const big = srcBlob || await opfsReadFile('zh', 'EnglishZH.big');
    const tga = await bigExtract(big, BAR_ATLAS);
    return tga ? tgaToCanvas(tga) : null;
  } catch { return null; }
}

// Draw the native bar into a 2d context, engine-style: capped frame with a
// tiled center, then 3px fill segments inset (10,5) — gold for progress,
// de-powered for the remainder. (W3DProgressBar.cpp, ImageDraw path.)
export function drawNativeBar(ctx, atlas, pct, W = 148, H = 16) {
  const { frameL, frameR, frameC, fill, empty } = NATIVE_BAR;
  const blit = (s, dx, dy, dw, dh) =>
    ctx.drawImage(atlas, s[0], s[1], s[2], s[3], dx, dy, dw, dh);
  ctx.clearRect(0, 0, W, H);
  const rightStart = W - frameR[2];
  ctx.save();
  ctx.beginPath();
  ctx.rect(frameL[2], 0, rightStart - frameL[2], H);
  ctx.clip();
  for (let x = frameL[2]; x < rightStart; x += frameC[2]) blit(frameC, x, 0, frameC[2], H);
  ctx.restore();
  blit(frameL, 0, 0, frameL[2], H);
  blit(frameR, rightStart, 0, frameR[2], H);
  const inner = W - 20, segW = fill[2];
  const slots = Math.floor(inner / segW);
  const lit = Math.floor((inner * Math.max(0, Math.min(100, pct)) / 100) / segW);
  for (let i = 0; i < slots; i++)
    blit(i < lit ? fill : empty, 10 + i * segW, 5, segW, H - 10);
}

// ---- Minimal ZIP reader (store + deflate entries) — no dependencies. ----
// Reads the central directory, extracts matching entries as Blobs.
export async function* zipEntries(file) {
  const size = file.size;
  const tailLen = Math.min(size, 65557);
  const tail = new DataView(await file.slice(size - tailLen).arrayBuffer());
  let eocd = -1;
  for (let i = tail.byteLength - 22; i >= 0; i--) {
    if (tail.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('Not a zip file (no end-of-central-directory)');
  const count = tail.getUint16(eocd + 10, true);
  const cdSize = tail.getUint32(eocd + 12, true);
  const cdOfs = tail.getUint32(eocd + 16, true);
  const cd = new DataView(await file.slice(cdOfs, cdOfs + cdSize).arrayBuffer());
  let p = 0;
  for (let i = 0; i < count; i++) {
    if (cd.getUint32(p, true) !== 0x02014b50) throw new Error('Bad central directory');
    const method = cd.getUint16(p + 10, true);
    const compSize = cd.getUint32(p + 20, true);
    const rawSize = cd.getUint32(p + 24, true);
    const nameLen = cd.getUint16(p + 28, true);
    const extraLen = cd.getUint16(p + 30, true);
    const commentLen = cd.getUint16(p + 32, true);
    const localOfs = cd.getUint32(p + 42, true);
    const nameBytes = new Uint8Array(cd.buffer, p + 46, nameLen);
    const name = new TextDecoder().decode(nameBytes);
    p += 46 + nameLen + extraLen + commentLen;
    if (name.endsWith('/')) continue;  // directory entry
    yield {
      name, method, compSize, rawSize,
      blob: async () => {
        // Local header: re-read name/extra lengths (they can differ from CD)
        const lh = new DataView(await file.slice(localOfs, localOfs + 30).arrayBuffer());
        if (lh.getUint32(0, true) !== 0x04034b50) throw new Error('Bad local header');
        const lNameLen = lh.getUint16(26, true);
        const lExtraLen = lh.getUint16(28, true);
        const dataStart = localOfs + 30 + lNameLen + lExtraLen;
        const comp = file.slice(dataStart, dataStart + compSize);
        if (method === 0) return comp;                       // stored
        if (method === 8) {                                  // deflate
          const ds = new DecompressionStream('deflate-raw');
          return new Response(comp.stream().pipeThrough(ds)).blob();
        }
        throw new Error(`Unsupported zip compression method ${method}`);
      },
    };
  }
}
