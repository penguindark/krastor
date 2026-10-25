# data/ — project assets

Development doc for this folder (see `../project.md`).

## Purpose
Runtime assets: fonts, images, icons (SVG), and later GLSL shaders.

## Current state (Phase 1)
Empty (`.gitkeep`). Assets arrive with later phases:
- fonts (monospaced font for the terminal I/O panel, Phase 5)
- `shaders/` — GLSL effect files for the game-loop surface (Phase 5)
- `icons/` — SVG icons for UI (Phase 5)

## Conventions
- English file names, lowercase, hyphen-separated (e.g. `jetbrains-mono.ttf`).
- Record the source and license of every third-party asset in a `LICENSES.md`
  here when it is added.
