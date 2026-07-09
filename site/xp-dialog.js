// xp-dialog.js — reusable XP-style modal message boxes for Igroteka pages.
//
// One shared implementation of the classic Windows XP dialog (title bar, white
// body with a status icon, centered button row) built on xp.css window chrome.
// Replaces the hand-rolled copies that lived in the play page (#crash), the
// desktop (files-missing window) and the party page (install gate).
//
//   XpDialog.error(html, opts)   red circle-X       "has encountered a problem"
//   XpDialog.warn(html, opts)    yellow ! triangle  warnings / prerequisites
//   XpDialog.info(html, opts)    blue i circle      notices
//   XpDialog.show(opts)          full control
//
// opts:
//   title    title-bar text (default "Igroteka")
//   icon     'error' | 'warning' | 'info' | null
//   html     body HTML (trusted app strings only — never user input)
//   width    px (default 420)
//   id       same-id show() replaces the open dialog (crash handlers may fire
//            repeatedly; they shouldn't stack boxes)
//   buttons  [{label, onclick, default}] — default [{label:'OK'}]. A button
//            closes the dialog after its onclick unless onclick returns false.
//   onClose  called when the title-bar X / Escape dismisses the dialog
//
// show() returns { close, el }. Dialogs are draggable by the title bar like the
// desktop windows; Enter presses the default button, Escape acts as the X.
(function () {
  "use strict";

  var ICONS = {
    // Red circle-X — same art as the game crash dialog.
    error:
      '<svg width="32" height="32" viewBox="0 0 32 32" style="flex:0 0 32px" aria-hidden="true">' +
      '<circle cx="16" cy="16" r="15" fill="#d83a2e"/>' +
      '<path d="M10 10 L22 22 M22 10 L10 22" stroke="#fff" stroke-width="4" stroke-linecap="round"/></svg>',
    // Yellow ! triangle — same art as the desktop storage warning.
    warning:
      '<svg width="32" height="32" viewBox="0 0 32 32" style="flex:0 0 32px" aria-hidden="true">' +
      '<path d="M16 3 L30 28 H2 Z" fill="#f8d21c" stroke="#8a7500" stroke-width="1.5" stroke-linejoin="round"/>' +
      '<rect x="14.7" y="11" width="2.6" height="9" rx="1" fill="#000"/>' +
      '<circle cx="16" cy="24" r="1.7" fill="#000"/></svg>',
    // Blue i circle.
    info:
      '<svg width="32" height="32" viewBox="0 0 32 32" style="flex:0 0 32px" aria-hidden="true">' +
      '<circle cx="16" cy="16" r="15" fill="#2b63cf"/>' +
      '<circle cx="16" cy="9.5" r="2.1" fill="#fff"/>' +
      '<rect x="14.4" y="13.5" width="3.2" height="11" rx="1.4" fill="#fff"/></svg>',
  };

  var STYLE =
    '.xpdlg-overlay{position:fixed;inset:0;z-index:9999;display:flex;' +
    'align-items:center;justify-content:center;background:rgba(0,0,0,.45)}' +
    '.xpdlg-overlay .window{box-shadow:4px 4px 16px rgba(0,0,0,.55)}' +
    '.xpdlg-body{display:flex;gap:14px;padding:16px 14px;align-items:flex-start;' +
    'background:#fff;font-size:12px;line-height:1.5;color:#000;letter-spacing:.5px}' +
    '.xpdlg-msg{min-width:0}' +
    '.xpdlg-btns{display:flex;justify-content:center;gap:8px;padding:10px}';

  function ensureStyle() {
    if (document.getElementById("xpdlg-style")) return;
    var s = document.createElement("style");
    s.id = "xpdlg-style";
    s.textContent = STYLE;
    document.head.appendChild(s);
  }

  var openById = {}; // id -> close()

  function show(opts) {
    opts = opts || {};
    ensureStyle();
    if (opts.id && openById[opts.id]) openById[opts.id]();

    var overlay = document.createElement("div");
    overlay.className = "xpdlg-overlay";
    overlay.innerHTML =
      '<div class="window" style="width:' + (opts.width || 420) + 'px;max-width:92vw">' +
      '<div class="title-bar">' +
      '<div class="title-bar-text"></div>' +
      '<div class="title-bar-controls"><button aria-label="Close"></button></div>' +
      "</div>" +
      '<div class="xpdlg-body">' +
      (ICONS[opts.icon] || "") +
      '<div class="xpdlg-msg"></div>' +
      "</div>" +
      '<div class="xpdlg-btns"></div>' +
      "</div>";
    overlay.querySelector(".title-bar-text").textContent = opts.title || "Igroteka";
    overlay.querySelector(".xpdlg-msg").innerHTML = opts.html || "";

    function close() {
      overlay.remove();
      document.removeEventListener("keydown", onKey, true);
      if (opts.id && openById[opts.id] === close) delete openById[opts.id];
    }
    if (opts.id) openById[opts.id] = close;

    var dismiss = function () {
      close();
      if (opts.onClose) opts.onClose();
    };
    overlay.querySelector('[aria-label="Close"]').onclick = dismiss;

    var btns = overlay.querySelector(".xpdlg-btns");
    var defaultBtn = null;
    (opts.buttons && opts.buttons.length ? opts.buttons : [{ label: "OK", default: true }])
      .forEach(function (b) {
        var el = document.createElement("button");
        el.textContent = b.label;
        el.onclick = function () {
          var keep = b.onclick && b.onclick() === false;
          if (!keep) close();
        };
        if (b.default) defaultBtn = el;
        btns.appendChild(el);
      });

    function onKey(e) {
      if (!document.body.contains(overlay)) return;
      if (e.key === "Escape") { e.preventDefault(); dismiss(); }
      if (e.key === "Enter" && defaultBtn) { e.preventDefault(); defaultBtn.click(); }
    }
    document.addEventListener("keydown", onKey, true);

    // Draggable by the title bar, like the desktop windows.
    var win = overlay.querySelector(".window");
    var bar = overlay.querySelector(".title-bar");
    bar.addEventListener("mousedown", function (e) {
      if (e.target.tagName === "BUTTON") return;
      var r = win.getBoundingClientRect();
      win.style.position = "fixed";
      win.style.left = r.left + "px";
      win.style.top = r.top + "px";
      win.style.margin = "0";
      var sx = e.clientX - r.left, sy = e.clientY - r.top;
      var move = function (e) {
        win.style.left = (e.clientX - sx) + "px";
        win.style.top = Math.max(0, e.clientY - sy) + "px";
      };
      var up = function () {
        removeEventListener("mousemove", move);
        removeEventListener("mouseup", up);
      };
      addEventListener("mousemove", move);
      addEventListener("mouseup", up);
    });

    document.body.appendChild(overlay);
    if (defaultBtn) defaultBtn.focus();
    return { close: close, el: overlay };
  }

  function preset(icon) {
    return function (html, o) {
      o = o || {};
      o.icon = icon;
      o.html = html;
      return show(o);
    };
  }

  window.XpDialog = {
    show: show,
    error: preset("error"),
    warn: preset("warning"),
    info: preset("info"),
  };
})();
