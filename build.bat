@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
rem Consola en UTF-8 ANTES de compilar. Sin esto, ninja NO registra NINGUNA
rem dependencia de cabecera y cualquier cambio en un .h deja el build stale:
rem CMake guarda el prefijo de /showIncludes en UTF-8 ("Nota: inclusion del
rem archivo:", la o acentuada como C3 B3) pero cl.exe lo emite en la codepage
rem de la consola (CP850, la o como A2). Ninja compara byte a byte, no casa, y
rem se queda sin deps -- por eso esas lineas se cuelan al log en vez de que las
rem consuma ninja. VSLANG=1033 no sirve aqui: solo esta instalado el paquete de
rem idioma 3082 (espanol), asi que cl no puede emitir en ingles.
chcp 65001 >nul
cmake --build build-ninja --config Debug
