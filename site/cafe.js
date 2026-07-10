// Igroteka cafe client: accounts + cloud-save connections.
// Same-origin API served by the cafe worker under /api/*.

async function req(path, opts = {}) {
  const r = await fetch(path, {
    headers: opts.body ? { 'content-type': 'application/json' } : undefined,
    ...opts,
  });
  let data = null;
  try { data = await r.json(); } catch { /* non-JSON error body */ }
  if (!r.ok) {
    const err = new Error((data && data.error) || `HTTP ${r.status}`);
    err.status = r.status;
    err.code = data && data.error;
    throw err;
  }
  return data;
}

// { user: {id, display_name, avatar_url, is_guest} | null, accounts: [], clouds: [] }
export const fetchMe = () => req('/api/me');

// { providers: {google, github, discord}, clouds: {gdrive, dropbox} }
export const fetchConfig = () => req('/api/config');

export const guestLogin = (name) =>
  req('/api/auth/guest', { method: 'POST', body: JSON.stringify(name ? { name } : {}) });

export const logout = () => req('/api/auth/logout', { method: 'POST' });

export const rename = (display_name) =>
  req('/api/me', { method: 'PATCH', body: JSON.stringify({ display_name }) });

export const disconnectCloud = (provider) =>
  req(`/api/cloud/${provider}`, { method: 'DELETE' });

// Sign-in / connect flows are full-page redirects (OAuth).
export const authStartURL = (provider, next = '/') =>
  `/api/auth/${provider}/start?next=${encodeURIComponent(next)}`;
export const connectStartURL = (provider, next = '/') =>
  `/api/connect/${provider}/start?next=${encodeURIComponent(next)}`;

// Short-lived access token for direct browser -> cloud calls (save files
// never pass through Igroteka servers). The save layer will build on this.
export const cloudToken = (provider) =>
  req(`/api/cloud/${provider}/token`, { method: 'POST' });
