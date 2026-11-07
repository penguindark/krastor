# tools/ — python prototypes & scripts

Development doc for this folder (see `../project.md`).

## Purpose
Python prototypes (model evaluation, downloaders, data prep) supporting the C++
product. Never part of the shipped agent.

## Environment
- Virtual env at `../.venv`, created from `C:\Python313\python.exe` (3.13.1).
- Activate: `..\.venv\Scripts\Activate.ps1` (from this folder) or call
  `.venv\Scripts\python.exe` explicitly. Never use the PATH python (3.11).
- `.venv` is git-ignored. Install deps: `.venv\Scripts\python.exe -m pip install -r tools\requirements.txt`

## Scripts
- `download_models.py` — downloads all STT/TTS models into `../models/` (idempotent,
  generates a README per model with source/license/files).
  `.venv\Scripts\python.exe tools\download_models.py [--only <name>]`
- `parrot.py` — **the Phase 2 parrot**, two engines:
  - `--engine vosk --lang it|en` — true streaming: word-level live partials while
    speaking (one language per run).
  - `--engine parakeet` — near-live: Silero VAD segments the mic stream, NVIDIA
    Parakeet TDT v3 transcribes each phrase (EN+IT auto-detect, punctuation).
  Both pipe the final text to the Piper TTS voice and play it back.
  `.venv\Scripts\python.exe tools\parrot.py --engine parakeet [--no-play] [--list-devices]`
- `roundtrip_test.py` — automated no-mic test: TTS -> WAV -> chunked streaming STT ->
  compare. Writes samples to `tools/out/` (git-ignored) for listening.
  `.venv\Scripts\python.exe tools\roundtrip_test.py`
- `bench_stt.py` — benchmark multilingual STT candidates (Whisper small vs Parakeet
  TDT v3) on the round-trip WAVs: accuracy + RTF.
  `.venv\Scripts\python.exe tools\bench_stt.py`
- `bench_gpu.py` - CPU vs CUDA benchmark (faster-whisper): small int8 CPU/CUDA and
  large-v3-turbo CUDA. Needs `tools/requirements-gpu.txt` installed.
  `.venv\Scripts\python.exe tools\bench_gpu.py`
- `gen_emoji_atlas.py` - renders the curated emoji set from Noto Color Emoji
  (CBDT, git-ignored source font; download URL in the docstring) into
  `data/fonts/emoji_atlas.png` + `emoji_atlas.json` (committed). Needs Pillow.
  `.venv\Scripts\python.exe tools\gen_emoji_atlas.py`

## Agent tools system (Phase 6, 2026-11-07)
Since Phase 6 this folder is ALSO the runtime tools folder of the agent (see
`../tools_plan.md`): first-level `tool_<name>.jsonc` manifests + the tool
executables (built by CMake into `tools/`, git-ignored like all `*.exe`).

| Tool | Manifest | Executable | What it does |
|---|---|---|---|
| `files` | `tool_files.jsonc` | `filetool.exe` (`filetool/filetool.cpp`) | list/stat/read (byte range)/write/edit/append/rename/mkdir/delete; UTF-8 paths; binary sniff. `delete` is permanent and **always asks** (manifest `ask_actions`, never silenced by "Always allow"); directories need `recursive:true` unless empty. `edit`/`write` results carry a structured line `diff` (LCS with prefix/suffix trim, 3-line context hunks, capped at 600 ops) for the GUI side-by-side renderer |
| `webfetch` | `tool_webfetch.jsonc` | `webfetch.exe` (`webfetch/webfetch.cpp`) | GET a http(s) URL, HTML -> readable text (scripts/styles stripped, entities decoded); refuses binary and non-http schemes |
| `websearch` | `tool_websearch.jsonc` | `websearch.exe` (`websearch/websearch.cpp`) | web search, no API key: **Bing first** (b_algo blocks, `ck/a?u=a1<base64>` unwrapped), DuckDuckGo html/lite as fallbacks - DDG answers an anti-bot challenge from many IPs. Returns title+URL+snippet per hit |
| `python` | `tool_python.jsonc` | `python/venv/Scripts/python.exe` + `python/runner.py` | runs inline `code` or `script`+`args` in the agent venv; every run asks; the manifest description marks it the **LAST RESORT** tool (files/webfetch first - the model kept spawning python for plain file reads) |

Shared HTML helpers (entity decode, HTML->text, title/fragment extraction,
URL decode) live in `html_util.h` (header-only, used by webfetch+websearch).

- `python/venv/` is the agent's **dedicated** virtualenv (git-ignored), created
  by `python/setup.ps1` (uses PATH python 3.11, unlike the dev `.venv` on
  3.13); packages in `python/requirements.txt` (requests, beautifulsoup4).
  `runner.py` speaks the tool protocol: JSON stdin -> `{"ok":...}` stdout,
  user-code stdout/traceback captured inside the envelope, refuses to run
  without Krastor's `_approved` stamp.
- Protocol + permission model (sandbox `~/Documents/KrastorWorkspace`,
  `deny_always` floors re-checked by the tools themselves): see
  `../tools_plan.md` and `../src/development.md`.
- Debug without the LLM: `..\bin\krastor.exe --tool-run files '{"action":"list","path":"."}'`
  (pwsh: single-quote the JSON, no backslash escapes); full suite:
  `..\bin\krastor.exe --tool-test`. `FILETOOL_DEBUG=<logfile>` makes filetool
  log its raw stdin.

## Current state (Phase 2, 2026-10-25)
Stack chosen: **Vosk** streaming STT + **Piper via sherpa-onnx** TTS (see
`../models/development.md` for the model inventory).

Round-trip results (TTS voice -> Vosk):
- EN (small model): near-perfect (only case/punctuation differences).
- IT (small model): understandable but word errors ("volpe rapida" -> "vuoi perà da").
- IT (big model vosk-model-it-0.22): near-perfect ("la volpe rapido... salta sopra il
  cane pigro", "ciao sono il tuo assistente vocale locale"), RTF ~0.19 -> **chosen as
  default Italian STT**; small model kept as `--small` fallback.

## Gotchas learned
- sherpa-onnx **2.0.0 removed** the built-in espeak-ng frontend for Piper VITS voices
  (needs external phonemizer). We pin `sherpa-onnx==1.13.8` which still bundles it.
- This Vosk build (0.3.45) **silently ignores float32 PCM**; feed int16 bytes.
- Piper voices from sherpa-onnx releases have no `lexicon.txt` (espeak-ng path) —
  pass empty lexicon + `data_dir=models/espeak-ng-data`.

## Planned
- `stt_eval.py` — WER comparison small vs large Vosk IT model (pending).
