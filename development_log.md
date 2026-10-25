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
