# scripts/ — build & dev scripts

Development doc for this folder (see `../project.md` section 7).

## Purpose
Developer convenience scripts wrapping the CMake presets.

## Contents
- `build.ps1` — Windows build helper.
  `scripts\build.ps1 -Toolchain msvc2026|msvc2022|mingw -Config Debug|Release`
  (default: `msvc2026 Debug`). Maps to the matching `CMakePresets.json` preset.
- `build.sh` — Linux build helper. `./scripts/build.sh [debug|release]`.

## Current state (Phase 1)
Both scripts present and working. Phase 2 will add model-download scripts here.
