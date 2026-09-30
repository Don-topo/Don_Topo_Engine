@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
rem Console in UTF-8 BEFORE building. Without this, ninja records NO header
rem dependency at all and any change to a .h leaves the build stale:
rem CMake stores the /showIncludes prefix in UTF-8 ("Nota: inclusion del
rem archivo:", with the accented o as C3 B3) but cl.exe emits it in the console's
rem codepage (CP850, the o as A2). Ninja compares byte by byte, it does not match, and
rem it ends up with no deps -- which is why those lines leak into the log instead of
rem being consumed by ninja. VSLANG=1033 does not help here: only the 3082 (Spanish)
rem language pack is installed, so cl cannot emit in English.
chcp 65001 >nul
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Debug
