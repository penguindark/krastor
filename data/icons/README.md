# data/icons — UI icon sources

Monochrome SVG icons rasterized at runtime into a sprite atlas by
`src/icons.cpp` (LunaSVG, vendored as the `libs/lunasvg` git submodule).
The atlas is tinted at draw time, so the icons are rendered white-on-alpha
regardless of the `currentColor` used here.

| File | Lucide name | Used for |
|---|---|---|
| `send.svg` | send | Send message button |
| `paperclip.svg` | paperclip | Attach clipboard image button |
| `ear.svg` | ear | Listen (mic) button |
| `volume-2.svg` | volume-2 | Test voice button |
| `square.svg` | square | Stop button |

Source: <https://github.com/lucide-icons/lucide> (`icons/<name>.svg`),
fetched from branch `main` on 2026-10-29. License: ISC. See
`../LICENSES.md`.

To add an icon: drop a fill-or-stroke SVG here; `IconAtlas` picks up every
`*.svg` in this folder automatically (name = file stem).
