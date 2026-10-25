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
