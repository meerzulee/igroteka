import type { Env } from './env';

export interface UserRow {
  id: string;
  handle: string | null;
  display_name: string;
  avatar_url: string | null;
  is_guest: number;
  created_at: number;
  last_seen_at: number | null;
}

export interface AccountRow {
  provider: string;
  provider_user_id: string;
  user_id: string;
  email: string | null;
}

export interface CloudRow {
  user_id: string;
  provider: string;
  account_id: string | null;
  account_label: string | null;
  refresh_token_enc: string;
  scope: string | null;
  created_at: number;
}

export async function getUser(env: Env, id: string): Promise<UserRow | null> {
  return env.DB.prepare('SELECT * FROM users WHERE id = ?').bind(id).first<UserRow>();
}

export async function createUser(env: Env, u: { id: string; display_name: string; avatar_url?: string | null; is_guest: boolean }): Promise<void> {
  await env.DB.prepare('INSERT INTO users (id, display_name, avatar_url, is_guest) VALUES (?, ?, ?, ?)')
    .bind(u.id, u.display_name, u.avatar_url ?? null, u.is_guest ? 1 : 0).run();
}

export async function updateUserProfile(env: Env, id: string, fields: { display_name?: string; avatar_url?: string | null; is_guest?: boolean }): Promise<void> {
  const sets: string[] = [];
  const binds: unknown[] = [];
  if (fields.display_name !== undefined) { sets.push('display_name = ?'); binds.push(fields.display_name); }
  if (fields.avatar_url !== undefined) { sets.push('avatar_url = ?'); binds.push(fields.avatar_url); }
  if (fields.is_guest !== undefined) { sets.push('is_guest = ?'); binds.push(fields.is_guest ? 1 : 0); }
  if (!sets.length) return;
  binds.push(id);
  await env.DB.prepare(`UPDATE users SET ${sets.join(', ')} WHERE id = ?`).bind(...binds).run();
}

// Remove a guest user that can never be signed back into: no linked identity
// and no live session. Any cloud_connections it still holds are unreachable
// (nothing can authenticate as this guest) and cascade-delete with the row —
// this is what keeps an abandoned guest from orphaning an encrypted token.
export async function deleteUserIfOrphanGuest(env: Env, id: string): Promise<void> {
  await env.DB.prepare(
    `DELETE FROM users WHERE id = ? AND is_guest = 1
       AND NOT EXISTS (SELECT 1 FROM oauth_accounts WHERE user_id = users.id)
       AND NOT EXISTS (SELECT 1 FROM sessions WHERE user_id = users.id)`,
  ).bind(id).run();
}

// Unconditional delete — used to roll back a just-created user that lost a
// first-link race. Cascades to its (as-yet nonexistent) child rows.
export async function deleteUser(env: Env, id: string): Promise<void> {
  await env.DB.prepare('DELETE FROM users WHERE id = ?').bind(id).run();
}

export async function getAccount(env: Env, provider: string, providerUserId: string): Promise<AccountRow | null> {
  return env.DB.prepare('SELECT * FROM oauth_accounts WHERE provider = ? AND provider_user_id = ?')
    .bind(provider, providerUserId).first<AccountRow>();
}

export async function linkAccount(env: Env, a: { provider: string; provider_user_id: string; user_id: string; email?: string | null }): Promise<void> {
  await env.DB.prepare('INSERT INTO oauth_accounts (provider, provider_user_id, user_id, email) VALUES (?, ?, ?, ?)')
    .bind(a.provider, a.provider_user_id, a.user_id, a.email ?? null).run();
}

export async function listAccounts(env: Env, userId: string): Promise<AccountRow[]> {
  const r = await env.DB.prepare('SELECT * FROM oauth_accounts WHERE user_id = ?').bind(userId).all<AccountRow>();
  return r.results;
}

export async function createSession(env: Env, idHash: string, userId: string, expiresAt: number): Promise<void> {
  await env.DB.prepare('INSERT INTO sessions (id, user_id, expires_at) VALUES (?, ?, ?)')
    .bind(idHash, userId, expiresAt).run();
}

export async function getSessionUser(env: Env, idHash: string): Promise<{ user: UserRow; expires_at: number } | null> {
  const row = await env.DB.prepare(
    `SELECT u.*, s.expires_at AS session_expires_at FROM sessions s JOIN users u ON u.id = s.user_id
      WHERE s.id = ? AND s.expires_at > unixepoch()`,
  ).bind(idHash).first<UserRow & { session_expires_at: number }>();
  if (!row) return null;
  const { session_expires_at, ...user } = row;
  return { user: user as UserRow, expires_at: session_expires_at };
}

export async function extendSession(env: Env, idHash: string, expiresAt: number): Promise<void> {
  await env.DB.prepare('UPDATE sessions SET expires_at = ? WHERE id = ?').bind(expiresAt, idHash).run();
}

export async function deleteSession(env: Env, idHash: string): Promise<void> {
  await env.DB.prepare('DELETE FROM sessions WHERE id = ?').bind(idHash).run();
}

export async function getCloud(env: Env, userId: string, provider: string): Promise<CloudRow | null> {
  return env.DB.prepare('SELECT * FROM cloud_connections WHERE user_id = ? AND provider = ?')
    .bind(userId, provider).first<CloudRow>();
}

export async function upsertCloud(env: Env, c: { user_id: string; provider: string; account_id: string | null; account_label: string | null; refresh_token_enc: string; scope: string | null }): Promise<void> {
  await env.DB.prepare(
    `INSERT INTO cloud_connections (user_id, provider, account_id, account_label, refresh_token_enc, scope)
     VALUES (?, ?, ?, ?, ?, ?)
     ON CONFLICT (user_id, provider) DO UPDATE SET
       account_id = excluded.account_id, account_label = excluded.account_label,
       refresh_token_enc = excluded.refresh_token_enc, scope = excluded.scope`,
  ).bind(c.user_id, c.provider, c.account_id, c.account_label, c.refresh_token_enc, c.scope).run();
}

export async function deleteCloud(env: Env, userId: string, provider: string): Promise<void> {
  await env.DB.prepare('DELETE FROM cloud_connections WHERE user_id = ? AND provider = ?')
    .bind(userId, provider).run();
}

export async function listClouds(env: Env, userId: string): Promise<CloudRow[]> {
  const r = await env.DB.prepare('SELECT * FROM cloud_connections WHERE user_id = ?').bind(userId).all<CloudRow>();
  return r.results;
}
