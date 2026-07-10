-- Igroteka cafe: accounts core.
-- Users are guest-first: a guest row is a real user that OAuth links later
-- promote in place (same id — anything keyed to the user survives the claim).

CREATE TABLE users (
  id            TEXT PRIMARY KEY,             -- crypto.randomUUID()
  handle        TEXT UNIQUE,                  -- reserved for lobby-era @handles
  display_name  TEXT NOT NULL,
  avatar_url    TEXT,
  is_guest      INTEGER NOT NULL DEFAULT 0,
  created_at    INTEGER NOT NULL DEFAULT (unixepoch()),
  last_seen_at  INTEGER
);

-- One row per linked identity. A user may link several providers; a
-- provider identity belongs to exactly one user.
CREATE TABLE oauth_accounts (
  provider          TEXT NOT NULL,            -- 'google' | 'github' | 'discord'
  provider_user_id  TEXT NOT NULL,
  user_id           TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  email             TEXT,
  created_at        INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (provider, provider_user_id)
);
CREATE INDEX oauth_accounts_user ON oauth_accounts(user_id);

-- Sessions: id is sha256(cookie token) — a DB leak exposes no usable tokens.
CREATE TABLE sessions (
  id          TEXT PRIMARY KEY,
  user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at  INTEGER NOT NULL DEFAULT (unixepoch()),
  expires_at  INTEGER NOT NULL
);
CREATE INDEX sessions_user ON sessions(user_id);

-- Cloud-save connections. Refresh tokens are AES-GCM encrypted with the
-- TOKEN_KEY worker secret; file bytes go browser -> provider directly and
-- never through Igroteka infrastructure.
CREATE TABLE cloud_connections (
  user_id            TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  provider           TEXT NOT NULL,           -- 'gdrive' | 'dropbox'
  account_id         TEXT,                    -- provider-side account id
  account_label      TEXT,                    -- email/name shown in UI
  refresh_token_enc  TEXT NOT NULL,
  scope              TEXT,
  created_at         INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (user_id, provider)
);
