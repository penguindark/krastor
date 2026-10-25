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

Verified: builds clean with `mingw-debug` and `msvc-2026-debug`; CLI smoke test
covers voice discovery, runtime switching, unknown-voice rejection, config display.
LLM calls need an API key (`config.json` or `KRASTOR_API_KEY`).

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

## Planned layout (roadmap)
- `media/` — STT (Parakeet/Vosk) + TTS (sherpa-onnx Piper) wrappers, mic, playback (Phase 4)
- `gui/` — GLFW/Nuklear game loop, GLSL passes, terminal I/O panel (Phase 5)
