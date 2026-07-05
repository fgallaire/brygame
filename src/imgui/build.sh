#!/usr/bin/env bash
# GUI POC for wasthon — fetch Dear ImGui and build its SDL2+OpenGL3 example to
# WASM. Decision: ImGui on the SDL2 backend (see README.md). The upstream clone
# is gitignored (fetched, like external/), only this recipe is tracked.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

# 1. fetch Dear ImGui (shallow) if absent
if [[ ! -d "$HERE/imgui" ]]; then
    git clone --depth 1 https://github.com/ocornut/imgui.git "$HERE/imgui"
fi

# 2. emscripten toolchain — wasthon's vendored emsdk (installs on first run)
source "$REPO/external/emsdk/emsdk_env.sh"

# 3. build the SDL2 backend example -> web/index.{html,js,wasm}
#    (-sUSE_SDL=2 = the same SDL2 emscripten port a future pygame port links)
cd "$HERE/imgui/examples/example_sdl2_opengl3"
make -f Makefile.emscripten

echo
echo "Built:  $(pwd)/web/  (index.wasm ~1.4 MB + index.js + index.html)"
echo "Serve:  python3 -m http.server 8900 --bind 127.0.0.1   # run from $HERE"
echo "Open :  http://127.0.0.1:8900/imgui/examples/example_sdl2_opengl3/web/index.html"
