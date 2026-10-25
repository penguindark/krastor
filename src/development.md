# src/ — C++ sources

Development doc for this folder (see `../project.md` for the master description).

## Purpose
All C++20 source code of the Krastor agent. Built by the root `CMakeLists.txt`;
executables are emitted to `../bin`, intermediates to `../obj`.

## Current state (Phase 5, 2026-10-26) — GUI chat interface
| Module | Responsibility |
|---|---|
| `main.cpp` | CLI REPL: commands (`/help /voices /voice <id> /say <text> /listen [sec] [idx] /devices /mic [sec] [idx] /transcribe <file> /reset /config /exit`), everything else goes to the LLM |
| `config.h/.cpp` | JSON config (`config.json`, see `config.example.json`); API key fallback `KRASTOR_API_KEY` -> `OPENAI_API_KEY`; `save()` used by the GUI. **Runtime config lives next to the executable** (`exe_dir()/config.json`, atomic tmp+rename write); a legacy `config.json` in the working directory is read as fallback and migrated on first launch. Sections: `llm` (incl. `context_size`, default 131072), `tts`, `stt`, `gui` (font_size, effect, effect_intensity), `agent` (name, continuous, ignore_while_speaking) |
| `conversation.h/.cpp` | message list (system + history), reset; `Message.images` carries base64 PNG attachments (vision input) |
| `llm_client.h/.cpp` | OpenAI-compatible `POST {base_url}/chat/completions`, JSON via nlohmann; messages with images are sent as multimodal `content` part arrays (`text` + `image_url` data URLs); parses the `usage` block (`prompt_tokens`/`completion_tokens`) + response `model` into `LlmReply`, falls back to a chars/4 estimate (`tokens_estimated`) when the server omits usage |
| `http_client.h` + `http_client_winhttp.cpp` / `http_client_curl.cpp` | minimal HTTPS POST: **WinHTTP on Windows** (OS TLS, identical on MinGW+MSVC), **libcurl on Linux** |
| `voices.h/.cpp` | TTS voice registry: discovers `models/tts-piper-*`, **runtime voice switching** (`select()` callable at any time; `tts.cpp` reads `current()`) |
| `tts.h/.cpp` | **TTS engine**: Piper via sherpa-onnx C API (`SherpaOnnxOfflineTts`, espeak-ng `data_dir`), model (re)loaded lazily when the selected voice changes; playback via miniaudio (float PCM wrapped in an in-memory WAV header); `speak()` blocks until playback ends; **`interrupt()`** (atomic cancel flag checked by the playback wait-loop, `ma_sound_stop`) enables barge-in; works silently when no output device exists |
| `stt.h/.cpp` | **STT engine**: NVIDIA Parakeet TDT 0.6B v3 int8 (`nemo_transducer`) + Silero VAD via sherpa-onnx C API, mic capture via miniaudio (16 kHz mono float). `listen()` returns the first VAD-closed utterance (~0.5 s after speech ends); `transcribe_file()` decodes wav/mp3/flac to 16 kHz mono for headless tests; `list_input_devices()` + explicit `device_index` selection (`stt.input_device` in config, -1 = system default); `monitor()` prints a live level meter to diagnose muted/blocked/wrong mics. **Stream mode** (`start_stream()/stop_stream()/streaming()`): feeder thread drains the mic ring, runs the VAD in 512-sample windows and hands every closed segment to a callback; `recognize()` decodes raw PCM on the caller's thread (used by the GUI hands-free pipeline). All thread bodies are exception-guarded (an escaping exception would `terminate()` the process). EN+IT auto-detected, punctuation + capitalization included |
| `ansi.h` | header-only **ANSI/SGR parser**: splits text with escape codes into styled runs (fg/bg 16+256+truecolor, bold); unsupported sequences stripped. Powers the terminal-style chat renderer |
| `clipboard.h/.cpp` | clipboard **image** capture (Win32 `CF_DIB` → RGBA → PNG via stb; Linux stub), `load_image_file()` (any stb-readable image → PNG, used by drag & drop), `base64_encode()` for data URLs |
| `fft.h` | header-only iterative radix-2 FFT (512-pt, Hann window applied by caller) |
| `audio_analysis.h/.cpp` | **SpectrumAnalyzer**: mic window → 64 log-spaced amplitude bands (dB-compressed, fast-attack/slow-release smoothing) + 128×64 scrolling history ring uploaded as R8 texture; `synthetic(t)` feed keeps effects alive without the mic |
| `effects.h/.cpp` | **EffectEngine**: pluggable GLSL background — built-in gradient + `data/shaders/*.frag` plugins compiled at runtime on a shared fullscreen-triangle vertex shader with a fixed uniform contract (`u_resolution/u_time/u_level/u_spectrum/u_spec_head/u_listening/u_speaking/u_intensity`); the plugin's `main()` is renamed via `#define main effect_main` and a real `main()` applies the global brightness (`u_intensity`, Settings "Effect brightness"); `scan()` lists plugins (display name from `// krastor-effect:` header), `select()` validates by compiling (broken plugin never replaces the running one), `check_reload()` hot-reloads on file change |
| `gui.h/.cpp` | **GUI (Phase 5)**: the window is the agent's main interface. Terminal-style chat panel with **styled-run rendering** (`nk_draw_text` per token): ANSI colors/bold/backgrounds, diff-style full-line backgrounds, indentation-preserving word wrap, auto-scroll pinned to newest line. **Per-response stats sub-line** under each reply (`model - time - tok/s`, dim gray, `~` prefix when tokens were estimated; elapsed measured in the async worker) and a **context-usage footer** on the status line (`ctx: N / total (x%)`, thousands separators, amber past 85%; total = `llm.context_size`, occupancy = last reply's `prompt_tokens`). **Ctrl +/-/0 font zoom** (ladder 12–30 px baked into the atlas at startup, `nk_style_set_font` swap, all row heights proportional to font height, debounced auto-save of `gui.font_size`). **Multiline input** (Enter sends, Shift+Enter newline, 16 KB buffer), cut/copy/paste via GLFW clipboard + custom paste hook (Ctrl+V pastes text **or attaches a clipboard image**), **drag & drop** (images → attachments, text files → inserted into the input), attachment chips (click to remove), **Img** button, **Listen** button (Parakeet STT, auto-sends the transcription), **Speak ON/OFF** auto-TTS toggle, Test-voice button, status line, gear icon → **settings modal** (agent name, LLM URL/model, voice, background effect + brightness, temperature, max tokens, context tokens, system prompt, ignore-while-busy checkbox; Save writes the exe-folder config and hot-reloads the LLM client). **Hands-free continuous cycle** (top-bar "Hands-free: ON/OFF" toggle, auto-starts when `agent.continuous`): Stt stream mode → pipeline worker thread (`recognize`) → utterance queue → render loop drains it and reuses `send()` (single LLM worker path); echo guard drops segments while thinking/speaking when `agent.ignore_while_speaking`; with the guard **off**, a heard utterance while speaking triggers **barge-in**: `tts.interrupt()` cuts playback and the utterance is processed. **Configurable agent name** (`agent.name`): window title, top bar, chat prefix and greeting. **Effect brightness** slider (`gui.effect_intensity`, 0.2–3, debounced auto-save) → `u_intensity` uniform. The main window is fully transparent (shader shows through); readability comes from the translucent chat group (alpha 105) and the opaque Settings modal (pushed `fixed_background`). UI font: JetBrains Mono (`data/fonts/`, fallback to built-in). LLM/STT/TTS run on worker threads (`std::async`/`std::thread`) so the 60 fps loop never blocks; a **GLSL fragment shader** draws the animated background behind the UI. `KRASTOR_UITEST=1` env seeds styled test lines + clipboard image and auto-sends one short LLM message (screenshot self-test of the stats line + footer). Launched with `krastor --gui`. |

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
  `nk_label_colored_wrap(ctx, text, color)` takes **no** alignment argument.
- Nuklear layout/render gotchas (cost hours):
  - `nk_layout_row_dynamic(ctx, 0, 1)` does **not** fill remaining space — it makes a
    min-height row. Give scrollable groups an explicit pixel height (window height
    minus the other fixed rows) via `nk_layout_row_begin`+`nk_layout_row_push(1.0f)`.
  - The window body is painted from `style.window.fixed_background` (use
    `nk_style_item_color(...)`), **not** `style.window.background` (that only colors
    dynamic panels/groups). Set both for a uniform translucent look.
  - `nk_window_set_scroll` scrolls the **outer window**, not the current group; use
    `nk_group_set_scroll(ctx, id, x, y)` with the group's own id. Nuklear only clamps
    the offset while the scrollbar is visible, so compute the bottom offset yourself
    (content height − visible height) rather than pinning to a huge value.
  - **`nk_glfw3_render` buffer args are BYTES, not vertex counts.** The small default
    (4096) holds ~200 vertices and silently drops every command after them — the
    classic "only the first row renders" symptom. Use `512*1024, 128*1024`.
- glad 0.1.36 CLI: `--api "gl=3.3" --profile core`, and it reads `gl.xml` from the
  CWD when present (downloaded from Khronos if absent).
- sherpa-onnx: use the **C API** (`c_api.h`) — the MSVC-built import lib links fine
  from MinGW when only `extern "C"` functions are used. Pin **v1.13.8**: v2.0.0
  ignores `data_dir` (espeak-ng) for VITS, which breaks Piper voices.
- miniaudio: pin the **0.11.22 release** header (master renamed the buffer/decoder
  APIs). `ma_decoder` only accepts containers, so raw float PCM is wrapped in a
  44-byte in-memory WAV header for playback.
- Nuklear 4.13.3 specifics (chat renderer):
  - There is **no public `nk_text_width`** — measure text via the font callback:
    `font->width(font->userdata, font->height, s, len)`.
  - `nk_font_atlas_add_from_file` is compiled out unless `NK_INCLUDE_STANDARD_IO`
    is defined; load the TTF into memory and use `nk_font_atlas_add_from_memory`.
  - `nk_draw_text` truncates text to the rect width, so per-token drawing must
    pass a rect at least as wide as the token (`nk_text_width`-measured + 2).
  - `nk_widget_bounds(ctx)` peeks at the last allocated widget's rect without
    consuming a slot — used to get absolute pixel coords for manual drawing.
  - Clipboard plugin signatures: `nk_plugin_paste = void(*)(nk_handle,
    nk_text_edit*)` — the callback must call `nk_textedit_paste` itself; the glfw
    backend installs text-only paste, we override `ctx.clip.paste` to fall back
    to clipboard-image attachment when the clipboard has no text.
  - `NK_EDIT_BOX | NK_EDIT_SIG_ENTER` mixes two enum types → `-Wdeprecated` on
    both toolchains; cast each flag to `nk_flags` before `|`.
  - Enter handling inside `nk_textedit_key`: with `NK_EDIT_CTRL_ENTER_NEWLINE`,
    Shift+Enter inserts `\n`; else `NK_EDIT_SIG_ENTER` sets `NK_EDIT_COMMITTED`;
    else Enter inserts `\n`. So multiline + Enter-sends works with
    `NK_EDIT_BOX | NK_EDIT_SIG_ENTER | NK_EDIT_CTRL_ENTER_NEWLINE`.
  - A `//` comment ending with a backslash swallows the next source line (line
    continuation) — bit us in `ansi.h` ("ESC \" comment).
- Win32 clipboard images: `CF_DIB` via `GlobalLock`; DIBs are bottom-up unless
  `biHeight < 0`; rows padded to 4 bytes. Re-encode to PNG with stb for the
  OpenAI `image_url` data URL.
- Nuklear 4.13.3: `nk_style_set_font` sets `style.font` and resets the min row
  height — in this version widget styles have **no** own font fields, so one call
  rescales every widget. `nk_font_atlas_add_from_memory` **borrows** the buffer
  until `nk_font_stash_end` (keep it alive until then).
- Nuklear draws `style.window.fixed_background` for **every fixed window**
  (no flag needed) — a translucent main window must set its alpha to 0, and
  popups/modals need their own pushed background. Dynamic windows (groups,
  combos) use `style.window.background` (an `nk_color`, push with
  `nk_style_push_color`, not `push_style_item`).
- Shader `main()` wrapper trick: prepend `#define main effect_main` + a
  `void effect_main();` prototype, append the real `main()` after
  `#undef main` — without the `#undef` the footer's `main` is renamed too and
  GLSL reports "already defined".
- GLSL `pow(x, y)` is **undefined for x < 0** — use `x*x` for squared falloff
  (`exp(-pow(r/a, 2.0))` silently renders garbage/negative-radius regions).
- An uncaught C++ exception on **any** thread calls `std::terminate` → MSVC
  "Debug Error! abort() has been called" with zero context (the GUI has no
  console). Mitigation: every thread body + `future::get()` is try/caught, and
  `main()` installs a `std::set_terminate` handler that logs the real cause to
  `exe_dir()/krastor_error.log`.
- `std::filesystem::rename` replaces an existing file on MinGW+MSVC → safe
  atomic-save primitive (write `.tmp`, rename).

## Planned layout (roadmap)
- `gui/` — GLSL effect passes, terminal I/O panel, attachments input (Phase 5)
- STT/TTS live directly in `src/` (`stt.*`, `tts.*`, `voices.*`) — no `media/` subfolder needed
