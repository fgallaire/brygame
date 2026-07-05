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
        "    def _wp_run(frame_fn, fps=60):",
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
        "                        _st['on'] = False; return",
        "            except BaseException:",
        "                import traceback; traceback.print_exc()",
        "                window.__wp_error(traceback.format_exc()); _st['on'] = False; return",
        "            _timer.request_animation_frame(_tick)",
        "        _timer.request_animation_frame(_tick)",
        "    _pg.run = _wp_run",
        "    _log('pygame pret', 'ok')",
        "    try:",
        "        exec(window.__wp_student, {'__name__': '__main__'})",
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
