# models/ — local AI models (folder is git-ignored)

Development doc for this folder (see `../project.md` section 6).

## Purpose
Local STT and TTS models (English + Italian). The folder itself is **git-ignored**;
weights never enter the repository.

## Policy
- One subfolder per model, named `<role>-<name>`.
- Each model subfolder contains a generated `README.md` recording: source URL,
  license, file list, intended use. Downloaded via `../tools/download_models.py`.

## Inventory (Phase 2, 2026-10-25)

| Folder | Role | Model | License | Notes |
|---|---|---|---|---|
| `stt-vosk-it/` | STT streaming | vosk-model-small-it-0.4 (~41 MB) | Apache-2.0 | word-level partials; accuracy OK, upgrade candidate: vosk-model-it-0.42 |
| `stt-vosk-en/` | STT streaming | vosk-model-small-en-us-0.15 (~40 MB) | Apache-2.0 | word-level partials; near-perfect in round-trip test |
| `tts-piper-it-it-paola/` | TTS Italian | Piper it_IT paola (medium) | BSD-3 / CC-BY | via sherpa-onnx VITS |
| `tts-piper-en-us-lessac/` | TTS English | Piper en_US lessac (medium) | BSD-3 / CC-BY | via sherpa-onnx VITS |
| `espeak-ng-data/` | TTS frontend | espeak-ng data (shared G2P) | GPL-3 | required by Piper voices on sherpa-onnx 1.x |

## Research notes (why these)
- sherpa-onnx streaming zipformer has **no Italian** model; Kokoro port in
  sherpa-onnx is zh+en only -> not usable for Italian.
- Vosk provides true streaming (partial words while speaking) for IT and EN with a
  C API -> direct path to the C++ integration (Phase 4).
- Piper has native `it_IT` voices (paola/miro/dii/riccardo) runnable through
  sherpa-onnx, which also has a C++ API.
- sherpa-onnx pinned to 1.13.8: 2.0.0 dropped the bundled espeak-ng frontend.

## Candidate upgrades (not downloaded)
- `vosk-model-it-0.42` (~450 MB) — better Italian accuracy, same streaming API.
- `vits-piper-it_IT-miro-high` / `-dii-high` — higher-quality Italian voices.
