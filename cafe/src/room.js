// LobbyRoom — one Durable Object per multiplayer room.
//
// Holds the roster and relays the WebRTC handshake (offer/answer/ICE) between
// peers. Uses the WebSocket Hibernation API so an idle room costs nothing and
// survives DO eviction: per-connection state lives in the socket's attachment,
// so the whole roster is reconstructable from ctx.getWebSockets() after a wake.
//
// It never sees SDP contents — it forwards opaque `data` blobs by target id.
// It never touches game traffic. It is deliberately dumb (see CLAUDE.md).
//
// Two kinds of room:
//  - OPEN (no stored party): anyone with the code joins, first-in is host. This
//    is the dev / casual LAN flow (e.g. room "lan", two browser tabs).
//  - PROTECTED (createParty was called => a verifier+secret live in ctx.storage):
//    the WS upgrade REQUIRES a valid signed token; host role comes from the
//    token, not connection order. This is the password-gated private party — a
//    leaked game URL carries no token, so the DO rejects it.

import { DurableObject } from "cloudflare:workers";
import { getIceServers } from "./turn.js";
import {
  b64uEncode, b64uDecode, randomBytes, deriveVerifier, constantTimeEqual,
  DEFAULT_ITERS, mintToken, verifyToken,
} from "./auth.js";

const MAX_PLAYERS = 8; // ZH tops out at 8; 1v1 is the v1.0 target.
const MAX_NAME = 24;
const MAX_CHAT = 500;
const TOKEN_TTL_SEC = 1800; // 30 min: outlives a human-paced gather + the
                            // post-launch WS reconnect. Reused from localStorage
                            // for in-session reconnects.
const AUTH_MAX_FAILS = 5;   // wrong-password attempts per IP before a lockout
const AUTH_LOCKOUT_MS = 60_000;

export class LobbyRoom extends DurableObject {
  // ---- RPC: create a password-protected party (called by the Worker /party) ----
  // `code` is the server-generated room code this DO is addressed by. Stores the
  // PBKDF2 verifier + a per-room HMAC secret, binds the creator as host, and
  // returns a signed host token. 409 if a party already lives here (never clobber
  // a live party — protects against a code collision).
  async createParty(code, password) {
    if (await this.ctx.storage.get("party"))
      return { error: "party already exists", status: 409 };
    const salt = randomBytes(16);
    const verifier = await deriveVerifier(password, salt, DEFAULT_ITERS);
    const secret = randomBytes(32);
    await this.ctx.storage.put("party", {
      code,
      salt: b64uEncode(salt),
      iters: DEFAULT_ITERS,
      verifier: b64uEncode(verifier),
      createdAt: Date.now(),
    });
    await this.ctx.storage.put("secret", b64uEncode(secret));
    const token = await mintToken(secret, { room: code, role: "host", ttlSec: TOKEN_TTL_SEC });
    return { token, role: "host" };
  }

  // ---- RPC: authenticate into an existing party (called by the Worker /auth) ----
  // Verifies the password against the stored verifier, rate-limits wrong guesses
  // per IP, and issues a signed guest token on success.
  async authenticate(code, password, ip) {
    const party = await this.ctx.storage.get("party");
    if (!party) return { error: "no such party", status: 404 };

    const rlKey = "af:" + ip;
    const now = Date.now();
    const rl = (await this.ctx.storage.get(rlKey)) || { n: 0, until: 0 };
    if (rl.until > now)
      return { error: "too many attempts — wait a minute", status: 429 };

    const verifier = await deriveVerifier(password, b64uDecode(party.salt), party.iters);
    if (!constantTimeEqual(verifier, b64uDecode(party.verifier))) {
      rl.n += 1;
      if (rl.n >= AUTH_MAX_FAILS) { rl.until = now + AUTH_LOCKOUT_MS; rl.n = 0; }
      await this.ctx.storage.put(rlKey, rl);
      return { error: "wrong password", status: 403 };
    }
    await this.ctx.storage.delete(rlKey); // reset the counter on success

    const secret = b64uDecode(await this.ctx.storage.get("secret"));
    const token = await mintToken(secret, { room: party.code, role: "guest", ttlSec: TOKEN_TTL_SEC });
    return { token, role: "guest" };
  }

  // ---- WebSocket upgrade (the Worker forwards the client Request here) ----
  async fetch(request) {
    const url = new URL(request.url);
    const name = (url.searchParams.get("name") || "player").slice(0, MAX_NAME);

    // Protected room? Then the token is the gate. This is the enforcement point:
    // no valid token => no socket => a leaked URL can't join.
    const party = await this.ctx.storage.get("party");
    let tokenRole = null;
    if (party) {
      const token = url.searchParams.get("token") || "";
      const secretB64 = await this.ctx.storage.get("secret");
      const claims = secretB64 ? await verifyToken(token, b64uDecode(secretB64)) : null;
      if (!claims || claims.r !== party.code)
        return new Response("unauthorized", { status: 401 });
      tokenRole = claims.role;
    }

    if (this.ctx.getWebSockets().length >= MAX_PLAYERS)
      return new Response("room full", { status: 403 });

    const [client, server] = Object.values(new WebSocketPair());

    const id = crypto.randomUUID().slice(0, 8);
    // Host: from the token role in a protected room (authoritative, survives the
    // reconnect-reshuffle when everyone launches at once); first-in for an open
    // room. In a protected room this is the single source of truth the engine's
    // hostIP() lookup depends on.
    const host = party ? tokenRole === "host" : this.ctx.getWebSockets().length === 0;

    // Stable per-connection slot (1-based, lowest free). The engine derives its
    // synthetic IP 10.0.0.<slot> from this. Server-assigned + stable (not a client
    // sort that reshuffles as peers join), so a peer's IP never changes and every
    // client agrees — the routable-identity invariant the transport relies on.
    const usedSlots = new Set(
      this.ctx.getWebSockets().map((s) => (s.deserializeAttachment() || {}).slot).filter(Boolean)
    );
    let slot = 1;
    while (usedSlots.has(slot)) slot++;

    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ id, name, host, ready: false, slot });

    server.send(
      JSON.stringify({
        t: "welcome",
        youAre: id,
        host,
        iceServers: await getIceServers(this.env),
      })
    );
    this.broadcastRoster();

    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(ws, raw) {
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    const me = ws.deserializeAttachment();
    if (!me) return;

    switch (msg.t) {
      // Relay an opaque SDP/ICE blob to one specific peer.
      case "signal": {
        const target = this.find(msg.to);
        if (target)
          target.send(JSON.stringify({ t: "signal", from: me.id, data: msg.data }));
        break;
      }

      case "ready": {
        me.ready = !!msg.ready;
        ws.serializeAttachment(me);
        this.broadcastRoster();
        break;
      }

      case "chat": {
        this.broadcast({
          t: "chat",
          from: me.id,
          name: me.name,
          text: String(msg.text ?? "").slice(0, MAX_CHAT),
        });
        break;
      }

      // Host launches. The seed is authoritative: every peer's engine seeds its
      // RNG from it (InitRandom), which is what keeps deterministic lockstep in
      // sync. Do NOT let clients pick their own seed.
      case "start": {
        if (!me.host) return;
        const seed = crypto.getRandomValues(new Uint32Array(1))[0] >>> 0;
        this.broadcast({
          t: "start",
          seed,
          map: msg.map ?? null,
          slots: this.roster(),
        });
        break;
      }
    }
  }

  webSocketClose(ws) {
    this.onGone(ws);
  }
  webSocketError(ws) {
    this.onGone(ws);
  }

  async onGone(ws) {
    try {
      ws.close();
    } catch {}
    // getWebSockets() still includes the closing socket during this handler, so
    // exclude it everywhere: host promotion AND the roster we broadcast.
    const remaining = this.ctx.getWebSockets().filter((s) => s !== ws);
    // Promote a new host ONLY in open rooms. In a protected room the host role is
    // token-bound — auto-promoting a guest would make the roster say "host" for a
    // peer whose engine launched as a joiner, misrouting everyone's hostIP().
    const party = await this.ctx.storage.get("party");
    if (!party) {
      const stillHosted = remaining.some((s) => (s.deserializeAttachment() || {}).host);
      if (!stillHosted && remaining.length) {
        const s = remaining[0];
        const a = s.deserializeAttachment();
        a.host = true;
        s.serializeAttachment(a);
      }
    }
    this.broadcastRoster(ws);
  }

  find(id) {
    for (const s of this.ctx.getWebSockets()) {
      const a = s.deserializeAttachment();
      if (a && a.id === id) return s;
    }
    return null;
  }

  roster(except) {
    return this.ctx
      .getWebSockets()
      .filter((s) => s !== except)
      .map((s) => s.deserializeAttachment())
      .filter(Boolean)
      .map(({ id, name, host, ready, slot }) => ({ id, name, host, ready, slot }));
  }

  broadcastRoster(except) {
    this.broadcast({ t: "roster", players: this.roster(except) }, except);
  }

  broadcast(obj, except) {
    const s = JSON.stringify(obj);
    for (const ws of this.ctx.getWebSockets()) {
      if (ws === except) continue;
      try {
        ws.send(s);
      } catch {}
    }
  }
}
