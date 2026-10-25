#!/usr/bin/env bash
# Krastor build helper (Linux).
# Usage: ./scripts/build.sh [debug|release]
set -e
CONFIG="${1:-debug}"
PRESET="linux-gcc-${CONFIG}"
echo "Building with preset: ${PRESET}"
cmake --preset "${PRESET}"
cmake --build --preset "${PRESET}"
echo "Done -> bin/krastor"
