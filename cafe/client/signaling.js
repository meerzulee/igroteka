// cafe browser client — connects to the signaling server, negotiates a WebRTC
// DataChannel to every other peer in the room, and surfaces lobby + data events.
//
// Full mesh (a channel to each peer) because deterministic lockstep needs every
// player's command packets at every other player. For 1v1 that's one channel.
//
// Events (CustomEvent, listen with addEventListener):
//   "roster"        detail: [{id,name,host,ready}]
//   "chat"          detail: {from,name,text}
//   "start"         detail: {seed,map,slots}
//   "channel-open"  detail: {peerId, channel}
//   "data"          detail: {peerId, data}   (ArrayBuffer — game bytes)
//   "peer-connected"/"peer-lost"/"channel-close"/"disconnected"

export class CafeClient extends EventTarget {
  constructor() {
    super();
    this.peers = new Map(); // peerId -> PeerLink
  }

  // base: e.g. "https://cafe.igroteka.mrz.sh" or "http://localhost:8787"
  connect(base, roomId, name) {
    const wsUrl =
      base.replace(/^http/, "ws") +
      `/room/${encodeURIComponent(roomId)}/ws?name=${encodeURIComponent(name)}`;
    this.ws = new WebSocket(wsUrl);
    this.ws.onmessage = (e) => this._onMsg(JSON.parse(e.data));
    this.ws.onclose = () => this._emit("disconnected");
    return new Promise((resolve, reject) => {
      this._welcome = resolve;
      this.ws.onerror = reject;
    });
  }

  ready(v) { this._send({ t: "ready", ready: v }); }
  chat(text) { this._send({ t: "chat", text }); }
  start(map) { this._send({ t: "start", map }); } // host only (server enforces)

  // Broadcast game bytes to every open channel (what the engine transport uses).
  broadcast(data) { for (const link of this.peers.values()) link.send(data); }

  _send(obj) { this.ws.send(JSON.stringify(obj)); }
  _signal(to, data) { this._send({ t: "signal", to, data }); }
  _emit(name, detail) { this.dispatchEvent(new CustomEvent(name, { detail })); }

  _onMsg(msg) {
    switch (msg.t) {
      case "welcome":
        this.id = msg.youAre;
        this.host = msg.host;
        this.iceServers = msg.iceServers;
        this._welcome?.(msg);
        break;
      case "roster":
        this._emit("roster", msg.players);
        this._reconcile(msg.players);
        break;
      case "signal":
        this._peer(msg.from)._onSignal(msg.data);
        break;
      case "chat":
        this._emit("chat", msg);
        break;
      case "start":
        this._emit("start", msg);
        break;
    }
  }

  _reconcile(players) {
    const others = new Set(players.map((p) => p.id).filter((id) => id !== this.id));
    for (const id of others) if (!this.peers.has(id)) this._peer(id);
    for (const id of [...this.peers.keys()])
      if (!others.has(id)) {
        this.peers.get(id).close();
        this.peers.delete(id);
      }
  }

  _peer(peerId) {
    let link = this.peers.get(peerId);
    if (!link) {
      link = new PeerLink(this, peerId);
      this.peers.set(peerId, link);
    }
    return link;
  }
}

// One RTCPeerConnection to one peer, driven by the MDN "perfect negotiation"
// pattern so simultaneous offers (glare) resolve deterministically.
class PeerLink {
  constructor(client, peerId) {
    this.client = client;
    this.peerId = peerId;
    // Exactly one side is polite; symmetric tie-break on id.
    this.polite = client.id < peerId;
    // Higher id creates the single shared DataChannel (avoids two channels).
    const initiator = client.id > peerId;
    this.makingOffer = false;
    this.ignoreOffer = false;

    const pc = new RTCPeerConnection({ iceServers: client.iceServers });
    this.pc = pc;

    pc.onicecandidate = ({ candidate }) => {
      if (candidate) client._signal(peerId, { candidate });
    };
    pc.onnegotiationneeded = async () => {
      try {
        this.makingOffer = true;
        await pc.setLocalDescription();
        client._signal(peerId, { description: pc.localDescription });
      } catch (e) {
        console.error("[cafe] negotiation", e);
      } finally {
        this.makingOffer = false;
      }
    };
    pc.onconnectionstatechange = () => {
      const s = pc.connectionState;
      if (s === "connected") client._emit("peer-connected", { peerId });
      if (s === "failed" || s === "disconnected" || s === "closed")
        client._emit("peer-lost", { peerId, state: s });
    };
    pc.ondatachannel = ({ channel }) => this._bind(channel);

    if (initiator) {
      // ordered:false + maxRetransmits:0 == unreliable/unordered, i.e. UDP —
      // exactly what the engine's transport layer expects. Reliability (ACK/
      // resend) is the engine's job, not the channel's.
      this._bind(pc.createDataChannel("game", { ordered: false, maxRetransmits: 0 }));
    }
  }

  _bind(ch) {
    this.channel = ch;
    ch.binaryType = "arraybuffer";
    ch.onopen = () => this.client._emit("channel-open", { peerId: this.peerId, channel: ch });
    ch.onmessage = (e) => this.client._emit("data", { peerId: this.peerId, data: e.data });
    ch.onclose = () => this.client._emit("channel-close", { peerId: this.peerId });
  }

  async _onSignal({ description, candidate }) {
    const pc = this.pc;
    try {
      if (description) {
        const collision =
          description.type === "offer" &&
          (this.makingOffer || pc.signalingState !== "stable");
        this.ignoreOffer = !this.polite && collision;
        if (this.ignoreOffer) return;

        await pc.setRemoteDescription(description);
        if (description.type === "offer") {
          await pc.setLocalDescription();
          this.client._signal(this.peerId, { description: pc.localDescription });
        }
      } else if (candidate) {
        try {
          await pc.addIceCandidate(candidate);
        } catch (e) {
          if (!this.ignoreOffer) throw e;
        }
      }
    } catch (e) {
      console.error("[cafe] signaling", e);
    }
  }

  send(data) {
    if (this.channel && this.channel.readyState === "open") this.channel.send(data);
  }
  close() {
    try { this.channel?.close(); } catch {}
    try { this.pc.close(); } catch {}
  }
}
