// cafe auth — the crypto core for password-gated private parties.
//
// Threat it defends: a leaked game URL (igroteka.mrz.sh/play/zh/?room=CODE)
// must NOT grant lobby access. The room code alone is useless without a token,
// and a token can only be minted by proving knowledge of the party password.
//
// Design (Codex-reviewed):
//  - Password is never stored. We store PBKDF2-HMAC-SHA256(password, salt) as a
//    verifier, plus the salt + iteration count.
//  - Each room has its own random HMAC secret. A join token is a STATELESS
//    HMAC-signed blob {r:room, role, exp, n:nonce}. Stateless => it survives DO
//    hibernation (an in-memory token set would not — the #1 review fix).
//  - The WS gate recomputes the HMAC from the room's stored secret and checks
//    exp before accepting the socket. The secret never leaves the DO.
//
// All primitives are Web Crypto (crypto.subtle), available in the Workers
// runtime. Constant-time comparison uses crypto.subtle.verify for the token and
// a byte-diff accumulator for the verifier (rate-limiting is the real brute
// force defence; see room.js).

const PBKDF2_ITERS = 100000; // ~tens of ms in the Workers runtime; fine per-auth
const enc = new TextEncoder();
const dec = new TextDecoder();

// ---- base64url (small inputs only: tokens, 16-32B secrets/salts) ----
export function b64uEncode(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
export function b64uDecode(str) {
  const norm = str.replace(/-/g, "+").replace(/_/g, "/");
  const bin = atob(norm);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// ---- high-entropy room code ----
// ~58 bits over a confusable-free alphabet (no 0/O/1/I/l). Unguessable, so a
// party room can't be squatted or brute-forced by code.
const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
export function newRoomCode(len = 10) {
  const bytes = crypto.getRandomValues(new Uint8Array(len));
  let s = "";
  for (let i = 0; i < len; i++) s += CODE_ALPHABET[bytes[i] % CODE_ALPHABET.length];
  return s;
}

export function randomBytes(n) {
  return crypto.getRandomValues(new Uint8Array(n));
}

// ---- password verifier (PBKDF2-HMAC-SHA256) ----
export async function deriveVerifier(password, saltBytes, iterations = PBKDF2_ITERS) {
  const base = await crypto.subtle.importKey(
    "raw", enc.encode(password), "PBKDF2", false, ["deriveBits"]
  );
  const bits = await crypto.subtle.deriveBits(
    { name: "PBKDF2", salt: saltBytes, iterations, hash: "SHA-256" }, base, 256
  );
  return new Uint8Array(bits);
}

export function constantTimeEqual(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

export const DEFAULT_ITERS = PBKDF2_ITERS;

// ---- stateless signed join token ----
async function hmacKey(secretBytes, usages) {
  return crypto.subtle.importKey(
    "raw", secretBytes, { name: "HMAC", hash: "SHA-256" }, false, usages
  );
}

// Mint {r:room, role, exp, n:nonce} signed with the room secret.
export async function mintToken(secretBytes, { room, role, ttlSec }) {
  const claims = {
    r: room,
    role,
    exp: Math.floor(Date.now() / 1000) + ttlSec,
    n: b64uEncode(randomBytes(6)),
  };
  const payload = enc.encode(JSON.stringify(claims));
  const key = await hmacKey(secretBytes, ["sign"]);
  const sig = new Uint8Array(await crypto.subtle.sign("HMAC", key, payload));
  return b64uEncode(payload) + "." + b64uEncode(sig);
}

// Verify signature + expiry. Returns the claims object or null. The HMAC verify
// is constant-time (crypto.subtle.verify). A token minted for another room can't
// pass here because each room signs with its own secret.
export async function verifyToken(token, secretBytes) {
  if (typeof token !== "string") return null;
  const dot = token.indexOf(".");
  if (dot <= 0) return null;
  let payload, sig;
  try {
    payload = b64uDecode(token.slice(0, dot));
    sig = b64uDecode(token.slice(dot + 1));
  } catch {
    return null;
  }
  const key = await hmacKey(secretBytes, ["verify"]);
  const ok = await crypto.subtle.verify("HMAC", key, sig, payload);
  if (!ok) return null;
  let claims;
  try {
    claims = JSON.parse(dec.decode(payload));
  } catch {
    return null;
  }
  if (!claims.exp || claims.exp < Math.floor(Date.now() / 1000)) return null;
  return claims;
}
