#!/usr/bin/env bash
# Krastor build helper (Linux).
# Usage: ./scripts/build.sh [debug|release]
set -e
CONFIG="${1:-debug}"
PRESET="linux-gcc-${CONFIG}"
echo "Building with preset: ${PRESET}"
# Source libraries live in git submodules (see libs/development.md).
if [ ! -f "$(dirname "$0")/../libs/lunasvg/CMakeLists.txt" ]; then
    echo "Initializing git submodules..."
    git -C "$(dirname "$0")/.." submodule update --init --recursive
fi
cmake --preset "${PRESET}"
cmake --build --preset "${PRESET}"
echo "Done -> bin/krastor"
