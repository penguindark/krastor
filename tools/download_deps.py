#!/usr/bin/env python
"""Krastor — download prebuilt third-party binaries that are git-ignored.

Models are handled by download_models.py; this script fetches the sherpa-onnx
runtime DLLs and import lib (TTS/STT engine, see libs/development.md) into
libs/sherpa-onnx/lib/. Run it once after cloning, before configuring on Windows.

Usage (from the repo root, with the project venv):
    .venv\\Scripts\\python tools\\download_deps.py
"""

import os
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

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(REPO_ROOT, "libs", "sherpa-onnx", "lib")


def main() -> int:
    os.makedirs(DEST, exist_ok=True)
    missing = [d for d in WANTED if not os.path.exists(os.path.join(DEST, d))]
    if not missing:
        print("all sherpa-onnx runtime DLLs already present in", DEST)
        return 0

    print(f"downloading {URL}")
    with tempfile.TemporaryDirectory() as tmp:
        archive = os.path.join(tmp, ASSET)
        urllib.request.urlretrieve(URL, archive)
        print("extracting:", ", ".join(missing))
        with tarfile.open(archive, "r:bz2") as tar:
            for member in tar.getmembers():
                name = os.path.basename(member.name)
                if name in WANTED and member.isfile():
                    src = tar.extractfile(member)
                    with open(os.path.join(DEST, name), "wb") as out:
                        out.write(src.read())
                    print("  ->", os.path.join("libs", "sherpa-onnx", "lib", name))

    left = [d for d in WANTED if not os.path.exists(os.path.join(DEST, d))]
    if left:
        print("ERROR: still missing:", ", ".join(left), file=sys.stderr)
        return 1
    print("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
