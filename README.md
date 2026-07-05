# Brygame

The real **pygame**, in your browser, zero install.

**Live: <https://fgallaire.github.io/brygame/>**

Brygame runs the upstream C of [pygame-ce](https://github.com/pygame-community/pygame-ce)
compiled to WebAssembly — not a JavaScript rewrite, not a subset — on
[wasthon](https://github.com/fgallaire/wasthon)'s C-API bridge and the Brython
Python runtime. Standard pygame code, straight in a web page. Built for
education: it runs on anything with a browser, Chromebooks included.

## One include, one game

A page is a canvas, one script include, and your pygame code:

```html
<canvas id="canvas" width="720" height="480" tabindex="0"></canvas>
<script src="wasthon-pygame.js"></script>

<script type="text/x-pygame">
import pygame

pygame.init()
screen = pygame.display.set_mode((720, 480))
ball = pygame.Rect(100, 100, 24, 24)
v = [4, 3]

def frame():
    for e in pygame.event.get():
        pass
    ball.move_ip(v)
    if ball.left < 0 or ball.right > 720:  v[0] = -v[0]
    if ball.top < 0 or ball.bottom > 480:  v[1] = -v[1]
    screen.fill((16, 16, 24))
    pygame.draw.ellipse(screen, (240, 170, 70), ball)
    pygame.display.flip()

pygame.run(frame)   # the browser owns the event loop: run(frame) at 60 fps
</script>
```

The one departure from desktop pygame: the browser cannot be blocked, so the
`while True:` loop becomes `pygame.run(frame)` — everything inside the loop is
unchanged (`event.get()`, `Rect`, `draw`, `display.flip()`, sprites, fonts,
sound).

## Demos

All live on the [hub](https://fgallaire.github.io/brygame/), all served from
`loader/`:

- [Animated breakout](https://fgallaire.github.io/brygame/pygame-ce-breakout.html) —
  a full game at 60 fps: paddle, ball, bricks, keyboard, sound.
- [Standard pygame](https://fgallaire.github.io/brygame/pygame-ce-standard.html) —
  literal student code (`import pygame`, `set_mode`, `Rect`, …) executed as-is.
- [Feature suite](https://fgallaire.github.io/brygame/pygame-ce-suite.html) —
  40+ checks across surfaces, rects, events, images, fonts, sound.
- [Runtime probe](https://fgallaire.github.io/brygame/pygame-ce-probe.html) —
  boot diagnostics: module init, submodules, SDL2 driver, pixel round-trips.
- [Sprite perf bench](https://fgallaire.github.io/brygame/pygame-perf.html) and
  [API tests](https://fgallaire.github.io/brygame/pygame-tests.html).
- [Dear ImGui](https://fgallaire.github.io/brygame/imgui-demo.html) — bonus: a
  C++ widget GUI (cimgui + ImPlot) driven from Python through the same bridge.

Run them locally:

```
python3 -m http.server --directory loader
```

## Building from source

No blobs are committed. `./build.sh` rebuilds everything the site serves:
it clones [wasthon](https://github.com/fgallaire/wasthon) (the generic C-API
bridge and the Brython runtime), pins the upstream trees to known-good commits
(pygame-ce, cimgui, cimplot), compiles `_pygame.{mjs,wasm}` and
`_imgui.{mjs,wasm}` with Emscripten, and collects the artifacts into `loader/`.

```
src/pygame/   port recipe: build script + shims for pygame-ce
src/imgui/    port recipe: cimgui + ImPlot binding
loader/       the site: hub, demo pages, wasthon-pygame.js (+ built artifacts)
build.sh      orchestrator: clone wasthon@main, inject src/, collect to loader/
```

## License

Copyright (C) 2026 Florent Gallaire <fgallaire@gmail.com>

BSD 3-Clause License — same as Brython. See `LICENSE` for the full text.
