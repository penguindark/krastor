"""Benchmark multilingual STT candidates (EN+IT auto-detect) on the round-trip WAVs.

Compares Whisper small (int8) and Parakeet TDT 0.6B v3 (int8) — both single models
covering English and Italian with automatic language detection — on the TTS-generated
samples in tools/out/ (run tools/roundtrip_test.py first to generate them).

Usage (from repo root):
    .venv\\Scripts\\python.exe tools\\bench_stt.py
"""

from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import sherpa_onnx
import soundfile as sf

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = REPO_ROOT / "tools" / "out"
NUM_THREADS = 8

WAVS = ("roundtrip_en_0.wav", "roundtrip_en_1.wav",
        "roundtrip_it_0.wav", "roundtrip_it_1.wav")


def bench(name: str, recognizer, wavs: tuple[str, ...]) -> None:
    print(f"\n=== {name} ===")
    total_audio = total_time = 0.0
    for f in wavs:
        path = OUT_DIR / f
        if not path.exists():
            print(f"[skip] {f} missing — run roundtrip_test.py first")
            return
        samples, rate = sf.read(path)
        stream = recognizer.create_stream()
        start = time.time()
        stream.accept_waveform(rate, samples.astype(np.float32))
        recognizer.decode_stream(stream)
        dt = time.time() - start
        total_audio += len(samples) / rate
        total_time += dt
        print(f"  {f}: {dt:.2f}s for {len(samples)/rate:.1f}s audio")
        print(f"    -> {stream.result.text.strip()}")
    print(f"  RTF: {total_time/total_audio:.2f} ({total_audio/total_time:.1f}x realtime)")


def main() -> None:
    whisper = REPO_ROOT / "models" / "stt-whisper-small"
    bench("Whisper small int8 (99 langs, auto-detect)",
          sherpa_onnx.OfflineRecognizer.from_whisper(
              encoder=str(whisper / "small-encoder.int8.onnx"),
              decoder=str(whisper / "small-decoder.int8.onnx"),
              tokens=str(whisper / "small-tokens.txt"),
              language="", num_threads=NUM_THREADS, provider="cpu"),
          WAVS)

    parakeet = REPO_ROOT / "models" / "stt-parakeet-multilingual"
    bench("Parakeet TDT 0.6B v3 int8 (25 langs incl EN+IT, auto-detect)",
          sherpa_onnx.OfflineRecognizer.from_transducer(
              encoder=str(parakeet / "encoder.int8.onnx"),
              decoder=str(parakeet / "decoder.int8.onnx"),
              joiner=str(parakeet / "joiner.int8.onnx"),
              tokens=str(parakeet / "tokens.txt"),
              num_threads=NUM_THREADS, provider="cpu",
              model_type="nemo_transducer"),
          WAVS)


if __name__ == "__main__":
    main()
