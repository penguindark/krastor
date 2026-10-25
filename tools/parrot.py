"""Parrot prototype: microphone -> streaming STT (Vosk) -> TTS (sherpa-onnx/Piper) -> speaker.

Speak into the microphone and watch the transcription appear word-by-word while you
talk (Vosk partial results). When you finish a sentence (endpoint detected), the same
text is synthesized back to you with the local Piper voice.

This validates the STT->TTS pipeline planned for the C++ agent (Phase 4).

Usage (from repo root, after tools/download_models.py):
    .venv\\Scripts\\python.exe tools\\parrot.py --lang it
    .venv\\Scripts\\python.exe tools\\parrot.py --lang en
    .venv\\Scripts\\python.exe tools\\parrot.py --lang en --no-play   # text only

Press Ctrl+C to quit.
"""

from __future__ import annotations

import argparse
import queue
import sys
from pathlib import Path

import json

import numpy as np
import sounddevice as sd
from vosk import KaldiRecognizer, Model, SetLogLevel

REPO_ROOT = Path(__file__).resolve().parent.parent
SAMPLE_RATE = 16000

STT_DIR = {"it": "stt-vosk-it-large", "en": "stt-vosk-en"}
STT_DIR_SMALL = {"it": "stt-vosk-it"}  # faster but noticeably less accurate Italian
TTS_DIR = {"it": "tts-piper-it-it-paola", "en": "tts-piper-en-us-lessac"}


def load_tts(lang: str):
    """Create a sherpa-onnx OfflineTts for the Piper voice matching the language."""
    import sherpa_onnx

    model_dir = REPO_ROOT / "models" / TTS_DIR[lang]
    onnx_files = list(model_dir.glob("*.onnx"))
    if not onnx_files:
        sys.exit(f"TTS model not found in {model_dir}. Run tools/download_models.py first.")
    lexicon = model_dir / "lexicon.txt"  # absent for espeak-ng voices (en/it piper)
    data_dir = REPO_ROOT / "models" / "espeak-ng-data"
    if not data_dir.exists():
        sys.exit("models/espeak-ng-data missing. Run tools/download_models.py first.")
    config = sherpa_onnx.OfflineTtsConfig(
        model=sherpa_onnx.OfflineTtsModelConfig(
            vits=sherpa_onnx.OfflineTtsVitsModelConfig(
                model=str(onnx_files[0]),
                lexicon=str(lexicon) if lexicon.exists() else "",
                tokens=str(model_dir / "tokens.txt"),
                data_dir=str(data_dir),
            ),
            num_threads=2,
        ),
        rule_fsts=",".join(str(p) for p in sorted(model_dir.glob("*.fst"))),
    )
    if not config.validate():
        sys.exit("Invalid TTS config")
    return sherpa_onnx.OfflineTts(config)


def speak(tts, text: str, play: bool) -> None:
    """Synthesize text and (optionally) play it through the default output device."""
    audio = tts.generate(text, sid=0, speed=1.0)
    samples = np.array(audio.samples, dtype=np.float32)
    print(f"[TTS] ({audio.sample_rate} Hz, {len(samples)/audio.sample_rate:.1f}s)")
    if play:
        sd.play(samples, samplerate=audio.sample_rate)
        sd.wait()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lang", choices=["it", "en"], default="it",
                        help="language of speech (selects STT + TTS voice)")
    parser.add_argument("--small", action="store_true",
                        help="use the small (faster, less accurate) Italian STT model")
    parser.add_argument("--no-play", action="store_true", help="do not play TTS audio")
    parser.add_argument("--list-devices", action="store_true", help="list audio devices and exit")
    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        return

    SetLogLevel(-1)
    stt_name = STT_DIR_SMALL.get(args.lang, STT_DIR[args.lang]) if args.small else STT_DIR[args.lang]
    stt_dir = REPO_ROOT / "models" / stt_name
    if not stt_dir.exists():
        sys.exit(f"STT model not found in {stt_dir}. Run tools/download_models.py first.")

    print(f"Loading STT model ({stt_name}) ...")
    stt_model = Model(str(stt_dir))
    print(f"Loading TTS voice ({TTS_DIR[args.lang]}) ...")
    tts = load_tts(args.lang)

    q: queue.Queue[np.ndarray] = queue.Queue()

    def callback(indata, frames, time_info, status) -> None:  # noqa: ANN001
        q.put(bytes(indata))

    recognizer = KaldiRecognizer(stt_model, SAMPLE_RATE)
    recognizer.SetWords(False)

    print(f"\nParrot ready (lang={args.lang}). Speak! Ctrl+C to quit.\n")
    with sd.RawInputStream(samplerate=SAMPLE_RATE, blocksize=8000, dtype="int16",
                           channels=1, callback=callback):
        try:
            while True:
                data = q.get()
                if recognizer.AcceptWaveform(data):
                    text = json.loads(recognizer.Result())["text"].strip()
                    if text:
                        print(f"\n[STT final] {text}")
                        speak(tts, text, play=not args.no_play)
                else:
                    partial = json.loads(recognizer.PartialResult()).get("partial", "")
                    if partial:
                        print(f"\r[speaking ] {partial}", end="", flush=True)
        except KeyboardInterrupt:
            print("\nBye.")


if __name__ == "__main__":
    main()
