// Token hashing + at-rest encryption for cloud refresh tokens.

const te = new TextEncoder();

export function randomToken(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  return b64url(bytes);
}

export async function sha256Hex(input: string): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', te.encode(input));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

function b64url(bytes: Uint8Array): string {
  return btoa(String.fromCharCode(...bytes))
    .replaceAll('+', '-').replaceAll('/', '_').replaceAll('=', '');
}

function b64encode(bytes: Uint8Array): string {
  let s = '';
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s);
}

function b64decode(s: string): Uint8Array {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

async function importKey(keyB64: string): Promise<CryptoKey> {
  const raw = b64decode(keyB64);
  if (raw.length !== 32) throw new Error('TOKEN_KEY must be base64 of exactly 32 bytes');
  return crypto.subtle.importKey('raw', raw, 'AES-GCM', false, ['encrypt', 'decrypt']);
}

// Output layout: base64(iv[12] || ciphertext+tag)
export async function encryptToken(plaintext: string, keyB64: string): Promise<string> {
  const key = await importKey(keyB64);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, key, te.encode(plaintext)));
  const out = new Uint8Array(iv.length + ct.length);
  out.set(iv, 0);
  out.set(ct, iv.length);
  return b64encode(out);
}

export async function decryptToken(payloadB64: string, keyB64: string): Promise<string> {
  const key = await importKey(keyB64);
  const payload = b64decode(payloadB64);
  const iv = payload.slice(0, 12);
  const ct = payload.slice(12);
  const pt = await crypto.subtle.decrypt({ name: 'AES-GCM', iv }, key, ct);
  return new TextDecoder().decode(pt);
}

// Opaque state payloads for the OAuth round trip, carried in a short-lived
// cookie. Not encrypted — contains no secrets beyond the CSRF state/verifier,
// which the cookie jar already protects.
export function encodeState(obj: unknown): string {
  return b64url(te.encode(JSON.stringify(obj)));
}

export function decodeState<T>(s: string): T | null {
  try {
    const pad = s.replaceAll('-', '+').replaceAll('_', '/');
    return JSON.parse(new TextDecoder().decode(b64decode(pad))) as T;
  } catch {
    return null;
  }
}
