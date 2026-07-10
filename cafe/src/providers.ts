import { Discord, Dropbox, GitHub, Google } from 'arctic';
import type { Env } from './env';

export type AuthProvider = 'google' | 'github' | 'discord';
export type CloudProvider = 'gdrive' | 'dropbox';

export const AUTH_PROVIDERS: AuthProvider[] = ['google', 'github', 'discord'];
export const CLOUD_PROVIDERS: CloudProvider[] = ['gdrive', 'dropbox'];

export function isAuthProvider(p: string): p is AuthProvider {
  return (AUTH_PROVIDERS as string[]).includes(p);
}

export function isCloudProvider(p: string): p is CloudProvider {
  return (CLOUD_PROVIDERS as string[]).includes(p);
}

function creds(env: Env, p: AuthProvider | CloudProvider): { id: string; secret: string } | null {
  const map = {
    google: [env.GOOGLE_CLIENT_ID, env.GOOGLE_CLIENT_SECRET],
    gdrive: [env.GOOGLE_CLIENT_ID, env.GOOGLE_CLIENT_SECRET],
    github: [env.GITHUB_CLIENT_ID, env.GITHUB_CLIENT_SECRET],
    discord: [env.DISCORD_CLIENT_ID, env.DISCORD_CLIENT_SECRET],
    dropbox: [env.DROPBOX_CLIENT_ID, env.DROPBOX_CLIENT_SECRET],
  } as const;
  const [id, secret] = map[p];
  return id && secret ? { id, secret } : null;
}

export function providerConfigured(env: Env, p: AuthProvider | CloudProvider): boolean {
  return creds(env, p) !== null;
}

export function googleClient(env: Env, origin: string, callbackPath: string): Google | null {
  const c = creds(env, 'google');
  return c && new Google(c.id, c.secret, origin + callbackPath);
}

export function githubClient(env: Env, origin: string): GitHub | null {
  const c = creds(env, 'github');
  return c && new GitHub(c.id, c.secret, origin + '/api/auth/github/callback');
}

export function discordClient(env: Env, origin: string): Discord | null {
  const c = creds(env, 'discord');
  return c && new Discord(c.id, c.secret, origin + '/api/auth/discord/callback');
}

export function dropboxClient(env: Env, origin: string): Dropbox | null {
  const c = creds(env, 'dropbox');
  return c && new Dropbox(c.id, c.secret, origin + '/api/connect/dropbox/callback');
}

// What each login flow asks for — identity only.
export const AUTH_SCOPES: Record<AuthProvider, string[]> = {
  google: ['openid', 'profile', 'email'],
  github: ['read:user', 'user:email'],
  discord: ['identify', 'email'],
};

// Cloud-save scopes. gdrive uses the hidden appDataFolder (non-sensitive
// scope, invisible to the user's Drive UI); dropbox is an "app folder" app.
export const GDRIVE_SCOPES = ['openid', 'email', 'https://www.googleapis.com/auth/drive.appdata'];
export const DROPBOX_SCOPES = ['account_info.read', 'files.metadata.read', 'files.content.read', 'files.content.write'];

export interface Profile {
  providerUserId: string;
  displayName: string;
  email: string | null;
  avatarUrl: string | null;
}

function decodeJwtPayload(jwt: string): Record<string, unknown> {
  const b64 = jwt.split('.')[1].replaceAll('-', '+').replaceAll('_', '/');
  return JSON.parse(new TextDecoder().decode(Uint8Array.from(atob(b64), (c) => c.charCodeAt(0))));
}

// The id_token arrived over TLS straight from Google's token endpoint, so its
// claims are trusted without local signature verification.
export function googleProfileFromIdToken(idToken: string): Profile {
  const c = decodeJwtPayload(idToken) as { sub: string; name?: string; email?: string; picture?: string };
  return {
    providerUserId: c.sub,
    displayName: c.name || c.email?.split('@')[0] || 'Player',
    email: c.email ?? null,
    avatarUrl: c.picture ?? null,
  };
}

export async function fetchGithubProfile(accessToken: string): Promise<Profile> {
  const headers = {
    authorization: `Bearer ${accessToken}`,
    accept: 'application/vnd.github+json',
    'user-agent': 'igroteka-cafe',
  };
  const r = await fetch('https://api.github.com/user', { headers });
  if (!r.ok) throw new Error(`github /user ${r.status}`);
  const u = (await r.json()) as { id: number; login: string; name: string | null; avatar_url: string | null; email: string | null };
  let email = u.email;
  if (!email) {
    const er = await fetch('https://api.github.com/user/emails', { headers });
    if (er.ok) {
      const emails = (await er.json()) as { email: string; primary: boolean; verified: boolean }[];
      email = emails.find((e) => e.primary && e.verified)?.email ?? null;
    }
  }
  return {
    providerUserId: String(u.id),
    displayName: u.name || u.login,
    email,
    avatarUrl: u.avatar_url,
  };
}

export async function fetchDiscordProfile(accessToken: string): Promise<Profile> {
  const r = await fetch('https://discord.com/api/users/@me', {
    headers: { authorization: `Bearer ${accessToken}` },
  });
  if (!r.ok) throw new Error(`discord /users/@me ${r.status}`);
  const u = (await r.json()) as { id: string; username: string; global_name: string | null; avatar: string | null; email: string | null };
  return {
    providerUserId: u.id,
    displayName: u.global_name || u.username,
    email: u.email,
    avatarUrl: u.avatar ? `https://cdn.discordapp.com/avatars/${u.id}/${u.avatar}.png?size=128` : null,
  };
}

export async function fetchDropboxAccount(accessToken: string): Promise<{ accountId: string; label: string }> {
  const r = await fetch('https://api.dropboxapi.com/2/users/get_current_account', {
    method: 'POST',
    headers: { authorization: `Bearer ${accessToken}` },
  });
  if (!r.ok) throw new Error(`dropbox get_current_account ${r.status}`);
  const a = (await r.json()) as { account_id: string; email: string; name?: { display_name?: string } };
  return { accountId: a.account_id, label: a.email || a.name?.display_name || 'Dropbox' };
}
