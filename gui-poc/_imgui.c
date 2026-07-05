/* wasthon GUI POC — Dear ImGui binding (via cimgui, pure C).
 *
 * Exposes a broad immediate-mode widget set + run(draw) to Python (Brython),
 * driven through wasthon's C-API bridge. The render loop calls back into the
 * Python draw() callback each frame (PyObject_CallObject).
 *
 * Mutating widgets follow the pyimgui convention: they take the current
 * value and return a (changed, value) tuple — Python scalars are immutable,
 * while ImGui mutates through a pointer. Vector widgets take/return a tuple.
 *
 * Args are parsed with the standard PyArg_ParseTuple format codes
 * ('s' string, 'f'/'d' float/double, 'i' int, 'p' predicate, 'O' object for
 * lists/bytes/callbacks). Sequences and bytes stay 'O' and are read by hand.
 */
#include <Python.h>                 /* -> wasthon.h (the bridge's C-API) */
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <float.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <SDL.h>
#include <SDL_mixer.h>             /* audio (emscripten port via -sUSE_SDL_MIXER=2) */
#include <GLES2/gl2.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include "cimgui.h"
#include "cimgui_impl.h"          /* CIMGUI_USE_SDL2/OPENGL3 passed via -D */

/* a real C image decoder (PNG/JPEG) compiled to WASM and driven from Python
 * through the bridge — load_image() turns encoded bytes into a GL texture. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include "cimplot.h"              /* ImPlot (another C++ lib) via cimplot, same recipe */

static PyObject *g_draw = NULL;     /* the Python draw() callback */
static SDL_Window *g_win = NULL;
static ImFont *g_font = NULL;       /* the embedded DejaVu font (for push_font_size) */
static Mix_Chunk *g_sounds[32];     /* loaded WAV chunks (load_sound / play_sound) */
static int g_sound_count = 0;

/* one frame: pump SDL events -> ImGui new-frame -> Python draw() -> render */
static void frame(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);   /* feed mouse/keyboard to ImGui */
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    igNewFrame();

    if (g_draw) {
        PyObject *r = PyObject_CallObject(g_draw, NULL);
        if (r) { Py_DECREF(r); } else { PyErr_Print(); }
    }

    igRender();
    int w = 0, h = 0;
    SDL_GetWindowSize(g_win, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
    SDL_GL_SwapWindow(g_win);
}

/* --- conversion helpers ----------------------------------------------- */

/* (a, b) tuple, stealing both references (PyTuple_SetItem semantics). */
static PyObject *tuple2(PyObject *a, PyObject *b) {
    PyObject *t = PyTuple_New(2);
    PyTuple_SetItem(t, 0, a);
    PyTuple_SetItem(t, 1, b);
    return t;
}

/* read n floats from a Python sequence into out[]; 0 (+ error set) on fail */
static int seq_to_floats(PyObject *seq, float *out, int n) {
    for (int i = 0; i < n; i++) {
        PyObject *it = PySequence_GetItem(seq, i);
        if (!it) return 0;
        out[i] = (float)PyFloat_AsDouble(it);
        Py_DECREF(it);
    }
    return 1;
}

static int seq_to_ints(PyObject *seq, int *out, int n) {
    for (int i = 0; i < n; i++) {
        PyObject *it = PySequence_GetItem(seq, i);
        if (!it) return 0;
        out[i] = (int)PyLong_AsLong(it);
        Py_DECREF(it);
    }
    return 1;
}

static PyObject *floats_to_tuple(const float *v, int n) {
    PyObject *t = PyTuple_New(n);
    for (int i = 0; i < n; i++) PyTuple_SetItem(t, i, PyFloat_FromDouble(v[i]));
    return t;
}

static PyObject *ints_to_tuple(const int *v, int n) {
    PyObject *t = PyTuple_New(n);
    for (int i = 0; i < n; i++) PyTuple_SetItem(t, i, PyLong_FromLong(v[i]));
    return t;
}

/* (changed, c[0], ..., c[n-1]) flat tuple, for color_edit/color_picker */
static PyObject *color_ret(bool changed, const float *c, int n) {
    PyObject *t = PyTuple_New(n + 1);
    PyTuple_SetItem(t, 0, PyBool_FromLong(changed));
    for (int i = 0; i < n; i++) PyTuple_SetItem(t, i + 1, PyFloat_FromDouble(c[i]));
    return t;
}

/* --- lifecycle / window ----------------------------------------------- */

/* keep the SDL window (= the canvas backing size) in sync with the browser
 * window so the whole viewport is usable. */
static EM_BOOL on_browser_resize(int type, const EmscriptenUiEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    int w = EM_ASM_INT({ return window.innerWidth | 0; });
    int h = EM_ASM_INT({ return window.innerHeight | 0; });
    if (w > 0 && h > 0 && g_win) SDL_SetWindowSize(g_win, w, h);
    return EM_TRUE;
}

static PyObject *py_run(PyObject *self, PyObject *args) {
    PyObject *cb;
    if (!PyArg_ParseTuple(args, "O", &cb)) return NULL;
    Py_INCREF(cb);
    Py_XDECREF(g_draw);
    g_draw = cb;

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    int sw = EM_ASM_INT({ return window.innerWidth | 0; });
    int sh = EM_ASM_INT({ return window.innerHeight | 0; });
    if (sw <= 0) sw = 1280;
    if (sh <= 0) sh = 800;
    g_win = SDL_CreateWindow("wasthon imgui",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, sw, sh,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl = SDL_GL_CreateContext(g_win);

    igCreateContext(NULL);
    ImPlot_CreateContext();

    /* Load a full-Latin TTF (embedded at link time via --embed-file) so French
     * accents, the oe ligature and the euro sign render — ImGui's built-in font
     * is ASCII-only. Range: Basic Latin .. Latin Extended-B + euro sign. */
    static const ImWchar ranges[] = {0x0020, 0x024F, 0x20AC, 0x20AC, 0};
    ImGuiIO *io = igGetIO_Nil();
    g_font = ImFontAtlas_AddFontFromFileTTF(io->Fonts, "/DejaVuSans.ttf", 18.0f, NULL, ranges);
    io->ConfigFlags |= ImGuiConfigFlags_DockingEnable;   /* docking branch feature */

    ImGui_ImplSDL2_InitForOpenGL(g_win, gl);
    ImGui_ImplOpenGL3_Init("#version 100");

    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_FALSE,
                                   on_browser_resize);

    /* simulate_infinite_loop=0: return to Brython, loop runs via rAF */
    emscripten_set_main_loop(frame, 0, 0);
    Py_RETURN_NONE;
}

static PyObject *py_begin(PyObject *self, PyObject *args) {
    const char *name;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "s|i", &name, &flags)) return NULL;
    igBegin(name, NULL, flags);
    Py_RETURN_NONE;
}

static PyObject *py_end(PyObject *self, PyObject *args) {
    igEnd();
    Py_RETURN_NONE;
}

static PyObject *py_set_next_window_size(PyObject *self, PyObject *args) {
    float w, h;
    if (!PyArg_ParseTuple(args, "ff", &w, &h)) return NULL;
    ImVec2 size = {w, h};
    igSetNextWindowSize(size, ImGuiCond_FirstUseEver);
    Py_RETURN_NONE;
}

static PyObject *py_set_next_window_pos(PyObject *self, PyObject *args) {
    float x, y;
    if (!PyArg_ParseTuple(args, "ff", &x, &y)) return NULL;
    ImVec2 pos = {x, y};
    ImVec2 pivot = {0.0f, 0.0f};
    igSetNextWindowPos(pos, ImGuiCond_FirstUseEver, pivot);
    Py_RETURN_NONE;
}

static PyObject *py_begin_child(PyObject *self, PyObject *args) {
    const char *id;
    float w = 0.0f, h = 0.0f;
    int border = 0;
    if (!PyArg_ParseTuple(args, "s|ffp", &id, &w, &h, &border)) return NULL;
    ImVec2 size = {w, h};
    int child_flags = border ? ImGuiChildFlags_Borders : 0;
    return PyBool_FromLong(igBeginChild_Str(id, size, child_flags, 0));
}

static PyObject *py_end_child(PyObject *self, PyObject *args) {
    igEndChild();
    Py_RETURN_NONE;
}

/* --- text ------------------------------------------------------------- */

static PyObject *py_text(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igText("%s", s);
    Py_RETURN_NONE;
}

static PyObject *py_text_colored(PyObject *self, PyObject *args) {
    float r, g, b;
    const char *s;
    if (!PyArg_ParseTuple(args, "fffs", &r, &g, &b, &s)) return NULL;
    ImVec4 col = {r, g, b, 1.0f};
    igTextColored(col, "%s", s);
    Py_RETURN_NONE;
}

static PyObject *py_text_wrapped(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igTextWrapped("%s", s);
    Py_RETURN_NONE;
}

static PyObject *py_text_disabled(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igTextDisabled("%s", s);
    Py_RETURN_NONE;
}

static PyObject *py_label_text(PyObject *self, PyObject *args) {
    const char *label, *val;
    if (!PyArg_ParseTuple(args, "ss", &label, &val)) return NULL;
    igLabelText(label, "%s", val);
    Py_RETURN_NONE;
}

static PyObject *py_bullet_text(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igBulletText("%s", s);
    Py_RETURN_NONE;
}

static PyObject *py_separator_text(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igSeparatorText(s);
    Py_RETURN_NONE;
}

/* --- buttons & booleans ----------------------------------------------- */

static PyObject *py_button(PyObject *self, PyObject *args) {
    const char *label;
    if (!PyArg_ParseTuple(args, "s", &label)) return NULL;
    ImVec2 size = {0.0f, 0.0f};
    return PyBool_FromLong(igButton(label, size));
}

static PyObject *py_checkbox(PyObject *self, PyObject *args) {
    const char *label;
    int state;
    if (!PyArg_ParseTuple(args, "sp", &label, &state)) return NULL;
    bool v = state ? true : false;
    bool changed = igCheckbox(label, &v);
    return tuple2(PyBool_FromLong(changed), PyBool_FromLong(v));
}

static PyObject *py_radio_button(PyObject *self, PyObject *args) {
    const char *label;
    int active;
    if (!PyArg_ParseTuple(args, "sp", &label, &active)) return NULL;
    return PyBool_FromLong(igRadioButton_Bool(label, active ? true : false));
}

static PyObject *py_selectable(PyObject *self, PyObject *args) {
    const char *label;
    int sel = 0, flags = 0;
    if (!PyArg_ParseTuple(args, "s|pi", &label, &sel, &flags)) return NULL;
    ImVec2 size = {0.0f, 0.0f};
    return PyBool_FromLong(igSelectable_Bool(label, sel ? true : false, flags, size));
}

/* --- sliders & drags (scalar) ----------------------------------------- */

static PyObject *py_slider_float(PyObject *self, PyObject *args) {
    const char *label;
    float v, vmin, vmax;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "sfff|i", &label, &v, &vmin, &vmax, &flags)) return NULL;
    bool changed = igSliderFloat(label, &v, vmin, vmax, "%.3f", flags);
    return tuple2(PyBool_FromLong(changed), PyFloat_FromDouble(v));
}

static PyObject *py_slider_int(PyObject *self, PyObject *args) {
    const char *label;
    int v, vmin, vmax, flags = 0;
    if (!PyArg_ParseTuple(args, "siii|i", &label, &v, &vmin, &vmax, &flags)) return NULL;
    bool changed = igSliderInt(label, &v, vmin, vmax, "%d", flags);
    return tuple2(PyBool_FromLong(changed), PyLong_FromLong(v));
}

static PyObject *py_drag_float(PyObject *self, PyObject *args) {
    const char *label;
    float v, speed = 1.0f, vmin = 0.0f, vmax = 0.0f;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "sf|fffi", &label, &v, &speed, &vmin, &vmax, &flags))
        return NULL;
    bool changed = igDragFloat(label, &v, speed, vmin, vmax, "%.3f", flags);
    return tuple2(PyBool_FromLong(changed), PyFloat_FromDouble(v));
}

static PyObject *py_drag_int(PyObject *self, PyObject *args) {
    const char *label;
    int v, vmin = 0, vmax = 0, flags = 0;
    float speed = 1.0f;
    if (!PyArg_ParseTuple(args, "si|fiii", &label, &v, &speed, &vmin, &vmax, &flags))
        return NULL;
    bool changed = igDragInt(label, &v, speed, vmin, vmax, "%d", flags);
    return tuple2(PyBool_FromLong(changed), PyLong_FromLong(v));
}

/* --- sliders / drags / inputs (vector: n = 2/3/4) --------------------- */

static PyObject *slider_floatN(PyObject *args, int n) {
    const char *label;
    PyObject *seq;
    float mn, mx;
    int f = 0;
    if (!PyArg_ParseTuple(args, "sOff|i", &label, &seq, &mn, &mx, &f)) return NULL;
    float v[4] = {0};
    if (!seq_to_floats(seq, v, n)) return NULL;
    bool ch = (n == 2) ? igSliderFloat2(label, v, mn, mx, "%.3f", f)
            : (n == 3) ? igSliderFloat3(label, v, mn, mx, "%.3f", f)
                       : igSliderFloat4(label, v, mn, mx, "%.3f", f);
    return tuple2(PyBool_FromLong(ch), floats_to_tuple(v, n));
}
static PyObject *py_slider_float2(PyObject *s, PyObject *a) { return slider_floatN(a, 2); }
static PyObject *py_slider_float3(PyObject *s, PyObject *a) { return slider_floatN(a, 3); }
static PyObject *py_slider_float4(PyObject *s, PyObject *a) { return slider_floatN(a, 4); }

static PyObject *drag_floatN(PyObject *args, int n) {
    const char *label;
    PyObject *seq;
    float sp = 1.0f, mn = 0.0f, mx = 0.0f;
    int f = 0;
    if (!PyArg_ParseTuple(args, "sO|fffi", &label, &seq, &sp, &mn, &mx, &f)) return NULL;
    float v[4] = {0};
    if (!seq_to_floats(seq, v, n)) return NULL;
    bool ch = (n == 2) ? igDragFloat2(label, v, sp, mn, mx, "%.3f", f)
            : (n == 3) ? igDragFloat3(label, v, sp, mn, mx, "%.3f", f)
                       : igDragFloat4(label, v, sp, mn, mx, "%.3f", f);
    return tuple2(PyBool_FromLong(ch), floats_to_tuple(v, n));
}
static PyObject *py_drag_float2(PyObject *s, PyObject *a) { return drag_floatN(a, 2); }
static PyObject *py_drag_float3(PyObject *s, PyObject *a) { return drag_floatN(a, 3); }
static PyObject *py_drag_float4(PyObject *s, PyObject *a) { return drag_floatN(a, 4); }

static PyObject *input_floatN(PyObject *args, int n) {
    const char *label;
    PyObject *seq;
    if (!PyArg_ParseTuple(args, "sO", &label, &seq)) return NULL;
    float v[4] = {0};
    if (!seq_to_floats(seq, v, n)) return NULL;
    bool ch = (n == 2) ? igInputFloat2(label, v, "%.3f", 0)
            : (n == 3) ? igInputFloat3(label, v, "%.3f", 0)
                       : igInputFloat4(label, v, "%.3f", 0);
    return tuple2(PyBool_FromLong(ch), floats_to_tuple(v, n));
}
static PyObject *py_input_float2(PyObject *s, PyObject *a) { return input_floatN(a, 2); }
static PyObject *py_input_float3(PyObject *s, PyObject *a) { return input_floatN(a, 3); }
static PyObject *py_input_float4(PyObject *s, PyObject *a) { return input_floatN(a, 4); }

static PyObject *input_intN(PyObject *args, int n) {
    const char *label;
    PyObject *seq;
    if (!PyArg_ParseTuple(args, "sO", &label, &seq)) return NULL;
    int v[4] = {0};
    if (!seq_to_ints(seq, v, n)) return NULL;
    bool ch = (n == 2) ? igInputInt2(label, v, 0)
            : (n == 3) ? igInputInt3(label, v, 0)
                       : igInputInt4(label, v, 0);
    return tuple2(PyBool_FromLong(ch), ints_to_tuple(v, n));
}
static PyObject *py_input_int2(PyObject *s, PyObject *a) { return input_intN(a, 2); }
static PyObject *py_input_int3(PyObject *s, PyObject *a) { return input_intN(a, 3); }
static PyObject *py_input_int4(PyObject *s, PyObject *a) { return input_intN(a, 4); }

/* --- text/number inputs (scalar) -------------------------------------- */

static PyObject *py_input_text(PyObject *self, PyObject *args) {
    const char *label, *cur;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "ss|i", &label, &cur, &flags)) return NULL;
    char buf[256];
    strncpy(buf, cur, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    bool changed = igInputText(label, buf, sizeof(buf), flags, NULL, NULL);
    return tuple2(PyBool_FromLong(changed), PyUnicode_FromString(buf));
}

static PyObject *py_input_text_multiline(PyObject *self, PyObject *args) {
    const char *label, *cur;
    float height = 0.0f;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "ss|fi", &label, &cur, &height, &flags)) return NULL;
    static char buf[4096];
    strncpy(buf, cur, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    ImVec2 size = {-1.0f, height};
    bool changed = igInputTextMultiline(label, buf, sizeof(buf), size, flags, NULL, NULL);
    return tuple2(PyBool_FromLong(changed), PyUnicode_FromString(buf));
}

static PyObject *py_input_float(PyObject *self, PyObject *args) {
    const char *label;
    float v;
    if (!PyArg_ParseTuple(args, "sf", &label, &v)) return NULL;
    bool changed = igInputFloat(label, &v, 0.0f, 0.0f, "%.3f", 0);
    return tuple2(PyBool_FromLong(changed), PyFloat_FromDouble(v));
}

static PyObject *py_input_int(PyObject *self, PyObject *args) {
    const char *label;
    int v;
    if (!PyArg_ParseTuple(args, "si", &label, &v)) return NULL;
    bool changed = igInputInt(label, &v, 1, 100, 0);
    return tuple2(PyBool_FromLong(changed), PyLong_FromLong(v));
}

/* --- selection / color ------------------------------------------------ */

static PyObject *py_combo(PyObject *self, PyObject *args) {
    const char *label;
    int current;
    PyObject *items;
    if (!PyArg_ParseTuple(args, "siO", &label, &current, &items)) return NULL;

    /* igCombo_Str wants the items as one "a\0b\0c\0\0" buffer. */
    Py_ssize_t n = PySequence_Size(items);
    if (n < 0) return NULL;
    Py_ssize_t total = 1;               /* trailing extra NUL */
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *it = PySequence_GetItem(items, i);
        const char *s = PyUnicode_AsUTF8(it);
        if (!s) { Py_DECREF(it); return NULL; }
        total += (Py_ssize_t)strlen(s) + 1;
        Py_DECREF(it);
    }
    char *buf = (char *)malloc(total);
    char *p = buf;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *it = PySequence_GetItem(items, i);
        const char *s = PyUnicode_AsUTF8(it);
        size_t len = strlen(s);
        memcpy(p, s, len + 1);          /* copy incl. NUL */
        p += len + 1;
        Py_DECREF(it);
    }
    *p = '\0';                          /* terminating empty string */

    bool changed = igCombo_Str(label, &current, buf, -1);
    free(buf);
    return tuple2(PyBool_FromLong(changed), PyLong_FromLong(current));
}

static PyObject *py_color_edit3(PyObject *self, PyObject *args) {
    const char *label;
    float col[3];
    int flags = 0;
    if (!PyArg_ParseTuple(args, "sfff|i", &label, &col[0], &col[1], &col[2], &flags))
        return NULL;
    bool changed = igColorEdit3(label, col, flags);
    return color_ret(changed, col, 3);
}

static PyObject *py_color_edit4(PyObject *self, PyObject *args) {
    const char *label;
    float col[4];
    int flags = 0;
    if (!PyArg_ParseTuple(args, "sffff|i", &label, &col[0], &col[1], &col[2], &col[3], &flags))
        return NULL;
    bool changed = igColorEdit4(label, col, flags);
    return color_ret(changed, col, 4);
}

static PyObject *py_color_picker3(PyObject *self, PyObject *args) {
    const char *label;
    float col[3];
    if (!PyArg_ParseTuple(args, "sfff", &label, &col[0], &col[1], &col[2])) return NULL;
    bool changed = igColorPicker3(label, col, 0);
    return color_ret(changed, col, 3);
}

static PyObject *py_color_picker4(PyObject *self, PyObject *args) {
    const char *label;
    float col[4];
    if (!PyArg_ParseTuple(args, "sffff", &label, &col[0], &col[1], &col[2], &col[3]))
        return NULL;
    bool changed = igColorPicker4(label, col, 0, NULL);
    return color_ret(changed, col, 4);
}

/* --- custom combo / tree_node_ex / extra sliders / input_double ------- */

static PyObject *py_begin_combo(PyObject *self, PyObject *args) {
    const char *label, *preview;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "ss|i", &label, &preview, &flags)) return NULL;
    return PyBool_FromLong(igBeginCombo(label, preview, flags));
}
static PyObject *py_end_combo(PyObject *self, PyObject *args) {
    igEndCombo();
    Py_RETURN_NONE;
}

static PyObject *py_tree_node_ex(PyObject *self, PyObject *args) {
    const char *label;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "s|i", &label, &flags)) return NULL;
    return PyBool_FromLong(igTreeNodeEx_Str(label, flags));
}

static PyObject *py_vslider_float(PyObject *self, PyObject *args) {
    const char *label;
    float w, h, v, mn, mx;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "sfffff|i", &label, &w, &h, &v, &mn, &mx, &flags))
        return NULL;
    ImVec2 size = {w, h};
    bool ch = igVSliderFloat(label, size, &v, mn, mx, "%.3f", flags);
    return tuple2(PyBool_FromLong(ch), PyFloat_FromDouble(v));
}

static PyObject *py_vslider_int(PyObject *self, PyObject *args) {
    const char *label;
    float w, h;
    int v, mn, mx, flags = 0;
    if (!PyArg_ParseTuple(args, "sffiii|i", &label, &w, &h, &v, &mn, &mx, &flags))
        return NULL;
    ImVec2 size = {w, h};
    bool ch = igVSliderInt(label, size, &v, mn, mx, "%d", flags);
    return tuple2(PyBool_FromLong(ch), PyLong_FromLong(v));
}

static PyObject *py_slider_angle(PyObject *self, PyObject *args) {
    const char *label;
    float rad, dmin = -360.0f, dmax = 360.0f;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "sf|ffi", &label, &rad, &dmin, &dmax, &flags)) return NULL;
    bool ch = igSliderAngle(label, &rad, dmin, dmax, "%.0f deg", flags);
    return tuple2(PyBool_FromLong(ch), PyFloat_FromDouble(rad));
}

static PyObject *py_drag_float_range2(PyObject *self, PyObject *args) {
    const char *label;
    float cmin, cmax, speed = 1.0f, lo_ = 0.0f, hi_ = 0.0f;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "sff|fffi", &label, &cmin, &cmax, &speed, &lo_, &hi_, &flags))
        return NULL;
    bool ch = igDragFloatRange2(label, &cmin, &cmax, speed, lo_, hi_, "%.3f", NULL, flags);
    PyObject *t = PyTuple_New(3);
    PyTuple_SetItem(t, 0, PyBool_FromLong(ch));
    PyTuple_SetItem(t, 1, PyFloat_FromDouble(cmin));
    PyTuple_SetItem(t, 2, PyFloat_FromDouble(cmax));
    return t;
}

static PyObject *py_input_double(PyObject *self, PyObject *args) {
    const char *label;
    double v;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "sd|i", &label, &v, &flags)) return NULL;
    bool ch = igInputDouble(label, &v, 0.0, 0.0, "%.6f", flags);
    return tuple2(PyBool_FromLong(ch), PyFloat_FromDouble(v));
}

/* --- menus ------------------------------------------------------------ */

static PyObject *py_begin_main_menu_bar(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igBeginMainMenuBar());
}
static PyObject *py_end_main_menu_bar(PyObject *self, PyObject *args) {
    igEndMainMenuBar();
    Py_RETURN_NONE;
}
static PyObject *py_begin_menu_bar(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igBeginMenuBar());
}
static PyObject *py_end_menu_bar(PyObject *self, PyObject *args) {
    igEndMenuBar();
    Py_RETURN_NONE;
}
static PyObject *py_begin_menu(PyObject *self, PyObject *args) {
    const char *label;
    int enabled = 1;
    if (!PyArg_ParseTuple(args, "s|p", &label, &enabled)) return NULL;
    return PyBool_FromLong(igBeginMenu(label, enabled ? true : false));
}
static PyObject *py_end_menu(PyObject *self, PyObject *args) {
    igEndMenu();
    Py_RETURN_NONE;
}
static PyObject *py_menu_item(PyObject *self, PyObject *args) {
    const char *label, *shortcut = NULL;
    int selected = 0, enabled = 1;
    if (!PyArg_ParseTuple(args, "s|zpp", &label, &shortcut, &selected, &enabled))
        return NULL;
    return PyBool_FromLong(igMenuItem_Bool(label, shortcut,
                                           selected ? true : false, enabled ? true : false));
}

/* --- tab bars --------------------------------------------------------- */

static PyObject *py_begin_tab_bar(PyObject *self, PyObject *args) {
    const char *id;
    if (!PyArg_ParseTuple(args, "s", &id)) return NULL;
    return PyBool_FromLong(igBeginTabBar(id, 0));
}
static PyObject *py_end_tab_bar(PyObject *self, PyObject *args) {
    igEndTabBar();
    Py_RETURN_NONE;
}
static PyObject *py_begin_tab_item(PyObject *self, PyObject *args) {
    const char *label;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "s|i", &label, &flags)) return NULL;
    return PyBool_FromLong(igBeginTabItem(label, NULL, flags));
}
static PyObject *py_end_tab_item(PyObject *self, PyObject *args) {
    igEndTabItem();
    Py_RETURN_NONE;
}

/* --- popups ----------------------------------------------------------- */

static PyObject *py_open_popup(PyObject *self, PyObject *args) {
    const char *id;
    if (!PyArg_ParseTuple(args, "s", &id)) return NULL;
    igOpenPopup_Str(id, 0);
    Py_RETURN_NONE;
}
static PyObject *py_begin_popup(PyObject *self, PyObject *args) {
    const char *id;
    if (!PyArg_ParseTuple(args, "s", &id)) return NULL;
    return PyBool_FromLong(igBeginPopup(id, 0));
}
static PyObject *py_begin_popup_modal(PyObject *self, PyObject *args) {
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return NULL;
    return PyBool_FromLong(igBeginPopupModal(name, NULL, 0));
}
static PyObject *py_end_popup(PyObject *self, PyObject *args) {
    igEndPopup();
    Py_RETURN_NONE;
}
static PyObject *py_close_current_popup(PyObject *self, PyObject *args) {
    igCloseCurrentPopup();
    Py_RETURN_NONE;
}

/* --- tables ----------------------------------------------------------- */

static PyObject *py_begin_table(PyObject *self, PyObject *args) {
    const char *id;
    int columns, flags = 0;
    if (!PyArg_ParseTuple(args, "si|i", &id, &columns, &flags)) return NULL;
    ImVec2 outer = {0.0f, 0.0f};
    return PyBool_FromLong(igBeginTable(id, columns, flags, outer, 0.0f));
}
static PyObject *py_end_table(PyObject *self, PyObject *args) {
    igEndTable();
    Py_RETURN_NONE;
}
static PyObject *py_table_next_row(PyObject *self, PyObject *args) {
    igTableNextRow(0, 0.0f);
    Py_RETURN_NONE;
}
static PyObject *py_table_next_column(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igTableNextColumn());
}
static PyObject *py_table_set_column_index(PyObject *self, PyObject *args) {
    int n;
    if (!PyArg_ParseTuple(args, "i", &n)) return NULL;
    return PyBool_FromLong(igTableSetColumnIndex(n));
}
static PyObject *py_table_setup_column(PyObject *self, PyObject *args) {
    const char *label;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "s|i", &label, &flags)) return NULL;
    igTableSetupColumn(label, flags, 0.0f, 0);
    Py_RETURN_NONE;
}
static PyObject *py_table_headers_row(PyObject *self, PyObject *args) {
    igTableHeadersRow();
    Py_RETURN_NONE;
}

/* --- layout & misc ---------------------------------------------------- */

static PyObject *py_progress_bar(PyObject *self, PyObject *args) {
    float frac;
    const char *overlay = NULL;
    if (!PyArg_ParseTuple(args, "f|z", &frac, &overlay)) return NULL;
    ImVec2 size = {-1.0f, 0.0f};        /* stretch to available width */
    igProgressBar(frac, size, overlay);
    Py_RETURN_NONE;
}

static PyObject *py_separator(PyObject *self, PyObject *args) {
    igSeparator();
    Py_RETURN_NONE;
}
static PyObject *py_same_line(PyObject *self, PyObject *args) {
    igSameLine(0.0f, -1.0f);
    Py_RETURN_NONE;
}
static PyObject *py_spacing(PyObject *self, PyObject *args) {
    igSpacing();
    Py_RETURN_NONE;
}
static PyObject *py_new_line(PyObject *self, PyObject *args) {
    igNewLine();
    Py_RETURN_NONE;
}
static PyObject *py_indent(PyObject *self, PyObject *args) {
    igIndent(0.0f);
    Py_RETURN_NONE;
}
static PyObject *py_unindent(PyObject *self, PyObject *args) {
    igUnindent(0.0f);
    Py_RETURN_NONE;
}
static PyObject *py_bullet(PyObject *self, PyObject *args) {
    igBullet();
    Py_RETURN_NONE;
}
static PyObject *py_begin_group(PyObject *self, PyObject *args) {
    igBeginGroup();
    Py_RETURN_NONE;
}
static PyObject *py_end_group(PyObject *self, PyObject *args) {
    igEndGroup();
    Py_RETURN_NONE;
}
static PyObject *py_dummy(PyObject *self, PyObject *args) {
    float w, h;
    if (!PyArg_ParseTuple(args, "ff", &w, &h)) return NULL;
    ImVec2 size = {w, h};
    igDummy(size);
    Py_RETURN_NONE;
}
static PyObject *py_push_item_width(PyObject *self, PyObject *args) {
    float w;
    if (!PyArg_ParseTuple(args, "f", &w)) return NULL;
    igPushItemWidth(w);
    Py_RETURN_NONE;
}
static PyObject *py_pop_item_width(PyObject *self, PyObject *args) {
    igPopItemWidth();
    Py_RETURN_NONE;
}
static PyObject *py_get_content_region_avail(PyObject *self, PyObject *args) {
    ImVec2 r = igGetContentRegionAvail();
    return tuple2(PyFloat_FromDouble(r.x), PyFloat_FromDouble(r.y));
}

static PyObject *py_tree_node(PyObject *self, PyObject *args) {
    const char *label;
    if (!PyArg_ParseTuple(args, "s", &label)) return NULL;
    return PyBool_FromLong(igTreeNode_Str(label));
}
static PyObject *py_tree_pop(PyObject *self, PyObject *args) {
    igTreePop();
    Py_RETURN_NONE;
}
static PyObject *py_collapsing_header(PyObject *self, PyObject *args) {
    const char *label;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "s|i", &label, &flags)) return NULL;
    return PyBool_FromLong(igCollapsingHeader_TreeNodeFlags(label, flags));
}

/* --- queries & tooltips ----------------------------------------------- */

static PyObject *py_is_item_hovered(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igIsItemHovered(0));
}
static PyObject *py_is_item_clicked(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igIsItemClicked(0));    /* 0 = left mouse button */
}
static PyObject *py_is_item_active(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igIsItemActive());
}
static PyObject *py_set_tooltip(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igSetTooltip("%s", s);
    Py_RETURN_NONE;
}
static PyObject *py_begin_tooltip(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igBeginTooltip());
}
static PyObject *py_end_tooltip(PyObject *self, PyObject *args) {
    igEndTooltip();
    Py_RETURN_NONE;
}

/* --- plots ------------------------------------------------------------ */

static PyObject *plot(PyObject *args, int histogram) {
    const char *label, *overlay = NULL;
    PyObject *seq;
    float height = 80.0f;
    if (!PyArg_ParseTuple(args, "sO|zf", &label, &seq, &overlay, &height)) return NULL;
    Py_ssize_t n = PySequence_Size(seq);
    if (n < 0) return NULL;
    float *vals = (float *)malloc(sizeof(float) * (n > 0 ? n : 1));
    if (!seq_to_floats(seq, vals, (int)n)) { free(vals); return NULL; }
    ImVec2 gsize = {0.0f, height};
    if (histogram)
        igPlotHistogram_FloatPtr(label, vals, (int)n, 0, overlay,
                                 FLT_MAX, FLT_MAX, gsize, sizeof(float));
    else
        igPlotLines_FloatPtr(label, vals, (int)n, 0, overlay,
                             FLT_MAX, FLT_MAX, gsize, sizeof(float));
    free(vals);
    Py_RETURN_NONE;
}
static PyObject *py_plot_lines(PyObject *s, PyObject *a) { return plot(a, 0); }
static PyObject *py_plot_histogram(PyObject *s, PyObject *a) { return plot(a, 1); }

static PyObject *py_get_framerate(PyObject *self, PyObject *args) {
    return PyFloat_FromDouble(igGetIO_Nil()->Framerate);
}

/* --- theme & styling -------------------------------------------------- */

static PyObject *py_set_theme(PyObject *self, PyObject *args) {
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return NULL;
    if (strcmp(name, "light") == 0)        igStyleColorsLight(NULL);
    else if (strcmp(name, "classic") == 0) igStyleColorsClassic(NULL);
    else                                   igStyleColorsDark(NULL);
    Py_RETURN_NONE;
}

static PyObject *py_push_style_color(PyObject *self, PyObject *args) {
    int idx;
    float r, g, b, a = 1.0f;
    if (!PyArg_ParseTuple(args, "ifff|f", &idx, &r, &g, &b, &a)) return NULL;
    ImVec4 col = {r, g, b, a};
    igPushStyleColor_Vec4(idx, col);
    Py_RETURN_NONE;
}

static PyObject *py_pop_style_color(PyObject *self, PyObject *args) {
    int count = 1;
    if (!PyArg_ParseTuple(args, "|i", &count)) return NULL;
    igPopStyleColor(count);
    Py_RETURN_NONE;
}

static PyObject *py_push_style_var(PyObject *self, PyObject *args) {
    int var;
    float value;
    if (!PyArg_ParseTuple(args, "if", &var, &value)) return NULL;
    igPushStyleVar_Float(var, value);
    Py_RETURN_NONE;
}

static PyObject *py_push_style_var2(PyObject *self, PyObject *args) {
    int var;
    float x, y;
    if (!PyArg_ParseTuple(args, "iff", &var, &x, &y)) return NULL;
    ImVec2 v = {x, y};
    igPushStyleVar_Vec2(var, v);
    Py_RETURN_NONE;
}

static PyObject *py_pop_style_var(PyObject *self, PyObject *args) {
    int count = 1;
    if (!PyArg_ParseTuple(args, "|i", &count)) return NULL;
    igPopStyleVar(count);
    Py_RETURN_NONE;
}

/* --- textures & images ------------------------------------------------ *
 * A texture is a GL texture id (int). The pixel data is a `bytes` of
 * width*height*4 RGBA8 — exactly the kind of pixel buffer a pygame-style
 * program produces, so blitting one is create_texture()/image(). These
 * must be called once the GL context exists, i.e. from inside draw().     */

static const char *rgba_check(PyObject *data, int w, int h) {
    const char *px = PyBytes_AsString(data);
    if (!px) return NULL;
    if (PyBytes_Size(data) < (Py_ssize_t)w * h * 4) {
        PyErr_SetString(PyExc_ValueError, "rgba bytes shorter than width*height*4");
        return NULL;
    }
    return px;
}

static PyObject *py_create_texture(PyObject *self, PyObject *args) {
    int w, h;
    PyObject *data;
    if (!PyArg_ParseTuple(args, "iiO", &w, &h, &data)) return NULL;
    const char *px = rgba_check(data, w, h);
    if (!px) return NULL;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return PyLong_FromLong((long)tex);
}

static PyObject *py_update_texture(PyObject *self, PyObject *args) {
    int tex, w, h;
    PyObject *data;
    if (!PyArg_ParseTuple(args, "iiiO", &tex, &w, &h, &data)) return NULL;
    const char *px = rgba_check(data, w, h);
    if (!px) return NULL;
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    Py_RETURN_NONE;
}

static PyObject *py_image(PyObject *self, PyObject *args) {
    int tex;
    float w, h;
    if (!PyArg_ParseTuple(args, "iff", &tex, &w, &h)) return NULL;
    ImTextureRef ref = {NULL, (ImTextureID)tex};
    ImVec2 size = {w, h};
    ImVec2 uv0 = {0.0f, 0.0f}, uv1 = {1.0f, 1.0f};
    igImage(ref, size, uv0, uv1);
    Py_RETURN_NONE;
}

static PyObject *py_image_button(PyObject *self, PyObject *args) {
    const char *id;
    int tex;
    float w, h;
    if (!PyArg_ParseTuple(args, "siff", &id, &tex, &w, &h)) return NULL;
    ImTextureRef ref = {NULL, (ImTextureID)tex};
    ImVec2 size = {w, h};
    ImVec2 uv0 = {0.0f, 0.0f}, uv1 = {1.0f, 1.0f};
    ImVec4 bg = {0, 0, 0, 0}, tint = {1, 1, 1, 1};
    return PyBool_FromLong(igImageButton(id, ref, size, uv0, uv1, bg, tint));
}

/* decode encoded image bytes (PNG/JPEG, via stb_image) -> GL texture. */
static PyObject *py_load_image(PyObject *self, PyObject *args) {
    PyObject *data;
    if (!PyArg_ParseTuple(args, "O", &data)) return NULL;
    const unsigned char *enc = (const unsigned char *)PyBytes_AsString(data);
    if (!enc) return NULL;
    Py_ssize_t len = PyBytes_Size(data);
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(enc, (int)len, &w, &h, &comp, 4);
    if (!px) {
        PyErr_SetString(PyExc_ValueError, "load_image: could not decode (PNG/JPEG expected)");
        return NULL;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    PyObject *t = PyTuple_New(3);
    PyTuple_SetItem(t, 0, PyLong_FromLong((long)tex));
    PyTuple_SetItem(t, 1, PyLong_FromLong(w));
    PyTuple_SetItem(t, 2, PyLong_FromLong(h));
    return t;
}

/* --- audio (SDL_mixer) ------------------------------------------------ */

static PyObject *py_load_sound(PyObject *self, PyObject *args) {
    PyObject *data;
    if (!PyArg_ParseTuple(args, "O", &data)) return NULL;
    const char *buf = PyBytes_AsString(data);
    if (!buf) return NULL;
    Py_ssize_t len = PyBytes_Size(data);
    if (g_sound_count >= 32) {
        PyErr_SetString(PyExc_RuntimeError, "load_sound: too many sounds");
        return NULL;
    }
    Mix_Chunk *c = Mix_LoadWAV_RW(SDL_RWFromConstMem(buf, (int)len), 1);
    if (!c) {
        PyErr_SetString(PyExc_ValueError, "load_sound: could not decode (WAV expected)");
        return NULL;
    }
    int id = g_sound_count++;
    g_sounds[id] = c;
    return PyLong_FromLong(id);
}

static PyObject *py_play_sound(PyObject *self, PyObject *args) {
    int id;
    if (!PyArg_ParseTuple(args, "i", &id)) return NULL;
    if (id >= 0 && id < g_sound_count && g_sounds[id])
        Mix_PlayChannel(-1, g_sounds[id], 0);
    Py_RETURN_NONE;
}

/* --- input (mouse / keyboard) ----------------------------------------- *
 * The per-frame input state ImGui already collected from SDL — everything
 * a game loop needs to react to the player. Read these from draw().       */

static PyObject *py_get_mouse_pos(PyObject *self, PyObject *args) {
    ImVec2 p = igGetMousePos();
    return tuple2(PyFloat_FromDouble(p.x), PyFloat_FromDouble(p.y));
}
static PyObject *py_get_mouse_delta(PyObject *self, PyObject *args) {
    ImVec2 d = igGetIO_Nil()->MouseDelta;
    return tuple2(PyFloat_FromDouble(d.x), PyFloat_FromDouble(d.y));
}
static PyObject *py_get_mouse_wheel(PyObject *self, PyObject *args) {
    return PyFloat_FromDouble(igGetIO_Nil()->MouseWheel);
}
static PyObject *py_is_mouse_down(PyObject *self, PyObject *args) {
    PyObject *bo = NULL;
    if (!PyArg_ParseTuple(args, "|O", &bo)) return NULL;
    return PyBool_FromLong(igIsMouseDown_Nil(bo ? (int)PyLong_AsLong(bo) : 0));
}
static PyObject *py_is_mouse_clicked(PyObject *self, PyObject *args) {
    PyObject *bo = NULL;
    if (!PyArg_ParseTuple(args, "|O", &bo)) return NULL;
    return PyBool_FromLong(igIsMouseClicked_Bool(bo ? (int)PyLong_AsLong(bo) : 0, false));
}
static PyObject *py_is_key_down(PyObject *self, PyObject *args) {
    PyObject *ko;
    if (!PyArg_ParseTuple(args, "O", &ko)) return NULL;
    return PyBool_FromLong(igIsKeyDown_Nil((int)PyLong_AsLong(ko)));
}
static PyObject *py_is_key_pressed(PyObject *self, PyObject *args) {
    PyObject *ko;
    if (!PyArg_ParseTuple(args, "O", &ko)) return NULL;
    return PyBool_FromLong(igIsKeyPressed_Bool((int)PyLong_AsLong(ko), false));
}

/* --- 2D drawing ------------------------------------------------------- *
 * Free-form primitives on the current window's draw list, in absolute
 * screen coordinates (anchor with get_cursor_screen_pos()). Turns a window
 * into a canvas — the basis for charts, sprites or a small game.          */

static ImU32 col_u32(double r, double g, double b, double a) {
    ImVec4 c = {(float)r, (float)g, (float)b, (float)a};
    return igColorConvertFloat4ToU32(c);
}

static PyObject *py_get_cursor_screen_pos(PyObject *self, PyObject *args) {
    ImVec2 p = igGetCursorScreenPos();
    return tuple2(PyFloat_FromDouble(p.x), PyFloat_FromDouble(p.y));
}

static PyObject *py_draw_line(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, r, g, b, a = 1.0f, th = 1.0f;
    if (!PyArg_ParseTuple(args, "fffffff|ff", &x1, &y1, &x2, &y2, &r, &g, &b, &a, &th))
        return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2};
    ImDrawList_AddLine(igGetWindowDrawList(), p1, p2, col_u32(r, g, b, a), th);
    Py_RETURN_NONE;
}

static PyObject *py_draw_rect(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, r, g, b, a = 1.0f, th = 1.0f, rnd = 0.0f;
    if (!PyArg_ParseTuple(args, "fffffff|fff", &x1, &y1, &x2, &y2, &r, &g, &b, &a, &th, &rnd))
        return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2};
    ImDrawList_AddRect(igGetWindowDrawList(), p1, p2, col_u32(r, g, b, a), rnd, th, 0);
    Py_RETURN_NONE;
}

static PyObject *py_draw_rect_filled(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, r, g, b, a = 1.0f, rnd = 0.0f;
    if (!PyArg_ParseTuple(args, "fffffff|ff", &x1, &y1, &x2, &y2, &r, &g, &b, &a, &rnd))
        return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2};
    ImDrawList_AddRectFilled(igGetWindowDrawList(), p1, p2, col_u32(r, g, b, a), rnd, 0);
    Py_RETURN_NONE;
}

static PyObject *py_draw_circle(PyObject *self, PyObject *args) {
    float cx, cy, rad, r, g, b, a = 1.0f, th = 1.0f;
    if (!PyArg_ParseTuple(args, "ffffff|ff", &cx, &cy, &rad, &r, &g, &b, &a, &th))
        return NULL;
    ImVec2 c = {cx, cy};
    ImDrawList_AddCircle(igGetWindowDrawList(), c, rad, col_u32(r, g, b, a), 0, th);
    Py_RETURN_NONE;
}

static PyObject *py_draw_circle_filled(PyObject *self, PyObject *args) {
    float cx, cy, rad, r, g, b, a = 1.0f;
    if (!PyArg_ParseTuple(args, "ffffff|f", &cx, &cy, &rad, &r, &g, &b, &a)) return NULL;
    ImVec2 c = {cx, cy};
    ImDrawList_AddCircleFilled(igGetWindowDrawList(), c, rad, col_u32(r, g, b, a), 0);
    Py_RETURN_NONE;
}

static PyObject *py_draw_triangle_filled(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, x3, y3, r, g, b, a = 1.0f;
    if (!PyArg_ParseTuple(args, "fffffffff|f",
                          &x1, &y1, &x2, &y2, &x3, &y3, &r, &g, &b, &a)) return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2}, p3 = {x3, y3};
    ImDrawList_AddTriangleFilled(igGetWindowDrawList(), p1, p2, p3, col_u32(r, g, b, a));
    Py_RETURN_NONE;
}

static PyObject *py_draw_text(PyObject *self, PyObject *args) {
    float x, y, r, g, b, a = 1.0f;
    const char *s;
    if (!PyArg_ParseTuple(args, "fffffs|f", &x, &y, &r, &g, &b, &s, &a)) return NULL;
    ImVec2 pos = {x, y};
    ImDrawList_AddText_Vec2(igGetWindowDrawList(), pos, col_u32(r, g, b, a), s, NULL);
    Py_RETURN_NONE;
}

/* blit a texture at an absolute screen rectangle (sprite-style). */
static PyObject *py_draw_image(PyObject *self, PyObject *args) {
    int tex;
    float x1, y1, x2, y2;
    if (!PyArg_ParseTuple(args, "iffff", &tex, &x1, &y1, &x2, &y2)) return NULL;
    ImTextureRef ref = {NULL, (ImTextureID)tex};
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2};
    ImVec2 uv0 = {0.0f, 0.0f}, uv1 = {1.0f, 1.0f};
    ImDrawList_AddImage(igGetWindowDrawList(), ref, p1, p2, uv0, uv1, 0xFFFFFFFFu);
    Py_RETURN_NONE;
}

/* --- advanced 2D drawing ---------------------------------------------- */

/* read a flat [x0,y0,x1,y1,...] sequence into a malloc'd ImVec2 array. */
static ImVec2 *seq_to_points(PyObject *seq, int *count) {
    Py_ssize_t n = PySequence_Size(seq);
    if (n < 0) return NULL;
    int npts = (int)(n / 2);
    ImVec2 *pts = (ImVec2 *)malloc(sizeof(ImVec2) * (npts > 0 ? npts : 1));
    for (int i = 0; i < npts; i++) {
        PyObject *xo = PySequence_GetItem(seq, 2 * i);
        PyObject *yo = PySequence_GetItem(seq, 2 * i + 1);
        pts[i].x = (float)PyFloat_AsDouble(xo);
        pts[i].y = (float)PyFloat_AsDouble(yo);
        Py_XDECREF(xo);
        Py_XDECREF(yo);
    }
    *count = npts;
    return pts;
}

static PyObject *py_draw_triangle(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, x3, y3, r, g, b, a = 1.0f, th = 1.0f;
    if (!PyArg_ParseTuple(args, "fffffffff|ff",
                          &x1, &y1, &x2, &y2, &x3, &y3, &r, &g, &b, &a, &th)) return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2}, p3 = {x3, y3};
    ImDrawList_AddTriangle(igGetWindowDrawList(), p1, p2, p3, col_u32(r, g, b, a), th);
    Py_RETURN_NONE;
}

static PyObject *py_draw_quad(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, x3, y3, x4, y4, r, g, b, a = 1.0f, th = 1.0f;
    if (!PyArg_ParseTuple(args, "fffffffffff|ff",
                          &x1, &y1, &x2, &y2, &x3, &y3, &x4, &y4, &r, &g, &b, &a, &th))
        return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2}, p3 = {x3, y3}, p4 = {x4, y4};
    ImDrawList_AddQuad(igGetWindowDrawList(), p1, p2, p3, p4, col_u32(r, g, b, a), th);
    Py_RETURN_NONE;
}

static PyObject *py_draw_quad_filled(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, x3, y3, x4, y4, r, g, b, a = 1.0f;
    if (!PyArg_ParseTuple(args, "fffffffffff|f",
                          &x1, &y1, &x2, &y2, &x3, &y3, &x4, &y4, &r, &g, &b, &a))
        return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2}, p3 = {x3, y3}, p4 = {x4, y4};
    ImDrawList_AddQuadFilled(igGetWindowDrawList(), p1, p2, p3, p4, col_u32(r, g, b, a));
    Py_RETURN_NONE;
}

static PyObject *py_draw_ngon(PyObject *self, PyObject *args) {
    float cx, cy, rad, r, g, b, a = 1.0f, th = 1.0f;
    int seg;
    if (!PyArg_ParseTuple(args, "fffifff|ff", &cx, &cy, &rad, &seg, &r, &g, &b, &a, &th))
        return NULL;
    ImVec2 c = {cx, cy};
    ImDrawList_AddNgon(igGetWindowDrawList(), c, rad, col_u32(r, g, b, a), seg, th);
    Py_RETURN_NONE;
}

static PyObject *py_draw_ngon_filled(PyObject *self, PyObject *args) {
    float cx, cy, rad, r, g, b, a = 1.0f;
    int seg;
    if (!PyArg_ParseTuple(args, "fffifff|f", &cx, &cy, &rad, &seg, &r, &g, &b, &a)) return NULL;
    ImVec2 c = {cx, cy};
    ImDrawList_AddNgonFilled(igGetWindowDrawList(), c, rad, col_u32(r, g, b, a), seg);
    Py_RETURN_NONE;
}

static PyObject *py_draw_bezier_cubic(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, x3, y3, x4, y4, r, g, b, a = 1.0f, th = 2.0f;
    if (!PyArg_ParseTuple(args, "fffffffffff|ff",
                          &x1, &y1, &x2, &y2, &x3, &y3, &x4, &y4, &r, &g, &b, &a, &th))
        return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2}, p3 = {x3, y3}, p4 = {x4, y4};
    ImDrawList_AddBezierCubic(igGetWindowDrawList(), p1, p2, p3, p4, col_u32(r, g, b, a), th, 0);
    Py_RETURN_NONE;
}

static PyObject *py_draw_polyline(PyObject *self, PyObject *args) {
    PyObject *seq;
    float r, g, b, a = 1.0f, th = 1.0f;
    int closed = 0;
    if (!PyArg_ParseTuple(args, "Offf|ffp", &seq, &r, &g, &b, &a, &th, &closed)) return NULL;
    int n = 0;
    ImVec2 *pts = seq_to_points(seq, &n);
    if (!pts) return NULL;
    int flags = closed ? ImDrawFlags_Closed : 0;
    ImDrawList_AddPolyline(igGetWindowDrawList(), pts, n, col_u32(r, g, b, a), th, flags);
    free(pts);
    Py_RETURN_NONE;
}

static PyObject *py_draw_poly_filled(PyObject *self, PyObject *args) {
    PyObject *seq;
    float r, g, b, a = 1.0f;
    if (!PyArg_ParseTuple(args, "Offf|f", &seq, &r, &g, &b, &a)) return NULL;
    int n = 0;
    ImVec2 *pts = seq_to_points(seq, &n);
    if (!pts) return NULL;
    ImDrawList_AddConvexPolyFilled(igGetWindowDrawList(), pts, n, col_u32(r, g, b, a));
    free(pts);
    Py_RETURN_NONE;
}

/* vertical gradient rectangle (top color -> bottom color). */
static PyObject *py_draw_rect_gradient(PyObject *self, PyObject *args) {
    float x1, y1, x2, y2, r1, g1, b1, r2, g2, b2, a = 1.0f;
    if (!PyArg_ParseTuple(args, "ffffffffff|f",
                          &x1, &y1, &x2, &y2, &r1, &g1, &b1, &r2, &g2, &b2, &a)) return NULL;
    ImVec2 p1 = {x1, y1}, p2 = {x2, y2};
    ImU32 top = col_u32(r1, g1, b1, a), bot = col_u32(r2, g2, b2, a);
    ImDrawList_AddRectFilledMultiColor(igGetWindowDrawList(), p1, p2, top, top, bot, bot);
    Py_RETURN_NONE;
}

/* --- drag & drop ------------------------------------------------------ *
 * payload = a short string; demonstrates passing state between two widgets
 * across the C boundary. Wrap a source widget in begin/end_drag_drop_source
 * + set_drag_drop_payload, a target in begin/accept/end_drag_drop_target.   */

static PyObject *py_begin_drag_drop_source(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igBeginDragDropSource(0));
}
static PyObject *py_end_drag_drop_source(PyObject *self, PyObject *args) {
    igEndDragDropSource();
    Py_RETURN_NONE;
}
static PyObject *py_set_drag_drop_payload(PyObject *self, PyObject *args) {
    const char *type, *val;
    if (!PyArg_ParseTuple(args, "ss", &type, &val)) return NULL;
    igSetDragDropPayload(type, val, strlen(val) + 1, 0);
    Py_RETURN_NONE;
}
static PyObject *py_begin_drag_drop_target(PyObject *self, PyObject *args) {
    return PyBool_FromLong(igBeginDragDropTarget());
}
static PyObject *py_end_drag_drop_target(PyObject *self, PyObject *args) {
    igEndDragDropTarget();
    Py_RETURN_NONE;
}
static PyObject *py_accept_drag_drop_payload(PyObject *self, PyObject *args) {
    const char *type;
    if (!PyArg_ParseTuple(args, "s", &type)) return NULL;
    const ImGuiPayload *p = igAcceptDragDropPayload(type, 0);
    if (p && p->Data) return PyUnicode_FromString((const char *)p->Data);
    Py_RETURN_NONE;
}

/* --- tables (advanced) ------------------------------------------------ */

static PyObject *py_table_setup_scroll_freeze(PyObject *self, PyObject *args) {
    int cols, rows;
    if (!PyArg_ParseTuple(args, "ii", &cols, &rows)) return NULL;
    igTableSetupScrollFreeze(cols, rows);
    Py_RETURN_NONE;
}
static PyObject *py_table_set_bg_color(PyObject *self, PyObject *args) {
    int target;
    float r, g, b, a = 1.0f;
    int col = -1;
    if (!PyArg_ParseTuple(args, "ifff|fi", &target, &r, &g, &b, &a, &col)) return NULL;
    igTableSetBgColor(target, col_u32(r, g, b, a), col);
    Py_RETURN_NONE;
}
static PyObject *py_table_get_sort_column(PyObject *self, PyObject *args) {
    ImGuiTableSortSpecs *s = igTableGetSortSpecs();
    if (!s || s->SpecsCount <= 0) Py_RETURN_NONE;
    const ImGuiTableColumnSortSpecs *c = &s->Specs[0];
    PyObject *t = PyTuple_New(2);
    PyTuple_SetItem(t, 0, PyLong_FromLong(c->ColumnIndex));
    PyTuple_SetItem(t, 1, PyBool_FromLong(c->SortDirection == ImGuiSortDirection_Ascending));
    return t;
}

/* --- fonts (variable size) -------------------------------------------- */

static PyObject *py_push_font_size(PyObject *self, PyObject *args) {
    float size;
    if (!PyArg_ParseTuple(args, "f", &size)) return NULL;
    igPushFont(g_font, size);
    Py_RETURN_NONE;
}
static PyObject *py_pop_font(PyObject *self, PyObject *args) {
    igPopFont();
    Py_RETURN_NONE;
}

/* --- ImPlot (interactive charts; a 2nd C++ lib bolted on the same way) - */

/* default-constructed spec (Stride=IMPLOT_AUTO etc.); cached after 1st use. */
static ImPlotSpec g_implot_spec;
static int g_spec_init = 0;
static ImPlotSpec implot_default_spec(void) {
    if (!g_spec_init) {
        ImPlotSpec *sp = ImPlotSpec_ImPlotSpec();
        g_implot_spec = *sp;
        ImPlotSpec_destroy(sp);
        g_spec_init = 1;
    }
    return g_implot_spec;
}

/* pull a malloc'd float array out of a Python sequence (caller frees). */
static float *seq_to_float_array(PyObject *seq, int *count) {
    Py_ssize_t n = PySequence_Size(seq);
    if (n < 0) return NULL;
    float *v = (float *)malloc(sizeof(float) * (n > 0 ? n : 1));
    if (!seq_to_floats(seq, v, (int)n)) { free(v); return NULL; }
    *count = (int)n;
    return v;
}

static PyObject *py_begin_plot(PyObject *self, PyObject *args) {
    const char *title;
    float w = -1.0f, h = 0.0f;
    if (!PyArg_ParseTuple(args, "s|ff", &title, &w, &h)) return NULL;
    ImVec2 size = {w, h};
    return PyBool_FromLong(ImPlot_BeginPlot(title, size, 0));
}
static PyObject *py_end_plot(PyObject *self, PyObject *args) {
    ImPlot_EndPlot();
    Py_RETURN_NONE;
}
static PyObject *py_setup_axes(PyObject *self, PyObject *args) {
    const char *x, *y;
    if (!PyArg_ParseTuple(args, "ss", &x, &y)) return NULL;
    ImPlot_SetupAxes(x, y, 0, 0);
    Py_RETURN_NONE;
}
static PyObject *py_plot_line(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    if (!PyArg_ParseTuple(args, "sO", &label, &seq)) return NULL;
    int n = 0;
    float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlot_PlotLine_FloatPtrInt(label, v, n, 1.0, 0.0, implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_bars(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    double bs = 0.67;
    if (!PyArg_ParseTuple(args, "sO|d", &label, &seq, &bs)) return NULL;
    int n = 0;
    float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlot_PlotBars_FloatPtrInt(label, v, n, bs, 0.0, implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_scatter(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    if (!PyArg_ParseTuple(args, "sO", &label, &seq)) return NULL;
    int n = 0;
    float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlot_PlotScatter_FloatPtrInt(label, v, n, 1.0, 0.0, implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_shaded(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    double yr = 0.0;
    if (!PyArg_ParseTuple(args, "sO|d", &label, &seq, &yr)) return NULL;
    int n = 0;
    float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlot_PlotShaded_FloatPtrInt(label, v, n, yr, 1.0, 0.0, implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}

/* plot a curve whose every point comes from a Python function: ImPlot calls
 * the C getter per index, which calls back into Python — deep C<->Python
 * re-entrancy (frame -> draw() -> here -> ImPlot -> getter -> Python). */
static PyObject *g_plot_getter = NULL;
static ImPlotPoint_c plot_getter_cb(int idx, void *user_data) {
    (void)user_data;
    ImPlotPoint_c p = {(double)idx, 0.0};
    if (!g_plot_getter) return p;
    PyObject *args = PyTuple_New(1);
    PyTuple_SetItem(args, 0, PyLong_FromLong(idx));
    PyObject *r = PyObject_CallObject(g_plot_getter, args);
    Py_DECREF(args);
    if (!r) { PyErr_Print(); return p; }
    PyObject *xo = PySequence_GetItem(r, 0);
    PyObject *yo = PySequence_GetItem(r, 1);
    if (xo && yo) { p.x = PyFloat_AsDouble(xo); p.y = PyFloat_AsDouble(yo); }
    else PyErr_Clear();
    Py_XDECREF(xo);
    Py_XDECREF(yo);
    Py_DECREF(r);
    return p;
}
static PyObject *py_plot_line_callback(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *fn;
    int count;
    if (!PyArg_ParseTuple(args, "sOi", &label, &fn, &count)) return NULL;
    g_plot_getter = fn;
    ImPlot_PlotLineG(label, plot_getter_cb, NULL, count, implot_default_spec());
    g_plot_getter = NULL;
    Py_RETURN_NONE;
}

static PyObject *py_plot_stairs(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    if (!PyArg_ParseTuple(args, "sO", &label, &seq)) return NULL;
    int n = 0; float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlot_PlotStairs_FloatPtrInt(label, v, n, 1.0, 0.0, implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_stems(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    if (!PyArg_ParseTuple(args, "sO", &label, &seq)) return NULL;
    int n = 0; float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlot_PlotStems_FloatPtrInt(label, v, n, 0.0, 1.0, 0.0, implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_inf_lines(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    if (!PyArg_ParseTuple(args, "sO", &label, &seq)) return NULL;
    int n = 0; float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlot_PlotInfLines_FloatPtr(label, v, n, implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_hist(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    int bins = -2 /* Sturges */;
    if (!PyArg_ParseTuple(args, "sO|i", &label, &seq, &bins)) return NULL;
    int n = 0; float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlotRange_c range = {0.0, 0.0};   /* auto range */
    ImPlot_PlotHistogram_FloatPtr(label, v, n, bins, 1.0, range, implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_error_bars(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *xs, *ys, *es;
    if (!PyArg_ParseTuple(args, "sOOO", &label, &xs, &ys, &es)) return NULL;
    int nx = 0, ny = 0, ne = 0;
    float *x = seq_to_float_array(xs, &nx);
    float *y = seq_to_float_array(ys, &ny);
    float *e = seq_to_float_array(es, &ne);
    int ok = (x && y && e);
    if (ok) {
        int n = nx < ny ? (nx < ne ? nx : ne) : (ny < ne ? ny : ne);
        ImPlot_PlotErrorBars_FloatPtrFloatPtrFloatPtrInt(label, x, y, e, n,
                                                         implot_default_spec());
    }
    free(x); free(y); free(e);
    if (!ok) return NULL;
    Py_RETURN_NONE;
}
static PyObject *py_plot_pie_chart(PyObject *self, PyObject *args) {
    PyObject *labels, *seq;
    double x, y, radius;
    if (!PyArg_ParseTuple(args, "OOddd", &labels, &seq, &x, &y, &radius)) return NULL;
    int n = 0; float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    Py_ssize_t nl = PySequence_Size(labels);
    if (nl < n) n = (int)nl;
    const char **arr = (const char **)malloc(sizeof(char *) * (n > 0 ? n : 1));
    PyObject **keep = (PyObject **)malloc(sizeof(PyObject *) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        keep[i] = PySequence_GetItem(labels, i);
        arr[i] = keep[i] ? PyUnicode_AsUTF8(keep[i]) : "";
    }
    ImPlot_PlotPieChart_FloatPtrStr(arr, v, n, x, y, radius, "%.0f", 90.0,
                                    implot_default_spec());
    for (int i = 0; i < n; i++) Py_XDECREF(keep[i]);
    free(arr); free(keep); free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_heatmap(PyObject *self, PyObject *args) {
    const char *label;
    PyObject *seq;
    int rows, cols;
    double mn = 0.0, mx = 0.0;
    if (!PyArg_ParseTuple(args, "sOii|dd", &label, &seq, &rows, &cols, &mn, &mx)) return NULL;
    int n = 0; float *v = seq_to_float_array(seq, &n);
    if (!v) return NULL;
    ImPlotPoint_c bmin = {0.0, 0.0}, bmax = {1.0, 1.0};
    ImPlot_PlotHeatmap_FloatPtr(label, v, rows, cols, mn, mx, NULL, bmin, bmax,
                                implot_default_spec());
    free(v);
    Py_RETURN_NONE;
}
static PyObject *py_plot_text(PyObject *self, PyObject *args) {
    const char *text;
    double x, y;
    if (!PyArg_ParseTuple(args, "sdd", &text, &x, &y)) return NULL;
    ImVec2 off = {0.0f, 0.0f};
    ImPlot_PlotText(text, x, y, off, implot_default_spec());
    Py_RETURN_NONE;
}
static PyObject *py_plot_dummy(PyObject *self, PyObject *args) {
    const char *label;
    if (!PyArg_ParseTuple(args, "s", &label)) return NULL;
    ImPlot_PlotDummy(label, implot_default_spec());
    Py_RETURN_NONE;
}
static PyObject *py_set_next_axes_to_fit(PyObject *self, PyObject *args) {
    ImPlot_SetNextAxesToFit();
    Py_RETURN_NONE;
}
static PyObject *py_setup_axis_limits(PyObject *self, PyObject *args) {
    int axis;
    double vmin, vmax;
    if (!PyArg_ParseTuple(args, "idd", &axis, &vmin, &vmax)) return NULL;
    ImPlot_SetupAxisLimits(axis, vmin, vmax, ImPlotCond_Once);
    Py_RETURN_NONE;
}

/* interactive drag tools: the value is mutated by mouse in C and handed back
 * to Python every frame — a live bidirectional value across the bridge. */
static PyObject *py_drag_point(PyObject *self, PyObject *args) {
    int id;
    double x, y;
    float r, g, b;
    if (!PyArg_ParseTuple(args, "iddfff", &id, &x, &y, &r, &g, &b)) return NULL;
    ImVec4 col = {r, g, b, 1.0f};
    bool changed = ImPlot_DragPoint(id, &x, &y, col, 6.0f, 0, NULL, NULL, NULL);
    PyObject *t = PyTuple_New(3);
    PyTuple_SetItem(t, 0, PyBool_FromLong(changed));
    PyTuple_SetItem(t, 1, PyFloat_FromDouble(x));
    PyTuple_SetItem(t, 2, PyFloat_FromDouble(y));
    return t;
}
static PyObject *py_drag_line_x(PyObject *self, PyObject *args) {
    int id;
    double x;
    float r, g, b;
    if (!PyArg_ParseTuple(args, "idfff", &id, &x, &r, &g, &b)) return NULL;
    ImVec4 col = {r, g, b, 1.0f};
    bool changed = ImPlot_DragLineX(id, &x, col, 2.0f, 0, NULL, NULL, NULL);
    return tuple2(PyBool_FromLong(changed), PyFloat_FromDouble(x));
}
static PyObject *py_drag_line_y(PyObject *self, PyObject *args) {
    int id;
    double y;
    float r, g, b;
    if (!PyArg_ParseTuple(args, "idfff", &id, &y, &r, &g, &b)) return NULL;
    ImVec4 col = {r, g, b, 1.0f};
    bool changed = ImPlot_DragLineY(id, &y, col, 2.0f, 0, NULL, NULL, NULL);
    return tuple2(PyBool_FromLong(changed), PyFloat_FromDouble(y));
}

/* --- more widgets / state / window ------------------------------------ */

static PyObject *py_begin_disabled(PyObject *self, PyObject *args) {
    PyObject *d = NULL;
    if (!PyArg_ParseTuple(args, "|O", &d)) return NULL;
    igBeginDisabled(d ? (PyObject_IsTrue(d) ? true : false) : true);
    Py_RETURN_NONE;
}
static PyObject *py_end_disabled(PyObject *self, PyObject *args) {
    igEndDisabled();
    Py_RETURN_NONE;
}

static PyObject *py_push_id(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igPushID_Str(s);
    Py_RETURN_NONE;
}
static PyObject *py_pop_id(PyObject *self, PyObject *args) {
    igPopID();
    Py_RETURN_NONE;
}

static PyObject *py_arrow_button(PyObject *self, PyObject *args) {
    const char *id;
    int dir;
    if (!PyArg_ParseTuple(args, "si", &id, &dir)) return NULL;
    return PyBool_FromLong(igArrowButton(id, dir));
}

static PyObject *py_color_button(PyObject *self, PyObject *args) {
    const char *id;
    float r, g, b, a = 1.0f, w = 0.0f, h = 0.0f;
    if (!PyArg_ParseTuple(args, "sfff|fff", &id, &r, &g, &b, &a, &w, &h)) return NULL;
    ImVec4 col = {r, g, b, a};
    ImVec2 size = {w, h};
    return PyBool_FromLong(igColorButton(id, col, 0, size));
}

static PyObject *py_list_box(PyObject *self, PyObject *args) {
    const char *label;
    int current;
    PyObject *items;
    if (!PyArg_ParseTuple(args, "siO", &label, &current, &items)) return NULL;
    int orig = current;
    Py_ssize_t n = PySequence_Size(items);
    if (n < 0) return NULL;
    ImVec2 size = {0.0f, 0.0f}, zero = {0.0f, 0.0f};
    if (igBeginListBox(label, size)) {
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *it = PySequence_GetItem(items, i);
            const char *s = PyUnicode_AsUTF8(it);
            if (s && igSelectable_Bool(s, (int)i == current, 0, zero)) current = (int)i;
            Py_DECREF(it);
        }
        igEndListBox();
    }
    return tuple2(PyBool_FromLong(current != orig), PyLong_FromLong(current));
}

/* --- time / sizes / cursor / scroll / focus --------------------------- */

static PyObject *py_get_time(PyObject *self, PyObject *args) {
    return PyFloat_FromDouble(igGetTime());
}
static PyObject *py_get_delta_time(PyObject *self, PyObject *args) {
    return PyFloat_FromDouble(igGetIO_Nil()->DeltaTime);
}
static PyObject *py_get_window_size(PyObject *self, PyObject *args) {
    ImVec2 s = igGetWindowSize();
    return tuple2(PyFloat_FromDouble(s.x), PyFloat_FromDouble(s.y));
}
static PyObject *py_get_window_pos(PyObject *self, PyObject *args) {
    ImVec2 p = igGetWindowPos();
    return tuple2(PyFloat_FromDouble(p.x), PyFloat_FromDouble(p.y));
}
static PyObject *py_get_display_size(PyObject *self, PyObject *args) {
    ImVec2 d = igGetIO_Nil()->DisplaySize;
    return tuple2(PyFloat_FromDouble(d.x), PyFloat_FromDouble(d.y));
}
static PyObject *py_calc_text_size(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    ImVec2 sz = igCalcTextSize(s, NULL, false, -1.0f);
    return tuple2(PyFloat_FromDouble(sz.x), PyFloat_FromDouble(sz.y));
}
static PyObject *py_get_cursor_pos(PyObject *self, PyObject *args) {
    ImVec2 p = igGetCursorPos();
    return tuple2(PyFloat_FromDouble(p.x), PyFloat_FromDouble(p.y));
}
static PyObject *py_set_cursor_pos(PyObject *self, PyObject *args) {
    float x, y;
    if (!PyArg_ParseTuple(args, "ff", &x, &y)) return NULL;
    ImVec2 p = {x, y};
    igSetCursorPos(p);
    Py_RETURN_NONE;
}
static PyObject *py_set_cursor_screen_pos(PyObject *self, PyObject *args) {
    float x, y;
    if (!PyArg_ParseTuple(args, "ff", &x, &y)) return NULL;
    ImVec2 p = {x, y};
    igSetCursorScreenPos(p);
    Py_RETURN_NONE;
}
static PyObject *py_set_item_tooltip(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igSetItemTooltip("%s", s);
    Py_RETURN_NONE;
}
static PyObject *py_set_scroll_here_y(PyObject *self, PyObject *args) {
    float ratio = 0.5f;
    if (!PyArg_ParseTuple(args, "|f", &ratio)) return NULL;
    igSetScrollHereY(ratio);
    Py_RETURN_NONE;
}
static PyObject *py_get_scroll_y(PyObject *self, PyObject *args) {
    return PyFloat_FromDouble(igGetScrollY());
}
static PyObject *py_get_scroll_max_y(PyObject *self, PyObject *args) {
    return PyFloat_FromDouble(igGetScrollMaxY());
}
static PyObject *py_set_keyboard_focus_here(PyObject *self, PyObject *args) {
    igSetKeyboardFocusHere(0);
    Py_RETURN_NONE;
}
static PyObject *py_set_item_default_focus(PyObject *self, PyObject *args) {
    igSetItemDefaultFocus();
    Py_RETURN_NONE;
}
static PyObject *py_align_text_to_frame_padding(PyObject *self, PyObject *args) {
    igAlignTextToFramePadding();
    Py_RETURN_NONE;
}
static PyObject *py_set_next_item_open(PyObject *self, PyObject *args) {
    int open = 1;
    if (!PyArg_ParseTuple(args, "|p", &open)) return NULL;
    igSetNextItemOpen(open ? true : false, 0);
    Py_RETURN_NONE;
}
static PyObject *py_get_frame_height(PyObject *self, PyObject *args) {
    return PyFloat_FromDouble(igGetFrameHeight());
}

/* --- clipboard / built-in windows ------------------------------------- */

static PyObject *py_get_clipboard_text(PyObject *self, PyObject *args) {
    const char *s = igGetClipboardText();
    return PyUnicode_FromString(s ? s : "");
}
static PyObject *py_set_clipboard_text(PyObject *self, PyObject *args) {
    const char *s;
    if (!PyArg_ParseTuple(args, "s", &s)) return NULL;
    igSetClipboardText(s);
    Py_RETURN_NONE;
}
static PyObject *py_show_demo_window(PyObject *self, PyObject *args) {
    igShowDemoWindow(NULL);    /* ImGui's own showcase of every widget */
    Py_RETURN_NONE;
}
static PyObject *py_show_metrics_window(PyObject *self, PyObject *args) {
    igShowMetricsWindow(NULL);
    Py_RETURN_NONE;
}

/* --- docking (docking-branch ImGui) ----------------------------------- */

static ImGuiID g_dock_root = 0;
static int g_dock_built = 0;

/* full-canvas dock space; windows can be dragged to dock into it. */
static PyObject *py_dockspace(PyObject *self, PyObject *args) {
    g_dock_root = igDockSpaceOverViewport(0, igGetMainViewport(), 0, NULL);
    return PyLong_FromLong((long)g_dock_root);
}

/* one-shot: split the dock space and pre-dock two named windows L/R. */
static PyObject *py_dock_two(PyObject *self, PyObject *args) {
    const char *ln, *rn;
    if (!PyArg_ParseTuple(args, "ss", &ln, &rn)) return NULL;
    if (g_dock_built || g_dock_root == 0) Py_RETURN_NONE;
    ImGuiID root = g_dock_root;
    igDockBuilderRemoveNode(root);
    igDockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(root, igGetMainViewport()->Size);
    ImGuiID left = 0, right = 0;
    igDockBuilderSplitNode(root, ImGuiDir_Left, 0.5f, &left, &right);
    igDockBuilderDockWindow(ln, left);
    igDockBuilderDockWindow(rn, right);
    igDockBuilderFinish(root);
    g_dock_built = 1;
    Py_RETURN_NONE;
}

static PyMethodDef imgui_methods[] = {
    {"run",                     py_run,                     METH_VARARGS, "run(draw): main loop, calls draw() each frame"},
    /* window / containers */
    {"begin",                   py_begin,                   METH_VARARGS, "begin(name, flags=0): open a window"},
    {"end",                     py_end,                     METH_NOARGS,  "end(): close the current window"},
    {"set_next_window_size",    py_set_next_window_size,    METH_VARARGS, "set_next_window_size(w, h)"},
    {"set_next_window_pos",     py_set_next_window_pos,     METH_VARARGS, "set_next_window_pos(x, y)"},
    {"begin_child",             py_begin_child,             METH_VARARGS, "begin_child(id, w=0, h=0, border=False) -> bool"},
    {"end_child",               py_end_child,               METH_NOARGS,  "end_child()"},
    /* text */
    {"text",                    py_text,                    METH_VARARGS, "text(s): a text line"},
    {"text_colored",            py_text_colored,            METH_VARARGS, "text_colored(r, g, b, s): colored text (0..1)"},
    {"text_wrapped",            py_text_wrapped,            METH_VARARGS, "text_wrapped(s): word-wrapped text"},
    {"text_disabled",           py_text_disabled,           METH_VARARGS, "text_disabled(s): dimmed text"},
    {"label_text",              py_label_text,              METH_VARARGS, "label_text(label, value): value + right-aligned label"},
    {"bullet_text",             py_bullet_text,             METH_VARARGS, "bullet_text(s): bulleted text line"},
    {"separator_text",          py_separator_text,          METH_VARARGS, "separator_text(label): labeled separator"},
    /* buttons & booleans */
    {"button",                  py_button,                  METH_VARARGS, "button(label) -> bool"},
    {"checkbox",                py_checkbox,                METH_VARARGS, "checkbox(label, state) -> (changed, state)"},
    {"radio_button",            py_radio_button,            METH_VARARGS, "radio_button(label, active) -> bool"},
    {"selectable",              py_selectable,              METH_VARARGS, "selectable(label, selected=False) -> bool"},
    /* sliders & drags */
    {"slider_float",            py_slider_float,            METH_VARARGS, "slider_float(label, value, vmin, vmax) -> (changed, value)"},
    {"slider_float2",           py_slider_float2,           METH_VARARGS, "slider_float2(label, (a,b), vmin, vmax) -> (changed, (a,b))"},
    {"slider_float3",           py_slider_float3,           METH_VARARGS, "slider_float3(label, (a,b,c), vmin, vmax) -> (changed, (a,b,c))"},
    {"slider_float4",           py_slider_float4,           METH_VARARGS, "slider_float4(label, (a,b,c,d), vmin, vmax) -> (changed, (...))"},
    {"slider_int",              py_slider_int,              METH_VARARGS, "slider_int(label, value, vmin, vmax) -> (changed, value)"},
    {"drag_float",              py_drag_float,              METH_VARARGS, "drag_float(label, value, speed=1.0, vmin=0.0, vmax=0.0) -> (changed, value)"},
    {"drag_float2",             py_drag_float2,             METH_VARARGS, "drag_float2(label, (a,b), speed=1.0, vmin=0, vmax=0) -> (changed, (a,b))"},
    {"drag_float3",             py_drag_float3,             METH_VARARGS, "drag_float3(label, (a,b,c), ...) -> (changed, (a,b,c))"},
    {"drag_float4",             py_drag_float4,             METH_VARARGS, "drag_float4(label, (a,b,c,d), ...) -> (changed, (...))"},
    {"drag_int",                py_drag_int,                METH_VARARGS, "drag_int(label, value, speed=1.0, vmin=0, vmax=0) -> (changed, value)"},
    /* inputs */
    {"input_text",              py_input_text,              METH_VARARGS, "input_text(label, value) -> (changed, value)"},
    {"input_text_multiline",    py_input_text_multiline,    METH_VARARGS, "input_text_multiline(label, value, height=0) -> (changed, value)"},
    {"input_float",             py_input_float,             METH_VARARGS, "input_float(label, value) -> (changed, value)"},
    {"input_float2",            py_input_float2,            METH_VARARGS, "input_float2(label, (a,b)) -> (changed, (a,b))"},
    {"input_float3",            py_input_float3,            METH_VARARGS, "input_float3(label, (a,b,c)) -> (changed, (a,b,c))"},
    {"input_float4",            py_input_float4,            METH_VARARGS, "input_float4(label, (a,b,c,d)) -> (changed, (...))"},
    {"input_int",               py_input_int,               METH_VARARGS, "input_int(label, value) -> (changed, value)"},
    {"input_int2",              py_input_int2,              METH_VARARGS, "input_int2(label, (a,b)) -> (changed, (a,b))"},
    {"input_int3",              py_input_int3,              METH_VARARGS, "input_int3(label, (a,b,c)) -> (changed, (a,b,c))"},
    {"input_int4",              py_input_int4,              METH_VARARGS, "input_int4(label, (a,b,c,d)) -> (changed, (...))"},
    /* selection / color */
    {"combo",                   py_combo,                   METH_VARARGS, "combo(label, current, items) -> (changed, current)"},
    {"color_edit3",             py_color_edit3,             METH_VARARGS, "color_edit3(label, r, g, b) -> (changed, r, g, b)"},
    {"color_edit4",             py_color_edit4,             METH_VARARGS, "color_edit4(label, r, g, b, a) -> (changed, r, g, b, a)"},
    {"color_picker3",           py_color_picker3,           METH_VARARGS, "color_picker3(label, r, g, b) -> (changed, r, g, b)"},
    {"color_picker4",           py_color_picker4,           METH_VARARGS, "color_picker4(label, r, g, b, a) -> (changed, r, g, b, a)"},
    {"begin_combo",             py_begin_combo,             METH_VARARGS, "begin_combo(label, preview, flags=0) -> bool (free custom content)"},
    {"end_combo",               py_end_combo,               METH_NOARGS,  "end_combo()"},
    {"tree_node_ex",            py_tree_node_ex,            METH_VARARGS, "tree_node_ex(label, flags=0) -> bool (use TREE_* flags)"},
    {"vslider_float",           py_vslider_float,           METH_VARARGS, "vslider_float(label, w, h, value, vmin, vmax, flags=0) -> (changed, value)"},
    {"vslider_int",             py_vslider_int,             METH_VARARGS, "vslider_int(label, w, h, value, vmin, vmax, flags=0) -> (changed, value)"},
    {"slider_angle",            py_slider_angle,            METH_VARARGS, "slider_angle(label, rad, deg_min=-360, deg_max=360, flags=0) -> (changed, rad)"},
    {"drag_float_range2",       py_drag_float_range2,       METH_VARARGS, "drag_float_range2(label, vmin, vmax, speed=1, lo=0, hi=0, flags=0) -> (changed, vmin, vmax)"},
    {"input_double",            py_input_double,            METH_VARARGS, "input_double(label, value, flags=0) -> (changed, value)"},
    /* menus */
    {"begin_main_menu_bar",     py_begin_main_menu_bar,     METH_NOARGS,  "begin_main_menu_bar() -> bool"},
    {"end_main_menu_bar",       py_end_main_menu_bar,       METH_NOARGS,  "end_main_menu_bar()"},
    {"begin_menu_bar",          py_begin_menu_bar,          METH_NOARGS,  "begin_menu_bar() -> bool (window needs WINDOW_MENU_BAR)"},
    {"end_menu_bar",            py_end_menu_bar,            METH_NOARGS,  "end_menu_bar()"},
    {"begin_menu",              py_begin_menu,              METH_VARARGS, "begin_menu(label, enabled=True) -> bool"},
    {"end_menu",                py_end_menu,                METH_NOARGS,  "end_menu()"},
    {"menu_item",               py_menu_item,               METH_VARARGS, "menu_item(label, shortcut=None, selected=False, enabled=True) -> bool"},
    /* tabs */
    {"begin_tab_bar",           py_begin_tab_bar,           METH_VARARGS, "begin_tab_bar(id) -> bool"},
    {"end_tab_bar",             py_end_tab_bar,             METH_NOARGS,  "end_tab_bar()"},
    {"begin_tab_item",          py_begin_tab_item,          METH_VARARGS, "begin_tab_item(label) -> bool"},
    {"end_tab_item",            py_end_tab_item,            METH_NOARGS,  "end_tab_item()"},
    /* popups */
    {"open_popup",              py_open_popup,              METH_VARARGS, "open_popup(id)"},
    {"begin_popup",             py_begin_popup,             METH_VARARGS, "begin_popup(id) -> bool"},
    {"begin_popup_modal",       py_begin_popup_modal,       METH_VARARGS, "begin_popup_modal(name) -> bool"},
    {"end_popup",               py_end_popup,               METH_NOARGS,  "end_popup()"},
    {"close_current_popup",     py_close_current_popup,     METH_NOARGS,  "close_current_popup()"},
    /* tables */
    {"begin_table",             py_begin_table,             METH_VARARGS, "begin_table(id, columns, flags=0) -> bool"},
    {"end_table",               py_end_table,               METH_NOARGS,  "end_table()"},
    {"table_next_row",          py_table_next_row,          METH_NOARGS,  "table_next_row()"},
    {"table_next_column",       py_table_next_column,       METH_NOARGS,  "table_next_column() -> bool"},
    {"table_set_column_index",  py_table_set_column_index,  METH_VARARGS, "table_set_column_index(n) -> bool"},
    {"table_setup_column",      py_table_setup_column,      METH_VARARGS, "table_setup_column(label, flags=0)"},
    {"table_headers_row",       py_table_headers_row,       METH_NOARGS,  "table_headers_row()"},
    /* layout & misc */
    {"progress_bar",            py_progress_bar,            METH_VARARGS, "progress_bar(fraction, overlay=None)"},
    {"separator",               py_separator,               METH_NOARGS,  "separator(): horizontal line"},
    {"same_line",               py_same_line,               METH_NOARGS,  "same_line(): keep next widget on the same line"},
    {"spacing",                 py_spacing,                 METH_NOARGS,  "spacing(): vertical spacing"},
    {"new_line",                py_new_line,                METH_NOARGS,  "new_line(): line break"},
    {"indent",                  py_indent,                  METH_NOARGS,  "indent(): indent following widgets"},
    {"unindent",                py_unindent,                METH_NOARGS,  "unindent(): undo indent()"},
    {"bullet",                  py_bullet,                  METH_NOARGS,  "bullet(): a bullet point"},
    {"begin_group",             py_begin_group,             METH_NOARGS,  "begin_group(): start a layout group"},
    {"end_group",               py_end_group,               METH_NOARGS,  "end_group(): end a layout group"},
    {"dummy",                   py_dummy,                   METH_VARARGS, "dummy(w, h): reserve empty space"},
    {"push_item_width",         py_push_item_width,         METH_VARARGS, "push_item_width(w): width of following widgets"},
    {"pop_item_width",          py_pop_item_width,          METH_NOARGS,  "pop_item_width(): undo push_item_width()"},
    {"get_content_region_avail",py_get_content_region_avail,METH_NOARGS,  "get_content_region_avail() -> (w, h)"},
    {"tree_node",               py_tree_node,               METH_VARARGS, "tree_node(label) -> bool (call tree_pop() if True)"},
    {"tree_pop",                py_tree_pop,                METH_NOARGS,  "tree_pop(): close a tree_node() that returned True"},
    {"collapsing_header",       py_collapsing_header,       METH_VARARGS, "collapsing_header(label) -> bool (True when open)"},
    /* queries & tooltips */
    {"is_item_hovered",         py_is_item_hovered,         METH_NOARGS,  "is_item_hovered() -> bool"},
    {"is_item_clicked",         py_is_item_clicked,         METH_NOARGS,  "is_item_clicked() -> bool (left button)"},
    {"is_item_active",          py_is_item_active,          METH_NOARGS,  "is_item_active() -> bool"},
    {"set_tooltip",             py_set_tooltip,             METH_VARARGS, "set_tooltip(s): tooltip on the last item when hovered"},
    {"begin_tooltip",           py_begin_tooltip,           METH_NOARGS,  "begin_tooltip() -> bool (custom tooltip content)"},
    {"end_tooltip",             py_end_tooltip,             METH_NOARGS,  "end_tooltip()"},
    /* plots */
    {"plot_lines",              py_plot_lines,              METH_VARARGS, "plot_lines(label, values, overlay=None, height=80)"},
    {"plot_histogram",          py_plot_histogram,          METH_VARARGS, "plot_histogram(label, values, overlay=None, height=80)"},
    {"get_framerate",           py_get_framerate,           METH_NOARGS,  "get_framerate() -> float (ImGui FPS estimate)"},
    /* theme & styling */
    {"set_theme",               py_set_theme,               METH_VARARGS, "set_theme('dark'|'light'|'classic')"},
    {"push_style_color",        py_push_style_color,        METH_VARARGS, "push_style_color(col, r, g, b, a=1.0): use a COL_* constant"},
    {"pop_style_color",         py_pop_style_color,         METH_VARARGS, "pop_style_color(count=1)"},
    {"push_style_var",          py_push_style_var,          METH_VARARGS, "push_style_var(var, value): a scalar STYLEVAR_* (rounding, alpha...)"},
    {"push_style_var2",         py_push_style_var2,         METH_VARARGS, "push_style_var2(var, x, y): a 2D STYLEVAR_* (padding, spacing...)"},
    {"pop_style_var",           py_pop_style_var,           METH_VARARGS, "pop_style_var(count=1)"},
    /* textures & images */
    {"create_texture",          py_create_texture,          METH_VARARGS, "create_texture(w, h, rgba_bytes) -> int (call from draw())"},
    {"update_texture",          py_update_texture,          METH_VARARGS, "update_texture(tex, w, h, rgba_bytes): re-upload pixels"},
    {"image",                   py_image,                   METH_VARARGS, "image(tex, w, h): draw a texture"},
    {"image_button",            py_image_button,            METH_VARARGS, "image_button(id, tex, w, h) -> bool"},
    {"load_image",              py_load_image,              METH_VARARGS, "load_image(png_or_jpeg_bytes) -> (tex, w, h)"},
    {"load_sound",              py_load_sound,              METH_VARARGS, "load_sound(wav_bytes) -> int (sound id)"},
    {"play_sound",              py_play_sound,              METH_VARARGS, "play_sound(id): play a loaded sound"},
    /* input */
    {"get_mouse_pos",           py_get_mouse_pos,           METH_NOARGS,  "get_mouse_pos() -> (x, y)"},
    {"get_mouse_delta",         py_get_mouse_delta,         METH_NOARGS,  "get_mouse_delta() -> (dx, dy) since last frame"},
    {"get_mouse_wheel",         py_get_mouse_wheel,         METH_NOARGS,  "get_mouse_wheel() -> float"},
    {"is_mouse_down",           py_is_mouse_down,           METH_VARARGS, "is_mouse_down(button=0) -> bool"},
    {"is_mouse_clicked",        py_is_mouse_clicked,        METH_VARARGS, "is_mouse_clicked(button=0) -> bool"},
    {"is_key_down",             py_is_key_down,             METH_VARARGS, "is_key_down(key) -> bool (use a KEY_* constant)"},
    {"is_key_pressed",          py_is_key_pressed,          METH_VARARGS, "is_key_pressed(key) -> bool (this frame)"},
    /* 2D drawing (current window draw list, absolute coords) */
    {"get_cursor_screen_pos",   py_get_cursor_screen_pos,   METH_NOARGS,  "get_cursor_screen_pos() -> (x, y): anchor for draw_*"},
    {"draw_line",               py_draw_line,               METH_VARARGS, "draw_line(x1,y1,x2,y2, r,g,b, a=1, thickness=1)"},
    {"draw_rect",               py_draw_rect,               METH_VARARGS, "draw_rect(x1,y1,x2,y2, r,g,b, a=1, thickness=1, rounding=0)"},
    {"draw_rect_filled",        py_draw_rect_filled,        METH_VARARGS, "draw_rect_filled(x1,y1,x2,y2, r,g,b, a=1, rounding=0)"},
    {"draw_circle",             py_draw_circle,             METH_VARARGS, "draw_circle(cx,cy,radius, r,g,b, a=1, thickness=1)"},
    {"draw_circle_filled",      py_draw_circle_filled,      METH_VARARGS, "draw_circle_filled(cx,cy,radius, r,g,b, a=1)"},
    {"draw_triangle_filled",    py_draw_triangle_filled,    METH_VARARGS, "draw_triangle_filled(x1,y1,x2,y2,x3,y3, r,g,b, a=1)"},
    {"draw_text",               py_draw_text,               METH_VARARGS, "draw_text(x,y, r,g,b, s, a=1)"},
    {"draw_image",              py_draw_image,              METH_VARARGS, "draw_image(tex, x1,y1,x2,y2): blit a texture (sprite) at a rect"},
    {"draw_triangle",           py_draw_triangle,           METH_VARARGS, "draw_triangle(x1,y1,x2,y2,x3,y3, r,g,b, a=1, thickness=1)"},
    {"draw_quad",               py_draw_quad,               METH_VARARGS, "draw_quad(x1,y1,...,x4,y4, r,g,b, a=1, thickness=1)"},
    {"draw_quad_filled",        py_draw_quad_filled,        METH_VARARGS, "draw_quad_filled(x1,y1,...,x4,y4, r,g,b, a=1)"},
    {"draw_ngon",               py_draw_ngon,               METH_VARARGS, "draw_ngon(cx,cy,radius,segments, r,g,b, a=1, thickness=1)"},
    {"draw_ngon_filled",        py_draw_ngon_filled,        METH_VARARGS, "draw_ngon_filled(cx,cy,radius,segments, r,g,b, a=1)"},
    {"draw_bezier_cubic",       py_draw_bezier_cubic,       METH_VARARGS, "draw_bezier_cubic(x1,y1,...,x4,y4, r,g,b, a=1, thickness=2)"},
    {"draw_polyline",           py_draw_polyline,           METH_VARARGS, "draw_polyline([x0,y0,x1,y1,...], r,g,b, a=1, thickness=1, closed=False)"},
    {"draw_poly_filled",        py_draw_poly_filled,        METH_VARARGS, "draw_poly_filled([x0,y0,...], r,g,b, a=1): convex polygon"},
    {"draw_rect_gradient",      py_draw_rect_gradient,      METH_VARARGS, "draw_rect_gradient(x1,y1,x2,y2, r1,g1,b1, r2,g2,b2, a=1): vertical gradient"},
    /* drag & drop */
    {"begin_drag_drop_source",  py_begin_drag_drop_source,  METH_NOARGS,  "begin_drag_drop_source() -> bool"},
    {"set_drag_drop_payload",   py_set_drag_drop_payload,   METH_VARARGS, "set_drag_drop_payload(type, value): a string payload"},
    {"end_drag_drop_source",    py_end_drag_drop_source,    METH_NOARGS,  "end_drag_drop_source()"},
    {"begin_drag_drop_target",  py_begin_drag_drop_target,  METH_NOARGS,  "begin_drag_drop_target() -> bool"},
    {"accept_drag_drop_payload",py_accept_drag_drop_payload,METH_VARARGS, "accept_drag_drop_payload(type) -> str | None"},
    {"end_drag_drop_target",    py_end_drag_drop_target,    METH_NOARGS,  "end_drag_drop_target()"},
    /* tables (advanced) */
    {"table_setup_scroll_freeze",py_table_setup_scroll_freeze,METH_VARARGS,"table_setup_scroll_freeze(cols, rows)"},
    {"table_set_bg_color",      py_table_set_bg_color,      METH_VARARGS, "table_set_bg_color(target, r,g,b, a=1, column=-1)"},
    {"table_get_sort_column",   py_table_get_sort_column,   METH_NOARGS,  "table_get_sort_column() -> (column, ascending) | None"},
    /* fonts */
    {"push_font_size",          py_push_font_size,          METH_VARARGS, "push_font_size(size): scale text (pair with pop_font)"},
    {"pop_font",                py_pop_font,                METH_NOARGS,  "pop_font()"},
    /* ImPlot — interactive charts */
    {"begin_plot",              py_begin_plot,              METH_VARARGS, "begin_plot(title, w=-1, h=0) -> bool (pair with end_plot)"},
    {"end_plot",                py_end_plot,                METH_NOARGS,  "end_plot()"},
    {"setup_axes",              py_setup_axes,              METH_VARARGS, "setup_axes(x_label, y_label)"},
    {"plot_line",               py_plot_line,               METH_VARARGS, "plot_line(label, values)"},
    {"plot_bars",               py_plot_bars,               METH_VARARGS, "plot_bars(label, values, bar_size=0.67)"},
    {"plot_scatter",            py_plot_scatter,            METH_VARARGS, "plot_scatter(label, values)"},
    {"plot_shaded",             py_plot_shaded,             METH_VARARGS, "plot_shaded(label, values, yref=0)"},
    {"plot_line_callback",      py_plot_line_callback,      METH_VARARGS, "plot_line_callback(label, func, count): func(i)->(x,y) called per point by C"},
    {"plot_stairs",             py_plot_stairs,             METH_VARARGS, "plot_stairs(label, values)"},
    {"plot_stems",              py_plot_stems,              METH_VARARGS, "plot_stems(label, values)"},
    {"plot_inf_lines",          py_plot_inf_lines,          METH_VARARGS, "plot_inf_lines(label, xs): vertical lines"},
    {"plot_hist",               py_plot_hist,               METH_VARARGS, "plot_hist(label, data, bins=-2): binned histogram"},
    {"plot_error_bars",         py_plot_error_bars,         METH_VARARGS, "plot_error_bars(label, xs, ys, err)"},
    {"plot_pie_chart",          py_plot_pie_chart,          METH_VARARGS, "plot_pie_chart(labels, values, x, y, radius)"},
    {"plot_heatmap",            py_plot_heatmap,            METH_VARARGS, "plot_heatmap(label, values, rows, cols, vmin=0, vmax=0)"},
    {"plot_text",               py_plot_text,               METH_VARARGS, "plot_text(text, x, y)"},
    {"plot_dummy",              py_plot_dummy,              METH_VARARGS, "plot_dummy(label): legend entry only"},
    {"set_next_axes_to_fit",    py_set_next_axes_to_fit,    METH_NOARGS,  "set_next_axes_to_fit(): autofit the next plot's axes"},
    {"setup_axis_limits",       py_setup_axis_limits,       METH_VARARGS, "setup_axis_limits(axis, vmin, vmax): use AXIS_X1/AXIS_Y1"},
    {"drag_point",              py_drag_point,              METH_VARARGS, "drag_point(id, x, y, r, g, b) -> (changed, x, y): draggable point"},
    {"drag_line_x",             py_drag_line_x,             METH_VARARGS, "drag_line_x(id, x, r, g, b) -> (changed, x): draggable vertical line"},
    {"drag_line_y",             py_drag_line_y,             METH_VARARGS, "drag_line_y(id, y, r, g, b) -> (changed, y): draggable horizontal line"},
    /* more widgets / state */
    {"begin_disabled",          py_begin_disabled,          METH_VARARGS, "begin_disabled(disabled=True): grey out the following widgets"},
    {"end_disabled",            py_end_disabled,            METH_NOARGS,  "end_disabled()"},
    {"push_id",                 py_push_id,                 METH_VARARGS, "push_id(s): unique id scope (for widgets in a loop)"},
    {"pop_id",                  py_pop_id,                  METH_NOARGS,  "pop_id()"},
    {"arrow_button",            py_arrow_button,            METH_VARARGS, "arrow_button(id, dir) -> bool (use a DIR_* constant)"},
    {"color_button",            py_color_button,            METH_VARARGS, "color_button(id, r,g,b, a=1, w=0, h=0) -> bool"},
    {"list_box",                py_list_box,                METH_VARARGS, "list_box(label, current, items) -> (changed, current)"},
    /* time / sizes / cursor / scroll / focus */
    {"get_time",                py_get_time,                METH_NOARGS,  "get_time() -> float (seconds since start)"},
    {"get_delta_time",          py_get_delta_time,          METH_NOARGS,  "get_delta_time() -> float (seconds since last frame)"},
    {"get_window_size",         py_get_window_size,         METH_NOARGS,  "get_window_size() -> (w, h)"},
    {"get_window_pos",          py_get_window_pos,          METH_NOARGS,  "get_window_pos() -> (x, y)"},
    {"get_display_size",        py_get_display_size,        METH_NOARGS,  "get_display_size() -> (w, h) of the whole canvas"},
    {"calc_text_size",          py_calc_text_size,          METH_VARARGS, "calc_text_size(s) -> (w, h)"},
    {"get_cursor_pos",          py_get_cursor_pos,          METH_NOARGS,  "get_cursor_pos() -> (x, y) window-local"},
    {"set_cursor_pos",          py_set_cursor_pos,          METH_VARARGS, "set_cursor_pos(x, y) window-local"},
    {"set_cursor_screen_pos",   py_set_cursor_screen_pos,   METH_VARARGS, "set_cursor_screen_pos(x, y) absolute"},
    {"set_item_tooltip",        py_set_item_tooltip,        METH_VARARGS, "set_item_tooltip(s): tooltip if the last item is hovered"},
    {"set_scroll_here_y",       py_set_scroll_here_y,       METH_VARARGS, "set_scroll_here_y(ratio=0.5): scroll to the current item"},
    {"get_scroll_y",            py_get_scroll_y,            METH_NOARGS,  "get_scroll_y() -> float"},
    {"get_scroll_max_y",        py_get_scroll_max_y,        METH_NOARGS,  "get_scroll_max_y() -> float"},
    {"set_keyboard_focus_here", py_set_keyboard_focus_here, METH_NOARGS,  "set_keyboard_focus_here(): focus the next widget"},
    {"set_item_default_focus",  py_set_item_default_focus,  METH_NOARGS,  "set_item_default_focus()"},
    {"align_text_to_frame_padding", py_align_text_to_frame_padding, METH_NOARGS, "align_text_to_frame_padding()"},
    {"set_next_item_open",      py_set_next_item_open,      METH_VARARGS, "set_next_item_open(open=True): pre-open next tree/header"},
    {"get_frame_height",        py_get_frame_height,        METH_NOARGS,  "get_frame_height() -> float"},
    /* clipboard / built-in windows */
    {"get_clipboard_text",      py_get_clipboard_text,      METH_NOARGS,  "get_clipboard_text() -> str"},
    {"set_clipboard_text",      py_set_clipboard_text,      METH_VARARGS, "set_clipboard_text(s)"},
    {"show_demo_window",        py_show_demo_window,        METH_NOARGS,  "show_demo_window(): ImGui's built-in showcase of everything"},
    {"show_metrics_window",     py_show_metrics_window,     METH_NOARGS,  "show_metrics_window(): ImGui debug/metrics window"},
    {"dockspace",               py_dockspace,               METH_NOARGS,  "dockspace(): full-canvas dock space (call once per frame, first)"},
    {"dock_two",                py_dock_two,                METH_VARARGS, "dock_two(left_window, right_window): pre-dock two windows side by side"},
    {NULL, NULL, 0, NULL}
};

/* expose a useful subset of ImGui flag constants (values from the C enums,
 * so they track whatever cimgui version is compiled in). */
static int imgui_exec(PyObject *m) {
    PyModule_AddIntConstant(m, "WINDOW_NO_TITLE_BAR",     ImGuiWindowFlags_NoTitleBar);
    PyModule_AddIntConstant(m, "WINDOW_NO_RESIZE",        ImGuiWindowFlags_NoResize);
    PyModule_AddIntConstant(m, "WINDOW_NO_MOVE",          ImGuiWindowFlags_NoMove);
    PyModule_AddIntConstant(m, "WINDOW_NO_SCROLLBAR",     ImGuiWindowFlags_NoScrollbar);
    PyModule_AddIntConstant(m, "WINDOW_NO_COLLAPSE",      ImGuiWindowFlags_NoCollapse);
    PyModule_AddIntConstant(m, "WINDOW_ALWAYS_AUTO_RESIZE", ImGuiWindowFlags_AlwaysAutoResize);
    PyModule_AddIntConstant(m, "WINDOW_NO_BACKGROUND",    ImGuiWindowFlags_NoBackground);
    PyModule_AddIntConstant(m, "WINDOW_MENU_BAR",         ImGuiWindowFlags_MenuBar);
    PyModule_AddIntConstant(m, "TABLE_BORDERS",           ImGuiTableFlags_Borders);
    PyModule_AddIntConstant(m, "TABLE_ROW_BG",            ImGuiTableFlags_RowBg);
    PyModule_AddIntConstant(m, "TABLE_RESIZABLE",         ImGuiTableFlags_Resizable);
    /* color slots for push_style_color */
    PyModule_AddIntConstant(m, "COL_TEXT",                ImGuiCol_Text);
    PyModule_AddIntConstant(m, "COL_TEXT_DISABLED",       ImGuiCol_TextDisabled);
    PyModule_AddIntConstant(m, "COL_WINDOW_BG",           ImGuiCol_WindowBg);
    PyModule_AddIntConstant(m, "COL_BORDER",              ImGuiCol_Border);
    PyModule_AddIntConstant(m, "COL_FRAME_BG",            ImGuiCol_FrameBg);
    PyModule_AddIntConstant(m, "COL_FRAME_BG_HOVERED",    ImGuiCol_FrameBgHovered);
    PyModule_AddIntConstant(m, "COL_FRAME_BG_ACTIVE",     ImGuiCol_FrameBgActive);
    PyModule_AddIntConstant(m, "COL_TITLE_BG",            ImGuiCol_TitleBg);
    PyModule_AddIntConstant(m, "COL_TITLE_BG_ACTIVE",     ImGuiCol_TitleBgActive);
    PyModule_AddIntConstant(m, "COL_BUTTON",              ImGuiCol_Button);
    PyModule_AddIntConstant(m, "COL_BUTTON_HOVERED",      ImGuiCol_ButtonHovered);
    PyModule_AddIntConstant(m, "COL_BUTTON_ACTIVE",       ImGuiCol_ButtonActive);
    PyModule_AddIntConstant(m, "COL_HEADER",              ImGuiCol_Header);
    PyModule_AddIntConstant(m, "COL_HEADER_HOVERED",      ImGuiCol_HeaderHovered);
    PyModule_AddIntConstant(m, "COL_HEADER_ACTIVE",       ImGuiCol_HeaderActive);
    PyModule_AddIntConstant(m, "COL_CHECK_MARK",          ImGuiCol_CheckMark);
    PyModule_AddIntConstant(m, "COL_SLIDER_GRAB",         ImGuiCol_SliderGrab);
    PyModule_AddIntConstant(m, "COL_PLOT_LINES",          ImGuiCol_PlotLines);
    PyModule_AddIntConstant(m, "COL_PLOT_HISTOGRAM",      ImGuiCol_PlotHistogram);
    /* style vars: scalar -> push_style_var, 2D -> push_style_var2 */
    PyModule_AddIntConstant(m, "STYLEVAR_ALPHA",            ImGuiStyleVar_Alpha);
    PyModule_AddIntConstant(m, "STYLEVAR_WINDOW_ROUNDING",  ImGuiStyleVar_WindowRounding);
    PyModule_AddIntConstant(m, "STYLEVAR_WINDOW_PADDING",   ImGuiStyleVar_WindowPadding);
    PyModule_AddIntConstant(m, "STYLEVAR_FRAME_ROUNDING",   ImGuiStyleVar_FrameRounding);
    PyModule_AddIntConstant(m, "STYLEVAR_FRAME_PADDING",    ImGuiStyleVar_FramePadding);
    PyModule_AddIntConstant(m, "STYLEVAR_FRAME_BORDER_SIZE",ImGuiStyleVar_FrameBorderSize);
    PyModule_AddIntConstant(m, "STYLEVAR_ITEM_SPACING",     ImGuiStyleVar_ItemSpacing);
    PyModule_AddIntConstant(m, "STYLEVAR_GRAB_ROUNDING",    ImGuiStyleVar_GrabRounding);
    /* mouse buttons + a useful subset of keys (for the is_mouse / is_key calls) */
    PyModule_AddIntConstant(m, "MOUSE_LEFT",   0);
    PyModule_AddIntConstant(m, "MOUSE_RIGHT",  1);
    PyModule_AddIntConstant(m, "MOUSE_MIDDLE", 2);
    PyModule_AddIntConstant(m, "KEY_LEFT",   ImGuiKey_LeftArrow);
    PyModule_AddIntConstant(m, "KEY_RIGHT",  ImGuiKey_RightArrow);
    PyModule_AddIntConstant(m, "KEY_UP",     ImGuiKey_UpArrow);
    PyModule_AddIntConstant(m, "KEY_DOWN",   ImGuiKey_DownArrow);
    PyModule_AddIntConstant(m, "KEY_SPACE",  ImGuiKey_Space);
    PyModule_AddIntConstant(m, "KEY_ENTER",  ImGuiKey_Enter);
    PyModule_AddIntConstant(m, "KEY_ESCAPE", ImGuiKey_Escape);
    PyModule_AddIntConstant(m, "KEY_A",      ImGuiKey_A);
    PyModule_AddIntConstant(m, "KEY_W",      ImGuiKey_W);
    PyModule_AddIntConstant(m, "KEY_S",      ImGuiKey_S);
    PyModule_AddIntConstant(m, "KEY_D",      ImGuiKey_D);
    /* directions (for arrow_button) */
    PyModule_AddIntConstant(m, "DIR_LEFT",  ImGuiDir_Left);
    PyModule_AddIntConstant(m, "DIR_RIGHT", ImGuiDir_Right);
    PyModule_AddIntConstant(m, "DIR_UP",    ImGuiDir_Up);
    PyModule_AddIntConstant(m, "DIR_DOWN",  ImGuiDir_Down);
    /* input_text flags */
    PyModule_AddIntConstant(m, "INPUT_CHARS_DECIMAL",     ImGuiInputTextFlags_CharsDecimal);
    PyModule_AddIntConstant(m, "INPUT_CHARS_HEXADECIMAL", ImGuiInputTextFlags_CharsHexadecimal);
    PyModule_AddIntConstant(m, "INPUT_CHARS_UPPERCASE",   ImGuiInputTextFlags_CharsUppercase);
    PyModule_AddIntConstant(m, "INPUT_CHARS_NO_BLANK",    ImGuiInputTextFlags_CharsNoBlank);
    PyModule_AddIntConstant(m, "INPUT_PASSWORD",          ImGuiInputTextFlags_Password);
    PyModule_AddIntConstant(m, "INPUT_READ_ONLY",         ImGuiInputTextFlags_ReadOnly);
    PyModule_AddIntConstant(m, "INPUT_ENTER_RETURNS_TRUE",ImGuiInputTextFlags_EnterReturnsTrue);
    PyModule_AddIntConstant(m, "INPUT_ALLOW_TAB_INPUT",   ImGuiInputTextFlags_AllowTabInput);
    PyModule_AddIntConstant(m, "INPUT_CTRL_ENTER_NEWLINE",ImGuiInputTextFlags_CtrlEnterForNewLine);
    /* slider / drag flags */
    PyModule_AddIntConstant(m, "SLIDER_LOGARITHMIC",   ImGuiSliderFlags_Logarithmic);
    PyModule_AddIntConstant(m, "SLIDER_NO_INPUT",      ImGuiSliderFlags_NoInput);
    PyModule_AddIntConstant(m, "SLIDER_ALWAYS_CLAMP",  ImGuiSliderFlags_AlwaysClamp);
    PyModule_AddIntConstant(m, "SLIDER_WRAP_AROUND",   ImGuiSliderFlags_WrapAround);
    PyModule_AddIntConstant(m, "SLIDER_NO_ROUND",      ImGuiSliderFlags_NoRoundToFormat);
    /* tree_node / collapsing_header flags */
    PyModule_AddIntConstant(m, "TREE_DEFAULT_OPEN",      ImGuiTreeNodeFlags_DefaultOpen);
    PyModule_AddIntConstant(m, "TREE_FRAMED",            ImGuiTreeNodeFlags_Framed);
    PyModule_AddIntConstant(m, "TREE_LEAF",              ImGuiTreeNodeFlags_Leaf);
    PyModule_AddIntConstant(m, "TREE_BULLET",            ImGuiTreeNodeFlags_Bullet);
    PyModule_AddIntConstant(m, "TREE_SELECTED",          ImGuiTreeNodeFlags_Selected);
    PyModule_AddIntConstant(m, "TREE_SPAN_FULL_WIDTH",   ImGuiTreeNodeFlags_SpanFullWidth);
    PyModule_AddIntConstant(m, "TREE_OPEN_ON_ARROW",     ImGuiTreeNodeFlags_OpenOnArrow);
    PyModule_AddIntConstant(m, "TREE_OPEN_ON_DOUBLE_CLICK", ImGuiTreeNodeFlags_OpenOnDoubleClick);
    /* selectable flags */
    PyModule_AddIntConstant(m, "SELECTABLE_SPAN_ALL_COLUMNS", ImGuiSelectableFlags_SpanAllColumns);
    PyModule_AddIntConstant(m, "SELECTABLE_ALLOW_DOUBLE_CLICK", ImGuiSelectableFlags_AllowDoubleClick);
    PyModule_AddIntConstant(m, "SELECTABLE_DISABLED",     ImGuiSelectableFlags_Disabled);
    /* tab item flags */
    PyModule_AddIntConstant(m, "TAB_SET_SELECTED",     ImGuiTabItemFlags_SetSelected);
    PyModule_AddIntConstant(m, "TAB_UNSAVED_DOCUMENT", ImGuiTabItemFlags_UnsavedDocument);
    PyModule_AddIntConstant(m, "TAB_NO_CLOSE_BUTTON",  ImGuiTabItemFlags_NoCloseButton);
    PyModule_AddIntConstant(m, "TAB_LEADING",          ImGuiTabItemFlags_Leading);
    PyModule_AddIntConstant(m, "TAB_TRAILING",         ImGuiTabItemFlags_Trailing);
    /* color edit flags */
    PyModule_AddIntConstant(m, "COLOR_NO_ALPHA",    ImGuiColorEditFlags_NoAlpha);
    PyModule_AddIntConstant(m, "COLOR_NO_PICKER",   ImGuiColorEditFlags_NoPicker);
    PyModule_AddIntConstant(m, "COLOR_NO_INPUTS",   ImGuiColorEditFlags_NoInputs);
    PyModule_AddIntConstant(m, "COLOR_NO_LABEL",    ImGuiColorEditFlags_NoLabel);
    PyModule_AddIntConstant(m, "COLOR_ALPHA_BAR",   ImGuiColorEditFlags_AlphaBar);
    PyModule_AddIntConstant(m, "COLOR_FLOAT",       ImGuiColorEditFlags_Float);
    PyModule_AddIntConstant(m, "COLOR_DISPLAY_HSV", ImGuiColorEditFlags_DisplayHSV);
    PyModule_AddIntConstant(m, "COLOR_DISPLAY_RGB", ImGuiColorEditFlags_DisplayRGB);
    /* combo flags */
    PyModule_AddIntConstant(m, "COMBO_HEIGHT_SMALL",    ImGuiComboFlags_HeightSmall);
    PyModule_AddIntConstant(m, "COMBO_HEIGHT_LARGE",    ImGuiComboFlags_HeightLarge);
    PyModule_AddIntConstant(m, "COMBO_NO_ARROW_BUTTON", ImGuiComboFlags_NoArrowButton);
    PyModule_AddIntConstant(m, "COMBO_NO_PREVIEW",      ImGuiComboFlags_NoPreview);
    PyModule_AddIntConstant(m, "COMBO_POPUP_ALIGN_LEFT",ImGuiComboFlags_PopupAlignLeft);
    /* advanced table flags + bg-color targets */
    PyModule_AddIntConstant(m, "TABLE_SORTABLE",     ImGuiTableFlags_Sortable);
    PyModule_AddIntConstant(m, "TABLE_SCROLL_X",     ImGuiTableFlags_ScrollX);
    PyModule_AddIntConstant(m, "TABLE_SCROLL_Y",     ImGuiTableFlags_ScrollY);
    PyModule_AddIntConstant(m, "TABLE_REORDERABLE",  ImGuiTableFlags_Reorderable);
    PyModule_AddIntConstant(m, "TABLE_HIDEABLE",     ImGuiTableFlags_Hideable);
    PyModule_AddIntConstant(m, "TABLE_NO_BORDERS_IN_BODY", ImGuiTableFlags_NoBordersInBody);
    PyModule_AddIntConstant(m, "TABLE_SIZING_STRETCH_PROP", ImGuiTableFlags_SizingStretchProp);
    PyModule_AddIntConstant(m, "TABLE_SIZING_FIXED_FIT",    ImGuiTableFlags_SizingFixedFit);
    PyModule_AddIntConstant(m, "TABLE_BG_ROW0",  ImGuiTableBgTarget_RowBg0);
    PyModule_AddIntConstant(m, "TABLE_BG_ROW1",  ImGuiTableBgTarget_RowBg1);
    PyModule_AddIntConstant(m, "TABLE_BG_CELL",  ImGuiTableBgTarget_CellBg);
    /* ImPlot axes (for setup_axis_limits) */
    PyModule_AddIntConstant(m, "AXIS_X1", ImAxis_X1);
    PyModule_AddIntConstant(m, "AXIS_Y1", ImAxis_Y1);
    return 0;
}

static PyModuleDef_Slot imgui_slots[] = {
    {Py_mod_exec, imgui_exec},
    {0, NULL}
};

static struct PyModuleDef imgui_module = {
    PyModuleDef_HEAD_INIT, "_imgui", "wasthon Dear ImGui binding (cimgui)",
    0, imgui_methods, imgui_slots, NULL, NULL, NULL
};

/* multi-phase init (PEP 489) — the path wasthon's bridge supports */
PyMODINIT_FUNC PyInit__imgui(void) {
    return PyModuleDef_Init(&imgui_module);
}
