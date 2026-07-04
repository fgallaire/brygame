# Brygame

The real **pygame**, in your browser, with zero install.

The upstream C of **pygame-ce**, compiled to WebAssembly and running on
**[wasthon](https://github.com/fgallaire/wasthon)** (Brython + compiled C modules).
100% standard pygame code that runs in the browser — built for education
(Chromebooks, zero install).

## Demos

The hub is `index.html`. Locally:

```
python3 -m http.server
```

- **The product — 1 include**: a page is just `<script src="wasthon-pygame.js">` + standard pygame code.
- **Animated breakout**, **sprite perf bench**, **feature suite**, standard `import pygame`, runtime probe.
- **Dear ImGui** (bonus): a widget GUI driven from Python.

## Deployment

GitHub Pages via Actions (`.github/workflows/deploy.yml`) — no build step, the site
is already built (`_pygame.*`, `_imgui.*` and `brython/` are versioned). Set once:
*Settings → Pages → Source = GitHub Actions*.
