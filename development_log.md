# Development Log

Chronological log of all development work. Newest entries at the bottom.
Format: `## YYYY-MM-DD — title` (see `AGENT.md` for rules).

---

## 2026-10-25 — Phase 1: project initialization

**Toolchain verification (Windows 10 dev machine):**
- MinGW g++ 16.2.0 (`C:\mingw64`) — full C++20 support confirmed.
- MSVC found via vswhere: VS Community 2022 (17.14, toolsets 14.38/14.44) and
  VS Community 2026 (18.10, toolset 14.51). MSVC 2026 chosen as default MSVC.
- CMake 4.4.3, git 2.53, Python 3.13.1 at `C:\Python313` (PATH python is 3.11 — not used).

**Decisions made (with user):**
- Folders: `src data obj bin models tools libs scripts` + `.venv`.
- Dual-toolchain rule: all code compiles with MinGW GCC and MSVC.
- GUI stack (built in Phase 5, design locked now): GLFW + glad + Nuklear + nanosvg +
  stb_image + md4c + nlohmann/json + libcurl + miniaudio — smallest/simplest-to-compile
  option. Game-loop window with GLSL effects.
- Terminal-style I/O panel with typed rendering of LLM output (JSON, Markdown, code).
- Attachment input via drag & drop / Ctrl+V (types decided in a later design session).
- `models/` git-ignored, one subfolder per model with README (source/license).
- `opencode.exe` / `opencode.jsonc` stay in place but are git-ignored.

**Actions:**
- Created folder structure and per-folder `development.md` files.
- Wrote `project.md` (master description), `AGENT.md` (base instructions + index),
  this log.
- Root `CMakeLists.txt` (C++20, outputs `bin/`, intermediates `obj/`) +
  `CMakePresets.json` (mingw, msvc-2026 default, msvc-2022, linux-gcc) +
  `scripts/build.ps1` / `scripts/build.sh`.
- Minimal `src/main.cpp` hello-world.
- `.gitignore` for all binaries/objects/models/.venv/opencode files.
- Python venv `.venv` created from `C:\Python313`.
- `git init` + initial commit.
- Verified build with `mingw-debug` and `msvc-2026-debug` presets -> `bin/krastor.exe`.

**Next:** Phase 2 — research and prototype local STT/TTS models (EN+IT) in `tools/`.

---

## 2026-10-25 — Phase 2: STT/TTS research and Python parrot prototype

**Research (web):** sherpa-onnx streaming zipformer has no Italian model; Kokoro
(sherpa-onnx port) is zh+en only. Chosen stack: **Vosk** for streaming STT (true
word-level partials for IT and EN, Apache-2.0, C API for later C++ use) and
**Piper voices via sherpa-onnx** for TTS (native it_IT + en_US voices, C++ API).

**Models downloaded** (via new `tools/download_models.py`, each with generated README):
`stt-vosk-it` (small-it-0.4), `stt-vosk-en` (small-en-us-0.15),
`tts-piper-it-it-paola` (medium), `tts-piper-en-us-lessac` (medium), `espeak-ng-data`.

**Scripts added:** `tools/download_models.py`, `tools/parrot.py` (mic -> streaming STT
with live partials -> TTS -> speaker), `tools/roundtrip_test.py` (automated TTS->STT
check, no mic), `tools/requirements.txt`.

**Results:** round-trip EN near-perfect; IT understandable but small model makes some
word errors — upgrade candidate `vosk-model-it-0.42` pending user listening test.
TTS WAV samples in `tools/out/` for the user to listen.

**Gotchas:** Vosk 0.3.45 silently ignores float32 PCM (feed int16); sherpa-onnx
2.0.0 removed the bundled espeak-ng frontend for Piper VITS voices (pinned
`sherpa-onnx==1.13.8`).

**Next:** user listens to samples and confirms model choices; then Phase 3 (core
agent) and Phase 4 (C++ integration of Vosk + sherpa-onnx).

---

## 2026-10-25 — Phase 2 addendum: Italian STT upgraded to big Vosk model

Tested `vosk-model-it-0.22` (1.2 GB, the correct big Italian model — `it-0.42` does
not exist) against the small one on the same TTS audio: small produced word errors
("vuoi perà da"), big produced near-perfect transcripts at RTF ~0.19 (5x faster than
real-time, streaming preserved). **Big model is now the default Italian STT**
(`models/stt-vosk-it-large/`); small kept via `parrot.py --small`.

**Next:** Phase 3 — core C++ agent (config, OpenAI-compatible HTTP client, loop).

---

## 2026-10-26 — Single multilingual STT research (EN+IT without model switching)

User asked for one STT model covering Italian + English, real-time fast, and to check
HW acceleration + HuggingFace options.

**HW:** i7-11800H, 64 GB RAM, RTX 3080 Laptop (CUDA available). CPU-only is already
sufficient for all candidates; GPU kept as later lever.

**Downloaded & benchmarked** (`tools/bench_stt.py` on the round-trip WAVs):
- `stt-whisper-small` (int8): RTF 0.6, EN perfect, IT weak.
- `stt-parakeet-multilingual` (NVIDIA Parakeet TDT 0.6B v3 int8, 25 langs, auto-detect,
  punctuation+capitalization): **RTF 0.15 (6.6x realtime)**, EN perfect, IT good.
  Runs natively on sherpa-onnx (`model_type="nemo_transducer"`) -> C++ path exists.

**Web research:** Nemotron 3.5 ASR Streaming (40 locales incl IT, true streaming,
RTF ~0.14 CPU) and Voxtral Mini 4B Realtime (13 langs, IT WER 3.3%, 240 ms delay)
are the true-streaming state of the art; sherpa-onnx support pending — revisit later.

**Recommendation:** Parakeet TDT v3 as the single EN+IT model (VAD near-live, final
~0.4 s after phrase); Vosk big-it kept where word-level live partials are required.
Decision to be confirmed by user after a live mic test.

**Next:** user live test -> confirm STT strategy -> Phase 3.

---

## 2026-10-26 — GPU acceleration benchmark (Whisper vs Parakeet)

Measured on the RTX 3080 with faster-whisper (CTranslate2 CUDA):
- Whisper small int8: CPU RTF ~0.55 -> CUDA ~0.13 (~4x).
- Whisper large-v3-turbo fp16 CUDA: RTF ~0.32 with **perfect Italian + punctuation**
  — the real GPU advantage is quality unlock, not just speed.
- Parakeet: no GPU wheel for py313; unnecessary anyway (CPU RTF 0.15, VAD endpointing
  dominates perceived latency).
Scripts: `tools/bench_gpu.py`, `tools/requirements-gpu.txt`.
C++ note: CUDA excludes MinGW; GPU path would be MSVC-only — keep CPU default and
design the STT interface provider-agnostic.

**Next:** user live test -> confirm STT strategy -> Phase 3.

---

## 2026-10-26 — Pruned unused models from disk

Removed (all re-downloadable via `tools/download_models.py`, results kept in
`models/development.md`): `stt-whisper-small` (1.3 GB, benchmark only, weak IT),
`hf-cache` (2.0 GB, faster-whisper benchmark cache), `stt-vosk-it` small (54 MB,
superseded by big model). Kept: Parakeet (chosen STT), Silero VAD, both Piper
voices, espeak-ng-data, Vosk big-it + small-en (pending live A/B test).
Freed ~3.3 GB.

---

## 2026-10-26 — Phase 3: core C++ agent (config, LLM client, conversation, voice registry)

**Modules added** (all build with MinGW + MSVC):
- `config.h/.cpp` — JSON config loader (`config.json`, gitignored; `config.example.json`
  committed); API key fallback KRASTOR_API_KEY -> OPENAI_API_KEY.
- `conversation.h/.cpp` — system prompt + message history, reset.
- `llm_client.h/.cpp` — OpenAI-compatible POST {base_url}/chat/completions.
- `http_client.h` + `http_client_winhttp.cpp` (Windows) / `http_client_curl.cpp` (Linux)
  — minimal HTTPS POST.
- `voices.h/.cpp` — TTS voice registry: discovers `models/tts-piper-*`, runtime
  switching via `select()` (user requirement: change voice during use).
- CLI REPL in `main.cpp`: /help /voices /voice <id> /reset /config /exit.

**HTTP decision:** cpp-httplib was vendored first but its HTTPS needs OpenSSL, absent
from the compiler-only MinGW install -> replaced by WinHTTP (OS TLS, both compilers)
on Windows and libcurl on Linux. httplib removed from libs/. nlohmann/json kept.

**Verified:** clean builds on mingw-debug + msvc-2026-debug; CLI smoke test exercised
voice discovery (en-us-lessac, it-it-paola), runtime switching, unknown-voice
rejection, config display. LLM calls require an API key.

**Gotchas:** winhttp.h uses `URL_COMPONENTS.nPort` on both toolchains; guard
`#pragma comment(lib)` with `_MSC_VER`; MinGW install is compiler-only.

**Next:** Phase 4 — local STT (Parakeet/Vosk) + TTS (sherpa-onnx Piper) in C++, wired
to the voice registry.

---

## 2026-10-26 — Phase 3 verified against local LM Studio

Connected the CLI to LM Studio at `http://192.168.0.3:8080` (model
`minicpm5-2b-heretic-abliterated`). Note: LM Studio's `/api/v1` is its REST API;
the OpenAI-compatible base is `http://192.168.0.3:8080/v1` (`/v1/chat/completions`).
Created local `config.json` (gitignored) with that base_url.

Live test through `bin/krastor.exe`: Italian self-introduction, arithmetic (12x8=96),
and multi-turn memory (recalled the user's name) all correct. Runtime voice switch
(/voice) works mid-session. WinHTTP handles plain http:// to the LAN host fine.

---

## 2026-10-26 — Phase 5 start: GUI foundation + gear/settings modal

User requested a gear icon opening a settings modal (LLM URL, TTS voice, other
params). This bootstraps the Phase 5 graphics foundation.

**Vendored into libs/**: GLFW 3.4 (full source, built via add_subdirectory so it
works on MinGW + MSVC), glad (OpenGL 3.3 core, generated with the pip glad tool),
Nuklear 4.13 (nuklear.h + glfw_opengl3 backend). nlohmann/json already present.

**New code**: `src/gui.h/.cpp` — GLFW window (GL 3.3 core), glad load, Nuklear init
with default font baked at 18px. A gear icon (drawn with nk_fill_circle +
nk_stroke_line) sits top-right; clicking toggles a **Settings modal** editing: LLM
base URL, model, TTS voice (combo from VoiceRegistry), temperature, max tokens,
system prompt. Save writes `config.json` (new `Config::save()`) and applies the
voice via `VoiceRegistry::select()`. Launched with `krastor --gui`; CLI unchanged.

**Build**: CMake now enables C, builds glfw, compiles glad.c + gui.cpp, links opengl32
(Windows) / OpenGL::GL (Linux). Verified: clean builds on mingw-debug + msvc-2026-debug;
GUI window opens and render loop stays alive; CLI regression OK.

**Nuklear API notes** (master differs from older docs): text field is
`nk_edit_string_zero_terminated`; alignment enums are combined (`NK_MIDDLE_LEFT`);
close check is `nk_window_is_closed`. glad CLI uses `--api gl=3.3 --profile core`.

**Next**: GLSL effect surface behind the UI, terminal I/O panel, then Phase 4 audio.

---

## 2026-10-26 — Phase 4: local STT/TTS in C++ (sherpa-onnx C API + miniaudio)

**Dependency strategy resolved**: sherpa-onnx ships prebuilt win-x64 shared libs
(MSVC-built). We use only the **C API** (`c_api.h`, extern "C"), so the MSVC import
lib links cleanly from MinGW too — verified on both toolchains. **Pinned v1.13.8**:
v2.0.0 ignores the espeak-ng `data_dir` for VITS/Piper, which would break our Piper
voices. Binaries (`*.dll` + import lib) are git-ignored; new
`tools/download_deps.py` fetches them; CMake copies the DLLs next to the exe and
fails configure with a hint if they are missing. miniaudio pinned to the 0.11.22
release header (master renamed APIs), implementation compiled once in
`libs/miniaudio_impl.c`.

**New modules**: `src/tts.h/.cpp` — Piper TTS via `SherpaOnnxOfflineTts`, model
lazily (re)loaded when `VoiceRegistry::current()` changes (runtime voice switching
works); playback = float PCM wrapped in an in-memory 44-byte WAV header into
`ma_decoder` -> `ma_engine`. `src/stt.h/.cpp` — Parakeet TDT v3 int8
(`nemo_transducer`) + Silero VAD via the offline-recognizer pattern of
`tools/parrot.py`; mic via miniaudio capture (16 kHz mono float); `listen()`
returns the first VAD-closed utterance; `transcribe_file()` decodes wav/mp3/flac
for headless tests. CLI: `/say <text>`, `/listen [sec]` (transcription goes to
the LLM), `/transcribe <file>`. Version bumped to 0.4.0.

**Verified** (MinGW + MSVC): `/say` speaks IT/EN with voice switching (1.6–2.2 s
audio generated in ~150–190 ms); `/transcribe` returns exact IT and EN
transcriptions incl. punctuation and accents; `/listen` opens the mic, VAD loop
runs, clean timeout on silence (one run even picked up room sound and answered it via
LM Studio). Gotchas: v2.0.0 `data_dir` deprecation; miniaudio master API churn;
`ma_decoder` needs a container (hence the WAV header trick); VAD C API takes
`(vad, samples, n)` without sample rate.

**Next**: Phase 5 — GLSL effect surface, terminal I/O panel, wire TTS/STT into the GUI.

---

## 2026-10-26 (later) — Microphone selection + input diagnostics

User reported `/listen` hearing nothing on their Realtek mic. Added input-device
selection to the app: `/devices` lists capture devices with indices (via
`ma_context_enumerate_devices`), `/mic [sec] [idx]` shows a live level meter +
peak (diagnoses wrong-device vs muted vs privacy-blocked), `/listen [sec] [idx]`
takes an explicit device index, and `stt.input_device` in `config.json`
persists the choice (-1 = system default). `Stt::Impl` now owns a persistent
`ma_context` and reopens the device when the index changes.

Diagnosis on this machine: Realtek IS the default device but the meter shows peak
0.0002 (silence) — typical of Windows privacy block (Settings > Privacy & security
> Microphone > let desktop apps access) or a muted/zero-gain mic; the error path
now prints the miniaudio error code (`ma_result_to_string` does not exist in
0.11.22). Builds on both toolchains; `/transcribe` regression still exact.

---

## 2026-10-26 (later) — Phase 5: GUI becomes the agent interface

Rewrote `src/gui.cpp`: the window is now the actual Krastor UI.
* Terminal-style chat panel: colored wrapped lines (you=blue, Krastor=green,
  sys=gray) in a scrollable `nk_group` pinned to the newest line.
* Input field + Send (Enter commits), **Listen** button (Parakeet STT on a worker
  thread, transcription auto-sent to the LLM), **Speak ON/OFF** auto-TTS toggle,
  Test-voice button, status line (thinking/listening/speaking/voice+mic info).
* Gear icon + settings modal kept; Save now hot-reloads the LlmClient.
* Threading: LLM call via `std::async` on a Conversation snapshot; STT via
  `std::async`; TTS via detached thread with an atomic speaking flag. The render
  loop only polls futures — never blocks.
* First **GLSL effect**: fullscreen big-triangle fragment shader (animated dark
  gradient + vignette, GLSL 330 core, no VBO via gl_VertexID) drawn before Nuklear;
  window/panel backgrounds made semi-transparent so it shows through.

Gotchas: this Nuklear's `nk_label_colored_wrap` has no align parameter;
`nk_layout_row_dynamic(ctx, 0, 1)` fills remaining space. Verified: clean builds
on both toolchains, GUI process alive with empty stderr (shader compiled).

---

## 2026-10-26 (later) — Phase 5: terminal-style renderer, clipboard images, drag & drop

**Terminal-style chat renderer** (`src/gui.cpp`): replaced plain `nk_label` lines
with a custom styled-run renderer. New header-only `src/ansi.h` parses ANSI/SGR
sequences (16/256/truecolor fg+bg, bold; strips cursor/OSC junk) into styled runs;
the renderer tokenizes (words + preserved space runs for indentation), word-waps at
measured pixel widths (UTF-8-safe hard breaks for overlong words) and draws each
token with `nk_draw_text` at absolute coords (`nk_widget_bounds` +
`nk_window_get_canvas`). Diff-style full-line backgrounds when a whole visual line
shares one bg color, per-token backgrounds otherwise. Verified by screenshot: colors,
bold, inline + full-line backgrounds, box-drawing glyphs all render.

**UI font**: JetBrains Mono (OFL) vendored at `data/fonts/jetbrains-mono.ttf`
(license recorded in `data/LICENSES.md`), loaded via
`nk_font_atlas_add_from_memory` (`..._from_file` needs `NK_INCLUDE_STANDARD_IO`)
with built-in-font fallback.

**Input**: multiline `NK_EDIT_BOX | NK_EDIT_SIG_ENTER | NK_EDIT_CTRL_ENTER_NEWLINE`
(Enter sends, Shift+Enter newline), 16 KB buffer for long pastes. Cut/copy/paste work
through the glfw backend clipboard; we override `ctx.clip.paste` so Ctrl+V with no
clipboard text falls back to image attachment.

**Image attachments -> LLM**: `src/clipboard.h/.cpp` reads Win32 `CF_DIB` ->
RGBA -> PNG (stb_image_write, vendored), `base64_encode`; `Conversation::Message`
gained `images` (base64 PNGs) and `add_user(text, images)`; `llm_client` sends
multimodal `content` part arrays (`text` + `image_url` data URLs). Attachment
chips above the input (click removes), **Img** button attaches the clipboard image.

**Drag & drop**: `glfwSetDropCallback` queues paths (mutex), consumed per frame:
image files (stb_image, vendored) become attachments, text files (.txt/.md/.log/
source exts) are inserted into the input field, others refused with a status line.

**Self-verification**: added `KRASTOR_UITEST=1` env hook seeding styled test lines +
clipboard image; screenshot pipeline (PowerShell CopyFromScreen, DPI-aware coords)
used to verify the render. Both toolchains (mingw-debug, msvc-2026-debug) build clean.

**Gotchas**: `nk_text_width` is not public in Nuklear 4.13.3 (use the font
callback); `nk_glfw3_render` buffer args are bytes; comment ending with backslash
swallows the next line; `NK_EDIT_BOX | NK_EDIT_SIG_ENTER` needs `nk_flags` casts.

---

## 2026-10-26 (later) — spectogram plan + config persistence + Ctrl +/- font zoom

**Plan**: `spectogram_plan.md` (repo root, referenced from `project.md` §4.3/§8)
specifies the next Phase 5 features: font zoom, pluggable GLSL background effects
(mic ring-spectrogram as first plugin), continuous hands-free cycle
(mic -> STT -> LLM -> text -> TTS), and exe-folder config persistence.

**Config persistence (plan step D-core)**: `Config::exe_dir()/runtime_path()/default_path()`
— the runtime config now lives **next to the executable** (`bin/config.json`);
a legacy working-directory `config.json` is still loaded and migrated on first
launch. `save()` is atomic (write `.tmp` + `std::filesystem::rename`). New
config sections `gui` (font_size, effect) and `agent` (continuous,
ignore_while_speaking) added to schema + example.

**Font zoom (plan step A)**: JetBrains Mono baked at 12–30 px (step 2) into the
atlas at startup; **Ctrl +/−** (main + numpad) and **Ctrl+0** (reset) edge-detected
in the render loop swap `nk_style_set_font` (in Nuklear 4.13.3 this rescales
every widget — style sub-structs have no own fonts). All fixed row heights
(top bar, chat, input, chips, status) are now proportional to the font height.
The validated size is auto-saved to `bin/config.json` debounced 500 ms.
Verified by screenshot at 18/26 px on both toolchains (live hotkey test left to
the user: synthetic key injection does not reach the GLFW window from this
session).

---

## 2026-10-26 (later) — Phase 5: pluggable GLSL effects + mic ring spectrogram

**Shader plugin system** (plan step B1): new `src/effects.h/.cpp` — background
effects are plain GLSL 330 `.frag` files in `data/shaders/` compiled at runtime
on a shared fullscreen-triangle vertex shader. Fixed uniform contract:
`u_resolution, u_time, u_level, u_spectrum (R8 history texture), u_spec_head,
u_listening, u_speaking`. Display names via `// krastor-effect:` header comment.
Settings modal gets a **Background effect** dropdown; selection is validated by
compiling (a broken plugin never replaces the running one) and persisted to
`gui.effect`. `check_reload()` hot-reloads the active file on change (verified
live by editing the shader while the app ran). Built-in animated gradient kept as
fallback plugin.

**Audio analysis** (plan step B2): `src/fft.h` (header-only radix-2 FFT),
`src/audio_analysis.h/.cpp` SpectrumAnalyzer — 512-sample window -> Hann ->
FFT -> 64 log-spaced bands (dB-compressed, fast attack / slow release) + 128x64
scrolling history ring uploaded as GL_R8 texture each frame. `Stt` gained a
lock-free 16k-sample mic ring buffer (single producer in the miniaudio callback,
release/acquire cursor) + `latest()` for the render thread. When the mic is
closed a `synthetic(t)` feed animates the effects without touching the mic.

**First plugin**: `data/shaders/ring_spectrogram.frag` — circular spectrogram
with woven cyan/magenta mesh ribbons around a ring (reference image style),
reacts to level/listening/speaking states. Verified by screenshot on both
toolchains; live mic reactivity arrives with the hands-free stream (plan step C).

Gotchas: `std::filesystem::last_write_time` returns `file_time_type` (not
`system_clock::time_point`); includes must go before `namespace {` in gui.cpp.

---

## 2026-10-26 (later) — hands-free cycle + effect brightness + agent name + transparency fix

**Hands-free continuous cycle** (plan step C): `Stt` gained stream mode —
`start_stream(device_index, on_utterance)` runs a feeder thread (drain mic ->
VAD 512-windows -> closed segments to callback), `stop_stream()` joins it and
closes the mic, `recognize()` decodes raw PCM on the caller's thread. The GUI
pipeline: feeder callback queues segments -> pipeline worker thread recognizes ->
`heard` queue -> render loop drains and reuses `send()` (single LLM worker
path preserved). Top-bar **Hands-free: ON/OFF** toggle (persisted to
`agent.continuous`, auto-starts at launch); echo guard drops segments while
thinking/speaking when `agent.ignore_while_speaking` (checkbox in Settings).
Verified: auto-start opens the mic and the status line reflects the state; live
speech->reply loop left to the user (no mic access from this session).

**Effect brightness** (user request "spectrogram is very dark"): new uniform
`u_intensity` + Settings slider (0.2-3, debounced auto-save to
`gui.effect_intensity`). Engine wraps every plugin: `#define main
effect_main` + prototype, footer `#undef main` + real `main()` multiplying
rgb — without the `#undef` GLSL reports "already defined" (the footer's main
gets renamed too).

**Root cause of the darkness**: Nuklear paints `style.window.fixed_background`
for EVERY fixed window — the main window's rgba(16,18,24,200) was covering the
shader at 78%. Main window now fully transparent (alpha 0), chat group gets a
translucent pushed `window.background` (alpha 105, via `nk_style_push_color`
— it is an nk_color, not style_item), Settings modal pushes its own opaque
fixed_background. Ring shader re-tuned after the fix removed the dimming: four
thin woven strands (2 cyan + 2 magenta), moderate boost; halo switched from
`pow(h,2.0)` (undefined for negative base!) to `h*h`. Result matches the
reference image.

**Configurable agent name**: `agent.name` (default "Krastor") shown in window
title, top bar, chat prefix and greeting; editable in Settings (Save applies
live via glfwSetWindowTitle).

Gotchas logged: Nuklear fixed_background semantics; GLSL pow(negative) undefined;
main()-wrapper pattern.

---

## 2026-10-26 (later) — barge-in + crash hardening (terminate handler)

**Barge-in** (user request): speaking while the TTS plays now cuts the playback
short. `Tts::interrupt()` sets an atomic cancel flag checked by the playback
wait-loop (`ma_sound_stop` + skips pending synthesis). In the hands-free heard
drain: with the ignore-guard **off**, a heard utterance while speaking calls
`tts.interrupt()` and is processed immediately (chat line "(speech interrupted
playback)"); with the guard on, segments are still dropped as before.

**Crash hardening**: user hit "Debug Error! abort() has been called" when
closing Settings — a bare `abort()` means an uncaught C++ exception on some
thread (terminate), not a Nuklear assert. Could not reproduce headless (no mic
input), so every remaining escape route was sealed: STT VAD feeder loop,
hands-free recognize loop, detached speak thread and both `future::get()`
calls in `poll()` are now try/caught (log + keep running). `main()` installs
a `std::set_terminate` handler that writes the real exception message to
`exe_dir()/krastor_error.log` — if the abort ever recurs, the cause is on disk.

---

## 2026-10-26 — chat UX increment: response stats + context footer (chat_ux_plan Step 1)

Implemented `chat_ux_plan.md` Step 1 (plan file committed with this change):

- `LlmReply` carries `model`/`prompt_tokens`/`completion_tokens`/
  `tokens_estimated`; `llm_client` parses the OpenAI `usage` block and
  falls back to a chars/4 estimate when the server omits it. Elapsed time is
  measured by the async worker lambda in `send()`.
- Each assistant reply gets a dim stats sub-line: `model - time - tok/s`
  (`~` prefix on the rate when estimated; `4.2s` / `11m 30s` formats).
- New setting `llm.context_size` (default 131072) + "Context tokens" field in
  the Settings modal; the status line footer shows `ctx: 5,231 / 131,072 (4%)`
  with thousands separators, amber past 85% (occupancy = last reply's prompt).
- `KRASTOR_UITEST=1` now auto-sends one short message (silent) so a screenshot
  proves both stats render; verified live against LM Studio:
  `minicpm5-2b-heretic-abliterated - 0.3s - 51.8 tok/s`, `ctx: 45 / 131,072 (0%)`.
- Both toolchains build clean.

---

## 2026-10-26 — window size persistence (chat_ux_plan Step 2)

- `gui.window_w`/`gui.window_h` in the config (defaults 900x640); the window
  is created at the saved size, clamped to >= 480x360.
- Resize detection in the render loop arms a 500 ms debounced `cfg.save()`
  (same pattern as the font zoom); a final `cfg.save()` after the loop keeps a
  resize from the last 500 ms before a clean exit.
- Verified end-to-end: config-seeded size restored at startup; Win32-resizing
  the window updated the config after the debounce; clean WM_CLOSE exit works.

---

## 2026-10-26 — chat wheel scrolling with follow-bottom (chat_ux_plan Step 3)

- `AppState::follow_bottom` replaces the per-frame force-pin: while following,
  the chat group is pinned to the newest line as before; a mouse wheel notch over
  the chat releases the pin and scrolls manually (3 text lines/notch, the Nuklear
  scroll delta is consumed so its native 10%-of-height step does not stack).
- Scrolling back down to within 4 px of the computed bottom re-enables follow.
- Wheel injection verified headlessly: `PostMessage(WM_MOUSEWHEEL)` reaches
  GLFW's window proc (the minimize test proved message delivery); the UITEST hook
  bypasses the hover gate because the harness cannot move the real cursor over
  the window. Screenshot after 3 up-notches shows the view scrolled to the top
  with the pin released. UITEST seed now adds 40 filler lines so the panel is
  scrollable.

---

## 2026-10-26 (later) — chat text selection & copy (chat_ux_plan Step 4)

The last step of `chat_ux_plan.md` — the chat panel is now selectable:

- `Word` layout tokens carry `cp_begin` (codepoint offset in the body, -1
  for the role prefix); `tokenize_runs`/`split_wide` propagate it through
  wrapping and wide-word splitting, and `draw_chat_line` records a per-frame
  hit-map (`ChatHitLine`/`ChatSeg`: pixel rect + cp range per token) plus
  paints the translucent accent-blue highlight behind selected runs.
- Virtual document = ANSI-stripped line texts joined by '\n' (rebuilt per
  frame); selection = codepoint offsets (UTF-8-safe for accents/emoji).
- Mouse: click focuses the chat (edit focus released via `nk_edit_unfocus`
  next frame), drag extends, plain click clears, double-click selects the word,
  triple-click the whole source line. Keyboard (chat focused, edge-detected):
  Shift+Left/Right/Up/Down/Home/End, Ctrl+A, Ctrl+C (raw UTF-8 to the
  clipboard, ANSI stripped), Esc clears; PageUp/PageDown/Home/End scroll.
- Input box audit: Nuklear's native mouse-select/Shift+arrows/Ctrl+C-X-V are
  intact — the backend sets `clip.copy` separately and only `clip.paste`
  is overridden by the image-paste hook.
- Verified headlessly: UITEST seeds a selection (screenshot shows the
  highlight across two styled lines) and, with posted WM_KEYDOWN messages,
  Ctrl+A + Ctrl+C put the full ANSI-stripped document (2720 chars, no ESC
  bytes) on the clipboard. Both toolchains build clean.

---

## 2026-10-26 (later) - fix: crash when closing Settings with the X button

Closing the Settings modal via its title-bar X button aborted the process
(MinGW: `Assertion failed: ctx->current->layout`; MSVC: "abort() has been
called", no `krastor_error.log` - so not a C++ exception).

- Root cause: in Nuklear 4.13.3 the X button handler only sets
  `NK_WINDOW_HIDDEN` on the panel (synced to the window at panel end), never
  `NK_WINDOW_CLOSED`. Our `nk_window_is_closed()` check therefore never fired,
  `SettingsState::open` stayed true, and the next frame's `nk_begin("Settings")`
  returned 0 with `win->layout == 0`; the first widget call then tripped
  `NK_ASSERT(ctx->current->layout)` in `nk_row_layout` (nuklear.h:22370).
- Reproduced headlessly with a temporary UITEST hook that called
  `nk_window_show(ctx, "Settings", NK_HIDDEN)` (identical flag effect to the
  X click) - process died with the same assert, confirming the chain.
- Fix in `settings_modal`: check the `nk_begin()` return value; on 0 (hidden),
  `nk_end`, `nk_window_show(..., NK_SHOWN)` to un-hide so the gear button can
  reopen it, pop the pushed style item, set `s.open = false` and return. The
  dead `nk_window_is_closed()` path was removed.
- Verified headlessly: hide -> close -> reopen -> hide cycles ran repeatedly
  with the app surviving; both toolchains build clean. Repro hooks removed.

Follow-up polish: while the Settings modal is open it overlaps the chat, but
Nuklear's `nk_input` is global - clicks/drags/wheel over the modal also hit the
chat group's hover rect and were processed as chat input. `chat_panel` now
skips the wheel-scroll, click and drag-extend branches while
`app.settings.open` (the keyboard selection path already had this gate).

---

## 2026-10-26 (later) - OpenCode-style chat input box

The keyboard input existed but was a cramped 44%-width box sharing its row
with the action buttons. Restyled to match the OpenCode TUI look:

- The input is now a full-width dark field (`NK_EDIT_BOX` with pushed edit
  styles: opaque near-black background, subtle border, light text, blue
  cursor, 10x6 padding) with a 3 px blue accent bar painted over its left
  edge via `nk_fill_rect(&ctx->current->buffer, ...)` after the widget.
- The slot rect is computed manually (4.13.3 has no `nk_window_get_cursor`):
  after `nk_layout_row_begin` the row top is `ctx->current->layout->at_y`,
  the first column starts at `bounds.x + style.window.padding.x`.
- Send / Img / Listen / Test voice moved to their own row (`btn_h`) below
  the box; `chat_h` accounts for the extra row. Behavior unchanged:
  Enter sends, Shift+Enter newline, Ctrl+V image paste hook intact.
- Verified by screenshot; both toolchains build clean.

---

## 2026-10-26 (later) - fix: input box dropped keystrokes (chat hover rect was the whole window)

Typing in the input box barely registered keys: clicking the box activated
the edit for exactly one frame, then our own chat-click handler unfocused it
again (`clear_edit_focus` -> `nk_edit_unfocus`).

- Root cause: in Nuklear 4.13.3 `nk_group_begin` does NOT create a window -
  a group is a panel inside the parent window. `nk_window_get_bounds()`
  inside the chat group therefore returned the MAIN window rect (0,0,w,h),
  so the chat hover check `over` was true for clicks anywhere - input box,
  buttons, top bar. Every input-box click also counted as a chat click.
- Reproduced headlessly with a temporary hook synthesizing `nk_input_button`
  at the box center + posted WM_CHAR: stderr showed the edit activating
  (es=5) then deactivating next frame (es=2), chars lost. After the fix the
  edit stays active (es=1) and all chars land.
- Fix: `gb = ctx->current->layout->bounds` (the group panel's own rect).
  This also makes the wheel-hover gate and the selection click/drag gate
  actually match the visible chat area.
- Also: input caret now 2 px wide accent blue (`edit.cursor_size` push) -
  Nuklear's default 1 px caret was nearly invisible on the dark field.
- Both toolchains build clean; all temporary hooks removed.

---

## 2026-10-26 (later) - llm_models.json: opencode.jsonc-style model registry

LLM endpoints/models now come from `llm_models.json` (project root, same
`provider` schema as opencode.jsonc: `options|settings.baseURL|apiKey`,
`models.<id>.name`, `models.<id>.limit.context|output`; `//` comments
allowed). The Settings modal only picks a model - URL, key and context
window come from the file.

- New `src/llm_models.{h,cpp}`: `LlmModelRegistry` parses the file into
  `provider/model` refs (accepts both `options` and `settings` spellings);
  `apply_model_ref()` overwrites `llm.base_url/api_key/model` (and
  `context_size` when `limit.context` is set). Lookup: exe dir, then cwd.
- `llm.model_ref` added to config.json (persisted selection). Empty ref or
  missing file keeps the old explicit base_url/model path working.
- `llm_models.json` seeded with LM Studio (http://192.168.0.3:8080/v1,
  minicpm5-2b-heretic-abliterated) + the providers from opencode.jsonc
  (ds4, halogen, crucible).
- Settings modal: Base URL / Model / Context-tokens fields removed, replaced
  by one "Model (from llm_models.json)" combo showing the display name,
  with the resolved base URL as a read-only label under it. Save applies
  the ref and rebuilds the LLM client (dirty flag).
- main.cpp applies the ref at startup for both CLI and GUI (prints
  `model: <ref> -> <base_url>`).
- Gotcha: nlohmann 3.12 `ignore_comments` defaults to **false** - jsonc
  files need `json::parse(file, nullptr, true, true)`.
- Verified: CLI startup resolves the lmstudio ref; screenshot of the modal
  shows the combo + endpoint. Both toolchains build clean.

---

## 2026-10-26 (later) - Output formatting: plan + Step 1 Markdown formatter

New plan `output_format_plan.md`: transform raw LLM replies into colored,
formatted chat output (opencode-style) via ANSI-emitting text plugins -
Markdown, syntax highlighting, Thought blocks, color emoji. Key enablers
verified: the glfw backend binds the texture per draw command (emoji atlas
needs no backend patch) and the chat already renders ANSI runs, so formatters
are pure string->string transforms; only the displayed text is styled
(conversation + TTS keep the raw reply).

Step 1 (this commit):
- `src/md_format.{h,cpp}`: Markdown -> ANSI/SGR. Headings (blue/cyan bold),
  **bold**, *italic* (bold+purple), `inline code` (khaki on dark bg), lists
  with orange bullets/numbers, blockquotes (dim + bar), rules, links
  (blue + dim url), column-aligned tables (dim bars, bold header), fenced
  blocks (dim language label + dark background). Input containing ESC bytes
  passes through untouched.
- `src/formatters.{h,cpp}`: `OutputFormatter` chain wrapper (kill-switch
  `gui.format_output`, default true; Step 2 will plug the highlighter in).
- Hooked into the GUI assistant line and the CLI reply print.
- Font atlas now bakes Latin-1 + general punctuation + box-drawing ranges
  (`nk_font_config.range`) - Italian accents and the `| -` bars no longer
  render as "?". (Gotcha: ranges go via `nk_font_config`, not a direct arg.)
- Build presets gained `"jobs": 16` (parallel compile; `env`/`parallel` are
  not valid build-preset fields - `jobs` is).
- Verified: KRASTOR_UITEST seeds a full markdown sample; screenshot shows
  every construct styled correctly. Both toolchains build clean.

## 2026-10-26 (later) - Output formatting Step 2: syntax highlighting

- `src/highlight.{h,cpp}`: `Highlighter` - per-line token scanner (comments/
  strings -> numbers -> identifiers) with state carried across lines for
  triple quotes and block comments; fg-only resets (39) keep the code-block
  background. Colors: keyword purple, type cyan, builtin yellow, string green,
  comment gray, number orange, decorator yellow.
- Language packs are data plugins: `data/languages/*.json` (name, aliases,
  line_comment, block_comment, strings, triples, keywords/types/builtins,
  decorators). Shipped: python, cpp(+c), json, jsonc, bash, powershell,
  javascript(+ts), cmake, ini. Lookup exe_dir()/data/languages then cwd;
  broken files are skipped.
- Wired: `AppState` loads the packs once and plugs a lambda into
  `OutputFormatter::set_highlighter`; same in the CLI. Unknown language tag
  keeps the block unhighlighted.
- Verified: UITEST python block renders print=yellow, 'hello'=green,
  comment=gray on the block background. Both toolchains clean.

## 2026-10-26 (later) - Output formatting Step 3: collapsible Thought blocks

- `llm_client`: `LlmReply::reasoning` filled from the DeepSeek-style
  `reasoning_content` field (LM Studio/vLLM) or `reasoning`; fallback
  extracts inline `` / `<thought>` blocks from content.
- `ChatLine` gains `thinking` / `think_meta` / `thought_open`; the chat
  draws an orange "+ Thought . 5.4s" header (opencode-style) whose click
  rect is recorded per frame (`thought_rects`) and toggles the block
  before any selection logic runs. Body = dim text behind a `|` bar,
  collapsed by default; TTS + conversation history never see reasoning.
- Known limit (v1): the thought body is not selectable (drawn outside the
  selection document).
- Verified: UITEST seeds an expanded Thought block; screenshot matches the
  opencode layout. Both toolchains clean.

## 2026-10-26 (later) - Output formatting Step 4: color emoji

- `tools/gen_emoji_atlas.py` (Pillow): renders ~400 curated emoji from Noto
  Color Emoji CBDT (source .ttf git-ignored, download URL in the docstring)
  into `data/fonts/emoji_atlas.png` (1024x1664, 64 px cells) + manifest json
  (committed). License recorded in data/LICENSES.md.
- `src/emoji.{h,cpp}`: EmojiAtlas loads the sheet into one GL_RGBA texture
  (stb_image - implementation already lived in clipboard.cpp, so no new lib)
  + codepoint->region map. Nuklear 4.13.3 `nk_image` uses pixel `region[]`,
  not UV floats.
- Chat renderer: `draw_token_mixed` splits tokens into text runs
  (nk_draw_text) and emoji quads (nk_draw_image, square advance = line
  height); the glfw backend binds textures per command so batches interleave.
  U+FE0F stripped (was rendering as "?").
- Bonus (from the plan): font atlas now bakes Latin-1/punctuation/box-drawing.
- Verified: UITEST emoji line renders all in color (zoomed screenshot);
  both toolchains clean.

## 2026-10-26 (later) - Stop button: instant LLM request + TTS cancellation

- `http_client`: new `CancelToken` (atomic flag + atomically exchanged
  platform handle + closer fn). `https_post(..., CancelToken*)`:
  - WinHTTP registers the request handle with the token; `cancel()` from the
    UI thread closes it cross-thread (the documented way to abort a
    synchronous call), so the pending send/receive fails immediately. A
    RequestGuard + atomic exchange guarantee the handle is closed exactly
    once whichever thread wins the race.
  - libcurl: `CURLOPT_XFERINFOFUNCTION` progress callback polls the flag
    (~100 ms granularity) -> CURLE_ABORTED_BY_CALLBACK -> error "cancelled".
- `LlmClient::complete(conversation, cancel)` passes the token through.
- GUI: **Stop button** in the action row (Send/Img/Listen/Test voice/Stop).
  `AppState::stop()` cancels the in-flight request (sys line "(stopped)")
  and always calls `tts.interrupt()` (no-op when silent). `poll()` detects
  the cancelled token and discards the aborted reply instead of printing an
  LLM error; the user turn stays in the conversation history.
- Verified: standalone test against a TCP listener that accepts but never
  responds -> https_post returned 0 ms after cancel() with error
  "cancelled". Screenshot shows the new button row. Both toolchains clean.

## 2026-10-26 (later) - FIX: LLM reply hang (infinite loop in extract_inline_thinking)

Symptom: the request reached LM Studio and the model answered, but the app
stayed on "thinking..." forever (CLI hung identically, so the bug was in the
shared path, not the GUI).

Root cause: `extract_inline_thinking` (Step 3) had an EMPTY string as its
first open/close tag pair (the special-token text was lost when the file was
written). `content.find("")` always succeeds at `pos` without advancing, the
`erase` removed zero chars and `pos` never moved -> infinite loop inside
`complete()` right after the HTTP response arrived.

- Rewrote the function over well-formed tags only: `<think>`, `<thinking>`,
  `<thought>` (open/close built from the tag name, so an empty delimiter is
  impossible by construction).
- Hardened the same bug class in `highlight.cpp`: empty `strings`/`triples`
  entries in a language pack would spin the scanner the same way; they are
  now skipped at load time.
- Verified: CLI round-trip against LM Studio returns the reply and exits;
  both toolchains clean.

## 2026-10-26 (later) - Code-block aesthetics + settings modal spacing

- Code blocks render as one continuous rectangle now:
  - interior blank lines carried no colored token, so the renderer painted
    no background and the shader showed through as a gap - `append_block_bg`
    now emits one space so the full-line background stays continuous;
  - the fence language label gets the block background too, becoming a dim
    full-width header bar attached to the block.
  - UITEST seeds the exact python script from the bug report (interior
    blank line) as a regression sample; pixel-probed screenshots confirm
    continuous (24,26,33) coverage.
- Settings modal: window `spacing.y` pushed 4->8 px inside the modal
  (label rows slimmed 26->18 px to compensate), so field groups no longer
  collide; content still fits the 620 px modal without a scrollbar
  (10 px was tested and overflowed).
- Language packs already cover the requested set via aliases: bash (sh/
  shell/zsh), python (py/python3), javascript (js/ts/typescript),
  cpp (c/c++/cc/cxx). Both toolchains clean.

## 2026-10-26 (later) - Config file confusion resolved

- Diagnosis: the Settings modal *was* loading config correctly - the app
  loads bin/config.json (next to the exe, see Config::default_path), while
  the user had edited the legacy repo-root config.json, which is only used
  as a fallback when bin/config.json does not exist.
- Merged the user's edits (Italian system prompt, max_tokens 64000,
  api_key) from the root config.json into bin/config.json.
- main.cpp now prints "config: <path>" at startup so the loaded file is
  always visible. Verified via UITEST screenshot: modal shows the merged
  values. Both toolchains clean.

## 2026-10-26 (later) - OpenCode-style markdown tables

- Tables now render as a full box-drawing grid like the OpenCode TUI:
  top/bottom borders, a rule between every row, violet header text and
  one space of padding on both sides of each cell.
- Column widths are measured on the *rendered* cell text (new `vis_cp`
  helper strips ANSI escapes): previously the raw markdown width (with
  backticks) was used, so inline-code cells desynchronized the `|` bars.
- GFM column alignment honored: the separator row's colons (`:---`,
  `---:`, `:---:`) left/right/center-pad every cell in the column.
- UITEST seeds a 3-column table (inline-code first column + centered
  last column) as the regression sample. Both toolchains clean.

## 2026-10-26 (later) - FIX: mouse-wheel chat scrolling

- Symptom: the wheel barely moved the chat and never accumulated; keyboard
  PageUp/PageDown were equally broken.
- Root cause: inside a Nuklear 4.13.3 group, ctx->current is still the MAIN
  window (groups are panels, not windows), so nk_window_get_scroll() read the
  main window's scrollbar.y (always 0) while nk_group_set_scroll() wrote the
  group's real offset in the parent window's value store. Every wheel notch
  therefore computed 0 + 3 lines and the view never accumulated past ~84 px;
  the follow-bottom re-enable check (sy >= bottom-4) could also never fire.
- Fix: read the offset with nk_group_get_scroll (same value store as the
  setter) in the wheel handler, the keyboard scroll commands and the
  follow-bottom check. Verified by injecting WM_MOUSEWHEEL messages into the
  UITEST build: 25 down notches now reach the bottom, 3 up notches stay put
  (no snap-back). Both toolchains clean.

## 2026-10-26 (later) - Floating action buttons replace the button row

- The full-width Send/Img/Listen/Test voice/Stop row is gone: the chat
  gained that vertical space back.
- New floating cluster in the lower-right corner (above the input box):
  five icon-only micro buttons drawn as color emoji-atlas sprites via
  nk_button_image in a borderless Nuklear window begun after the main
  window (so it floats on top): send 🚀, attach image 📎, listen 👂,
  test voice 🎵, stop ❌. Each shows a tooltip (tiny popup window) on
  hover; send/listen dim while the worker is busy; text fallback labels when the emoji atlas is missing.
- Enter-to-send, attach/stop/send/listen/voice actions unchanged, just relocated. Both toolchains clean.

## 2026-10-26 (later) - FIX: floating action buttons now actually receive clicks

- The cluster added above was drawn in a separate borderless Nuklear window
  begun after the full-size main window. In 4.13.3 only the topmost window
  gets input (every other window is forced to NK_WINDOW_ROM), and the main
  window always overlaps the child - so the fab clicks never registered and
  the main window widgets were read-only. The ❌ Stop button therefore never
  silenced the TTS.
- Rewritten without extra windows: the five buttons are drawn directly into
  the main window's command buffer at absolute rects via the internal
  nk_do_button_image / nk_do_button_text (NK_LIB, available because gui.cpp
  defines NK_IMPLEMENTATION). Tooltip is a manually drawn rounded bubble in
  the same buffer. The chat selection handler ignores clicks inside the
  strip (AppState::fab_rect guard).
- Verified end-to-end by temporarily injecting a synthetic mouse click into
  ctx->input on the Send slot: with the input cleared the send fired; the
  injection code was removed afterwards. Both toolchains clean.

## 2026-10-29 - SVG UI icons (LunaSVG git submodule)

- The floating action buttons now draw real vector icons instead of color
  emoji. New `data/icons/` holds 5 Lucide SVGs (ISC): send, paperclip, ear,
  volume-2, square - stroke-based, a few KB total.
- **LunaSVG 3.5.0** (MIT) enters as a **git submodule** (`libs/lunasvg`,
  pinned tag v3.5.0): only the URL + commit SHA live in the repo, no library
  sources in git. plutovg is vendored inside the lunasvg repo itself, so no
  nested submodule. Built as a static lib (examples off, system-font scan off,
  external-resource loading off). Chosen over nanosvg (the original plan):
  nanosvg is unmaintained and cannot rasterize strokes.
- Dependency policy recorded in `libs/development.md`: full-source libraries
  -> git submodule; prebuilt binaries -> download_deps.py; single headers ->
  vendored. `scripts/build.ps1`/`build.sh` auto-run
  `git submodule update --init --recursive` when the submodule is missing;
  CMake aborts with the same hint. Fixed the broken msvc preset mapping in
  build.ps1 on the way (msvc2026 -> msvc-2026-<cfg>).
- **Isolation**: `src/icons.h/.cpp` (`IconAtlas`) is the ONLY translation
  unit that includes `lunasvg.h`. It rasterizes every `*.svg` in the
  folder into one white/alpha GL sprite atlas (64 px cells, deterministic
  sorted layout); `currentColor` is rewritten to white before parsing so
  icons tint at draw time. `gui.cpp` sees just the IconAtlas API - same
  pattern as EmojiAtlas. FAB render chain: SVG atlas -> emoji atlas -> text.
- **Tests**: `krastor --icon-test` headless mode (no GL): rasterizes the
  folder, asserts every icon's ink coverage is in 2%..75% (not blank, not a
  filled square), dumps `icon_atlas_test.png` for visual inspection - all
  5 icons PASS. GUI screenshot tests: SVG icons crisp on the FAB strip;
  renaming `data/icons` degrades correctly to the emoji atlas.
  Both toolchains clean.
