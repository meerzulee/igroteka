#!/usr/bin/env python3
"""Dev server for the OPFS build of RTW-in-browser.

Serves the repo with the cross-origin-isolation headers (COOP/COEP) that
SharedArrayBuffer requires — the OPFS fs-worker bridge (Approach B) uses SAB +
Atomics to make async OPFS reads synchronous for the interpreter worker.

Run from the repo root:  python3 fortochka/runtime/web/serve.py [port]
Then open  http://127.0.0.1:8091/fortochka/build-web/import.html  (import once)
then       http://127.0.0.1:8091/fortochka/build-web/rtw.html
"""
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        # Cross-origin isolation → SharedArrayBuffer is available.
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        # No caching, so a rebuilt wasm/js is always picked up on reload.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8091
    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"serving (COOP/COEP, cross-origin isolated) on http://127.0.0.1:{port}")
    httpd.serve_forever()
