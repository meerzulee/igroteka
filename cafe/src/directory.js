// Directory — a single Durable Object listing the PUBLIC (no-password) lobbies.
//
// Private (password) parties are never listed; only open rooms register here so
// players can browse and join without a code. Rooms upsert their live player
// count and remove themselves when empty; list() also prunes anything stale as
// a backstop against a room that died without deregistering.

import { DurableObject } from "cloudflare:workers";

const STALE_MS = 2 * 60 * 60 * 1000; // 2h — matches the room reap window
const MAX_LISTED = 100;

export class Directory extends DurableObject {
  async upsert(code, meta) {
    const rooms = (await this.ctx.storage.get("rooms")) || {};
    rooms[code] = { code, name: meta.name, players: meta.players ?? 0,
                    max: meta.max ?? 8, ts: Date.now() };
    await this.ctx.storage.put("rooms", rooms);
  }

  async remove(code) {
    const rooms = (await this.ctx.storage.get("rooms")) || {};
    if (rooms[code]) { delete rooms[code]; await this.ctx.storage.put("rooms", rooms); }
  }

  async list() {
    const rooms = (await this.ctx.storage.get("rooms")) || {};
    const now = Date.now();
    let changed = false;
    for (const code of Object.keys(rooms)) {
      if (now - rooms[code].ts > STALE_MS) { delete rooms[code]; changed = true; }
    }
    if (changed) await this.ctx.storage.put("rooms", rooms);
    return Object.values(rooms)
      .sort((a, b) => b.ts - a.ts)
      .slice(0, MAX_LISTED)
      .map(({ code, name, players, max }) => ({ code, name, players, max }));
  }
}
