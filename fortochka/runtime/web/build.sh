#!/usr/bin/env bash
# Build the zhweb WASM (RTW-in-browser) and patch the emscripten glue.
# Run from anywhere:  fortochka/runtime/web/build.sh
# Outputs the gitignored artifacts build-web/zhweb.{js,wasm} + copies the shell.
set -euo pipefail
cd "$(dirname "$0")/../.."   # -> fortochka/

SRC=(
  zhelezo/src/exec.cpp zhelezo/src/decode.cpp peload/src/peload.cpp
  runtime/src/machine.cpp k32web/src/k32web.cpp u32web/src/u32web.cpp
  d9web/src/d9web.cpp sysweb/src/sysweb.cpp runtime/web/zhweb.cpp
)

mkdir -p build-web
emcc -std=gnu++20 -O2 -fwasm-exceptions \
  -Izhelezo/include -Ipeload/include -Iruntime/include -Ik32web/include \
  -Iu32web/include -Id9web/include -Isysweb/include \
  "${SRC[@]}" \
  -sEXPORTED_FUNCTIONS='["_zhweb_boot","_zhweb_slice","_zhweb_run","_zhweb_fb_ptr","_zhweb_fb_width","_zhweb_fb_height","_zhweb_icount","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU32","setValue","getValue"]' \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=3GB -sSTACK_SIZE=8MB \
  -sENVIRONMENT=worker,web \
  -o build-web/zhweb.js

# ALLOW_MEMORY_GROWTH makes WASM memory a *resizable* ArrayBuffer; current Chrome
# rejects TextDecoder.decode() on a VIEW of one. The guest CRT's printf output goes
# through emscripten's UTF8ArrayToString -> UTF8Decoder.decode(HEAPU8.subarray(..)),
# which throws ("The provided ArrayBuffer value must not be resizable"). .slice()
# returns a fresh, non-resizable copy — decode is happy. Fixed-memory avoids this
# but OOMs at ui_0.pak, so we keep growth and patch the one decode call.
perl -i -pe 's/UTF8Decoder\.decode\(heapOrArray\.subarray\(idx,endPtr\)\)/UTF8Decoder.decode(heapOrArray.slice(idx,endPtr))/g' build-web/zhweb.js
if grep -q "UTF8Decoder.decode(heapOrArray.slice(idx,endPtr))" build-web/zhweb.js; then
  echo "patched UTF8 decoder (subarray -> slice)"
else
  echo "WARN: UTF8 decoder patch did not apply — emscripten glue pattern changed;"
  echo "      re-grep 'UTF8Decoder.decode(' in build-web/zhweb.js and update the perl regex."
fi

# The browser shell (page + worker) is version-controlled here in runtime/web/;
# copy it next to the artifacts so the server can serve one directory.
cp runtime/web/rtw.html runtime/web/worker.js build-web/

echo "built build-web/zhweb.{js,wasm}  ($(stat -f%z build-web/zhweb.wasm) bytes)"
