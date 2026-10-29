# scripts/ — build & dev scripts

Development doc for this folder (see `../project.md` section 7).

## Purpose
Developer convenience scripts wrapping the CMake presets.

## Contents
- `build.ps1` — Windows build helper.
  `scripts\build.ps1 -Toolchain msvc2026|msvc2022|mingw -Config Debug|Release`
  (default: `msvc2026 Debug`). Maps to the matching `CMakePresets.json` preset
  (`msvc2026` -> `msvc-2026-<cfg>` etc.). Auto-runs
  `git submodule update --init --recursive` when a source-library submodule
  (currently `libs/lunasvg`) is missing.
- `build.sh` — Linux build helper. `./scripts/build.sh [debug|release]`.
  Same submodule auto-init.

## Current state (Phase 5, 2026-10-29)
Both scripts present and working on the dual Windows toolchain. Model
downloads live in `tools/download_deps.py` (run via the .venv python).
