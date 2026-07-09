// party.js — the password gate for private Igroteka multiplayer.
//
// Flow (Model B: gate here, gather in the in-game lobby):
//   Create  -> POST /party {password}          -> {roomCode, token, role:"host"}
//   Join    -> POST /room/<code>/auth {password}-> {roomCode, token, role:"guest"}
// On success the signed token is stashed in localStorage keyed by room (same
// origin as the game), then we launch /play/zh/?room=<code>. The game reads the
// token from localStorage and presents it to the cafe WS. No token => the DO
// rejects the socket, so a leaked game link can't join without the password.
//
// The token is the only secret that crosses to the game. We never put the
// password in a URL or in localStorage.

(function () {
  "use strict";

  var params = new URLSearchParams(location.search);
  // cafe base is overridable for local testing (?cafe=http://localhost:8799).
  var CAFE = params.get("cafe") || "https://cafe-nw.mrz.sh";
  var GAME = "/play/zh/"; // same-origin game boot

  var $ = function (id) { return document.getElementById(id); };
  var mode = "create"; // or "join"

  var el = {
    forms: $("forms"), form: $("form"), name: $("name"), code: $("code"),
    codeRow: $("codeRow"), pass: $("pass"), go: $("go"), err: $("err"),
    tabCreate: $("tabCreate"), tabJoin: $("tabJoin"),
    createdPanel: $("createdPanel"), createdCode: $("createdCode"),
    copyCode: $("copyCode"), shareLink: $("shareLink"), enter: $("enter"),
    startOver: $("startOver"),
  };

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
          { label: "Run Setup", default: true, onclick: function () { location.href = "/?run=setup"; } },
          { label: "Close", onclick: function () { location.href = "/"; } },
        ],
        onClose: function () { location.href = "/"; },
      }
    );
    return; // don't wire the create/join forms
  }

  // Remember the player's name across visits.
  el.name.value = localStorage.getItem("cafe-name") || "";

  function setMode(m) {
    mode = m;
    var join = m === "join";
    el.tabCreate.classList.toggle("on", !join);
    el.tabJoin.classList.toggle("on", join);
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
    localStorage.setItem("cafe-name", el.name.value.trim() || "General");
  }

  function launch(code) {
    var name = encodeURIComponent(el.name.value.trim() || "General");
    var url = GAME + "?room=" + encodeURIComponent(code) + "&player=" + name;
    // Carry a cafe override through to the game so both talk to the same server
    // (default is cafe-nw.mrz.sh for both; the override only matters for local
    // testing and staging).
    var cafeOverride = params.get("cafe");
    if (cafeOverride) url += "&cafe=" + encodeURIComponent(cafeOverride);
    location.href = url;
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
        showCreated(r.data.roomCode);
      } else {
        var code = el.code.value.trim();
        if (!code) return fail("Enter the room code your host shared.");
        var a = await post("/room/" + encodeURIComponent(code) + "/auth", { password: pass });
        if (!a.ok) return failDialog(a.data.error || "Could not join — check the code and password.");
        stash(a.data.roomCode, a.data.token, a.data.role);
        launch(a.data.roomCode); // guests launch straight in
      }
    } catch (err) {
      failDialog("Could not reach the party server. Check your connection and try again.");
    }
  };

  // Host: after creating, reveal the code to share, then Enter Battle launches.
  function showCreated(code) {
    el.forms.classList.add("hidden");
    el.createdPanel.classList.remove("hidden");
    el.createdCode.textContent = code;
    var link = location.origin + "/party?code=" + encodeURIComponent(code);
    el.shareLink.innerHTML = "Share link (code only): <a href=\"" + link + "\">" + link + "</a>";
    el.enter.onclick = function () { launch(code); };
    el.copyCode.onclick = function () {
      navigator.clipboard && navigator.clipboard.writeText(code);
      el.copyCode.textContent = "copied ✓";
      setTimeout(function () { el.copyCode.textContent = "copy code"; }, 1500);
    };
  }
  el.startOver.onclick = function (e) {
    e.preventDefault();
    el.createdPanel.classList.add("hidden");
    el.forms.classList.remove("hidden");
    setMode("create");
    el.go.disabled = false;
    el.go.textContent = "Create Party";
  };
})();
