# Asset licenses

## data/fonts/jetbrains-mono.ttf
- **JetBrains Mono** (variable weight TTF, default instance = Regular)
- Source: <https://github.com/google/fonts> (`ofl/jetbrainsmono/JetBrainsMono[wght].ttf`)
- License: SIL Open Font License 1.1 (OFL)
- Used as the UI/chat font (monospace, box-drawing + extended glyphs for the
  terminal-style output panel). Loaded at runtime from `data/fonts/`.

## data/fonts/emoji_atlas.png + emoji_atlas.json
- **Noto Color Emoji** (CBDT color bitmaps), rendered to a sprite-sheet atlas
  by `tools/gen_emoji_atlas.py`; the source font `NotoColorEmoji.ttf` is
  git-ignored and re-downloadable (URL in the script docstring).
- Source: <https://github.com/googlefonts/noto-emoji> (gh-pages branch,
  fonts/NotoColorEmoji.ttf)
- License: SIL Open Font License 1.1 (OFL)
- Used for full-color emoji glyphs in the chat panel (src/emoji.cpp).

## data/icons/*.svg
- **Lucide** icon set (ISC license), 11 icons: `send`, `paperclip`, `ear`,
  `volume-2`, `square`, `settings`, `mic`, `mic-off`, `volume-x`, `x`,
  `check`.
- Source: <https://github.com/lucide-icons/lucide> (`icons/<name>.svg`,
  fetched from the `main` branch on 2026-10-29)
- License: ISC
- Rasterized at runtime to a white/alpha sprite atlas by `src/icons.cpp`
  (LunaSVG) for the floating action buttons, the top bar, the attachment
  chips and the Settings modal buttons.
