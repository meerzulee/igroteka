export interface Env {
  DB: D1Database;

  // OAuth client credentials. IDs are vars (public by nature); secrets are
  // worker secrets (`wrangler secret put`) / .dev.vars locally. A provider
  // with an empty id is simply reported as unconfigured by /api/config.
  GOOGLE_CLIENT_ID?: string;
  GOOGLE_CLIENT_SECRET?: string;
  GITHUB_CLIENT_ID?: string;
  GITHUB_CLIENT_SECRET?: string;
  DISCORD_CLIENT_ID?: string;
  DISCORD_CLIENT_SECRET?: string;
  DROPBOX_CLIENT_ID?: string;
  DROPBOX_CLIENT_SECRET?: string;

  // base64 of 32 random bytes; encrypts cloud refresh tokens at rest.
  TOKEN_KEY?: string;
}
