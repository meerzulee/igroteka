# cafe

WebRTC **signaling + lobby** for Igroteka multiplayer. MIT, engine-independent.

This server does **not** host or relay gameplay. Game traffic is peer-to-peer
(browser ↔ browser) over WebRTC DataChannels. cafe only:

1. brokers the WebRTC handshake (relays SDP offer/answer + ICE candidates), and
2. holds ephemeral lobby state (roster, ready, host, the authoritative start seed).

Once the DataChannel opens, the two engines talk directly. cafe never sees a
command packet. This is why you don't host a "game server" — there isn't one.

```
browser A ─┐                          ┌─ browser B
           ├─ WS ─► cafe Worker + DO ◄─┤     (handshake only)
           │        (one DO per room)  │
           └────── WebRTC DataChannel ─┘     (all game bytes, P2P)
```

## Architecture

- **Worker** (`src/worker.js`) — routes `/health`, `/ice`, and the
  `/room/<id>/ws` WebSocket upgrade to the right room.
- **LobbyRoom** (`src/room.js`) — one Durable Object per room. Uses the
  **WebSocket Hibernation API**, so idle rooms cost nothing and survive eviction
  (per-player state lives in each socket's attachment; the roster is rebuilt from
  `ctx.getWebSockets()`). Relays `signal` blobs by target id; broadcasts roster.
- **ICE** (`src/turn.js`) — returns STUN (free, always) plus Cloudflare TURN
  relay creds when configured. TURN is the fallback for the ~15% of peer pairs
  that can't hole-punch; it's the only piece that costs money ($0.05/GB relayed).
- **Client** (`client/signaling.js`) — `CafeClient`: joins a room and opens a
  full-mesh set of DataChannels using MDN "perfect negotiation". The channel is
  `ordered:false, maxRetransmits:0` (unreliable/unordered = UDP), which is what
  the engine's transport layer expects; the engine's own ACK layer handles reliability.

## Run locally

```bash
cd cafe
npm install
npm run dev            # wrangler dev on http://localhost:8787
```

Then serve the demo and open it in two tabs:

```bash
# from cafe/client, any static server, e.g.:
python3 -m http.server 8080
# open http://localhost:8080/demo.html in two tabs, set server to
# http://localhost:8787, join the same room, watch the DataChannel open.
```

`wrangler dev` runs the Durable Object locally (Miniflare), so the full
signaling flow works offline. Two tabs on one machine connect over host-candidate
ICE without needing STUN/TURN.

## Deploy

```bash
npm run deploy         # wrangler deploy
```

Optional TURN relay (needed for players behind symmetric NAT):

```bash
# create a TURN key in the Cloudflare dashboard: Realtime → TURN
wrangler secret put TURN_KEY_ID
wrangler secret put TURN_KEY_API_TOKEN
```

Without those secrets, `/ice` returns STUN only and most — but not all — players
can still connect.

## Wire protocol

See `src/protocol.js` for the full message list. The `start` message carries an
authoritative `seed` identical for every peer — it feeds each engine's
`InitRandom`, keeping the deterministic-lockstep sims in step.

## Status

Scaffold. **Server verified** by an automated two-client Durable Object test
(11/11: join, host assignment, roster, targeted signal relay, chat broadcast,
host-only start with an identical seed to all peers, host-promotion + roster
shrink on leave). The browser P2P DataChannel (`client/demo.html`,
perfect-negotiation) is written but **not yet driven in a real browser** — that's
the two-tab demo to run next. **Not yet wired to the engine** —
that waits on the Phase-0 determinism gate (see `../PLAN.md`). The engine side
will reimplement the `UDP` class over a DataChannel (`datachannel-wasm`); the
`start` seed and command-packet bytes are the contract between the two halves.
