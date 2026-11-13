# Krastor

A desktop LLM agent with voice in and voice out, written in **C++20** — no
Electron, no Python runtime, no cloud services of its own. You bring an
OpenAI-compatible LLM endpoint (LM Studio, Ollama, or any `/v1/chat/completions`
API); everything else — speech recognition, text-to-speech, sessions, tools —
runs locally on your machine.

The window is a real-time rendered surface: a GLSL shader background that
reacts to the assistant's state, with a terminal-style chat on top.

> Status: v0.4.x, Windows 10/11 x64 (Linux on the roadmap). Actively developed.

## Features

**Voice, local and bilingual (EN + IT)**
- 🎤 STT via Sherpa-onnx (Parakeet multilingual auto-detect EN/IT, Whisper/Vosk options)
- 🔊 TTS via Piper voices, spoken replies with barge-in (talk over Krastor to cut it short)
- 🎧 Hands-free continuous cycle: mic → VAD → STT → LLM → text → TTS, with echo guard
- 🧹 RNNoise mic denoise + speexdsp echo cancellation

**Terminal-style chat GUI (GLFW + Nuklear)**
- Markdown rendering: headings, tables with box borders, code blocks with
  modular syntax highlighting (JSON language packs: code, markup, yaml,
  markdown modes), Thought blocks, color emoji
- Full ANSI/SGR support: the agent can print colored output and ANSI art
  (16/256/truecolor), line-level passthrough keeps markdown around it intact
- Text selection & copy (raw UTF-8), follow-bottom scrolling, Ctrl±0 font zoom,
  input history (Up/Down) + line-kill keys (Ctrl+K, Ctrl+Shift+K),
  key repeat speed configurable in Settings (`gui.key_repeat_speed`),
  drag & drop and Ctrl+V attachments (images render inline, sounds play
  in an inline mini player with transport, volume and mute)
- Per-response stats (model · time · tok/s) and a live context-usage footer;
  generous configurable LLM request timeout (`llm.timeout_s`) for slow
  big-context local servers

**GLSL shader backgrounds with a real event feed**
- Pluggable `.frag` plugins, hot-reload, compile-validated, "off" choice
- Uniforms carry the whole assistant state: mic level + spectrum ring,
  listening, speaking, **LLM-thinking**, key-press pulses

**Agent tools (permission-gated)**
- `files` (read/write/edit/delete with side-by-side diffs in chat),
  `webfetch`, `websearch`, `python` (sandboxed venv, last resort)
- Allow-once / always-allow / deny modal, path sandbox, per-tool manifests

**Sessions**
- One folder per session (append-only JSONL + attachments), GUI manager,
  opencode-style slash commands, LLM-powered `/compact` with auto-compact
  and context-overflow retry

## Quick start (Windows)

```powershell
# 1. clone (submodules: lunasvg, rnnoise, speexdsp)
git clone --recursive git@github.com:penguindark/krastor.git
cd krastor

# 2. external binaries (sherpa-onnx + RNNoise weights, ~40 MB)
python tools\download_deps.py

# 3. voice models (optional; the chat works without them)
python tools\download_models.py --only espeak-ng-data
python tools\download_models.py --only tts-piper-it-it-paola
python tools\download_models.py --only vad-silero
python tools\download_models.py --only stt-parakeet-multilingual

# 4. build (MinGW GCC 16 or MSVC 2022/2026, CMake presets)
.\scripts\build.ps1 -Toolchain mingw -Config Release

# 5. configure + run  (from the repo root!)
copy config.example.jsonc bin\config.jsonc
# edit bin\llm_models.jsonc with your LLM endpoint (comments allowed)
.\bin\krastor.exe --gui
```

Prerequisites: Git, CMake 4.x, a C++20 compiler (MinGW-w64 GCC **or** Visual
Studio with Desktop C++), Python 3 (download scripts only), an OpenGL 3.3 GPU.
Full walkthrough: [`COLD_START.md`](COLD_START.md).

## Headless self-tests

No mocks — the test modes exercise the real subsystems:

```powershell
.\bin\krastor.exe --tool-test      # tools, permissions, diffs, compactor
.\bin\krastor.exe --session-test   # session store, switching, compaction
.\bin\krastor.exe --media-test     # image/sound blocks, fit-inside, split
.\bin\krastor.exe --gui-test       # modal/window-stacking regression
.\bin\krastor.exe --tts-filter-test
.\bin\krastor.exe --fx-test        # mic denoise / echo cancel
```

## Tech stack

| Area | What |
|---|---|
| Language | C++20, dual toolchain (MinGW GCC + MSVC) on every commit |
| GUI | GLFW 3 + Nuklear (immediate mode), JetBrains Mono, LunaSVG icons |
| Graphics | OpenGL 3.3, pluggable GLSL fragment-shader backgrounds |
| Voice | sherpa-onnx (STT), Piper (TTS), Silero VAD, RNNoise, speexdsp |
| LLM | any OpenAI-compatible `/v1/chat/completions` endpoint |
| Bundled libs | GLFW, Nuklear, glad, nlohmann/json, miniaudio, stb (submodules: lunasvg, rnnoise, speexdsp) |

## Documentation

- [`project.md`](project.md) — vision, architecture, roadmap
- [`development_log.md`](development_log.md) — chronological development history
- [`COLD_START.md`](COLD_START.md) — clone-to-running guide (IT)
- [`AGENT.md`](AGENT.md) - entry point + index of folder-level docs

## License

Licensed under the **Apache License 2.0** — free to use, modify and
redistribute (even commercially), as long as the copyright and attribution
notices are kept.

**"Krastor" is a trademark of Dario Deledda.** Apache-2.0 Section 6 does not
grant any trademark rights: you may describe the origin of the code, but
forks and redistributed derivatives must use a different name. See
[LICENSE](LICENSE) for details.
