# src/ — C++ sources

Development doc for this folder (see `../project.md` for the master description).

## Purpose
All C++20 source code of the Krastor agent. Built by the root `CMakeLists.txt`;
executables are emitted to `../bin`, intermediates to `../obj`.

## Current state (Phase 4, 2026-10-26) — local voice I/O in C++
| Module | Responsibility |
|---|---|
| `main.cpp` | CLI REPL: commands (`/help /voices /voice <id> /say <text> /listen [sec] [idx] /devices /mic [sec] [idx] /transcribe <file> /reset /config /exit`), everything else goes to the LLM |
| `config.h/.cpp` | JSON config (`config.json`, see `config.example.json`); API key fallback `KRASTOR_API_KEY` -> `OPENAI_API_KEY`; `save()` used by the GUI |
| `conversation.h/.cpp` | message list (system + history), reset semantics |
| `llm_client.h/.cpp` | OpenAI-compatible `POST {base_url}/chat/completions`, JSON via nlohmann |
| `http_client.h` + `http_client_winhttp.cpp` / `http_client_curl.cpp` | minimal HTTPS POST: **WinHTTP on Windows** (OS TLS, identical on MinGW+MSVC), **libcurl on Linux** |
| `voices.h/.cpp` | TTS voice registry: discovers `models/tts-piper-*`, **runtime voice switching** (`select()` callable at any time; `tts.cpp` reads `current()`) |
| `tts.h/.cpp` | **TTS engine**: Piper via sherpa-onnx C API (`SherpaOnnxOfflineTts`, espeak-ng `data_dir`), model (re)loaded lazily when the selected voice changes; playback via miniaudio (float PCM wrapped in an in-memory WAV header); `speak()` blocks until playback ends; works silently when no output device exists |
| `stt.h/.cpp` | **STT engine**: NVIDIA Parakeet TDT 0.6B v3 int8 (`nemo_transducer`) + Silero VAD via sherpa-onnx C API, mic capture via miniaudio (16 kHz mono float). `listen()` returns the first VAD-closed utterance (~0.5 s after speech ends); `transcribe_file()` decodes wav/mp3/flac to 16 kHz mono for headless tests; `list_input_devices()` + explicit `device_index` selection (`stt.input_device` in config, -1 = system default); `monitor()` prints a live level meter to diagnose muted/blocked/wrong mics. EN+IT auto-detected, punctuation + capitalization included |
| `gui.h/.cpp` | **GUI (Phase 5)**: the window is the agent's main interface — terminal-style chat panel (colored, wrapped, auto-scroll), text input + Send (Enter works), **Listen** button (Parakeet STT, auto-sends the transcription), **Speak ON/OFF** auto-TTS toggle, Test-voice button, status line, and the **gear icon → settings modal** (LLM URL/model, voice, temperature, max tokens, system prompt; Save writes `config.json` and hot-reloads the LLM client). LLM/STT/TTS run on worker threads (`std::async`/`std::thread`) so the 60 fps loop never blocks; a **GLSL fragment shader** draws an animated gradient background behind the semi-transparent panels. Launched with `krastor --gui`. |

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
  not `NK_CENTER`); window-close check is `nk_window_is_closed`;
  `nk_label_colored_wrap(ctx, text, color)` takes **no** alignment argument;
  `nk_layout_row_dynamic(ctx, 0, 1)` = fill remaining space (used for the chat group);
  `nk_window_set_scroll` inside a `nk_group_begin` scrolls that group.
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
