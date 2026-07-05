#!/usr/bin/env bash
# wasthon pygame POC — compile the SDL2 engine glue (_pygame.c) + the wasthon
# bridge to WASM, link into _pygame.mjs/.wasm, copy into loader/ for the demo
# (loader/pygame-demo.html). Same recipe as gui-poc/build_binding.sh, minus
# cimgui/fonts/audio — just SDL2 (emscripten port).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OBJ="$HERE/obj"; mkdir -p "$OBJ"

source "$REPO/external/emsdk/emsdk_env.sh"

# 1. the glue (against wasthon's C-API headers in src/) + SDL2
emcc -c -Os -sUSE_SDL=2 -I "$REPO/src" "$HERE/_pygame.c" -o "$OBJ/_pygame.o"

# 2. the wasthon bridge
emcc -O3 -c -I "$REPO/src" "$REPO/src/wasthon.c" -o "$OBJ/wasthon.o"

# 3. link -> _pygame.mjs/.wasm (multi-phase module, PyInit__pygame)
RTM='"HEAPU8","HEAP32","HEAPF32","HEAPF64","HEAP16","UTF8ToString","stringToUTF8","lengthBytesUTF8"'
emcc -O2 "$OBJ"/*.o \
    --js-library "$REPO/src/wasthon.js" -sUSE_SDL=2 \
    -s ALLOW_MEMORY_GROWTH=1 -s ALLOW_TABLE_GROWTH=1 \
    -s EXPORTED_FUNCTIONS='["_PyInit__pygame","_wasthon_init","_wasthon_module_create","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS="[$RTM]" \
    -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME='wasthon_pygame' \
    -o "$OBJ/_pygame.mjs"

# 4. copy into loader/ for the demo page
cp "$OBJ/_pygame.mjs" "$OBJ/_pygame.wasm" "$REPO/loader/"
echo
echo "Built $OBJ/_pygame.{mjs,wasm}  (+ copied to loader/)."
echo "Demo: (cd loader && python3 -m http.server 8911) -> http://127.0.0.1:8911/pygame-demo.html"
