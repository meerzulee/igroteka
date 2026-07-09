// ICE server list for RTCPeerConnection.
//
// STUN is free and always present — it only helps peers discover their public
// address for NAT hole-punching; no bytes relay through it. Cloudflare answers
// STUN binding requests at turn.cloudflare.com; Google's is a fallback.
//
// TURN is the relay used when hole-punching fails (~10-20% of peer pairs behind
// symmetric NAT). It DOES carry game bytes, so it costs money ($0.05/GB on
// Cloudflare Realtime TURN). It's optional: without creds we return STUN only,
// and those ~15% of players just can't connect until TURN is configured.
//
// To enable TURN, set two secrets on this Worker:
//   wrangler secret put TURN_KEY_ID
//   wrangler secret put TURN_KEY_API_TOKEN
// (Create the key in the Cloudflare dashboard: Realtime -> TURN.)

const STUN_ONLY = [
  { urls: "stun:stun.cloudflare.com:3478" },
  { urls: "stun:stun.l.google.com:19302" },
];

export async function getIceServers(env) {
  if (!env.TURN_KEY_ID || !env.TURN_KEY_API_TOKEN) return STUN_ONLY;

  try {
    // NOTE: confirm this path against current Cloudflare Realtime TURN docs
    // before shipping — the credential-generation endpoint has changed shape
    // across versions. STUN_ONLY fallback keeps the lobby working regardless.
    const res = await fetch(
      `https://rtc.live.cloudflare.com/v1/turn/keys/${env.TURN_KEY_ID}/credentials/generate`,
      {
        method: "POST",
        headers: {
          authorization: `Bearer ${env.TURN_KEY_API_TOKEN}`,
          "content-type": "application/json",
        },
        body: JSON.stringify({ ttl: 86400 }),
      }
    );
    if (!res.ok) return STUN_ONLY;

    const data = await res.json();
    const ice = data.iceServers; // may be a single object or an array
    const turn = Array.isArray(ice) ? ice : ice ? [ice] : [];
    // Always keep a STUN entry alongside TURN so direct P2P is still attempted.
    return [STUN_ONLY[0], ...turn];
  } catch {
    return STUN_ONLY;
  }
}
