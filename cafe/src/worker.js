// cafe — WebRTC signaling + lobby Worker for Igroteka multiplayer.
// MIT. Engine-independent: this box only brokers the WebRTC handshake and holds
// ephemeral lobby state. Game traffic never flows through here — it goes
// peer-to-peer (browser <-> browser) over the DataChannels this server helps open.
//
// Routes:
//   GET  /health            -> liveness
//   GET  /ice               -> ICE server list (STUN always; TURN if configured)
//   GET  /room/<id>/ws      -> WebSocket upgrade into the LobbyRoom Durable Object
//
// One Durable Object instance per room id (deterministic via getByName).

import { LobbyRoom } from "./room.js";
import { getIceServers } from "./turn.js";

export { LobbyRoom };

const CORS = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET,POST,OPTIONS",
  "access-control-allow-headers": "content-type",
};

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json", ...CORS },
  });

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "OPTIONS") return new Response(null, { headers: CORS });

    if (url.pathname === "/health") return json({ ok: true, service: "cafe" });

    if (url.pathname === "/ice") return json({ iceServers: await getIceServers(env) });

    // /room/<id>/ws  -> hand the upgrade to that room's Durable Object.
    const m = url.pathname.match(/^\/room\/([A-Za-z0-9_-]{1,64})\/ws$/);
    if (m) {
      if (request.headers.get("Upgrade") !== "websocket")
        return json({ error: "expected websocket upgrade" }, 426);
      const stub = env.LOBBY.getByName(m[1]);
      return stub.fetch(request);
    }

    return json({ error: "not found" }, 404);
  },
};
