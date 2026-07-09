// cafe wire protocol — JSON messages over the lobby WebSocket.
// Shared reference for server (room.js) and client (signaling.js). Not imported
// anywhere; it exists so the message shapes live in one documented place.
//
// The DataChannel that this handshake opens carries the ENGINE'S bytes (lockstep
// command packets), which are opaque to cafe. cafe only speaks the lobby JSON below.
//
// ── client → server ──────────────────────────────────────────────────────────
//  join is implicit in the WS URL: /room/<id>/ws?name=<name>
//  { t:"signal", to:<peerId>, data:<opaque> }   relay SDP/ICE to one peer
//  { t:"ready",  ready:<bool> }                 toggle ready in the lobby
//  { t:"chat",   text:<string> }                lobby chat (≤500 chars)
//  { t:"start",  map:<string?> }                host-only: launch the match
//
// ── server → client ──────────────────────────────────────────────────────────
//  { t:"welcome", youAre:<peerId>, host:<bool>, iceServers:[...] }
//  { t:"roster",  players:[{id,name,host,ready}] }   sent on every change
//  { t:"signal",  from:<peerId>, data:<opaque> }     relayed handshake
//  { t:"chat",    from:<peerId>, name:<string>, text:<string> }
//  { t:"start",   seed:<uint32>, map:<string?>, slots:[{id,name,host,ready}] }
//
// The `seed` in "start" is authoritative and identical for all peers — it feeds
// each engine's InitRandom so the deterministic-lockstep simulations stay in sync.

export const MSG = {
  SIGNAL: "signal",
  READY: "ready",
  CHAT: "chat",
  START: "start",
  WELCOME: "welcome",
  ROSTER: "roster",
};
