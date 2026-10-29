#!/usr/bin/env bash
# Krastor build helper (Linux).
# Usage: ./scripts/build.sh [debug|release]
set -e
CONFIG="${1:-debug}"
PRESET="linux-gcc-${CONFIG}"
echo "Building with preset: ${PRESET}"
# Source libraries live in git submodules (see libs/development.md).
if [ ! -f "$(dirname "$0")/../libs/lunasvg/CMakeLists.txt" ] ||
   [ ! -f "$(dirname "$0")/../libs/rnnoise/include/rnnoise.h" ] ||
   [ ! -f "$(dirname "$0")/../libs/speexdsp/include/speex/speex_echo.h" ]; then
    echo "Initializing git submodules..."
    git -C "$(dirname "$0")/.." submodule update --init --recursive
fi
cmake --preset "${PRESET}"
cmake --build --preset "${PRESET}"
echo "Done -> bin/krastor"
