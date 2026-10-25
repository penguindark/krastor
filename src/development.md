# src/ — C++ sources

Development doc for this folder (see `../project.md` for the master description).

## Purpose
All C++20 source code of the Krastor agent. Built by the root `CMakeLists.txt`;
executables are emitted to `../bin`, intermediates to `../obj`.

## Current state (Phase 1)
- `main.cpp` — hello-world skeleton. Prints version, detected compiler
  (MinGW GCC vs MSVC) and C++ standard, proving the dual-toolchain pipeline.

## Conventions
- C++20, no compiler-specific extensions without a guarded abstraction
  (must build with both MinGW GCC and MSVC).
- English comments. One module per `.cpp/.h` pair; subfolders will be introduced
  per subsystem (e.g. `agent/`, `io/`, `gui/`, `audio/`) as phases land.
- Warnings are errors-in-spirit: `/W4` on MSVC, `-Wall -Wextra` on GCC.

## Planned layout (roadmap)
- `core/` — config, conversation loop, OpenAI-compatible HTTP client (Phase 3)
- `media/` — STT/TTS wrappers, mic capture, playback (Phase 4)
- `gui/` — GLFW/Nuklear game loop, GLSL passes, terminal I/O panel (Phase 5)
