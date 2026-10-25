# Krastor — LLM Agent with Voice and Graphics Interface

Master project description. This file is the source of truth for what the project is and
where it is going. See `AGENT.md` for agent working rules and index of folder-level
`development.md` files. All development history is recorded in `development_log.md`.

## 1. Vision

Krastor is a desktop agent for LLMs written in C++20. It:

- **Accepts input** from keyboard **and** microphone (local STT).
- **Thinks** by calling a remote LLM through an OpenAI-compatible HTTP API
  (`/v1/chat/completions` style, classic HTTP call).
- **Responds** as text and/or audio (local TTS).
- **Lives** in a videogame-style window: a real-time rendered surface with GLSL shader
  effects and a UI layer (text, buttons, menus, icons, SVG) on top.

## 2. Requirements

### Functional
- Input: keyboard text, microphone speech (STT, local), file attachments via drag & drop
  and Ctrl+V (images, documents, audio — accepted types to be finalized in a design session).
- Output: text in a terminal-style I/O panel and/or spoken audio (TTS, local).
- LLM: remote, OpenAI-compatible v1 HTTP interface (base URL, model and key configurable).
- STT and TTS must be **local** and good in **English and Italian**. Model selection is a
  dedicated research step (Phase 2).

### Non-functional
- Language: C++20.
- Target platforms: Windows 10 (dev machine) and Linux.
- Dual toolchain rule on Windows: all code must compile with **both** MinGW GCC and MSVC.
- All documentation, comments and logs in **English**.

## 3. Toolchain (verified on dev machine)

| Tool | Location | Version | Notes |
|---|---|---|---|
| MinGW GCC (g++) | `C:\mingw64\bin\g++.exe` | 16.2.0 (ucrt-posix-seh) | Full C++20 |
| MSVC 2026 (default MSVC) | `C:\Program Files\Microsoft Visual Studio\18\Community` | toolset 14.51 | Full C++20 |
| MSVC 2022 (fallback) | `C:\Program Files\Microsoft Visual Studio\2022\Community` | toolset 14.44 | Full C++20 |
| CMake | on PATH | 4.4.3 | Presets-based builds |
| Git | on PATH | 2.53 | |
| Python | `C:\Python313\python.exe` | 3.13.1 | Only via `.venv`; never the PATH python (3.11) |

## 4. Architecture (planned)

```
+-----------+     +----------------+     +---------------------+
|  Input    | --> |  Agent core    | --> |  Output             |
| keyboard  |     |  conversation  |     | terminal I/O panel  |
| mic ->STT |     |  loop, config  |     | TTS -> speakers     |
| files     |     |  HTTP client   |     | attachments render  |
+-----------+     +----------------+     +---------------------+
                          |
                 remote LLM (OpenAI-compatible v1 HTTP)
```

### 4.1 Terminal-style I/O panel
The main chat surface is a scrollable, monospaced, terminal-style text view. An
**output renderer** inspects each LLM response and routes content blocks by type:
- plain text -> terminal stream;
- ```json``` -> validated (nlohmann/json) + syntax-highlighted;
- ```python``` / ```cpp``` / other code -> code block with per-language coloring + copy;
- Markdown -> styled rendering via md4c (headings, lists, links);
- registry is extensible for future output types.

### 4.2 Attachments (input)
- Drag & drop of files via GLFW drop callback (Windows + Linux).
- Ctrl+V: text via GLFW clipboard; image/file clipboard on Windows via shell clipboard
  API (Linux equivalent TBD).
- Input box + attachment list (icon, name, type) sent alongside the message.
- Which file types are accepted is decided in a future design session.

### 4.3 GUI (Phase 5, design locked, built later)
Game-loop window: continuous frame loop, GLSL shader background ("living surface"),
UI toolkit drawn on top each frame.

Minimal library stack (all vendored in `libs/`, MIT/zlib, compile with MinGW + MSVC,
Windows + Linux):

| Need | Library | Compile cost |
|---|---|---|
| Window, game loop, input, drag&drop, clipboard | GLFW | prebuilt lib, zero compilation |
| OpenGL 3.3 core loading | glad | 2 files |
| UI toolkit (text, buttons, menus, icons, multiline edit) | Nuklear | 1 header |
| SVG rasterizing | nanosvg | 2 files |
| PNG/JPG | stb_image | 1 header |
| Markdown parsing | md4c | 2 files (C) |
| JSON | nlohmann/json | 1 header |
| HTTP to LLM API | libcurl | link only |
| Audio playback (TTS) | miniaudio | 1 header |

Frame model: update -> GLSL background pass -> UI pass -> swap buffers.

## 5. Folder structure

```
src/       C++ sources (CMakeLists.txt at root)
data/      assets: fonts, images, shaders (.glsl later)
obj/       build intermediates (CMake binary dirs) — git ignored
bin/       executables — git ignored
models/    local AI models, one subfolder per model — git ignored
tools/     python prototypes/scripts (uses .venv)
libs/      vendored third-party libraries (populated in GUI phase)
scripts/   build/dev scripts
.venv/     python virtual env (C:\Python313) — git ignored
```

Each tracked folder has a `development.md` describing its development in detail.

## 6. Models policy (`models/`)

- `models/` is git ignored; weights never enter the repository.
- One subfolder per model, named `<role>-<name>` (e.g. `stt-whisper-large-v3`,
  `tts-piper-it-it-riccardo`).
- Each model subfolder contains a `README.md` recording: source URL, license, file list,
  intended use. The README is committed via an explicit exception rule if needed.
- STT/TTS candidates to evaluate in Phase 2 (EN+IT): whisper.cpp, sherpa-onnx (STT);
  Piper, sherpa-onnx VITS, Kokoro (TTS). Prototypes in Python first.

## 7. Build system

- Root `CMakeLists.txt`, C++20 via `target_compile_features(... cxx_std_20)`.
- `CMakePresets.json` is the single entry point:
  - `mingw-debug` / `mingw-release` — MinGW Makefiles, `C:\mingw64`
  - `msvc-2026-debug` / `msvc-2026-release` — **default MSVC** (VS 18 2026)
  - `msvc-2022-debug` / `msvc-2022-release` — fallback (VS 17 2022)
  - `linux-gcc-debug` / `linux-gcc-release`
- Build intermediates live in `obj/<preset>/`, executables land in `bin/`.
- Helper scripts: `scripts/build.ps1` (Windows), `scripts/build.sh` (Linux).
- Rule: code must keep compiling with both MinGW GCC and MSVC on Windows; no
  compiler-specific extensions without a guarded abstraction.

## 8. Roadmap

- **Phase 1 — Initialization (done 2026-10-25).** Folders, docs, git, dual-toolchain
  CMake build, python venv, models policy. Hello-world builds with MinGW + MSVC 2026.
- **Phase 2 — Model research (done 2026-10-25).** Stack chosen: Vosk streaming STT +
  Piper/sherpa-onnx TTS (EN+IT). Python parrot prototype validates the pipeline.
- **Phase 3 — Core agent (done 2026-10-26).** Config loader, OpenAI-compatible HTTP
  client (WinHTTP/libcurl), conversation loop, CLI with runtime TTS voice selection.
- **Phase 4 — Local STT/TTS integration** in C++ (mic capture + playback pipeline).
- **Phase 5 — GUI (started 2026-10-26).** Foundation landed: GLFW + OpenGL 3.3 +
  Nuklear window with a gear icon opening a settings modal (LLM URL, TTS voice,
  params). Remaining: GLSL effect surface, terminal I/O panel, attachments input.
