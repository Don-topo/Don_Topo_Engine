#!/usr/bin/env bash
# Linux equivalent of configure.bat / configure-release.bat.
# Usage: ./configure.sh [linux-debug|linux-release]
set -euo pipefail
cd "$(dirname "$0")"
preset="${1:-linux-debug}"
missing() { echo "Missing '$1'. Install it with: $2" >&2; exit 1; }
command -v cmake >/dev/null || missing cmake "sudo apt install cmake"
command -v ninja >/dev/null || missing ninja "sudo apt install ninja-build"
command -v glslc >/dev/null || missing glslc "sudo apt install glslc"
ver=$(cmake --version | head -1 | awk '{print $3}')
if [ "$(printf '%s\n3.25\n' "$ver" | sort -V | head -1)" != "3.25" ]; then
    echo "cmake $ver is too old: 3.25 or newer is needed (Ubuntu 22.04 ships 3.22)." >&2
    exit 1
fi
cmake --preset "$preset"
