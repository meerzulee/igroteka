// cafe — WebRTC signaling + lobby Worker for Igroteka multiplayer.
// MIT. Engine-independent: this box only brokers the WebRTC handshake and holds
// ephemeral lobby state. Game traffic never flows through here — it goes
// peer-to-peer (browser <-> browser) over the DataChannels this server helps open.
//
// Routes:
//   GET  /health              -> liveness
//   GET  /ice                 -> ICE server list (STUN always; TURN if configured)
//   POST /party               -> create a password-protected private party
//                                {password} -> {roomCode, token, role:"host"}
//   POST /room/<code>/auth    -> join an existing party with the password
//                                {password} -> {roomCode, token, role:"guest"}
//   GET  /room/<code>/ws       -> WebSocket upgrade into the LobbyRoom DO
//                                (protected rooms require ?token=)
//
// One Durable Object instance per room id (deterministic via getByName).

import { LobbyRoom } from "./room.js";
import { Throttle } from "./throttle.js";
import { Directory } from "./directory.js";
import { getIceServers } from "./turn.js";
import { newRoomCode } from "./auth.js";

export { LobbyRoom, Throttle, Directory };

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

const MIN_PASSWORD = 4;
const PARTY_CREATE_LIMIT = 20; // per IP per hour — runaway-loop guard, not security

async function readBody(request) {
  const body = await request.json().catch(() => ({}));
  return {
    password: typeof body?.password === "string" ? body.password : "",
    name: typeof body?.name === "string" ? body.name : "",
  };
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const ip = request.headers.get("CF-Connecting-IP") || "0.0.0.0";

    if (request.method === "OPTIONS") return new Response(null, { headers: CORS });

    if (url.pathname === "/health") return json({ ok: true, service: "cafe" });

    if (url.pathname === "/ice") return json({ iceServers: await getIceServers(env) });

    // GET /lobbies — the public (no-password) lobby directory.
    if (url.pathname === "/lobbies") {
      const rooms = await env.DIRECTORY.getByName("global").list();
      return json({ lobbies: rooms });
    }

    // POST /party — create a party. Server picks the room code (high entropy,
    // unguessable). An empty password makes it a PUBLIC lobby (listed, code = the
    // only credential); a password makes it PRIVATE (never listed, token-gated).
    if (url.pathname === "/party" && request.method === "POST") {
      const allowed = await env.THROTTLE.getByName("global").hit(ip, PARTY_CREATE_LIMIT, 3_600_000);
      if (!allowed) return json({ error: "rate limited — try later" }, 429);

      const { password, name } = await readBody(request);
      if (password && password.length < MIN_PASSWORD)
        return json({ error: `password must be at least ${MIN_PASSWORD} characters` }, 400);

      const code = newRoomCode();
      const res = await env.LOBBY.getByName(code).createParty(code, password, name);
      if (res.error) return json({ error: res.error }, res.status || 400);
      return json({ roomCode: code, token: res.token, role: res.role, open: res.open });
    }

    // POST /room/<code>/auth — get a join token. Public rooms ignore the
    // password; private rooms require it.
    const a = url.pathname.match(/^\/room\/([A-Za-z0-9_-]{1,64})\/auth$/);
    if (a && request.method === "POST") {
      const { password } = await readBody(request);
      const res = await env.LOBBY.getByName(a[1]).authenticate(a[1], password, ip);
      if (res.error) return json({ error: res.error }, res.status || 403);
      return json({ roomCode: a[1], token: res.token, role: res.role });
    }

    // GET /room/<code>/ws — hand the upgrade to that room's Durable Object.
    // The DO enforces the token gate for protected rooms.
    const m = url.pathname.match(/^\/room\/([A-Za-z0-9_-]{1,64})\/ws$/);
    if (m) {
      if (request.headers.get("Upgrade") !== "websocket")
        return json({ error: "expected websocket upgrade" }, 426);
      return env.LOBBY.getByName(m[1]).fetch(request);
    }

    return json({ error: "not found" }, 404);
  },
};
