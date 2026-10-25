# src/ — C++ sources

Development doc for this folder (see `../project.md` for the master description).

## Purpose
All C++20 source code of the Krastor agent. Built by the root `CMakeLists.txt`;
executables are emitted to `../bin`, intermediates to `../obj`.

## Current state (Phase 3, 2026-10-26) — core agent CLI
| Module | Responsibility |
|---|---|
| `main.cpp` | CLI REPL: commands (`/help /voices /voice <id> /reset /config /exit`), everything else goes to the LLM |
| `config.h/.cpp` | JSON config (`config.json`, see `config.example.json`); API key fallback `KRASTOR_API_KEY` -> `OPENAI_API_KEY` |
| `conversation.h/.cpp` | message list (system + history), reset semantics |
| `llm_client.h/.cpp` | OpenAI-compatible `POST {base_url}/chat/completions`, JSON via nlohmann |
| `http_client.h` + `http_client_winhttp.cpp` / `http_client_curl.cpp` | minimal HTTPS POST: **WinHTTP on Windows** (OS TLS, identical on MinGW+MSVC), **libcurl on Linux** |
| `voices.h/.cpp` | TTS voice registry: discovers `models/tts-piper-*`, **runtime voice switching** (`select()` callable at any time; Phase 4 TTS engine reads `current()`) |
| `gui.h/.cpp` | **GUI foundation (Phase 5 start)**: GLFW window + OpenGL 3.3 + Nuklear. A **gear icon** (top-right, drawn with Nuklear canvas primitives) opens a **settings modal** to edit LLM base URL, model, TTS voice (combo), temperature, max tokens, system prompt; Save writes `config.json` and applies the voice. Launched with `krastor --gui`. |

Verified: builds clean with `mingw-debug` and `msvc-2026-debug`; the GUI window opens
and the render loop runs (process stays alive); CLI mode unchanged.

## Conventions
- C++20, no compiler-specific extensions without a guarded abstraction
  (must build with both MinGW GCC and MSVC). Example guard: WinHTTP vs libcurl
  selected by CMake per platform.
- English comments. Warnings: `/W4 /utf-8` on MSVC, `-Wall -Wextra` on GCC.
- UTF-8 console output on Windows (`SetConsoleOutputCP`).

## Gotchas learned
- MinGW's and MSVC's `winhttp.h` both use `URL_COMPONENTS.nPort` (not `nPortNumber`).
- `#pragma comment(lib, ...)` must be guarded with `#ifdef _MSC_VER` (GCC warns).
- The MinGW install is compiler-only (no curl/openssl headers) — hence WinHTTP.
- Nuklear (master 4.13): text field is `nk_edit_string_zero_terminated` (no
  `nk_input_text`); alignment uses combined enums (`NK_MIDDLE_LEFT`, `NK_MIDDLE_CENTER`,
  not `NK_CENTER`); window-close check is `nk_window_is_closed`.
- glad 0.1.36 CLI: `--api "gl=3.3" --profile core`, and it reads `gl.xml` from the
  CWD when present (downloaded from Khronos if absent).

## Planned layout (roadmap)
- `media/` — STT (Parakeet/Vosk) + TTS (sherpa-onnx Piper) wrappers, mic, playback (Phase 4)
- `gui/` — GLFW/Nuklear game loop, GLSL passes, terminal I/O panel (Phase 5)
