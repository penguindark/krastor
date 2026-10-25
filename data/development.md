# data/ — project assets

Development doc for this folder (see `../project.md`).

## Purpose
Runtime assets: fonts, images, icons (SVG), and later GLSL shaders.

## Current state (Phase 5, 2026-10-26)
- `fonts/jetbrains-mono.ttf` — JetBrains Mono variable TTF (OFL, see `LICENSES.md`).
  Loaded at runtime by `src/gui.cpp` as the UI/chat font (monospace + box-drawing
  glyphs for the terminal-style panel). Loaded from memory with a fallback to
  Nuklear's built-in font if the file is missing.
- Still to come: `shaders/` (GLSL effect files — currently embedded in gui.cpp),
  `icons/` (SVG icons for UI).

## Conventions
- English file names, lowercase, hyphen-separated (e.g. `jetbrains-mono.ttf`).
- Record the source and license of every third-party asset in a `LICENSES.md`
  here when it is added.
