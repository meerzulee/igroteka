// party.js — the password gate + gathering lobby for private Igroteka multiplayer.
//
// Flow:
//   Create -> POST /party {password}           -> {roomCode, token, role:"host"}
//   Join   -> POST /room/<code>/auth {password} -> {roomCode, token, role:"guest"}
// On success the signed token goes to localStorage keyed by room (same origin as
// the game) and the page enters the LOBBY: live roster, chat, ready toggles,
// host moderation (kick). The host's Start Game broadcasts "start" and every
// member launches /play/zh/?room=<code> together; the game page reads the token
// from localStorage and presents it to the cafe WS. No token => the DO rejects
// the socket, so a leaked game link can't join without the password.
//
// The token is the only secret that crosses to the game. We never put the
// password in a URL or in localStorage.
//
// ?embed=1: running inside a desktop program window (iframe) — the host window
// supplies the chrome and navigation happens on the top frame.

(function () {
  "use strict";

  var params = new URLSearchParams(location.search);
  // cafe base: ?cafe= override wins; a localhost-served site defaults to the
  // local wrangler dev cafe (:8799) so dev needs no URL param; production
  // talks to cafe-nw.mrz.sh.
  var LOCAL = location.hostname === "localhost" || location.hostname === "127.0.0.1";
  var CAFE = params.get("cafe") || (LOCAL ? "http://localhost:8799" : "https://cafe-nw.mrz.sh");
  var GAME = "/play/zh/"; // same-origin game boot
  var EMBED = params.get("embed") === "1";
  if (EMBED) document.body.classList.add("embed");

  var $ = function (id) { return document.getElementById(id); };
  var mode = "create"; // or "join"

  var el = {
    forms: $("forms"), form: $("form"), name: $("name"), code: $("code"),
    codeRow: $("codeRow"), pass: $("pass"), go: $("go"), err: $("err"),
    tabCreate: $("tabCreate"), tabJoin: $("tabJoin"),
    lobbyPanel: $("lobbyPanel"), createdCode: $("createdCode"),
    copyCode: $("copyCode"), shareLink: $("shareLink"),
    roster: $("roster"), chatlog: $("chatlog"), chatMsg: $("chatMsg"),
    chatSend: $("chatSend"), readyBox: $("readyBox"),
    startGame: $("startGame"), waitMsg: $("waitMsg"), leaveLobby: $("leaveLobby"),
  };

  // Navigations leave the app: from an embedded program window they must move
  // the desktop (top frame), not the iframe.
  function nav(url) {
    (EMBED ? window.top : window).location.href = url;
  }

  // Multiplayer requires the game installed on this device — you can't join a
  // match without the files. The Igroteka desktop only surfaces this page once
  // installed, but a shared /party link could land an uninstalled visitor here,
  // so gate again with a native XP warning box. (The game launch also re-checks
  // OPFS and bounces to Setup.)
  if (!localStorage.getItem("zh-installed")) {
    document.querySelector(".window").style.display = "none"; // hide the party form
    XpDialog.warn(
      "You need <b>C&amp;C Generals: Zero Hour</b> installed on this device " +
      "before you can join a multiplayer party.<br><br>" +
      "Run <b>Setup — C&amp;C Generals: Zero Hour</b> to install it, then come " +
      "back. Your game files stay on your device — Igroteka hosts nothing.",
      {
        title: "Multiplayer Party",
        buttons: [
          { label: "Run Setup", default: true, onclick: function () { nav("/?run=setup"); } },
          { label: "Close", onclick: function () { nav("/"); } },
        ],
        onClose: function () { nav("/"); },
      }
    );
    return; // don't wire the create/join forms
  }

  // Remember the player's name across visits — restored here, saved as they
  // type (not only on submit), so it survives even an abandoned form.
  el.name.value = localStorage.getItem("cafe-name") || "";
  el.name.addEventListener("input", function () {
    var v = el.name.value.trim();
    if (v) localStorage.setItem("cafe-name", v);
  });

  function setMode(m) {
    mode = m;
    var join = m === "join";
    // xp.css tabs style off aria-selected
    el.tabCreate.setAttribute("aria-selected", String(!join));
    el.tabJoin.setAttribute("aria-selected", String(join));
    el.codeRow.classList.toggle("hidden", !join);
    el.go.textContent = join ? "Join Party" : "Create Party";
    el.err.textContent = "";
  }
  el.tabCreate.onclick = function () { setMode("create"); };
  el.tabJoin.onclick = function () { setMode("join"); };

  // A shared link (/party?code=ABC) prefills the code and flips to Join — but
  // never carries the password, so the link alone grants nothing. Default to
  // Create (which hides the room-code field).
  var preCode = params.get("code");
  if (preCode) { el.code.value = preCode.trim(); setMode("join"); }
  else setMode("create");

  // Inline red text for local field validation; native XP error box for
  // server-reported failures (wrong password, rate limit, network down).
  function fail(msg) {
    el.err.textContent = msg;
    el.go.disabled = false;
    el.go.textContent = mode === "join" ? "Join Party" : "Create Party";
  }
  function failDialog(msg) {
    fail("");
    XpDialog.error(msg, { title: "Multiplayer Party", id: "party-err" });
  }

  async function post(path, body) {
    var res = await fetch(CAFE + path, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
    var data = await res.json().catch(function () { return {}; });
    return { ok: res.ok, status: res.status, data: data };
  }

  // Persist the token + role for a room, keyed by room (same origin as the game).
  function stash(code, token, role) {
    localStorage.setItem("cafe-token:" + code, token);
    localStorage.setItem("cafe-role:" + code, role);
    localStorage.setItem("cafe-name", myName());
  }

  function myName() {
    return el.name.value.trim() || localStorage.getItem("cafe-name") || "General";
  }

  function launch(code) {
    var url = GAME + "?room=" + encodeURIComponent(code) +
      "&player=" + encodeURIComponent(myName());
    // Carry a cafe override through to the game so both talk to the same server
    // (default is cafe-nw.mrz.sh for both; the override only matters for local
    // testing and staging).
    var cafeOverride = params.get("cafe");
    if (cafeOverride) url += "&cafe=" + encodeURIComponent(cafeOverride);
    nav(url);
  }

  el.form.onsubmit = async function (e) {
    e.preventDefault();
    var name = el.name.value.trim();
    var pass = el.pass.value;
    if (!name) return fail("Enter a name.");
    if (!pass || pass.length < 4) return fail("Password must be at least 4 characters.");

    el.go.disabled = true;
    el.go.textContent = "Working…";

    try {
      if (mode === "create") {
        var r = await post("/party", { password: pass });
        if (!r.ok) return failDialog(r.data.error || "Could not create the party.");
        stash(r.data.roomCode, r.data.token, r.data.role);
        enterLobby(r.data.roomCode, r.data.role, r.data.token);
      } else {
        var code = el.code.value.trim();
        if (!code) return fail("Enter the room code your host shared.");
        var a = await post("/room/" + encodeURIComponent(code) + "/auth", { password: pass });
        if (!a.ok) return failDialog(a.data.error || "Could not join — check the code and password.");
        stash(a.data.roomCode, a.data.token, a.data.role);
        enterLobby(a.data.roomCode, a.data.role, a.data.token);
      }
    } catch (err) {
      failDialog("Could not reach the party server. Check your connection and try again.");
    }
  };

  // ---- lobby ----------------------------------------------------------------
  // Same room WS the game itself uses; here it only carries roster/chat/kick/
  // start. It closes when we navigate into the game, which reopens it there.
  var ws = null, myId = null, iAmHost = false, launching = false, kicked = false;

  function sysLine(text) {
    var li = document.createElement("li");
    li.className = "sys";
    li.textContent = text;
    el.chatlog.appendChild(li);
    el.chatlog.scrollTop = el.chatlog.scrollHeight;
  }

  function chatLine(name, text) {
    var li = document.createElement("li");
    var b = document.createElement("b");
    b.textContent = name + ": ";       // textContent — never trust player input
    li.appendChild(b);
    li.appendChild(document.createTextNode(text));
    el.chatlog.appendChild(li);
    el.chatlog.scrollTop = el.chatlog.scrollHeight;
  }

  function renderRoster(players) {
    el.roster.textContent = "";
    players.forEach(function (p) {
      var li = document.createElement("li");
      var who = document.createElement("span");
      who.className = "who";
      who.textContent = p.name + (p.id === myId ? " (you)" : "");
      li.appendChild(who);
      if (p.host) {
        var tag = document.createElement("span");
        tag.className = "tag";
        tag.textContent = "host";
        li.appendChild(tag);
      }
      if (p.ready) {
        var ok = document.createElement("span");
        ok.className = "ok";
        ok.textContent = "✓ ready";
        li.appendChild(ok);
      }
      if (iAmHost && p.id !== myId) {
        var kickBtn = document.createElement("button");
        kickBtn.type = "button";
        kickBtn.textContent = "Kick";
        kickBtn.onclick = function () {
          var d = XpDialog.warn(
            "Remove <b></b> from the party?<br>They will need the password to come back.",
            {
              title: "Kick player", id: "kick",
              buttons: [
                { label: "Kick", default: true, onclick: function () { send({ t: "kick", id: p.id }); } },
                { label: "Cancel" },
              ],
            });
          // player name into the <b> via textContent — never trust player input
          d.el.querySelector(".xpdlg-msg b").textContent = p.name;
        };
        li.appendChild(kickBtn);
      }
      el.roster.appendChild(li);
    });
  }

  function send(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
  }

  function enterLobby(code, role, token) {
    el.forms.classList.add("hidden");
    el.lobbyPanel.classList.remove("hidden");
    el.createdCode.value = code;
    iAmHost = role === "host";
    el.startGame.classList.toggle("hidden", !iAmHost);
    el.waitMsg.classList.toggle("hidden", iAmHost);

    if (iAmHost) {
      var link = location.origin + "/party?code=" + encodeURIComponent(code);
      el.shareLink.textContent = "Share link (code only): ";
      var a = document.createElement("a");
      a.href = link;
      a.textContent = link;
      el.shareLink.appendChild(a);
    } else {
      el.shareLink.textContent = "";
    }

    var wsUrl = CAFE.replace(/^http/, "ws") + "/room/" + encodeURIComponent(code) +
      "/ws?name=" + encodeURIComponent(myName()) + "&token=" + encodeURIComponent(token);
    var welcomed = false;
    ws = new WebSocket(wsUrl);
    ws.onmessage = function (e) {
      var m;
      try { m = JSON.parse(e.data); } catch (err) { return; }
      switch (m.t) {
        case "welcome":
          welcomed = true;
          myId = m.youAre;
          sysLine("connected — room " + code);
          break;
        case "roster":
          renderRoster(m.players);
          break;
        case "chat":
          chatLine(m.name, m.text);
          break;
        case "kicked":
          kicked = true;
          break;
        case "start":
          launching = true;
          sysLine("host started the game — launching…");
          launch(code);
          break;
      }
    };
    ws.onclose = function () {
      if (launching) return;
      leaveToForms();
      if (kicked) {
        kicked = false;
        XpDialog.error("You were removed from the party by the host.",
          { title: "Multiplayer Party", id: "party-err" });
      } else if (!welcomed) {
        // Gate rejected us: stale/expired token. Password again.
        localStorage.removeItem("cafe-token:" + code);
        localStorage.removeItem("cafe-role:" + code);
        XpDialog.error("Your party session has expired. Enter the password to rejoin.",
          { title: "Multiplayer Party", id: "party-err" });
        el.code.value = code;
        setMode("join");
      } else {
        XpDialog.warn("Connection to the party lobby was lost.",
          { title: "Multiplayer Party", id: "party-err" });
      }
    };
  }

  function leaveToForms() {
    if (ws) { try { ws.onclose = null; ws.close(); } catch (e) {} ws = null; }
    el.lobbyPanel.classList.add("hidden");
    el.forms.classList.remove("hidden");
    el.roster.textContent = "";
    el.chatlog.textContent = "";
    el.readyBox.checked = false;
    fail("");
  }

  el.chatSend.onclick = function () {
    var text = el.chatMsg.value.trim();
    if (!text) return;
    send({ t: "chat", text: text });
    el.chatMsg.value = "";
  };
  el.chatMsg.addEventListener("keydown", function (e) {
    if (e.key === "Enter") { e.preventDefault(); el.chatSend.onclick(); }
  });
  el.readyBox.onchange = function () { send({ t: "ready", ready: el.readyBox.checked }); };
  el.startGame.onclick = function () { send({ t: "start" }); };
  el.leaveLobby.onclick = function (e) { e.preventDefault(); leaveToForms(); };

  el.copyCode.onclick = function () {
    navigator.clipboard && navigator.clipboard.writeText(el.createdCode.value);
    el.copyCode.textContent = "Copied ✓";
    setTimeout(function () { el.copyCode.textContent = "Copy"; }, 1500);
  };

  // Reconnect nicety: arriving with ?code= while holding a still-valid token for
  // that room (e.g. the host refreshed) goes straight back into the lobby; if
  // the token has gone stale the WS gate rejects it and we fall back to Join.
  if (preCode) {
    var storedTok = localStorage.getItem("cafe-token:" + preCode);
    var storedRole = localStorage.getItem("cafe-role:" + preCode);
    if (storedTok && storedRole) enterLobby(preCode.trim(), storedRole, storedTok);
  }
})();
