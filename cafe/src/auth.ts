import { generateCodeVerifier, generateState, type OAuth2Tokens } from 'arctic';
import type { Env } from './env';
import {
  createUser, deleteUser, deleteUserIfOrphanGuest, getAccount, linkAccount, listAccounts,
  listClouds, updateUserProfile, type UserRow,
} from './db';
import { cookie, cookieName, json, readCookie, redirect, safeNext } from './http';
import { decodeState, encodeState } from './crypto';
import { endSession, readSession, startSession, type SessionCtx } from './session';
import {
  AUTH_SCOPES, discordClient, fetchDiscordProfile, fetchGithubProfile, githubClient,
  googleClient, googleProfileFromIdToken, providerConfigured, type AuthProvider, type Profile,
} from './providers';

const OAUTH_BASE = 'igro_oauth';

export interface OAuthState {
  p: string;            // provider the flow started for
  s: string;            // CSRF state
  v?: string;           // PKCE verifier (google/discord)
  next: string;         // where to land afterwards
  mode: 'auth' | 'connect';
  // The user id this flow was started under, if any. Its presence marks a
  // link/claim/connect intent; the callback requires the landing session to
  // match it. This binds the flow to a session independently of the state
  // cookie's integrity, so a tossed cookie can't retarget a victim's account.
  uid?: string;
}

export function oauthStateCookie(payload: OAuthState | null, secure: boolean): string {
  return cookie(cookieName(OAUTH_BASE, secure), payload ? encodeState(payload) : '', {
    path: '/', secure, maxAge: payload ? 600 : 0,
  });
}

export function readOauthState(req: Request): OAuthState | null {
  const raw = readCookie(req, OAUTH_BASE);
  return raw ? decodeState<OAuthState>(raw) : null;
}

// Guests are named Guest-XXXX until they claim the account.
function guestName(): string {
  const n = crypto.getRandomValues(new Uint32Array(1))[0] % 10000;
  return `Guest-${String(n).padStart(4, '0')}`;
}

function isGuestDefaultName(name: string): boolean {
  return /^(Guest|Гость)[-\s]?\d*$/i.test(name);
}

// ---- GET /api/auth/:provider/start?next=/ ----
export async function authStart(req: Request, env: Env, provider: AuthProvider): Promise<Response> {
  const url = new URL(req.url);
  if (!providerConfigured(env, provider)) return json({ error: 'provider_not_configured' }, 503);
  const secure = url.protocol === 'https:';
  const state = generateState();
  const next = safeNext(url.searchParams.get('next'));
  // A signed-in user (guest or full) starting a flow means "link/claim". Bind
  // it to their id so only their own session can complete the link.
  const session = await readSession(env, req);

  let authUrl: URL;
  let verifier: string | undefined;
  if (provider === 'google') {
    verifier = generateCodeVerifier();
    authUrl = googleClient(env, url.origin, '/api/auth/google/callback')!
      .createAuthorizationURL(state, verifier, AUTH_SCOPES.google);
  } else if (provider === 'discord') {
    verifier = generateCodeVerifier();
    authUrl = discordClient(env, url.origin)!.createAuthorizationURL(state, verifier, AUTH_SCOPES.discord);
  } else {
    authUrl = githubClient(env, url.origin)!.createAuthorizationURL(state, AUTH_SCOPES.github);
  }

  return redirect(authUrl.toString(), [
    oauthStateCookie({ p: provider, s: state, v: verifier, next, mode: 'auth', uid: session?.user.id }, secure),
  ]);
}

// ---- GET /api/auth/:provider/callback ----
export async function authCallback(req: Request, env: Env, provider: AuthProvider): Promise<Response> {
  const url = new URL(req.url);
  const secure = url.protocol === 'https:';
  const clearState = oauthStateCookie(null, secure);
  const fail = (reason: string) => redirect(`/?acct=err&reason=${encodeURIComponent(reason)}`, [clearState]);

  const st = readOauthState(req);
  const code = url.searchParams.get('code');
  const state = url.searchParams.get('state');
  if (!st || st.mode !== 'auth' || st.p !== provider) return fail('state_mismatch');
  if (!code || !state || state !== st.s) return fail(url.searchParams.get('error') || 'state_mismatch');

  const session = await readSession(env, req);
  // A link/claim flow (state carries a uid) must complete in the exact session
  // that began it. This is the load-bearing check: even a planted state cookie
  // can't graft an identity onto another account, because the attacker cannot
  // supply the victim's uid *and* land in the victim's session. Checked before
  // the provider round-trip so a mismatched flow costs nothing.
  const wantsLink = !!st.uid;
  if (wantsLink && (!session || session.user.id !== st.uid)) return fail('session_changed');

  let tokens: OAuth2Tokens;
  let profile: Profile;
  try {
    if (provider === 'google') {
      tokens = await googleClient(env, url.origin, '/api/auth/google/callback')!
        .validateAuthorizationCode(code, st.v!);
      profile = googleProfileFromIdToken(tokens.idToken());
    } else if (provider === 'discord') {
      tokens = await discordClient(env, url.origin)!.validateAuthorizationCode(code, st.v ?? null);
      profile = await fetchDiscordProfile(tokens.accessToken());
    } else {
      tokens = await githubClient(env, url.origin)!.validateAuthorizationCode(code);
      profile = await fetchGithubProfile(tokens.accessToken());
    }
  } catch (e) {
    console.error(`oauth ${provider} callback:`, e);
    return fail('exchange_failed');
  }

  const linked = await getAccount(env, provider, profile.providerUserId);
  const setCookies = [clearState];

  let userId: string;
  if (linked) {
    // The identity already belongs to someone: sign in as its owner. A full
    // account mid-link that points elsewhere is a conflict, not a switch.
    if (wantsLink && session && !session.user.is_guest && linked.user_id !== session.user.id) {
      return fail('linked_to_other_account');
    }
    userId = linked.user_id;
  } else if (wantsLink && session) {
    // Link the fresh identity to the bound session user; promote a guest in
    // place so anything keyed to the user id survives the claim.
    userId = session.user.id;
    await linkAccount(env, { provider, provider_user_id: profile.providerUserId, user_id: userId, email: profile.email });
    if (session.user.is_guest) {
      await updateUserProfile(env, userId, {
        is_guest: false,
        display_name: isGuestDefaultName(session.user.display_name) ? profile.displayName : session.user.display_name,
        avatar_url: session.user.avatar_url ?? profile.avatarUrl,
      });
    }
  } else {
    // Brand-new login. Guard the first-link race: if a concurrent flow linked
    // this identity first, roll back our throwaway user and adopt the winner.
    userId = crypto.randomUUID();
    await createUser(env, { id: userId, display_name: profile.displayName, avatar_url: profile.avatarUrl, is_guest: false });
    try {
      await linkAccount(env, { provider, provider_user_id: profile.providerUserId, user_id: userId, email: profile.email });
    } catch (e) {
      await deleteUser(env, userId);
      const winner = await getAccount(env, provider, profile.providerUserId);
      if (!winner) { console.error(`link race with no winner (${provider}):`, e); return fail('exchange_failed'); }
      userId = winner.user_id;
    }
  }

  // Fresh session on every login/link (rotation).
  if (session) {
    await endSession(env, session.idHash, secure);
    if (session.user.id !== userId) await deleteUserIfOrphanGuest(env, session.user.id);
  }
  setCookies.push(await startSession(env, userId, secure));
  return redirect(safeNext(st.next) + (st.next.includes('?') ? '&' : '?') + 'acct=in', setCookies);
}

// ---- POST /api/auth/guest ----
export async function guestLogin(req: Request, env: Env): Promise<Response> {
  const url = new URL(req.url);
  const existing = await readSession(env, req);
  if (existing) return json({ ok: true, already: true });

  let name = guestName();
  try {
    const body = (await req.json()) as { name?: string };
    if (typeof body.name === 'string' && body.name.trim()) name = body.name.trim().slice(0, 24);
  } catch { /* empty body is fine */ }

  const id = crypto.randomUUID();
  await createUser(env, { id, display_name: name, is_guest: true });
  const setCookie = await startSession(env, id, url.protocol === 'https:');
  return json({ ok: true }, 200, { 'set-cookie': setCookie });
}

// ---- POST /api/auth/logout ----
export async function logout(req: Request, env: Env): Promise<Response> {
  const secure = new URL(req.url).protocol === 'https:';
  const session = await readSession(env, req);
  const clear = session ? await endSession(env, session.idHash, secure) : null;
  if (session?.user.is_guest) await deleteUserIfOrphanGuest(env, session.user.id);
  return json({ ok: true }, 200, clear ? { 'set-cookie': clear } : {});
}

// ---- GET /api/me ----
export async function me(req: Request, env: Env, session: SessionCtx | null): Promise<Response> {
  if (!session) return json({ user: null });
  const [accounts, clouds] = await Promise.all([
    listAccounts(env, session.user.id),
    listClouds(env, session.user.id),
  ]);
  const h: Record<string, string> = {};
  if (session.setCookies.length) h['set-cookie'] = session.setCookies[0];
  return json({
    user: publicUser(session.user),
    accounts: accounts.map((a) => a.provider),
    clouds: clouds.map((c) => ({ provider: c.provider, label: c.account_label })),
  }, 200, h);
}

// ---- PATCH /api/me {display_name} ----
export async function patchMe(req: Request, env: Env, session: SessionCtx): Promise<Response> {
  let body: { display_name?: string };
  try {
    body = (await req.json()) as { display_name?: string };
  } catch {
    return json({ error: 'bad_json' }, 400);
  }
  if (typeof body.display_name === 'string') {
    const name = body.display_name.trim().slice(0, 24);
    if (!name) return json({ error: 'empty_name' }, 400);
    await updateUserProfile(env, session.user.id, { display_name: name });
  }
  return json({ ok: true });
}

export function publicUser(u: UserRow) {
  return {
    id: u.id,
    display_name: u.display_name,
    avatar_url: u.avatar_url,
    is_guest: !!u.is_guest,
  };
}
