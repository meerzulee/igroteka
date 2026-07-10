#!/usr/bin/env python3
"""Local dev server for the igroteka site.

Serves ./site with the cross-origin-isolation headers (COOP/COEP) that the Rome:
Total War play page needs — its OPFS asset bridge (Fortochka fs-worker) uses
SharedArrayBuffer + Atomics, which browsers gate behind those headers. `wrangler
dev` also works (it reads site/_headers) and additionally runs the cafe worker for
/api; this is the lighter, no-build option when you just want the static site.

  python3 dev-serve.py [port]        # default 8788

Then open, in Chrome/Edge or Safari:
  http://127.0.0.1:8788/                          desktop
  http://127.0.0.1:8788/play/rtw/import.html      install (pick your RomeTW folder)
  http://127.0.0.1:8788/play/rtw/?debug=1         play WITH the live log panel
"""
import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "site")


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def end_headers(self):
        # Cross-origin isolation -> SharedArrayBuffer is available.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")  # always see fresh builds
        super().end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8788
    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"igroteka dev server (COOP/COEP, cross-origin isolated) — serving ./site")
    print(f"  desktop : http://127.0.0.1:{port}/")
    print(f"  install : http://127.0.0.1:{port}/play/rtw/import.html")
    print(f"  play+log: http://127.0.0.1:{port}/play/rtw/?debug=1")
    httpd.serve_forever()
