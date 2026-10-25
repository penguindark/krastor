# models/ — local AI models (folder is git-ignored)

Development doc for this folder (see `../project.md` section 6).

## Purpose
Local STT and TTS models (English + Italian). The folder itself is **git-ignored**;
weights never enter the repository.

## Policy
- One subfolder per model, named `<role>-<name>`.
- Each model subfolder contains a generated `README.md` recording: source URL,
  license, file list, intended use. Downloaded via `../tools/download_models.py`.

## Inventory (pruned 2026-10-26 — currently on disk)

| Folder | Role | Model | License | Notes |
|---|---|---|---|---|
| `stt-parakeet-multilingual/` | STT multilingual | NVIDIA Parakeet TDT 0.6B v3 int8 (~650 MB) | CC-BY-4.0 | **chosen single EN+IT model**: auto-detect, punctuation+capitalization, RTF ~0.15 |
| `stt-vosk-it-large/` | STT streaming | vosk-model-it-0.22 (~1.2 GB) | Apache-2.0 | word-level live partials (live A/B test vs Parakeet pending) |
| `stt-vosk-en/` | STT streaming | vosk-model-small-en-us-0.15 (~40 MB) | Apache-2.0 | word-level partials (live A/B test vs Parakeet pending) |
| `tts-piper-it-it-paola/` | TTS Italian | Piper it_IT paola (medium) | BSD-3 / CC-BY | via sherpa-onnx VITS |
| `tts-piper-en-us-lessac/` | TTS English | Piper en_US lessac (medium) | BSD-3 / CC-BY | via sherpa-onnx VITS |
| `vad-silero/` | VAD | silero_vad.onnx (~2 MB) | BSD-3 | segments mic audio for Parakeet near-live mode |
| `espeak-ng-data/` | TTS frontend | espeak-ng data (shared G2P) | GPL-3 | required by Piper voices on sherpa-onnx 1.x |

## Pruned from disk (test results kept in this file + development_log.md;
re-download anytime with `tools/download_models.py --only <name>`)

| Folder | Model | Why pruned | Result on record |
|---|---|---|---|
| `stt-whisper-small/` | Whisper small int8 | benchmark only; Italian weak | RTF 0.6 CPU / 0.13 CUDA; IT garbled ("su pezcan epiglio") |
| `hf-cache/` | faster-whisper cache (small, large-v3-turbo) | GPU benchmark artifacts | large-v3-turbo CUDA: perfect IT at RTF 0.32 — documented GPU upgrade path |
| `stt-vosk-it/` | vosk-model-small-it-0.4 | superseded by `stt-vosk-it-large` | word errors ("vuoi perà da"); `parrot.py --small` needs re-download |

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

## GPU benchmark (RTX 3080, tools/bench_gpu.py, 2026-10-26)

| Config | RTF | Italian quality |
|---|---|---|
| Whisper small int8 / CPU | ~0.55 | weak ("su pezcan epiglio") |
| Whisper small int8 / CUDA | ~0.13 | same model -> same weak IT |
| **Whisper large-v3-turbo fp16 / CUDA** | ~0.32 | **perfect** ("La volpe rapida marrone salta sopra il cane pigro.") |

Takeaways:
- GPU speeds Whisper ~4x on the same model, but its real value is **unlocking
  large-v3-turbo**: perfect EN+IT with punctuation at ~3x realtime — impossible on CPU.
- Parakeet needs no GPU: already RTF 0.15 on CPU; perceived latency is dominated by
  VAD endpointing (~0.5 s silence wait), so GPU gains would be imperceptible.
- C++ constraint: CUDA does not support MinGW — a GPU path means MSVC + onnxruntime
  CUDA (or DirectML). Keep CPU inference as the default; design the STT interface so
  a GPU provider can be added later.

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
