@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Debug
