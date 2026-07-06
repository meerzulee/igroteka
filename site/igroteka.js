// Igroteka shared: asset manifest, OPFS store, zip extraction.
// Rule: game bytes flow device/cloud -> browser OPFS. Never through our servers.

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

const norm = (name) => name.toLowerCase();

const zhSet = new Set(ZH_MANIFEST.zh.map(norm));
const baseSet = new Set(ZH_MANIFEST.base.map(norm));
const scriptSet = new Set(ZH_MANIFEST.scripts.map(norm));

// Classify a filename (any path) into its OPFS bucket, or null if not needed.
export function classify(path) {
  const name = norm(path.split('/').pop());
  if (zhSet.has(name)) return { dir: 'zh', name: canonical(name) };
  if (baseSet.has(name)) return { dir: 'base', name: canonical(name) };
  if (scriptSet.has(name)) return { dir: 'scripts', name: canonical(name) };
  return null;
}

function canonical(lower) {
  for (const list of Object.values(ZH_MANIFEST)) {
    for (const f of list) if (norm(f) === lower) return f;
  }
  return lower;
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
  const out = { zh: [], base: [], scripts: [] };
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
