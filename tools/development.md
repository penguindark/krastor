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
- `parrot.py` — **the Phase 2 parrot**: microphone -> streaming STT (Vosk, word-level
  partial results shown live) -> TTS (sherpa-onnx Piper) -> speaker.
  `.venv\Scripts\python.exe tools\parrot.py --lang it|en [--no-play] [--list-devices]`
- `roundtrip_test.py` — automated no-mic test: TTS -> WAV -> chunked streaming STT ->
  compare. Writes samples to `tools/out/` (git-ignored) for listening.
  `.venv\Scripts\python.exe tools\roundtrip_test.py`

## Current state (Phase 2, 2026-10-25)
Stack chosen: **Vosk** streaming STT + **Piper via sherpa-onnx** TTS (see
`../models/development.md` for the model inventory).

Round-trip results (TTS voice -> Vosk small models):
- EN: near-perfect (only case/punctuation differences).
- IT: understandable but the *small* Italian model makes errors on some words
  ("volpe rapida" -> "vuoi perà da"). Upgrade candidate: `vosk-model-it-0.42`
  (~450 MB, much better accuracy, still streaming) — decision pending user listening test.

## Gotchas learned
- sherpa-onnx **2.0.0 removed** the built-in espeak-ng frontend for Piper VITS voices
  (needs external phonemizer). We pin `sherpa-onnx==1.13.8` which still bundles it.
- This Vosk build (0.3.45) **silently ignores float32 PCM**; feed int16 bytes.
- Piper voices from sherpa-onnx releases have no `lexicon.txt` (espeak-ng path) —
  pass empty lexicon + `data_dir=models/espeak-ng-data`.

## Planned
- `stt_eval.py` — WER comparison small vs large Vosk IT model (pending).
