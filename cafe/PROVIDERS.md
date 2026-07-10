# Cafe OAuth provider setup

Accounts work without any of this (guest profiles need zero config). Each
provider you configure lights up its button; the rest stay greyed out
(`/api/config` tells the UI what's live).

Two environments, two redirect URI sets:

| | Production | Local dev |
|---|---|---|
| Origin | `https://igroteka.mrz.sh` | `http://localhost:8787` (or the `--port` you pass) |

Secrets go in:
- **Prod:** `npx wrangler secret put <NAME>` (run in `cafe/`, uses `--config ../wrangler.toml` via npm scripts, or just `cd cafe && npx wrangler secret put NAME --config ../wrangler.toml`)
- **Dev:** `.dev.vars` at repo root (gitignored; template in `.dev.vars.example`)

Client IDs are not secret: prod values go in `wrangler.toml` `[vars]`, dev
values in `.dev.vars`.

## TOKEN_KEY (required for cloud saves)

```sh
openssl rand -base64 32   # -> TOKEN_KEY
```

Encrypts Drive/Dropbox refresh tokens at rest in D1. Without it, sign-in works
but cloud connects return `token_key_missing`.

## Google (sign-in + Google Drive saves)

One OAuth client covers both flows.

1. https://console.cloud.google.com → create project `igroteka` → APIs & Services
2. OAuth consent screen: External, app name Igroteka, scopes: `openid`,
   `email`, `profile`, `.../auth/drive.appdata` (all non-sensitive — no
   verification review needed)
3. Enable the **Google Drive API** (APIs & Services → Library)
4. Credentials → Create OAuth client ID → Web application. Authorized redirect URIs (all four):
   - `https://igroteka.mrz.sh/api/auth/google/callback`
   - `https://igroteka.mrz.sh/api/connect/gdrive/callback`
   - `http://localhost:8787/api/auth/google/callback`
   - `http://localhost:8787/api/connect/gdrive/callback`
5. → `GOOGLE_CLIENT_ID` (var) + `GOOGLE_CLIENT_SECRET` (secret)

Saves live in the hidden `appDataFolder` — invisible in the user's Drive UI,
scoped to our client ID only.

## GitHub (sign-in)

GitHub OAuth apps allow **one** callback URL each → make two apps (prod + dev).

1. https://github.com/settings/developers → New OAuth App
2. Prod app: homepage `https://igroteka.mrz.sh`, callback
   `https://igroteka.mrz.sh/api/auth/github/callback`
3. Dev app: callback `http://localhost:8787/api/auth/github/callback`
4. → `GITHUB_CLIENT_ID` + `GITHUB_CLIENT_SECRET` per environment

## Discord (sign-in)

1. https://discord.com/developers/applications → New Application `Igroteka`
2. OAuth2 → Redirects (both):
   - `https://igroteka.mrz.sh/api/auth/discord/callback`
   - `http://localhost:8787/api/auth/discord/callback`
3. → `DISCORD_CLIENT_ID` + `DISCORD_CLIENT_SECRET`

## Dropbox (cloud saves)

1. https://www.dropbox.com/developers/apps → Create app
2. Scoped access → **App folder** (saves land in `Apps/Igroteka/`, nothing else reachable)
3. Permissions tab: `account_info.read`, `files.metadata.read`,
   `files.content.read`, `files.content.write` → Submit
4. Settings tab → Redirect URIs (both):
   - `https://igroteka.mrz.sh/api/connect/dropbox/callback`
   - `http://localhost:8787/api/connect/dropbox/callback`
5. App key/secret → `DROPBOX_CLIENT_ID` + `DROPBOX_CLIENT_SECRET`

## Deploy checklist

```sh
cd cafe
npm run migrate:remote                      # D1 schema (already applied once)
npx wrangler secret put TOKEN_KEY --config ../wrangler.toml
npx wrangler secret put GOOGLE_CLIENT_SECRET --config ../wrangler.toml   # etc.
# client IDs -> wrangler.toml [vars]
npm run deploy
```

## Notes

- No rate limiting in v1 (guest creation is an unauthenticated INSERT).
  Cloudflare WAF default rules are the only shield — revisit before lobby
  launch.
- Legal line (PLAN.md): saves/replays are user-created data and may flow
  through user-chosen clouds; game assets never touch these connections —
  asset import stays a purely client-side wizard flow.
