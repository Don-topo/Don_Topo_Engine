#!/usr/bin/env bash
# Linux equivalent of build.bat / build-release.bat.
# Usage: ./build.sh [linux-debug|linux-release]
set -euo pipefail
cd "$(dirname "$0")"
cmake --build --preset "${1:-linux-debug}"
