#!/usr/bin/env bash
# Equivalente Linux de configure.bat / configure-release.bat.
# Uso: ./configure.sh [linux-debug|linux-release]
set -euo pipefail
cd "$(dirname "$0")"
preset="${1:-linux-debug}"
falta() { echo "Falta '$1'. Instalar: $2" >&2; exit 1; }
command -v cmake >/dev/null || falta cmake "sudo apt install cmake"
command -v ninja >/dev/null || falta ninja "sudo apt install ninja-build"
command -v glslc >/dev/null || falta glslc "sudo apt install glslc"
ver=$(cmake --version | head -1 | awk '{print $3}')
if [ "$(printf '%s\n3.25\n' "$ver" | sort -V | head -1)" != "3.25" ]; then
    echo "cmake $ver es demasiado viejo: hace falta 3.25 o superior (Ubuntu 22.04 trae 3.22)." >&2
    exit 1
fi
cmake --preset "$preset"
