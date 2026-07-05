/* wasthon pygame POC — a minimal, *performant* pygame engine glue.
 *
 * Not the pure-Python-over-canvas emulation (that path is slow: per-pixel work
 * runs in interpreted Python). Here the engine is C on SDL2's accelerated 2D
 * renderer (SDL_Renderer, backed by WebGL under emscripten), driven from Python
 * through wasthon's C-API bridge — exactly the recipe proven by the ImGui
 * binding (gui-poc/_imgui.c). SDL2 is emscripten's port (-sUSE_SDL=2), shared
 * with that GUI work.
 *
 * The blocking `while True` game loop can't run in a browser (single-threaded,
 * no yield). So the loop is inverted like the ImGui binding: the game body is a
 * draw() callback and `run(draw)` drives it via emscripten_set_main_loop (rAF).
 * Feeding the browser one frame at a time. (Running unmodified while-True code
 * would need ASYNCIFY — a later step; even pygbag asks users to go async.)
 *
 * v0 surface: set_mode / fill / rect / circle / line / flip, an event queue
 * (QUIT/KEYDOWN/KEYUP) and key_pressed() polling. Enough for a real bouncing
 * game. The pygame-shaped Python facade (import pygame; Surface; draw.circle)
 * comes next; this proves the engine + loop + input on wasthon.
 */
#include <Python.h>                 /* -> wasthon.h (the bridge's C-API) */
#include <stdbool.h>
#include <math.h>
#include <emscripten.h>
#include <SDL.h>

/* --- pygame-compatible constants (real pygame values, for forward compat) --- */
#define PG_QUIT     256
#define PG_KEYDOWN  768
#define PG_KEYUP    769

static SDL_Window   *g_win  = NULL;
static SDL_Renderer *g_ren  = NULL;
static PyObject     *g_draw = NULL;     /* the Python draw() callback */
static int           g_w = 0, g_h = 0;

/* a tiny event ring buffer: frame() fills it from SDL, get_events() drains it */
#define EVQ_CAP 1024
static struct { int type; int key; } g_evq[EVQ_CAP];
static int g_ev_head = 0, g_ev_tail = 0;

static void ev_push(int type, int key) {
    int n = (g_ev_head + 1) % EVQ_CAP;
    if (n == g_ev_tail) return;         /* full: drop (queue overflow) */
    g_evq[g_ev_head].type = type;
    g_evq[g_ev_head].key  = key;
    g_ev_head = n;
}

/* one frame: pump SDL events into the queue, then call the Python draw() which
 * reads events, updates state and renders (fill/rect/.../flip). */
static void frame(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:    ev_push(PG_QUIT, 0); break;
            case SDL_KEYDOWN: ev_push(PG_KEYDOWN, ev.key.keysym.sym); break;
            case SDL_KEYUP:   ev_push(PG_KEYUP,   ev.key.keysym.sym); break;
            default: break;
        }
    }
    if (g_draw) {
        PyObject *r = PyObject_CallObject(g_draw, NULL);
        if (r) Py_DECREF(r); else PyErr_Print();
    }
}

/* --- display -------------------------------------------------------------- */

static PyObject *py_set_mode(PyObject *self, PyObject *args) {
    int w, h;
    if (!PyArg_ParseTuple(args, "ii", &w, &h)) return NULL;
    if (!g_win) {
        SDL_Init(SDL_INIT_VIDEO);
        g_win = SDL_CreateWindow("wasthon pygame",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_SHOWN);
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    } else {
        SDL_SetWindowSize(g_win, w, h);
    }
    g_w = w; g_h = h;
    Py_RETURN_NONE;
}

static PyObject *py_set_caption(PyObject *self, PyObject *args) {
    const char *title;
    if (!PyArg_ParseTuple(args, "s", &title)) return NULL;
    if (g_win) SDL_SetWindowTitle(g_win, title);
    Py_RETURN_NONE;
}

static PyObject *py_flip(PyObject *self, PyObject *args) {
    if (g_ren) SDL_RenderPresent(g_ren);
    Py_RETURN_NONE;
}

/* --- drawing (on the screen renderer) ------------------------------------- */

static PyObject *py_fill(PyObject *self, PyObject *args) {
    int r, g, b;
    if (!PyArg_ParseTuple(args, "iii", &r, &g, &b)) return NULL;
    if (g_ren) {
        SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
        SDL_RenderClear(g_ren);
    }
    Py_RETURN_NONE;
}

static PyObject *py_rect(PyObject *self, PyObject *args) {
    int r, g, b, x, y, w, h;
    if (!PyArg_ParseTuple(args, "iiiiiii", &r, &g, &b, &x, &y, &w, &h)) return NULL;
    if (g_ren) {
        SDL_Rect rc = {x, y, w, h};
        SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
        SDL_RenderFillRect(g_ren, &rc);
    }
    Py_RETURN_NONE;
}

static PyObject *py_circle(PyObject *self, PyObject *args) {
    int r, g, b, cx, cy, rad;
    if (!PyArg_ParseTuple(args, "iiiiii", &r, &g, &b, &cx, &cy, &rad)) return NULL;
    if (g_ren && rad > 0) {
        SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
        for (int dy = -rad; dy <= rad; dy++) {
            int dx = (int)(sqrt((double)(rad * rad - dy * dy)) + 0.5);
            SDL_RenderDrawLine(g_ren, cx - dx, cy + dy, cx + dx, cy + dy);
        }
    }
    Py_RETURN_NONE;
}

static PyObject *py_line(PyObject *self, PyObject *args) {
    int r, g, b, x1, y1, x2, y2, width = 1;
    if (!PyArg_ParseTuple(args, "iiiiiii|i", &r, &g, &b, &x1, &y1, &x2, &y2, &width))
        return NULL;
    if (g_ren) {
        SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
        for (int i = 0; i < width; i++)   /* fake thickness by stacking lines */
            SDL_RenderDrawLine(g_ren, x1, y1 + i, x2, y2 + i);
    }
    Py_RETURN_NONE;
}

/* --- events / input ------------------------------------------------------- */

static PyObject *py_get_events(PyObject *self, PyObject *args) {
    PyObject *list = PyList_New(0);
    while (g_ev_tail != g_ev_head) {
        PyObject *t = PyTuple_New(2);
        PyTuple_SetItem(t, 0, PyLong_FromLong(g_evq[g_ev_tail].type));
        PyTuple_SetItem(t, 1, PyLong_FromLong(g_evq[g_ev_tail].key));
        PyList_Append(list, t);
        Py_DECREF(t);
        g_ev_tail = (g_ev_tail + 1) % EVQ_CAP;
    }
    return list;
}

/* key_pressed(keycode) -> bool : live keyboard state (for smooth movement) */
static PyObject *py_key_pressed(PyObject *self, PyObject *args) {
    int keycode;
    if (!PyArg_ParseTuple(args, "i", &keycode)) return NULL;
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)keycode);
    return PyBool_FromLong(state && state[sc]);
}

/* --- main loop ------------------------------------------------------------ */

static PyObject *py_run(PyObject *self, PyObject *args) {
    PyObject *cb;
    if (!PyArg_ParseTuple(args, "O", &cb)) return NULL;
    Py_INCREF(cb);
    Py_XDECREF(g_draw);
    g_draw = cb;
    /* simulate_infinite_loop=0: return to Brython; loop runs via rAF */
    emscripten_set_main_loop(frame, 0, 0);
    Py_RETURN_NONE;
}

/* --- module --------------------------------------------------------------- */

static PyMethodDef pygame_methods[] = {
    {"set_mode",    py_set_mode,    METH_VARARGS, "set_mode(w, h): create the window/renderer"},
    {"set_caption", py_set_caption, METH_VARARGS, "set_caption(title)"},
    {"fill",        py_fill,        METH_VARARGS, "fill(r, g, b): clear the screen"},
    {"rect",        py_rect,        METH_VARARGS, "rect(r, g, b, x, y, w, h): filled rectangle"},
    {"circle",      py_circle,      METH_VARARGS, "circle(r, g, b, cx, cy, rad): filled circle"},
    {"line",        py_line,        METH_VARARGS, "line(r, g, b, x1, y1, x2, y2, width=1)"},
    {"flip",        py_flip,        METH_NOARGS,  "flip(): present the frame"},
    {"get_events",  py_get_events,  METH_NOARGS,  "get_events() -> [(type, key), ...]"},
    {"key_pressed", py_key_pressed, METH_VARARGS, "key_pressed(keycode) -> bool"},
    {"run",         py_run,         METH_VARARGS, "run(draw): main loop, calls draw() each frame"},
    {NULL, NULL, 0, NULL}
};

static int pygame_exec(PyObject *m) {
    PyModule_AddIntConstant(m, "QUIT",    PG_QUIT);
    PyModule_AddIntConstant(m, "KEYDOWN", PG_KEYDOWN);
    PyModule_AddIntConstant(m, "KEYUP",   PG_KEYUP);
    /* arrow / common keys (SDL keycodes = pygame K_* values) */
    PyModule_AddIntConstant(m, "K_LEFT",   SDLK_LEFT);
    PyModule_AddIntConstant(m, "K_RIGHT",  SDLK_RIGHT);
    PyModule_AddIntConstant(m, "K_UP",     SDLK_UP);
    PyModule_AddIntConstant(m, "K_DOWN",   SDLK_DOWN);
    PyModule_AddIntConstant(m, "K_SPACE",  SDLK_SPACE);
    PyModule_AddIntConstant(m, "K_ESCAPE", SDLK_ESCAPE);
    PyModule_AddIntConstant(m, "K_RETURN", SDLK_RETURN);
    /* letters a..z */
    for (int c = 0; c < 26; c++) {
        char name[8];
        snprintf(name, sizeof(name), "K_%c", 'a' + c);
        PyModule_AddIntConstant(m, name, SDLK_a + c);
    }
    return 0;
}

static PyModuleDef_Slot pygame_slots[] = {
    {Py_mod_exec, pygame_exec},
    {0, NULL}
};

static struct PyModuleDef pygame_module = {
    PyModuleDef_HEAD_INIT, "_pygame", "wasthon pygame engine (SDL2)",
    0, pygame_methods, pygame_slots, NULL, NULL, NULL
};

/* multi-phase init (PEP 489) — the path wasthon's bridge supports */
PyMODINIT_FUNC PyInit__pygame(void) {
    return PyModuleDef_Init(&pygame_module);
}
