"""Download local STT/TTS models into the git-ignored models/ folder.

Each model lives in its own subfolder (policy: ../models/development.md) and gets
a generated README.md recording source URL, license and file list.

Usage (from repo root):
    .venv\\Scripts\\python.exe tools\\download_models.py            # download all
    .venv\\Scripts\\python.exe tools\\download_models.py --only stt-vosk-it
"""

from __future__ import annotations

import argparse
import tarfile
import zipfile
from datetime import date
from pathlib import Path
from urllib.request import urlretrieve

REPO_ROOT = Path(__file__).resolve().parent.parent
MODELS_DIR = REPO_ROOT / "models"

# name -> (url, archive kind, license note)
MODELS: dict[str, dict[str, str]] = {
    "stt-vosk-it": {
        "url": "https://alphacephei.com/vosk/models/vosk-model-small-it-0.4.zip",
        "kind": "zip",
        "license": "Apache-2.0 (model trained on Mozilla Common Voice)",
        "purpose": "Streaming STT, Italian (word-level partial results)",
    },
    "stt-vosk-it-large": {
        "url": "https://alphacephei.com/vosk/models/vosk-model-it-0.22.zip",
        "kind": "zip",
        "license": "Apache-2.0 (model trained on Mozilla Common Voice + MLS + MTEED)",
        "purpose": "Streaming STT, Italian big (~1.2 GB, WER 8.10 cv-test vs 16.88 small)",
    },
    "stt-vosk-en": {
        "url": "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip",
        "kind": "zip",
        "license": "Apache-2.0 (model trained on LibriSpeech)",
        "purpose": "Streaming STT, English (word-level partial results)",
    },
    "espeak-ng-data": {
        "url": "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/espeak-ng-data.zip",
        "kind": "zip",
        "license": "GPL-3.0 (espeak-ng project)",
        "purpose": "Shared G2P frontend (espeak-ng data) required by Piper VITS voices",
    },
    "tts-piper-it-it-paola": {
        "url": "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/"
             "vits-piper-it_IT-paola-medium.tar.bz2",
        "kind": "tar.bz2",
        "license": "BSD-3 (Piper voice Paola, CC-BY from Mozilla/Common Voice)",
        "purpose": "TTS Italian voice 'paola' (medium quality), run via sherpa-onnx",
    },
    "tts-piper-en-us-lessac": {
        "url": "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/"
             "vits-piper-en_US-lessac-medium.tar.bz2",
        "kind": "tar.bz2",
        "license": "BSD-3 (Piper voice lessac, CC-BY-4.0)",
        "purpose": "TTS English (US) voice 'lessac' (medium quality), run via sherpa-onnx",
    },
}


def _extract(archive: Path, kind: str, dest: Path) -> None:
    """Extract archive and flatten the single top-level folder if present."""
    tmp = dest / "_tmp"
    tmp.mkdir(exist_ok=True)
    if kind == "zip":
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(tmp)
    else:
        with tarfile.open(archive, "r:bz2") as tf:
            tf.extractall(tmp)
    entries = list(tmp.iterdir())
    src_root = entries[0] if len(entries) == 1 and entries[0].is_dir() else tmp
    for item in src_root.iterdir():
        item.rename(dest / item.name)
    if src_root != tmp:
        src_root.rmdir()
    tmp.rmdir()
    archive.unlink()


def download(name: str) -> Path:
    info = MODELS[name]
    dest = MODELS_DIR / name
    if dest.exists() and any(dest.iterdir()):
        print(f"[skip] {name}: already present at {dest}")
        return dest
    dest.mkdir(parents=True, exist_ok=True)
    archive = dest / Path(info["url"]).name
    print(f"[get ] {name}: {info['url']}")
    urlretrieve(info["url"], archive)
    _extract(archive, info["kind"], dest)

    readme = dest / "README.md"
    files = "\n".join(f"- `{p.relative_to(dest)}`" for p in sorted(dest.rglob("*")) if p.is_file())
    readme.write_text(
        f"# {name}\n\n"
        f"- Purpose: {info['purpose']}\n"
        f"- Source: {info['url']}\n"
        f"- License: {info['license']}\n"
        f"- Downloaded: {date.today().isoformat()}\n\n"
        f"## Files\n\n{files}\n",
        encoding="utf-8",
    )
    print(f"[ok  ] {name} -> {dest}")
    return dest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=list(MODELS), action="append",
                        help="download only the named model(s); default: all")
    args = parser.parse_args()
    MODELS_DIR.mkdir(exist_ok=True)
    for name in (args.only or MODELS):
        download(name)


if __name__ == "__main__":
    main()
