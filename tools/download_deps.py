#!/usr/bin/env python
"""Krastor — download prebuilt third-party binaries that are git-ignored.

Models are handled by download_models.py; this script fetches
  1. the sherpa-onnx runtime DLLs and import lib (TTS/STT engine, see
     libs/development.md) into libs/sherpa-onnx/lib/;
  2. the RNNoise model weights (generated C tables, not shipped in the
     xiph/rnnoise git repo) into libs/rnnoise-data/src/.
Run it once after cloning, before configuring on Windows.

Usage (from the repo root, with the project venv):
    .venv\\Scripts\\python tools\\download_deps.py
"""

import hashlib
import os
import ssl
import sys
import tarfile
import tempfile
import urllib.request

VERSION = "1.13.8"
ASSET = f"sherpa-onnx-v{VERSION}-win-x64-shared-MD-Release.tar.bz2"
URL = f"https://github.com/k2-fsa/sherpa-onnx/releases/download/v{VERSION}/{ASSET}"

# Only the sherpa-onnx binaries are git-ignored (global *.dll/*.lib rules);
# the C API header is committed under libs/sherpa-onnx/include/.
WANTED = [
    "sherpa-onnx-c-api.dll",
    "sherpa-onnx-c-api.lib",
    "onnxruntime.dll",
    "onnxruntime_providers_shared.dll",
]

# RNNoise model weights: the git submodule (libs/rnnoise) deliberately ships
# without them; upstream publishes them as a sha256-addressed tarball. The
# hash doubles as the checksum (same scheme as libs/rnnoise/download_model.sh).
RNNOISE_MODEL_SHA = "0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37"
RNNOISE_URL = (
    f"https://media.xiph.org/rnnoise/models/rnnoise_data-{RNNOISE_MODEL_SHA}.tar.gz"
)
RNNOISE_WANTED = ["rnnoise_data.c", "rnnoise_data.h"]

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(REPO_ROOT, "libs", "sherpa-onnx", "lib")
RNNOISE_DEST = os.path.join(REPO_ROOT, "libs", "rnnoise-data", "src")


def _ssl_context() -> ssl.SSLContext:
    """Prefer certifi's fresh CA bundle when installed (media.xiph.org has had
    expired intermediates in older Windows trust stores)."""
    try:
        import certifi

        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


def _fetch(url: str, dest: str, wanted: list[str], flatten: bool) -> int:
    print(f"downloading {url}")
    ctx = _ssl_context()
    with tempfile.TemporaryDirectory() as tmp:
        archive = os.path.join(tmp, os.path.basename(url))
        with urllib.request.urlopen(url, context=ctx, timeout=120) as r, \
                open(archive, "wb") as out:
            out.write(r.read())
        if flatten:  # verify the content-addressed tarball
            digest = hashlib.sha256(open(archive, "rb").read()).hexdigest()
            if digest != RNNOISE_MODEL_SHA:
                print(f"ERROR: sha256 mismatch ({digest})", file=sys.stderr)
                return 1
        mode = "r:bz2" if archive.endswith(".bz2") else "r:gz"
        print("extracting:", ", ".join(wanted))
        with tarfile.open(archive, mode) as tar:
            for member in tar.getmembers():
                name = os.path.basename(member.name)
                if name in wanted and member.isfile():
                    src = tar.extractfile(member)
                    with open(os.path.join(dest, name), "wb") as out:
                        out.write(src.read())
                    print("  ->", os.path.relpath(os.path.join(dest, name), REPO_ROOT))
    return 0


def main() -> int:
    os.makedirs(DEST, exist_ok=True)
    missing = [d for d in WANTED if not os.path.exists(os.path.join(DEST, d))]
    if not missing:
        print("all sherpa-onnx runtime DLLs already present in", DEST)
    elif _fetch(URL, DEST, WANTED, flatten=False) != 0:
        return 1

    os.makedirs(RNNOISE_DEST, exist_ok=True)
    missing = [d for d in RNNOISE_WANTED if not os.path.exists(os.path.join(RNNOISE_DEST, d))]
    if not missing:
        print("RNNoise model data already present in", RNNOISE_DEST)
    elif _fetch(RNNOISE_URL, RNNOISE_DEST, RNNOISE_WANTED, flatten=True) != 0:
        return 1

    left = [d for d in WANTED if not os.path.exists(os.path.join(DEST, d))]
    left += [d for d in RNNOISE_WANTED if not os.path.exists(os.path.join(RNNOISE_DEST, d))]
    if left:
        print("ERROR: still missing:", ", ".join(left), file=sys.stderr)
        return 1
    print("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
