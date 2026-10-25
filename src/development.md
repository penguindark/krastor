# src/ — C++ sources

Development doc for this folder (see `../project.md` for the master description).

## Purpose
All C++20 source code of the Krastor agent. Built by the root `CMakeLists.txt`;
executables are emitted to `../bin`, intermediates to `../obj`.

## Current state (Phase 4, 2026-10-26) — local voice I/O in C++
| Module | Responsibility |
|---|---|
| `main.cpp` | CLI REPL: commands (`/help /voices /voice <id> /say <text> /listen [sec] /transcribe <file> /reset /config /exit`), everything else goes to the LLM |
| `config.h/.cpp` | JSON config (`config.json`, see `config.example.json`); API key fallback `KRASTOR_API_KEY` -> `OPENAI_API_KEY`; `save()` used by the GUI |
| `conversation.h/.cpp` | message list (system + history), reset semantics |
| `llm_client.h/.cpp` | OpenAI-compatible `POST {base_url}/chat/completions`, JSON via nlohmann |
| `http_client.h` + `http_client_winhttp.cpp` / `http_client_curl.cpp` | minimal HTTPS POST: **WinHTTP on Windows** (OS TLS, identical on MinGW+MSVC), **libcurl on Linux** |
| `voices.h/.cpp` | TTS voice registry: discovers `models/tts-piper-*`, **runtime voice switching** (`select()` callable at any time; `tts.cpp` reads `current()`) |
| `tts.h/.cpp` | **TTS engine**: Piper via sherpa-onnx C API (`SherpaOnnxOfflineTts`, espeak-ng `data_dir`), model (re)loaded lazily when the selected voice changes; playback via miniaudio (float PCM wrapped in an in-memory WAV header); `speak()` blocks until playback ends; works silently when no output device exists |
| `stt.h/.cpp` | **STT engine**: NVIDIA Parakeet TDT 0.6B v3 int8 (`nemo_transducer`) + Silero VAD via sherpa-onnx C API, mic capture via miniaudio (16 kHz mono float). `listen()` returns the first VAD-closed utterance (~0.5 s after speech ends); `transcribe_file()` decodes wav/mp3/flac to 16 kHz mono for headless tests. EN+IT auto-detected, punctuation + capitalization included |
| `gui.h/.cpp` | **GUI foundation (Phase 5 start)**: GLFW window + OpenGL 3.3 + Nuklear. A **gear icon** (top-right, drawn with Nuklear canvas primitives) opens a **settings modal** to edit LLM base URL, model, TTS voice (combo), temperature, max tokens, system prompt; Save writes `config.json` and applies the voice. Launched with `krastor --gui`. |

Verified (both `mingw-debug` and `msvc-2026-debug`): `/say` speaks IT+EN through the
speakers with runtime voice switching; `/transcribe` returns exact transcriptions
(punctuation, accents) of generated test WAVs in both languages; `/listen` opens the
microphone, runs the VAD loop and times out cleanly on silence. sherpa-onnx binaries
are git-ignored — run `tools/download_deps.py` once after cloning.

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
- sherpa-onnx: use the **C API** (`c_api.h`) — the MSVC-built import lib links fine
  from MinGW when only `extern "C"` functions are used. Pin **v1.13.8**: v2.0.0
  ignores `data_dir` (espeak-ng) for VITS, which breaks Piper voices.
- miniaudio: pin the **0.11.22 release** header (master renamed the buffer/decoder
  APIs). `ma_decoder` only accepts containers, so raw float PCM is wrapped in a
  44-byte in-memory WAV header for playback.

## Planned layout (roadmap)
- `gui/` — GLSL effect passes, terminal I/O panel, attachments input (Phase 5)
- STT/TTS live directly in `src/` (`stt.*`, `tts.*`, `voices.*`) — no `media/` subfolder needed
