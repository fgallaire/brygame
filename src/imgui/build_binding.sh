#!/usr/bin/env bash
# wasthon GUI — Python binding of Dear ImGui via cimgui (pure C).
# Fetches cimgui, compiles its C API (core + SDL2/OpenGL3 backends), the _imgui
# glue and the wasthon bridge to WASM, links them into _imgui.mjs/.wasm, and
# copies into loader/ for the demo page (loader/imgui-demo.html).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OBJ="$HERE/obj"; mkdir -p "$OBJ"

# 1. fetch cimgui (docking branch = imgui + docking; bundles imgui as submodule)
if [[ ! -d "$HERE/cimgui" ]]; then
    git clone --depth 1 --recursive -b docking_inter https://github.com/cimgui/cimgui.git "$HERE/cimgui"
fi
CI="$HERE/cimgui"

# 1b. font asset: a full-Latin TTF embedded into the module so French accents,
#     the oe ligature and the euro sign render (ImGui's built-in font is ASCII).
FONT="$HERE/assets/DejaVuSans.ttf"
if [[ ! -f "$FONT" ]]; then
    mkdir -p "$HERE/assets"
    SYS=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
    if [[ -f "$SYS" ]]; then cp "$SYS" "$FONT"
    else curl -fsSL -o "$FONT" \
        https://github.com/dejavu-fonts/dejavu-fonts/raw/master/ttf/DejaVuSans.ttf
    fi
fi

# 1c. stb_image (single-header C image decoder, embedded into the glue TU)
if [[ ! -f "$HERE/stb/stb_image.h" ]]; then
    mkdir -p "$HERE/stb"
    curl -fsSL -o "$HERE/stb/stb_image.h" \
        https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
fi

# 1d. cimplot (C bindings of ImPlot; bundles implot as a submodule). Built
#     against cimgui's imgui (NOT its own) so there is a single imgui.
if [[ ! -d "$HERE/cimplot" ]]; then
    git clone --depth 1 --recursive https://github.com/cimgui/cimplot.git "$HERE/cimplot"
fi
CP="$HERE/cimplot"

source "$REPO/external/emsdk/emsdk_env.sh"

# IMPORTANT: every imgui translation unit (core, backends, glue) MUST share the
# SAME imgui config defines, else sizeof(ImGuiIO) differs at runtime and the
# backend init aborts on "Mismatched struct layout!".
IMDEF="-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS"
COMMON="-Os -sUSE_SDL=2 -sDISABLE_EXCEPTION_CATCHING=1 $IMDEF"

# 2a. cimgui core + imgui core
for f in cimgui imgui/imgui imgui/imgui_draw imgui/imgui_tables imgui/imgui_widgets imgui/imgui_demo; do
    em++ -c $COMMON -I "$CI" -I "$CI/imgui" "$CI/$f.cpp" -o "$OBJ/$(basename "$f").o"
done

# 2b. C-wrapped backends — imgui backends MUST use IMGUI_IMPL_API=extern "C" so
#     their linkage matches cimgui_impl.h (otherwise: 'different language linkage')
em++ -c $COMMON -DCIMGUI_USE_SDL2 -DCIMGUI_USE_OPENGL3 -DIMGUI_IMPL_API='extern "C"' \
    -I "$CI" -I "$CI/imgui" -I "$CI/imgui/backends" "$CI/cimgui_impl.cpp" -o "$OBJ/cimgui_impl.o"
em++ -c $COMMON -DIMGUI_IMPL_API='extern "C"' -I "$CI/imgui" -I "$CI/imgui/backends" \
    "$CI/imgui/backends/imgui_impl_sdl2.cpp"    -o "$OBJ/imgui_impl_sdl2.o"
em++ -c $COMMON -DIMGUI_IMPL_API='extern "C"' -I "$CI/imgui" -I "$CI/imgui/backends" \
    "$CI/imgui/backends/imgui_impl_opengl3.cpp" -o "$OBJ/imgui_impl_opengl3.o"

# 2a'. ImPlot core + cimplot wrapper, compiled against cimgui's imgui.
em++ -c $COMMON -I "$CI/imgui" -I "$CP/implot" "$CP/implot/implot.cpp"       -o "$OBJ/implot.o"
em++ -c $COMMON -I "$CI/imgui" -I "$CP/implot" "$CP/implot/implot_items.cpp" -o "$OBJ/implot_items.o"
em++ -c $COMMON -I "$CP" -I "$CP/implot" -I "$CI" -I "$CI/imgui" "$CP/cimplot.cpp" -o "$OBJ/cimplot.o"

# 2c. the glue (C, SAME imgui defines, against wasthon's C-API headers in src/)
emcc -c -Os -sUSE_SDL=2 -sUSE_SDL_MIXER=2 $IMDEF -DCIMGUI_USE_SDL2 -DCIMGUI_USE_OPENGL3 \
    -I "$REPO/src" -I "$CI" -I "$CI/imgui" -I "$HERE/stb" -I "$CP" "$HERE/_imgui.c" -o "$OBJ/_imgui.o"

# 2d. the wasthon bridge
emcc -O3 -c -I "$REPO/src" "$REPO/src/wasthon.c" -o "$OBJ/wasthon.o"

# 3. link everything -> _imgui.mjs/.wasm (multi-phase module, PyInit__imgui)
RTM='"HEAPU8","HEAP32","HEAPF32","HEAPF64","HEAP16","UTF8ToString","stringToUTF8","lengthBytesUTF8"'
emcc -O2 "$OBJ"/*.o \
    --js-library "$REPO/src/wasthon.js" -sUSE_SDL=2 -sUSE_SDL_MIXER=2 \
    --embed-file "$FONT"@/DejaVuSans.ttf \
    -s ALLOW_MEMORY_GROWTH=1 -s ALLOW_TABLE_GROWTH=1 \
    -s EXPORTED_FUNCTIONS='["_PyInit__imgui","_wasthon_init","_wasthon_module_create","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS="[$RTM]" \
    -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME='wasthon_imgui' \
    -o "$OBJ/_imgui.mjs"

# 4. copy into loader/ for the demo page
cp "$OBJ/_imgui.mjs" "$OBJ/_imgui.wasm" "$REPO/loader/"
echo
echo "Built $OBJ/_imgui.{mjs,wasm}  (+ copied to loader/)."
echo "Demo: (cd loader && python3 -m http.server 8910) -> http://127.0.0.1:8910/imgui-demo.html"
