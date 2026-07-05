/* wasthon-pygame.js — le vrai pygame-ce (C -> WASM) dans le navigateur, en 1 include.
 *
 * Usage (page eleve) :
 *   <canvas id="canvas" width="640" height="480"></canvas>
 *   <script src="wasthon-pygame.js"></script>
 *   <script type="text/x-pygame">
 *       import pygame
 *       from pygame.locals import *
 *       ...   # code pygame 100% standard
 *   </script>
 *
 * Ce script charge Brython + le module WASM pygame-ce, monte le package pygame
 * (le vrai __init__.py par-dessus les sous-modules C), puis execute le code de
 * la balise <script type="text/x-pygame">. Aucune plomberie cote eleve.
 */
(function () {
    "use strict";
    var SELF = document.currentScript;                 // capture synchrone (null en async)
    var BASE = new URL(".", SELF.src).href;            // dossier de ce script

    function log(msg, cls) {
        var el = document.getElementById("wp-log");
        if (el) {
            var s = document.createElement("span");
            s.style.color = cls === "err" ? "#f77" : cls === "ok" ? "#9f9" : "#9cf";
            s.textContent = (cls === "err" ? "❌ " : "· ") + msg + "\n";
            el.appendChild(s);
        }
        (cls === "err" ? console.error : console.log)("[wasthon-pygame]", msg);
    }
    window.__wp_log = log;

    function loadScript(src) {
        return new Promise(function (res, rej) {
            var s = document.createElement("script");
            s.src = src; s.onload = res; s.onerror = function () { rej(new Error("load " + src)); };
            document.head.appendChild(s);
        });
    }

    // Les sous-modules Python du package pygame (les modules C, eux, sont dans le WASM).
    var PY_MODS = ["_audio", "_camera_opencv", "_data_classes", "_debug", "_sprite",
        "camera", "colordict", "cursors", "freetype", "ftfont", "locals", "macosx",
        "midi", "pkgdata", "sndarray", "sprite", "surfarray", "sysfont", "typing", "version"];

    // Le transform `while True:` -> frame() (v1, niveau source).
    // Valide par equivalence executable (CPython 10/10, Brython 5/5) ;
    // tout doute = pas de transform (le code part tel quel).
    var TRANSFORM_PY = `
# The while-True -> frame() source transform (v1, loader-level).
# Pure ast + textual rewriting with positions. No ast.walk (Brython's is partial).
import ast


def _k(n):
    return type(n).__name__


def _children(n):
    """iter_child_nodes replacement: Brython's $B.ast nodes do not pass
    isinstance(x, ast.AST) (walk yields nothing there), but _fields and
    direct attribute access work — descend via _fields."""
    for f in (getattr(n, '_fields', None) or ()):
        v = getattr(n, f, None)
        if isinstance(v, list):
            for x in v:
                if hasattr(x, '_fields'):
                    yield x
        elif hasattr(v, '_fields'):
            yield v



def _bound_names(nodes):
    """Names BOUND anywhere in these statements, skipping nested function/class
    scopes (their bindings are theirs). Walrus inside comprehensions leaks to
    the enclosing scope (PEP 572) so comprehensions ARE walked. Over-collection
    is harmless here: the original context is module level, so a 'global x'
    for a read-only name is a no-op."""
    out = set()

    def expr(e):
        if e is None or _k(e) == 'Lambda':
            return
        if _k(e) == 'NamedExpr':
            out.add(e.target.id)
            expr(e.value)
            return
        for child in _children(e):
            if _k(child) == 'Lambda':
                continue
            expr(child)

    def target(t):
        if _k(t) == 'Name':
            out.add(t.id)
        elif _k(t) in ('Tuple', 'List'):
            for e in t.elts:
                target(e)
        elif _k(t) == 'Starred':
            target(t.value)
        # Attribute/Subscript targets bind nothing

    def pattern(p):
        if p is None:
            return
        if _k(p) == 'MatchAs':
            if p.name:
                out.add(p.name)
            pattern(p.pattern)
        elif _k(p) == 'MatchStar':
            if p.name:
                out.add(p.name)
        elif _k(p) == 'MatchMapping':
            if p.rest:
                out.add(p.rest)
            for sp in p.patterns:
                pattern(sp)
        elif _k(p) in ('MatchSequence', 'MatchOr'):
            for sp in p.patterns:
                pattern(sp)
        elif _k(p) == 'MatchClass':
            for sp in p.patterns:
                pattern(sp)
            for sp in p.kwd_patterns:
                pattern(sp)

    def stmt(s):
        if _k(s) in ('FunctionDef', 'AsyncFunctionDef'):
            out.add(s.name)          # the def name itself binds
            for d in s.decorator_list:
                expr(d)
            for dflt in list(s.args.defaults) + [d for d in s.args.kw_defaults if d]:
                expr(dflt)
            return                    # body = nested scope: skip
        if _k(s) == 'ClassDef':
            out.add(s.name)
            for d in s.decorator_list:
                expr(d)
            for b in s.bases:
                expr(b)
            return
        if _k(s) == 'Assign':
            for t in s.targets:
                target(t)
            expr(s.value)
        elif _k(s) == 'AugAssign':
            target(s.target)
            expr(s.value)
        elif _k(s) == 'AnnAssign':
            if s.value is not None:
                target(s.target)
            expr(s.value)
        elif _k(s) in ('For', 'AsyncFor'):
            target(s.target)
            expr(s.iter)
            for b in s.body + s.orelse:
                stmt(b)
        elif _k(s) == 'While':
            expr(s.test)
            for b in s.body + s.orelse:
                stmt(b)
        elif _k(s) == 'If':
            expr(s.test)
            for b in s.body + s.orelse:
                stmt(b)
        elif _k(s) in ('With', 'AsyncWith'):
            for item in s.items:
                expr(item.context_expr)
                if item.optional_vars is not None:
                    target(item.optional_vars)
            for b in s.body:
                stmt(b)
        elif _k(s) == 'Try':
            for b in s.body + s.orelse + s.finalbody:
                stmt(b)
            for h in s.handlers:
                # except-as names are NOT collected: CPython deletes them at
                # handler exit anyway (same observability as a frame-local),
                # and Brython's 'global x' + 'except E as x' fails to bind
                for b in h.body:
                    stmt(b)
        elif _k(s) == 'Match':
            expr(s.subject)
            for c in s.cases:
                pattern(c.pattern)
                if c.guard is not None:
                    expr(c.guard)
                for b in c.body:
                    stmt(b)
        elif _k(s) == 'Import':
            for a in s.names:
                out.add((a.asname or a.name).split('.')[0])
        elif _k(s) == 'ImportFrom':
            for a in s.names:
                if a.name != '*':
                    out.add(a.asname or a.name)
        elif _k(s) == 'Delete':
            for t in s.targets:
                target(t)
        elif _k(s) == 'Expr':
            expr(s.value)
        elif _k(s) == 'Return':
            expr(s.value)
        elif _k(s) == 'Raise':
            expr(s.exc)
            expr(s.cause)
        elif _k(s) == 'Assert':
            expr(s.test)
            expr(s.msg)
        # Pass/Break/Continue/Global/Nonlocal: nothing

    for s in nodes:
        stmt(s)
    return out


def _loop_breaks(nodes):
    """(lineno, col, kind) of break/continue belonging to THE game loop:
    walk statements but do not descend into nested loops (their break/continue
    are theirs) nor nested function/class scopes."""
    hits = []

    def stmt(s):
        if _k(s) in ('Break', 'Continue'):
            hits.append((s.lineno, s.col_offset,
                         'break' if _k(s) == 'Break' else 'continue'))
        elif _k(s) == 'If':
            for b in s.body + s.orelse:
                stmt(b)
        elif _k(s) in ('With', 'AsyncWith'):
            for b in s.body:
                stmt(b)
        elif _k(s) == 'Try':
            for b in s.body + s.orelse + s.finalbody:
                stmt(b)
            for h in s.handlers:
                for b in h.body:
                    stmt(b)
        elif _k(s) == 'Match':
            for c in s.cases:
                for b in c.body:
                    stmt(b)
        # For/While/nested defs: their break/continue stay

    for s in nodes:
        stmt(s)
    return hits


def _has_display_flip(nodes):
    """Does the body (at any depth, nested scopes included: a helper called
    from the loop counts) contain a  <something>.display.flip()  or
    <something>.display.update()  call?"""
    found = []

    def visit(n):
        if _k(n) == 'Call':
            f = n.func
            if (_k(f) == 'Attribute' and f.attr in ('flip', 'update')
                    and _k(f.value) == 'Attribute'
                    and f.value.attr == 'display'):
                found.append(True)
        for child in _children(n):
            visit(child)

    for s in nodes:
        visit(s)
    return bool(found)


def _clock_tick_fps(nodes):
    """If the body contains  <x>.tick(N)  with a constant int N, return N."""
    fps = []

    def visit(n):
        if (_k(n) == 'Call' and _k(n.func) == 'Attribute'
                and n.func.attr in ('tick', 'tick_busy_loop') and n.args
                and _k(n.args[0]) == 'Constant'
                and isinstance(n.args[0].value, int)):
            fps.append(n.args[0].value)
        for child in _children(n):
            visit(child)

    for s in nodes:
        visit(s)
    return fps[0] if fps else None


def transform(src):
    """Return (new_src, info) — new_src is src unchanged when no transform
    applies. Never raises: any doubt means 'no transform'."""
    try:
        tree = ast.parse(src)
    except SyntaxError:
        return src, 'no-transform: syntax error (left to the normal path)'

    candidates = [(i, s) for i, s in enumerate(tree.body)
                  if _k(s) == 'While' and _has_display_flip(s.body)]
    if not candidates:
        return src, 'no-transform: no top-level game loop found'
    if len(candidates) > 1:
        return src, 'no-transform: several top-level game loops (ambiguous)'
    idx, loop = candidates[0]
    if loop.orelse:
        return src, 'no-transform: while...else'
    if loop.test.end_lineno != loop.lineno:
        return src, 'no-transform: multi-line while header'
    if loop.body[0].lineno == loop.lineno:
        return src, 'no-transform: single-line while body'

    lines = src.split('\\n')

    # exact source of the condition
    cond = lines[loop.test.lineno - 1][loop.test.col_offset:loop.test.end_col_offset]
    is_true = _k(loop.test) == 'Constant' and loop.test.value is True

    body_indent = ' ' * loop.body[0].col_offset
    names = _bound_names(loop.body)
    tail = tree.body[idx + 1:]
    tail_names = _bound_names(tail)
    fps = _clock_tick_fps(loop.body)

    # rewrite break/continue of THIS loop, bottom-up so columns stay valid
    for ln, col, kind in sorted(_loop_breaks(loop.body), reverse=True):
        line = lines[ln - 1]
        word = 'break' if kind == 'break' else 'continue'
        assert line[col:col + len(word)] == word, (line, col, word)
        repl = 'return False' if kind == 'break' else 'return'
        lines[ln - 1] = line[:col] + repl + line[col + len(word):]

    # the while header becomes the frame def (+ global + cond check)
    header = ['def __wp_frame__():']
    if names:
        header.append(body_indent + 'global ' + ', '.join(sorted(names)))
    if not is_true:
        header.append(body_indent + 'if not (' + cond + '): return False')
    lines[loop.lineno - 1] = '\\n'.join(header)

    # the tail (statements after the loop) runs on clean stop
    tail_start = tail[0].lineno - 1 if tail else len(lines)
    body_end = loop.end_lineno            # last line of the loop body (1-based)
    tail_lines = lines[tail_start:] if tail else []
    lines = lines[:body_end]              # cut everything after the loop body
    lines.append('def __wp_after__():')
    if tail_names:
        lines.append('    global ' + ', '.join(sorted(tail_names)))
    if tail_lines:
        lines.extend('    ' + l for l in tail_lines)
    else:
        lines.append('    pass')
    lines.append('__wp_run_fn__(__wp_frame__, fps=%d, after=__wp_after__)'
                 % (fps if fps else 60))

    return '\\n'.join(lines), ('transformed: fps=%s, %d global(s), %d tail line(s)'
                              % (fps or 60, len(names), len(tail_lines)))
`;

    // Le loader Python (plomberie) : monte pygame puis exec le code eleve.
    // Definit window.__wp_run ; appele par le bootstrap une fois tout pret.
    var LOADER_PY = [
        "from browser import window",
        "def __wp_run():",
        "    import sys, types, importlib.util",
        "    _log = window.__wp_log",
        "    _src = dict(window.__wp_pysrc)",
        "    _pg = window.__wp_raw",
        "    # numpy absent : eviter le scan de finders (spam XHR) + surfarray=MissingModule",
        "    sys.modules['numpy'] = None",
        "    _ilu_find = importlib.util.find_spec",
        "    def _find_spec(name, *a, **k):",
        "        if name == 'numpy' or name.startswith('numpy.'):",
        "            return None",
        "        return _ilu_find(name, *a, **k)",
        "    importlib.util.find_spec = _find_spec",
        "    def reg(sub):",
        "        full = 'pygame.' + sub",
        "        if full in sys.modules or sub not in _src:",
        "            return",
        "        mod = types.ModuleType(full)",
        "        mod.__name__ = full; mod.__package__ = 'pygame'; mod.__file__ = 'pygame/' + sub + '.py'",
        "        sys.modules[full] = mod",
        "        try:",
        "            exec(_src[sub], mod.__dict__); setattr(_pg, sub, mod)",
        "        except BaseException:",
        "            sys.modules.pop(full, None)",
        "    for _m in ('colordict', '_data_classes', 'pkgdata'):",
        "        reg(_m)",
        "    window.__wp_pyinit()",
        "    for _name in dir(_pg):",
        "        if type(getattr(_pg, _name)).__name__ == 'module':",
        "            sys.modules['pygame.' + _name] = getattr(_pg, _name)",
        "    for _m in ('version', 'locals', 'typing', 'sprite', '_sprite'):",
        "        reg(_m)",
        "    # sysfont : le vrai scanne les polices OS (absentes) -> stub minimal",
        "    _sf = types.ModuleType('pygame.sysfont'); _sf.__name__ = 'pygame.sysfont'; _sf.__package__ = 'pygame'",
        "    exec('def SysFont(name, size, bold=False, italic=False, constructor=None):\\n'",
        "         '    import pygame.font\\n'",
        "         '    return pygame.font.Font(None, size)\\n'",
        "         'def get_fonts():\\n    return []\\n'",
        "         'def match_font(name, bold=False, italic=False):\\n    return None\\n', _sf.__dict__)",
        "    sys.modules['pygame.sysfont'] = _sf; setattr(_pg, 'sysfont', _sf)",
        "    exec(window.__wp_initsrc, _pg.__dict__)",
        "    # pygame.run(frame) : extension wasthon pour la boucle de jeu. Un",
        "    # `while True` gelerait l'onglet (Brython compile en boucle JS) ; on",
        "    # pilote frame() par requestAnimationFrame. frame renvoyant False = stop.",
        "    # PAS-A-PAS FIXE : la vitesse du jeu = `fps` pas de logique par seconde",
        "    # QUEL QUE SOIT le vsync. Si l'ecran rend a 30 Hz (frame > 16,7 ms sur",
        "    # un Chromebook ARM : rAF saute un vsync sur deux), on rattrape avec 2",
        "    # pas par tick ; a 120 Hz on saute un tick sur deux. Sans ca, un jeu",
        "    # ecrit en pixels-par-frame va 2x trop lent (ou trop vite).",
        "    from browser import timer as _timer",
        "    def _wp_run(frame_fn, fps=60, after=None):",
        "        _st = {'on': True, 'last': None, 'acc': 0.0}",
        "        _step = 1000.0 / fps",
        "        def _tick(_t):",
        "            if not _st['on']:",
        "                return",
        "            if _st['last'] is None:",
        "                _steps = 1",
        "            else:",
        "                _st['acc'] += _t - _st['last']",
        "                if _st['acc'] > 4 * _step:",
        "                    _st['acc'] = 4 * _step   # onglet cache/pause : pas de rafale",
        "                _steps = int(_st['acc'] // _step)",
        "                if _steps > 3:",
        "                    _steps = 3               # au-dela, on ralentit vraiment",
        "            _st['last'] = _t",
        "            _st['acc'] -= _steps * _step",
        "            if _st['acc'] < 0:",
        "                _st['acc'] = 0.0",
        "            try:",
        "                for _i in range(_steps):",
        "                    _r = frame_fn()",
        "                    if _r is False:",
        "                        _st['on'] = False",
        "                        if after is not None:",
        "                            after()",
        "                        return",
        "            except SystemExit:",
        "                _st['on'] = False; return   # sys.exit() = arret PROPRE (queue sautee, comme desktop)",
        "            except BaseException:",
        "                import traceback; traceback.print_exc()",
        "                window.__wp_error(traceback.format_exc()); _st['on'] = False; return",
        "            _timer.request_animation_frame(_tick)",
        "        _timer.request_animation_frame(_tick)",
        "    _pg.run = _wp_run",
        "    # Clock non bloquante : le vrai Clock.tick fait un delai SDL (~16 ms)",
        "    # qui exploserait le budget vsync dans frame() ; ici le rAF EST",
        "    # l'horloge — tick() rend le dt reel sans bloquer.",
        "    import time as _wp_time",
        "    class _WPClock:",
        "        def __init__(self):",
        "            self._last = _wp_time.time(); self._dts = []",
        "        def tick(self, framerate=0):",
        "            _now = _wp_time.time(); _dt = (_now - self._last) * 1000.0",
        "            self._last = _now",
        "            self._dts.append(_dt)",
        "            if len(self._dts) > 10: self._dts.pop(0)",
        "            return int(_dt)",
        "        tick_busy_loop = tick",
        "        def get_time(self):",
        "            return int(self._dts[-1]) if self._dts else 0",
        "        get_rawtime = get_time",
        "        def get_fps(self):",
        "            if not self._dts: return 0.0",
        "            _avg = sum(self._dts) / len(self._dts)",
        "            return 1000.0 / _avg if _avg > 0 else 0.0",
        "    try:",
        "        _pg.time.Clock = _WPClock; _pg.Clock = _WPClock",
        "    except BaseException:",
        "        pass",
        "    # while True verbatim : transform source->source (module TRANSFORM_PY)",
        "    _tns = {}",
        "    exec(window.__wp_transform_py, _tns)",
        "    _student_src, _tinfo = _tns['transform'](window.__wp_student)",
        "    _log('boucle: ' + _tinfo, 'ok' if 'transformed' in _tinfo else 'step')",
        "    _log('pygame pret', 'ok')",
        "    try:",
        "        exec(_student_src, {'__name__': '__main__', '__wp_run_fn__': _wp_run})",
        "        _log('code eleve execute', 'ok')",
        "    except BaseException as _e:",
        "        import traceback",
        "        _log('erreur: ' + repr(_e), 'err')",
        "        _log(traceback.format_exc()[-1200:], 'err')",
        "        window.__wp_error(traceback.format_exc())",
        "window.__wp_run = __wp_run",
        "__wp_run()",                                   // auto-appel : les vars sont pretes
    ].join("\n");

    async function boot() {
        try {
            // pages sans banniere d'erreur eleve -> no-op (ne casse pas l'appel Python)
            if (typeof window.__wp_error !== "function") window.__wp_error = function () {};
            log("chargement de Brython + pygame-ce …");
            if (!window.brython) {
                log("  fetch brython.js …");
                await loadScript(BASE + "brython/brython.js");
                log("  fetch brython_stdlib.js …");
                await loadScript(BASE + "brython/brython_stdlib.js");
            }
            log("  Brython OK, WASM …");
            var canvas = document.getElementById("canvas");
            if (!canvas) {
                canvas = document.createElement("canvas");
                canvas.id = "canvas"; canvas.width = 640; canvas.height = 480; canvas.tabIndex = 0;
                document.body.appendChild(canvas);
            }
            var factory = (await import(BASE + "_pygame.mjs?v=" + Date.now())).default;
            log("  _pygame.mjs importe, instanciation …");
            var M = await factory({ canvas: canvas });
            log("  WASM instancie, wasthon_init …");
            M._wasthon_init();
            var rt = M.wasthon, $B = rt.$B;
            log("  wasthon_init OK, assets + .py …");

            var pygameMod;
            try { pygameMod = $B.module.$factory("pygame", ""); }
            catch (e) {
                pygameMod = $B.module.tp_new($B.module);
                $B.module.tp_init(pygameMod, "pygame", (rt._b_ && rt._b_.None) || $B.builtins.None);
            }
            pygameMod.__path__ = ["pygame"];
            pygameMod.__file__ = "pygame/__init__.py";
            pygameMod.$is_package = true;
            $B.imported["pygame"] = pygameMod;
            window.__wp_raw = pygameMod;

            M.FS.writeFile("/freesansbold.ttf",
                new Uint8Array(await (await fetch(BASE + "pygame/freesansbold.ttf")).arrayBuffer()));

            var pysrc = {};
            await Promise.all(PY_MODS.map(async function (n) {
                try { pysrc[n] = await (await fetch(BASE + "pygame/" + n + ".py")).text(); } catch (e) {}
            }));
            window.__wp_pysrc = pysrc;
            window.__wp_initsrc = await (await fetch(BASE + "pygame/__init__.py")).text();
            window.__wp_pyinit = function () { M._PyInit_base(); };

            var studentEl = document.querySelector(
                'script[type="text/x-pygame"], script[type="text/python-pygame"]');
            window.__wp_student = studentEl ? studentEl.textContent : "";
            window.__wp_transform_py = TRANSFORM_PY;

            // injecter le loader Python (definit __wp_run), lancer Brython, appeler __wp_run.
            log("  tout pret, execution du loader Python …");
            try { window.brython({ debug: 0 }); } catch (e) {}   // init runtime/options (aucun script text/python)
            // $B.run_script(script, src, name, url, run_loop) compile la source
            // et lance $B.loop() -> execute tout de suite. Pas besoin d'injecter
            // un <script> (fragile quand Brython est charge en async). On passe
            // un element script factice (jamais insere) : la boucle Brython fait
            // script.dispatchEvent('load') a la fin -> il faut un vrai objet DOM.
            $B.run_script(document.createElement("script"), LOADER_PY, "__wasthon_pygame__", BASE, true);
        } catch (e) {
            log("echec bootstrap: " + (e && e.message ? e.message : e), "err");
            console.error(e);
        }
    }
    boot();
})();
