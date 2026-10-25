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
- LLM: remote, OpenAI-compatible v1 HTTP interface. Endpoints/models come from
  **`llm_models.json`** (opencode.jsonc `provider` schema, LM Studio included);
  Settings only picks the model (`llm.model_ref`); explicit `llm.base_url` /
  `model` in config.json remain the fallback when no ref is selected.
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
The main chat surface is a scrollable, monospaced (JetBrains Mono), terminal-style
text view. Messages are parsed by `src/ansi.h` (ANSI/SGR: 16/256/truecolor fg+bg,
bold; other escapes stripped) and drawn as styled runs with `nk_draw_text`:
- diff-style full-line backgrounds when a whole visual line shares one bg color;
- inline per-token backgrounds otherwise;
- indentation-preserving word wrap with UTF-8-safe hard breaks.
LLM replies are styled by the output formatting plugins (`src/formatters.h`:
Markdown, syntax highlighting, Thought blocks, color emoji) before reaching this
renderer.
Implemented: per-response stats sub-line (`model - time -
tok/s`) + context-usage footer (`llm.context_size`), window-size persistence,
mouse-wheel/keyboard chat scrolling with follow-bottom, and chat text selection &
copy (mouse drag, double-click word, triple-click line, Shift+arrows, Ctrl+A/C,
ANSI-stripped raw UTF-8 clipboard).

### 4.2 Attachments (input) — implemented
- **Drag & drop** of files via GLFW drop callback: images (png/jpg/bmp/gif/tga)
  become attachments; text files (.txt/.md/.log/source) are inserted into the
  input field; anything else is refused with a status line.
- **Ctrl+V** in the input field: text via GLFW clipboard; when the clipboard holds
  no text but a bitmap (Win32 `CF_DIB` → PNG), it is attached as an image.
  The **Img** button attaches the clipboard image explicitly.
- Attachment chips above the input (size shown, click to remove); sent to the LLM
  as base64 PNG data URLs in OpenAI multimodal `content` parts.

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
- **Phase 4 — Local STT/TTS integration (done 2026-10-26).** sherpa-onnx C API
  (pinned 1.13.8) + miniaudio: Piper TTS with runtime voice switching, Parakeet
  multilingual STT with Silero VAD and mic capture, `/say`, `/listen`, `/transcribe`.
- **Phase 5 — GUI (started 2026-10-26).** Chat interface live: terminal-style I/O
  panel with ANSI/SGR styled rendering (colors, bold, diff-style line backgrounds),
  multiline input (Enter sends, Shift+Enter newline), cut/copy/paste, **image
  attachments** via Ctrl+V (clipboard bitmap) and **drag & drop** (images attach,
  text files insert), sent to the LLM as OpenAI multimodal parts; Listen/auto-speak
  wired to the voice engines on worker threads, GLSL animated background, settings
  modal, JetBrains Mono UI font. **Ctrl +/-/0 font zoom** (pre-baked font ladder
12–30 px, layout rows scale with font height, size auto-saved debounced to the
exe-folder config). **Pluggable GLSL background effects**: `data/shaders/*.frag`
plugins with a fixed uniform contract (mic level + 64-band spectrum history
texture + brightness), Settings dropdown, compile-validated selection persisted to
`gui.effect`, hot-reload on file edit; first plugin `ring_spectrogram.frag`
draws audio-reactive cyan/magenta mesh ribbons (synthetic animation when the mic
is closed). **Hands-free continuous cycle**: mic → VAD → STT → LLM → text → TTS
with echo guard and `agent.continuous` auto-start; **barge-in** (speak over the
TTS to cut it short when the ignore-guard is off). **Configurable agent name**
and **effect brightness** in Settings. **Chat UX increment**:
per-response stats sub-line + context-usage footer (`llm.context_size`),
window-size persistence, wheel/keyboard scrolling with follow-bottom, chat text
selection & UTF-8 copy (mouse + Shift+arrows). **Output formatting plugins**:
Markdown -> ANSI formatter styles LLM
replies (headings, bold/italic, inline code, lists, quotes, tables), data-driven
syntax highlighting (`data/languages/*.json`), collapsible "Thought" blocks for
model reasoning, and full-color emoji via a Noto sprite atlas. **Stop button**
aborts an in-flight LLM request instantly (cross-thread WinHTTP handle close /
curl progress abort) and cuts TTS playback mid-utterance.
