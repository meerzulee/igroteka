// LobbyRoom — one Durable Object per multiplayer room.
//
// Holds the roster and relays the WebRTC handshake (offer/answer/ICE) between
// peers. Uses the WebSocket Hibernation API so an idle room costs nothing and
// survives DO eviction: per-connection state lives in the socket's attachment,
// so the whole roster is reconstructable from ctx.getWebSockets() after a wake.
//
// It never sees SDP contents — it forwards opaque `data` blobs by target id.
// It never touches game traffic. It is deliberately dumb (see CLAUDE.md).

import { DurableObject } from "cloudflare:workers";
import { getIceServers } from "./turn.js";

const MAX_PLAYERS = 8; // ZH tops out at 8; 1v1 is the v1.0 target.
const MAX_NAME = 24;
const MAX_CHAT = 500;

export class LobbyRoom extends DurableObject {
  // WebSocket upgrade. The Worker forwards the client Request here.
  async fetch(request) {
    const url = new URL(request.url);
    const name = (url.searchParams.get("name") || "player").slice(0, MAX_NAME);

    if (this.ctx.getWebSockets().length >= MAX_PLAYERS)
      return new Response("room full", { status: 403 });

    const [client, server] = Object.values(new WebSocketPair());

    const id = crypto.randomUUID().slice(0, 8);
    const host = this.ctx.getWebSockets().length === 0; // first in = host

    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ id, name, host, ready: false });

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

  onGone(ws) {
    try {
      ws.close();
    } catch {}
    // getWebSockets() still includes the closing socket during this handler, so
    // exclude it everywhere: host promotion AND the roster we broadcast.
    const remaining = this.ctx.getWebSockets().filter((s) => s !== ws);
    const stillHosted = remaining.some((s) => (s.deserializeAttachment() || {}).host);
    if (!stillHosted && remaining.length) {
      const s = remaining[0];
      const a = s.deserializeAttachment();
      a.host = true;
      s.serializeAttachment(a);
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
      .map(({ id, name, host, ready }) => ({ id, name, host, ready }));
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
