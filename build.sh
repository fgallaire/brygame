#!/usr/bin/env bash
# Brygame — build the WASM/mjs artifacts. These are NEVER committed: they are
# produced here (locally or in CI) from source.
#
# The generic C-API bridge (src/wasthon.*) and Brython come from the wasthon
# repo (@main); the pygame/imgui build recipes and the loader glue live HERE.
# The external source trees are pinned to the exact commits known to work, so a
# rebuild reproduces the artifacts we already validated. Outputs land at the
# repo root, ready for GitHub Pages.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

WASTHON_REPO="${WASTHON_REPO:-https://github.com/fgallaire/wasthon.git}"
WASTHON_REF="${WASTHON_REF:-main}"
EMSCRIPTEN_VERSION="${EMSCRIPTEN_VERSION:-5.0.7}"
PYGAME_CE_COMMIT="c650ba8d214577808ebbc8cdb9db9fd934dc8162"
CIMGUI_COMMIT="053280dfff63a74cc56a3e493671bee4bb6c60e4"      # docking_inter (+ imgui submodule)
CIMPLOT_COMMIT="999ce3e173aa0d7b5a3d08d6727dda028b7e8e25"     # (+ implot submodule)

W="$HERE/.wasthon"

echo "=== clone wasthon @ ${WASTHON_REF} (bridge + Brython) ==="
rm -rf "$W"
git clone --depth 1 -b "$WASTHON_REF" "$WASTHON_REPO" "$W"

# The pygame/imgui build recipes live in THIS repo (the port); the wasthon
# clone only provides the generic bridge (src/) and Brython. Inject the
# recipes into the clone so they run with the same layout as always
# (REPO="$HERE/.." resolves to the clone).
mkdir -p "$W/pygame-poc" "$W/gui-poc"
cp -r "$HERE/pygame-poc/." "$W/pygame-poc/"
cp -r "$HERE/gui-poc/."    "$W/gui-poc/"

echo "=== install emsdk ${EMSCRIPTEN_VERSION} (the build scripts source external/emsdk) ==="
git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$W/external/emsdk"
( cd "$W/external/emsdk" && ./emsdk install "$EMSCRIPTEN_VERSION" && ./emsdk activate "$EMSCRIPTEN_VERSION" )

# Pin an external source tree to an exact commit (fetch-by-SHA, so it works even
# if the upstream branch has moved). The build scripts skip their own fetch when
# the target dir already exists, so these win.
pin() {  # url dir commit [--recursive]
    local url="$1" dir="$2" commit="$3" rec="${4:-}"
    echo "=== pin $(basename "$dir") @ ${commit:0:10} ==="
    rm -rf "$dir"; mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" checkout -q FETCH_HEAD
    if [ "$rec" = "--recursive" ]; then
        git -C "$dir" submodule update -q --init --recursive --depth 1
    fi
}
pin https://github.com/pygame-community/pygame-ce.git "$W/pygame-poc/pygame-ce" "$PYGAME_CE_COMMIT"
pin https://github.com/cimgui/cimgui.git  "$W/gui-poc/cimgui"  "$CIMGUI_COMMIT"  --recursive
pin https://github.com/cimgui/cimplot.git "$W/gui-poc/cimplot" "$CIMPLOT_COMMIT" --recursive

echo "=== build pygame-ce  -> _pygame.{mjs,wasm} + pygame/ package ==="
bash "$W/pygame-poc/build_pygame_ce.sh"
echo "=== build imgui binding (cimgui + ImPlot)  -> _imgui.{mjs,wasm} ==="
bash "$W/gui-poc/build_binding.sh"

echo "=== collect artifacts into $HERE ==="
cp "$W/loader/_pygame.mjs" "$W/loader/_pygame.wasm" "$HERE/"
cp "$W/loader/_imgui.mjs"  "$W/loader/_imgui.wasm"  "$HERE/"
rm -rf "$HERE/pygame";  cp -r "$W/loader/pygame"  "$HERE/pygame"
rm -rf "$HERE/brython"; cp -r "$W/loader/brython" "$HERE/brython"
cp "$W/Wasthon.png" "$HERE/" 2>/dev/null || cp "$W/loader/Wasthon.png" "$HERE/" 2>/dev/null || true

echo "=== done: _pygame.* _imgui.* wasthon-pygame.js pygame/ brython/ ==="
