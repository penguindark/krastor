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
| `stt-vosk-it-large/` | STT streaming | vosk-model-it-0.22 (~1.2 GB) | Apache-2.0 | **default IT**: WER 8.10 cv-test, RTF ~0.19 on dev machine |
| `stt-vosk-it/` | STT streaming | vosk-model-small-it-0.4 (~41 MB) | Apache-2.0 | fast fallback (`--small`), noticeably worse accuracy |
| `stt-vosk-en/` | STT streaming | vosk-model-small-en-us-0.15 (~40 MB) | Apache-2.0 | word-level partials; near-perfect in round-trip test |
| `tts-piper-it-it-paola/` | TTS Italian | Piper it_IT paola (medium) | BSD-3 / CC-BY | via sherpa-onnx VITS |
| `tts-piper-en-us-lessac/` | TTS English | Piper en_US lessac (medium) | BSD-3 / CC-BY | via sherpa-onnx VITS |
| `stt-whisper-small/` | STT multilingual | sherpa-onnx-whisper-small (~640 MB int8) | BSD-3/MIT | 99 langs auto-detect; RTF ~0.6 CPU; IT accuracy weak |
| `stt-parakeet-multilingual/` | STT multilingual | NVIDIA Parakeet TDT 0.6B v3 int8 (~650 MB) | CC-BY-4.0 | **25 langs incl EN+IT auto-detect, punctuation+capitalization, RTF ~0.15** — best single-model candidate |
| `espeak-ng-data/` | TTS frontend | espeak-ng data (shared G2P) | GPL-3 | required by Piper voices on sherpa-onnx 1.x |

## Hardware acceleration (dev machine, checked 2026-10-26)
i7-11800H (8C/16T), 64 GB RAM, **RTX 3080 Laptop (CUDA available)**. All chosen
models run faster than real-time on CPU alone; CUDA (onnxruntime-gpu / sherpa-onnx
CUDA build) is a later optimization lever, not a requirement.

## STT benchmark (tools/bench_stt.py, TTS-generated EN+IT samples, CPU 8 threads)

| Model | EN | IT | RTF | Streaming |
|---|---|---|---|---|
| Vosk big it-0.22 | needs separate model | near-perfect | 0.19 | word-level partials |
| Whisper small int8 | perfect | weak (garbled endings) | 0.6 | VAD near-live only |
| Parakeet TDT v3 int8 | perfect + punctuation | good (minor artifacts on TTS voice) | **0.15** | VAD near-live (~0.4 s after phrase) |

Web research (2026): Nemotron 3.5 ASR Streaming (40 locales incl IT, true streaming,
RTF ~0.14 CPU) and Mistral Voxtral Mini 4B Realtime (13 langs, IT WER 3.3%, 240 ms
delay) are the state-of-the-art streaming options; neither is confirmed to run on
sherpa-onnx yet — revisit when C++ support lands.

## Research notes (why these)
- sherpa-onnx streaming zipformer has **no Italian** model; Kokoro port in
  sherpa-onnx is zh+en only -> not usable for Italian.
- Vosk provides true streaming (partial words while speaking) for IT and EN with a
  C API -> direct path to the C++ integration (Phase 4).
- Piper has native `it_IT` voices (paola/miro/dii/riccardo) runnable through
  sherpa-onnx, which also has a C++ API.
- sherpa-onnx pinned to 1.13.8: 2.0.0 dropped the bundled espeak-ng frontend.

## Candidate upgrades (not downloaded)
- `vits-piper-it_IT-miro-high` / `-dii-high` — higher-quality Italian voices.
