#!/usr/bin/env bash
# wasthon pygame port — build REAL pygame-ce (not the SDL glue POC) as one
# static WASM module against the wasthon C-API bridge. pygame-ce ships a
# BUILD_STATIC mode (its whole engine merged into base.c) + an Emscripten
# Setup; we reproduce that with emcc, linking the wasthon bridge instead of
# CPython. Produces loader/_pygame.mjs/.wasm.
#
# The remaining gap is bridge C-API functions, filled incrementally (CHANGELOG).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
SRC="$HERE/pygame-ce/src_c"
OBJ="$HERE/obj"; mkdir -p "$OBJ"

if [[ ! -d "$SRC" ]]; then
    echo "ERROR: pygame-ce clone missing at $HERE/pygame-ce (git clone https://github.com/pygame-community/pygame-ce)."
    exit 1
fi

# --- patch the clone (idempotent): the handle model forbids `x->ob_type`;
#     route it through Py_TYPE(x). grep-guarded so re-runs are no-ops. ---
if grep -rql "\->ob_type" "$SRC"/*.c "$SRC"/*.h 2>/dev/null; then
    sed -i 's/(\([A-Za-z_][A-Za-z0-9_]*\))->ob_type/Py_TYPE(\1)/g' \
        "$SRC/font.c" "$SRC/event.c" "$SRC/joystick.c" "$SRC/geometry.h"
    sed -i 's/\([A-Za-z_][A-Za-z0-9_]*\)->ob_type/Py_TYPE(\1)/g' "$SRC/display.c"
    sed -i 's/((PyObject \*)self)->ob_type/Py_TYPE((PyObject *)self)/g' "$SRC/_freetype.c"
    echo "patched ->ob_type -> Py_TYPE()"
fi

source "$REPO/external/emsdk/emsdk_env.sh"

# Shared flags. PY_VERSION_HEX=3.15 empties pythoncapi_compat.h (all its blocks
# are `#if PY_VERSION_HEX < X`). NO_SDL2 drops the cython _sdl2.* renderer
# submodules (not compiled here). PG_* version macros are normally meson-gen'd.
DEFS="-DBUILD_STATIC -DNO_SDL2 -DPY_VERSION_HEX=0x030F00F0 \
      -DPG_MAJOR_VERSION=2 -DPG_MINOR_VERSION=5 -DPG_PATCH_VERSION=8 -DPG_VERSION_TAG=\"\""
# SDL2_IMAGE_FORMATS: the emscripten SDL2_image port compiles NO decoder by
# default — without it image.load says "Unsupported image format".
PORTS="-sUSE_SDL=2 -sUSE_SDL_TTF=2 -sUSE_SDL_IMAGE=2 -sSDL2_IMAGE_FORMATS=png -sUSE_SDL_MIXER=2 -sUSE_FREETYPE=1"
INC="-I $REPO/src -I $SRC/include -I $SRC"

echo "compiling base.c (34 merged .c) ..."
emcc -c -Os $DEFS $PORTS $INC "$SRC/base.c" -o "$OBJ/base.o"
echo "compiling bitmask.c + SDL_gfxPrimitives.c (separate units) ..."
emcc -c -Os $DEFS -sUSE_SDL=2 $INC "$SRC/bitmask.c"                -o "$OBJ/bitmask.o"
emcc -c -Os        -sUSE_SDL=2 -I "$SRC/include" -I "$SRC" "$SRC/SDL_gfx/SDL_gfxPrimitives.c" -o "$OBJ/SDL_gfxPrimitives.o"
echo "compiling shims + wasthon bridge ..."
emcc -c -Os -sUSE_SDL=2 -I "$SRC/include" -I "$SRC" "$HERE/pygame_shims.c" -o "$OBJ/pygame_shims.o"
emcc -O3 -c -I "$REPO/src" "$REPO/src/wasthon.c" -o "$OBJ/wasthon.o"

echo "linking _pygame.mjs ..."
# FS exported + forced: pygame-ce's emscripten rwobject only supports PATHS
# (the Python file-like branch is compiled out upstream) — assets are written
# into MEMFS from JS (M.FS.writeFile) and loaded by path, pygbag-style.
RTM='"HEAPU8","HEAP32","HEAPF32","HEAPF64","HEAP16","UTF8ToString","stringToUTF8","lengthBytesUTF8","FS"'
emcc -O2 "$OBJ"/base.o "$OBJ"/wasthon.o "$OBJ"/bitmask.o "$OBJ"/SDL_gfxPrimitives.o "$OBJ"/pygame_shims.o \
    --js-library "$REPO/src/wasthon.js" $PORTS \
    -sFORCE_FILESYSTEM=1 \
    -sERROR_ON_UNDEFINED_SYMBOLS=0 -sALLOW_MEMORY_GROWTH=1 -sALLOW_TABLE_GROWTH=1 \
    -sEXPORTED_FUNCTIONS='["_PyInit_pygame_static","_PyInit_base","_wasthon_init","_wasthon_module_create","_malloc","_free"]' \
    -sEXPORTED_RUNTIME_METHODS="[$RTM]" \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME='wasthon_pygame' \
    -o "$OBJ/_pygame.mjs"

cp "$OBJ/_pygame.mjs" "$OBJ/_pygame.wasm" "$REPO/loader/"

# Python-side modules the C init imports at PyInit time (PyInit_color needs
# pygame.colordict, PyInit_system needs pygame._data_classes). The probe execs
# them into sys.modules BEFORE calling PyInit_base. Missing colordict made the
# color->int mapping silently yield 0: everything painted BLACK-on-black —
# the root cause of the "black canvas" (found by Florent from the console).
# Ship pygame's whole Python package (__init__.py etc.) so a page can run
# 100% STANDARD `import pygame` code (pygame-ce-standard.html). The loader
# pre-registers the C submodules in sys.modules then execs the real
# __init__.py. Assets (default font) go to MEMFS by path.
mkdir -p "$REPO/loader/pygame"
cp "$SRC/../src_py/"*.py "$REPO/loader/pygame/"
cp "$SRC/../src_py/freesansbold.ttf" "$REPO/loader/pygame/"
# Override pkgdata: upstream resolves resources via importlib.resources against
# a real package dir; ours reads MEMFS by path (font.c reads only `.name`).
cat > "$REPO/loader/pygame/pkgdata.py" <<'PKGDATA'
# wasthon shim: assets live in MEMFS at "/<identifier>" (written by the loader).
def getResource(identifier, pkgname=__name__):
    return _Resource("/" + identifier)


class _Resource:
    def __init__(self, path):
        self.name = path
        self._f = None

    def read(self, *args):
        if self._f is None:
            self._f = open(self.name, "rb")
        return self._f.read(*args)

    def close(self):
        if self._f is not None:
            self._f.close()
            self._f = None
PKGDATA
echo
echo "linked. remaining undefined symbols:"
# (none expected; ERROR_ON_UNDEFINED_SYMBOLS=0 stubs any that slip)
echo "Built $OBJ/_pygame.{mjs,wasm} (+ copied to loader/)."
echo "Probe: (cd loader && python3 -m http.server 8912) -> http://127.0.0.1:8912/pygame-ce-probe.html"
