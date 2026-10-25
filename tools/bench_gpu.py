"""GPU vs CPU STT benchmark (faster-whisper / CTranslate2, CUDA on RTX 3080).

Measures what hardware acceleration buys for Whisper on the round-trip WAVs:
- whisper small int8 on CPU vs CUDA (same model, speed only)
- whisper large-v3-turbo float16 on CUDA (what the GPU *unlocks* quality-wise)

Parakeet is not included: sherpa-onnx has no CUDA wheel for Python 3.13 and the
model already runs at RTF 0.15 on CPU (GPU gain would be imperceptible because
VAD endpointing dominates perceived latency).

Usage (from repo root, after tools/download_models.py and roundtrip_test.py):
    .venv\\Scripts\\python.exe -m pip install -r tools\\requirements-gpu.txt
    .venv\\Scripts\\python.exe tools\\bench_gpu.py
"""

from __future__ import annotations

import time
from pathlib import Path

import soundfile as sf
from faster_whisper import WhisperModel

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = REPO_ROOT / "tools" / "out"
HF_CACHE = REPO_ROOT / "models" / "hf-cache"

WAVS = ("roundtrip_en_0.wav", "roundtrip_it_0.wav", "roundtrip_it_1.wav")


def bench(label: str, model_name: str, device: str, compute_type: str) -> None:
    print(f"\n=== {label} ===")
    model = WhisperModel(model_name, device=device, compute_type=compute_type,
                         download_root=str(HF_CACHE))
    total_audio = total_time = 0.0
    for f in WAVS:
        path = OUT_DIR / f
        duration = len(sf.read(path)[0]) / sf.read(path)[1]
        start = time.time()
        segments, info = model.transcribe(str(path), language=None, beam_size=1)
        text = " ".join(s.text.strip() for s in segments)
        dt = time.time() - start
        total_audio += duration
        total_time += dt
        print(f"  {f}: {dt:.2f}s lang={info.language} -> {text}")
    print(f"  RTF {total_time/total_audio:.3f} ({total_audio/total_time:.1f}x realtime)"
          "  (first call includes warmup)")


def main() -> None:
    bench("Whisper small int8 / CPU", "small", "cpu", "int8")
    bench("Whisper small int8 / CUDA", "small", "cuda", "int8")
    bench("Whisper large-v3-turbo float16 / CUDA", "large-v3-turbo", "cuda", "float16")


if __name__ == "__main__":
    main()
