// Small HTTP helpers shared by all handlers.

export function json(data: unknown, status = 200, headers: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: { 'content-type': 'application/json; charset=utf-8', ...headers },
  });
}

// __Host- prefix over HTTPS: locks the cookie to this exact host (Secure,
// Path=/, no Domain), so a sibling subdomain on the same registrable domain
// cannot plant it. Plain name over local http where the prefix's Secure
// requirement can't be met.
export function cookieName(base: string, secure: boolean): string {
  return secure ? `__Host-${base}` : base;
}

// Read a cookie by base name, tolerating either the prefixed or plain form so
// a single deployment (http dev / https prod) resolves without threading the
// secure flag through every read.
export function readCookie(req: Request, base: string): string | undefined {
  const jar = parseCookies(req);
  return jar[`__Host-${base}`] ?? jar[base];
}

export function parseCookies(req: Request): Record<string, string> {
  const out: Record<string, string> = {};
  const raw = req.headers.get('cookie');
  if (!raw) return out;
  for (const part of raw.split(';')) {
    const eq = part.indexOf('=');
    if (eq < 0) continue;
    out[part.slice(0, eq).trim()] = part.slice(eq + 1).trim();
  }
  return out;
}

export interface CookieOpts {
  maxAge?: number;       // seconds; 0 deletes
  path?: string;
  secure?: boolean;
  sameSite?: 'Lax' | 'Strict' | 'None';
}

export function cookie(name: string, value: string, opts: CookieOpts = {}): string {
  const parts = [`${name}=${value}`];
  parts.push(`Path=${opts.path ?? '/'}`);
  parts.push('HttpOnly');
  parts.push(`SameSite=${opts.sameSite ?? 'Lax'}`);
  if (opts.secure) parts.push('Secure');
  if (opts.maxAge !== undefined) parts.push(`Max-Age=${opts.maxAge}`);
  return parts.join('; ');
}

// CSRF guard for state-changing endpoints: if the browser sent an Origin, it
// must be our own. (Session cookies are SameSite=Lax as the first layer.)
export function sameOrigin(req: Request): boolean {
  const origin = req.headers.get('origin');
  if (!origin) return true; // non-browser client (curl); cookie auth still applies
  return origin === new URL(req.url).origin;
}

// Only ever redirect within our own site.
export function safeNext(next: string | null): string {
  if (!next || !next.startsWith('/') || next.startsWith('//')) return '/';
  return next;
}

export function redirect(location: string, setCookies: string[] = []): Response {
  const h = new Headers({ location });
  for (const c of setCookies) h.append('set-cookie', c);
  return new Response(null, { status: 302, headers: h });
}
