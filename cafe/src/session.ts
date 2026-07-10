import type { Env } from './env';
import type { UserRow } from './db';
import { createSession, deleteSession, extendSession, getSessionUser } from './db';
import { cookie, cookieName, readCookie } from './http';
import { randomToken, sha256Hex } from './crypto';

const SESSION_BASE = 'igro_session';
const SESSION_TTL = 30 * 24 * 3600;      // 30 days
const RENEW_BELOW = 20 * 24 * 3600;      // sliding renewal threshold

export interface SessionCtx {
  user: UserRow;
  idHash: string;
  setCookies: string[]; // renewal cookie, when issued
}

function now(): number {
  return Math.floor(Date.now() / 1000);
}

// Over HTTPS the session cookie is __Host- prefixed (Secure, Path=/, no
// Domain): browsers forbid any other host — including a sibling under the
// registrable domain — from setting it, which is what blocks cookie-tossing
// on the shared mrz.sh zone. Local http dev falls back to a plain name.
export function sessionCookie(token: string, secure: boolean, maxAge = SESSION_TTL): string {
  return cookie(cookieName(SESSION_BASE, secure), token, { path: '/', secure, maxAge });
}

export async function startSession(env: Env, userId: string, secure: boolean): Promise<string> {
  const token = randomToken();
  await createSession(env, await sha256Hex(token), userId, now() + SESSION_TTL);
  return sessionCookie(token, secure);
}

export async function readSession(env: Env, req: Request): Promise<SessionCtx | null> {
  const token = readCookie(req, SESSION_BASE);
  if (!token) return null;
  const idHash = await sha256Hex(token);
  const found = await getSessionUser(env, idHash);
  if (!found) return null;
  const secure = new URL(req.url).protocol === 'https:';
  const setCookies: string[] = [];
  if (found.expires_at - now() < RENEW_BELOW) {
    await extendSession(env, idHash, now() + SESSION_TTL);
    setCookies.push(sessionCookie(token, secure));
  }
  return { user: found.user, idHash, setCookies };
}

export async function endSession(env: Env, idHash: string, secure: boolean): Promise<string> {
  await deleteSession(env, idHash);
  return cookie(cookieName(SESSION_BASE, secure), '', { path: '/', secure, maxAge: 0 });
}
