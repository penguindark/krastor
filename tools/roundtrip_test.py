"""Automated round-trip test: TTS -> WAV -> streaming STT -> compare.

No microphone needed: synthesizes sample sentences with the Piper voice, feeds the
audio through the Vosk recognizer in small chunks (simulating a live mic stream),
and prints the transcription so quality can be judged for both English and Italian.

Usage (from repo root, after tools/download_models.py):
    .venv\\Scripts\\python.exe tools\\roundtrip_test.py
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from vosk import KaldiRecognizer, Model, SetLogLevel

from parrot import SAMPLE_RATE, STT_DIR, TTS_DIR, load_tts

REPO_ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = REPO_ROOT / "tools" / "out"

SENTENCES = {
    "en": [
        "The quick brown fox jumps over the lazy dog.",
        "Hello, I am your local voice assistant.",
    ],
    "it": [
        "La volpe rapida marrone salta sopra il cane pigro.",
        "Ciao, sono il tuo assistente vocale locale.",
    ],
}


def transcribe(model_dir: Path, samples: np.ndarray, rate: int) -> str:
    """Run streaming STT over synthesized audio in 0.2 s chunks (simulated mic).

    Note: this Vosk build requires int16 PCM bytes; float input is silently ignored.
    """
    rec = KaldiRecognizer(Model(str(model_dir)), rate)
    pcm = (np.clip(samples, -1.0, 1.0) * 32767).astype(np.int16)
    chunk = int(rate * 0.2)
    parts: list[str] = []
    for i in range(0, len(pcm), chunk):
        block = pcm[i:i + chunk].tobytes()
        if rec.AcceptWaveform(block):
            parts.append(json.loads(rec.Result())["text"])
    parts.append(json.loads(rec.FinalResult())["text"])
    return " ".join(p for p in parts if p).strip()


def main() -> None:
    SetLogLevel(-1)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    import soundfile as sf

    for lang, sentences in SENTENCES.items():
        print(f"\n=== {lang.upper()} ===")
        tts = load_tts(lang)
        stt_dir = REPO_ROOT / "models" / STT_DIR[lang]
        for i, sentence in enumerate(sentences):
            audio = tts.generate(sentence, sid=0, speed=1.0)
            samples = np.array(audio.samples, dtype=np.float32)
            wav_path = OUT_DIR / f"roundtrip_{lang}_{i}.wav"
            sf.write(wav_path, samples, audio.sample_rate)
            got = transcribe(stt_dir, samples, audio.sample_rate)
            ok = "OK " if got.lower().replace(".", "") == sentence.lower().replace(".", "") else "DIFF"
            print(f"[{ok}] in : {sentence}")
            print(f"      out: {got}   (wav: {wav_path.name})")


if __name__ == "__main__":
    main()
