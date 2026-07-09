// Throttle — a single Durable Object that rate-limits party CREATION per IP.
//
// /auth (password guessing) is rate-limited inside the room DO where the
// per-room state naturally lives. /party (party creation) has no room yet — it
// mints a brand-new DO each call — so an unbounded-creation abuser needs a
// shared counter. This is that shared counter: getByName("global"), one fixed
// window per IP. Cheap, best-effort; not a security boundary (the token/verifier
// is), just a guard against a runaway loop spinning up millions of parties.

import { DurableObject } from "cloudflare:workers";

export class Throttle extends DurableObject {
  // Returns true if this hit is allowed, false if the IP is over `limit`
  // within the current `windowMs` window.
  async hit(ip, limit, windowMs) {
    const now = Date.now();
    const key = "ip:" + ip;
    let rec = await this.ctx.storage.get(key);
    if (!rec || rec.reset < now) rec = { n: 0, reset: now + windowMs };
    rec.n++;
    await this.ctx.storage.put(key, rec);
    return rec.n <= limit;
  }
}
