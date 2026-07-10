// Igroteka cafe worker: accounts + cloud-save connections (lobby comes next).
// Deployed together with the static site: asset paths are served before the
// worker runs, so everything here answers under /api/*.

import type { Env } from './env';
import { json, sameOrigin } from './http';
import { readSession } from './session';
import { authCallback, authStart, guestLogin, logout, me, patchMe } from './auth';
import { connectCallback, connectStart, disconnectCloud, mintCloudToken } from './cloud';
import {
  AUTH_PROVIDERS, CLOUD_PROVIDERS, isAuthProvider, isCloudProvider, providerConfigured,
} from './providers';

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);
    const path = url.pathname;

    if (!path.startsWith('/api/')) return json({ error: 'not_found' }, 404);

    // CSRF: state-changing requests must come from our own origin (session
    // cookies are SameSite=Lax as the first layer).
    if (req.method !== 'GET' && req.method !== 'HEAD' && !sameOrigin(req)) {
      return json({ error: 'bad_origin' }, 403);
    }

    try {
      return await route(req, env, path);
    } catch (e) {
      console.error(`${req.method} ${path}:`, e);
      return json({ error: 'internal' }, 500);
    }
  },
} satisfies ExportedHandler<Env>;

async function route(req: Request, env: Env, path: string): Promise<Response> {
  const m = req.method;

  if (m === 'GET' && path === '/api/health') return json({ ok: true });

  // Which providers this deployment has credentials for — the UI greys out
  // the rest instead of bouncing through a doomed redirect.
  if (m === 'GET' && path === '/api/config') {
    return json({
      providers: Object.fromEntries(AUTH_PROVIDERS.map((p) => [p, providerConfigured(env, p)])),
      clouds: Object.fromEntries(CLOUD_PROVIDERS.map((p) => [p, providerConfigured(env, p) && !!env.TOKEN_KEY])),
    });
  }

  // /api/auth/:provider/(start|callback)
  const auth = path.match(/^\/api\/auth\/([a-z]+)\/(start|callback)$/);
  if (auth && m === 'GET') {
    const [, provider, step] = auth;
    if (!isAuthProvider(provider)) return json({ error: 'unknown_provider' }, 404);
    return step === 'start' ? await authStart(req, env, provider) : await authCallback(req, env, provider);
  }

  if (m === 'POST' && path === '/api/auth/guest') return guestLogin(req, env);
  if (m === 'POST' && path === '/api/auth/logout') return logout(req, env);

  const session = await readSession(env, req);

  if (path === '/api/me') {
    if (m === 'GET') return me(req, env, session);
    if (m === 'PATCH') return session ? patchMe(req, env, session) : json({ error: 'auth_required' }, 401);
  }

  // /api/connect/:provider/(start|callback)
  const connect = path.match(/^\/api\/connect\/([a-z]+)\/(start|callback)$/);
  if (connect && m === 'GET') {
    const [, provider, step] = connect;
    if (!isCloudProvider(provider)) return json({ error: 'unknown_provider' }, 404);
    return step === 'start'
      ? connectStart(req, env, provider, session)
      : connectCallback(req, env, provider, session);
  }

  // /api/cloud/:provider[/token]
  const cloud = path.match(/^\/api\/cloud\/([a-z]+)(\/token)?$/);
  if (cloud) {
    const [, provider, tokenPart] = cloud;
    if (!isCloudProvider(provider)) return json({ error: 'unknown_provider' }, 404);
    if (!session) return json({ error: 'auth_required' }, 401);
    if (m === 'POST' && tokenPart) return mintCloudToken(req, env, provider, session);
    if (m === 'DELETE' && !tokenPart) return disconnectCloud(req, env, provider, session);
  }

  return json({ error: 'not_found' }, 404);
}
