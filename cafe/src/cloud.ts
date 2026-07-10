import { generateCodeVerifier, generateState, OAuth2RequestError } from 'arctic';
import type { Env } from './env';
import { deleteCloud, getCloud, upsertCloud } from './db';
import { json, redirect, safeNext } from './http';
import { decryptToken, encryptToken } from './crypto';
import type { SessionCtx } from './session';
import { oauthStateCookie, readOauthState } from './auth';
import {
  DROPBOX_SCOPES, GDRIVE_SCOPES, dropboxClient, fetchDropboxAccount, googleClient,
  googleProfileFromIdToken, providerConfigured, type CloudProvider,
} from './providers';

// Cloud saves ride the user's own cloud. The worker stores only an encrypted
// refresh token and mints short-lived access tokens for the browser; file
// bytes flow browser -> provider directly, never through Igroteka.

// ---- GET /api/connect/:provider/start?next=/ ----
export function connectStart(req: Request, env: Env, provider: CloudProvider, session: SessionCtx | null): Response {
  const url = new URL(req.url);
  if (!session) return json({ error: 'auth_required' }, 401);
  if (!providerConfigured(env, provider)) return json({ error: 'provider_not_configured' }, 503);
  if (!env.TOKEN_KEY) return json({ error: 'token_key_missing' }, 503);
  const secure = url.protocol === 'https:';
  const state = generateState();
  const next = safeNext(url.searchParams.get('next'));

  let authUrl: URL;
  let verifier: string | undefined;
  if (provider === 'gdrive') {
    verifier = generateCodeVerifier();
    authUrl = googleClient(env, url.origin, '/api/connect/gdrive/callback')!
      .createAuthorizationURL(state, verifier, GDRIVE_SCOPES);
    // Google only returns a refresh token for offline + forced-consent flows.
    authUrl.searchParams.set('access_type', 'offline');
    authUrl.searchParams.set('prompt', 'consent');
    authUrl.searchParams.set('include_granted_scopes', 'true');
  } else {
    authUrl = dropboxClient(env, url.origin)!.createAuthorizationURL(state, DROPBOX_SCOPES);
    authUrl.searchParams.set('token_access_type', 'offline');
  }

  return redirect(authUrl.toString(), [
    oauthStateCookie({ p: provider, s: state, v: verifier, next, mode: 'connect', uid: session.user.id }, secure),
  ]);
}

// ---- GET /api/connect/:provider/callback ----
export async function connectCallback(req: Request, env: Env, provider: CloudProvider, session: SessionCtx | null): Promise<Response> {
  const url = new URL(req.url);
  const secure = url.protocol === 'https:';
  const clearState = oauthStateCookie(null, secure);
  const fail = (reason: string) => redirect(`/?acct=err&reason=${encodeURIComponent(reason)}`, [clearState]);

  if (!session) return fail('auth_required');
  const st = readOauthState(req);
  const code = url.searchParams.get('code');
  const state = url.searchParams.get('state');
  if (!st || st.mode !== 'connect' || st.p !== provider) return fail('state_mismatch');
  if (!code || !state || state !== st.s) return fail(url.searchParams.get('error') || 'state_mismatch');
  // The connection is written under the session that started the flow; a
  // tossed state cookie can't attach an attacker's cloud to the victim.
  if (st.uid !== session.user.id) return fail('session_changed');

  try {
    let refreshToken: string;
    let accountId: string | null = null;
    let label: string | null = null;
    let scope: string;
    if (provider === 'gdrive') {
      const tokens = await googleClient(env, url.origin, '/api/connect/gdrive/callback')!
        .validateAuthorizationCode(code, st.v!);
      if (!tokens.hasRefreshToken()) return fail('no_refresh_token');
      refreshToken = tokens.refreshToken();
      const p = googleProfileFromIdToken(tokens.idToken());
      accountId = p.providerUserId;
      label = p.email;
      scope = GDRIVE_SCOPES.join(' ');
    } else {
      const tokens = await dropboxClient(env, url.origin)!.validateAuthorizationCode(code);
      if (!tokens.hasRefreshToken()) return fail('no_refresh_token');
      refreshToken = tokens.refreshToken();
      const acct = await fetchDropboxAccount(tokens.accessToken());
      accountId = acct.accountId;
      label = acct.label;
      scope = DROPBOX_SCOPES.join(' ');
    }

    await upsertCloud(env, {
      user_id: session.user.id,
      provider,
      account_id: accountId,
      account_label: label,
      refresh_token_enc: await encryptToken(refreshToken, env.TOKEN_KEY!),
      scope,
    });
  } catch (e) {
    console.error(`connect ${provider} callback:`, e);
    return fail('exchange_failed');
  }

  return redirect(safeNext(st.next) + (st.next.includes('?') ? '&' : '?') + `acct=connected-${provider}`, [clearState]);
}

// ---- POST /api/cloud/:provider/token ----
// Mint a short-lived access token for direct browser -> provider calls.
export async function mintCloudToken(req: Request, env: Env, provider: CloudProvider, session: SessionCtx): Promise<Response> {
  const url = new URL(req.url);
  if (!env.TOKEN_KEY) return json({ error: 'token_key_missing' }, 503);
  const row = await getCloud(env, session.user.id, provider);
  if (!row) return json({ error: 'not_connected' }, 404);

  let refreshToken: string;
  try {
    refreshToken = await decryptToken(row.refresh_token_enc, env.TOKEN_KEY);
  } catch (e) {
    console.error('token decrypt failed:', e);
    return json({ error: 'decrypt_failed' }, 500);
  }

  try {
    const tokens = provider === 'gdrive'
      ? await googleClient(env, url.origin, '/api/connect/gdrive/callback')!.refreshAccessToken(refreshToken)
      : await dropboxClient(env, url.origin)!.refreshAccessToken(refreshToken);
    return json({
      access_token: tokens.accessToken(),
      expires_in: tokens.accessTokenExpiresInSeconds(),
      provider,
    });
  } catch (e) {
    // Only a real grant rejection (revoked/expired refresh token) kills the
    // connection. A network blip or provider 5xx is transient — keep the row
    // and let the client retry, or a stormy provider would wipe every link.
    if (e instanceof OAuth2RequestError && e.code === 'invalid_grant') {
      console.warn(`refresh ${provider} rejected (invalid_grant), dropping connection`);
      await deleteCloud(env, session.user.id, provider);
      return json({ error: 'connection_revoked' }, 410);
    }
    console.error(`refresh ${provider} transient failure:`, e);
    return json({ error: 'refresh_failed' }, 502);
  }
}

// ---- DELETE /api/cloud/:provider ----
export async function disconnectCloud(req: Request, env: Env, provider: CloudProvider, session: SessionCtx): Promise<Response> {
  const url = new URL(req.url);
  const row = await getCloud(env, session.user.id, provider);
  if (row && env.TOKEN_KEY) {
    // Best-effort remote revocation; the row goes away regardless.
    try {
      const refreshToken = await decryptToken(row.refresh_token_enc, env.TOKEN_KEY);
      if (provider === 'gdrive') {
        await googleClient(env, url.origin, '/api/connect/gdrive/callback')!.revokeToken(refreshToken);
      } else {
        await dropboxClient(env, url.origin)!.revokeToken(refreshToken);
      }
    } catch (e) {
      console.warn(`revoke ${provider} failed (continuing):`, e);
    }
  }
  await deleteCloud(env, session.user.id, provider);
  return json({ ok: true });
}
