"""Parrot prototype: microphone -> STT -> TTS (sherpa-onnx/Piper) -> speaker.

Two STT engines:
  --engine vosk      True streaming (Vosk): words appear live while you speak.
                     Single language per run (--lang it|en).
  --engine parakeet  Near-live (NVIDIA Parakeet TDT v3 + Silero VAD): one model for
                     EN+IT with auto language detection; the final text of each
                     phrase appears ~0.4 s after you finish it.

When a sentence is recognized, the same text is synthesized back to you with the
local Piper voice. This validates the STT->TTS pipeline planned for the C++ agent.

Usage (from repo root, after tools/download_models.py):
    .venv\\Scripts\\python.exe tools\\parrot.py --engine vosk --lang it
    .venv\\Scripts\\python.exe tools\\parrot.py --engine parakeet
    .venv\\Scripts\\python.exe tools\\parrot.py --engine parakeet --no-play   # text only

Press Ctrl+C to quit.
"""

from __future__ import annotations

import argparse
import json
import queue
import sys
from pathlib import Path

import numpy as np
import sounddevice as sd
from vosk import KaldiRecognizer, Model, SetLogLevel

REPO_ROOT = Path(__file__).resolve().parent.parent
SAMPLE_RATE = 16000

STT_DIR = {"it": "stt-vosk-it-large", "en": "stt-vosk-en"}
STT_DIR_SMALL = {"it": "stt-vosk-it"}  # faster but noticeably less accurate Italian
TTS_DIR = {"it": "tts-piper-it-it-paola", "en": "tts-piper-en-us-lessac"}
PARAKEET_DIR = "stt-parakeet-multilingual"
VAD_MODEL = "vad-silero/silero_vad.onnx"


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


def mic_queue() -> tuple[sd.RawInputStream, queue.Queue[bytes]]:
    """Start an int16 mono microphone stream; return (stream, queue of raw blocks)."""
    q: queue.Queue[bytes] = queue.Queue()

    def callback(indata, frames, time_info, status) -> None:  # noqa: ANN001
        q.put(bytes(indata))

    stream = sd.RawInputStream(samplerate=SAMPLE_RATE, blocksize=8000, dtype="int16",
                               channels=1, callback=callback)
    stream.start()
    return stream, q


def run_vosk(args, tts) -> None:
    """True streaming STT: live word-level partials, one language per run."""
    stt_name = STT_DIR_SMALL.get(args.lang, STT_DIR[args.lang]) if args.small else STT_DIR[args.lang]
    stt_dir = REPO_ROOT / "models" / stt_name
    if not stt_dir.exists():
        sys.exit(f"STT model not found in {stt_dir}. Run tools/download_models.py first.")
    print(f"Loading Vosk STT ({stt_name}) ...")
    recognizer = KaldiRecognizer(Model(str(stt_dir)), SAMPLE_RATE)

    stream, q = mic_queue()
    print(f"\nParrot ready (vosk, lang={args.lang}). Speak! Ctrl+C to quit.\n")
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
    finally:
        stream.stop()
        stream.close()


def run_parakeet(args, tts) -> None:
    """Near-live multilingual STT: Silero VAD segments speech, Parakeet transcribes
    each segment (EN+IT auto-detected, punctuation included)."""
    import sherpa_onnx

    d = REPO_ROOT / "models" / PARAKEET_DIR
    vad_path = REPO_ROOT / "models" / VAD_MODEL
    if not d.exists() or not vad_path.exists():
        sys.exit("Parakeet or Silero VAD model missing. Run tools/download_models.py first.")
    print("Loading Parakeet TDT v3 (multilingual EN+IT) ...")
    recognizer = sherpa_onnx.OfflineRecognizer.from_transducer(
        encoder=str(d / "encoder.int8.onnx"),
        decoder=str(d / "decoder.int8.onnx"),
        joiner=str(d / "joiner.int8.onnx"),
        tokens=str(d / "tokens.txt"),
        num_threads=8, provider="cpu", model_type="nemo_transducer")
    vad_config = sherpa_onnx.VadModelConfig()
    vad_config.silero_vad.model = str(vad_path)
    vad_config.silero_vad.threshold = 0.5
    vad_config.silero_vad.min_silence_duration = 0.5   # finalize ~0.5 s after you stop
    vad_config.silero_vad.max_speech_duration = 15.0
    vad_config.sample_rate = SAMPLE_RATE
    detector = sherpa_onnx.VoiceActivityDetector(vad_config, buffer_size_in_seconds=60)

    stream, q = mic_queue()
    print("\nParrot ready (parakeet, EN+IT auto). Speak! Ctrl+C to quit.\n")
    pcm_buf = np.zeros(0, dtype=np.int16)
    try:
        while True:
            pcm_buf = np.concatenate([pcm_buf, np.frombuffer(q.get(), dtype=np.int16)])
            while len(pcm_buf) >= 512:
                chunk, pcm_buf = pcm_buf[:512], pcm_buf[512:]
                detector.accept_waveform(chunk.astype(np.float32))
            while not detector.empty():
                seg = detector.front
                stream_rec = recognizer.create_stream()
                stream_rec.accept_waveform(SAMPLE_RATE, np.array(seg.samples, dtype=np.float32))
                recognizer.decode_stream(stream_rec)
                text = stream_rec.result.text.strip()
                detector.pop()
                if text:
                    print(f"[STT final] {text}")
                    speak(tts, text, play=not args.no_play)
    finally:
        stream.stop()
        stream.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", choices=["vosk", "parakeet"], default="vosk",
                        help="vosk = word-level live partials (one language); "
                             "parakeet = one EN+IT model, final per phrase")
    parser.add_argument("--lang", choices=["it", "en"], default="it",
                        help="language of speech (vosk engine: STT + TTS voice; "
                             "parakeet engine: TTS voice only)")
    parser.add_argument("--small", action="store_true",
                        help="use the small (faster, less accurate) Italian STT model")
    parser.add_argument("--no-play", action="store_true", help="do not play TTS audio")
    parser.add_argument("--list-devices", action="store_true", help="list audio devices and exit")
    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        return

    SetLogLevel(-1)
    tts = load_tts(args.lang)
    if args.engine == "vosk":
        run_vosk(args, tts)
    else:
        run_parakeet(args, tts)


if __name__ == "__main__":
    main()
