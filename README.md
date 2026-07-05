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

GitHub Pages via Actions (`.github/workflows/deploy.yml`): `build.sh` clones
[wasthon](https://github.com/fgallaire/wasthon), pins the upstream source trees and
builds the WASM artifacts (`_pygame.*`, `_imgui.*`) plus the runtime (`brython/`,
`pygame/`) from source at deploy time. No blobs are committed. Set once:
*Settings → Pages → Source = GitHub Actions*.

## License

Copyright (C) 2026 Florent Gallaire <fgallaire@gmail.com>

BSD 3-Clause License — same as Brython. See `LICENSE` for the full text.
