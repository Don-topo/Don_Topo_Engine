#!/usr/bin/env bash
# Equivalente Linux de build.bat / build-release.bat.
# Uso: ./build.sh [linux-debug|linux-release]
set -euo pipefail
cd "$(dirname "$0")"
cmake --build --preset "${1:-linux-debug}"
