# Portabilidad a Linux: editor, runtime, tests y exportador

Fecha: 2026-09-11
Estado: aprobado para plan de implementación

## Objetivo

Que el motor compile y funcione en Linux (Ubuntu/Debian recientes, GCC 12+):
el editor (Sandbox) abre un proyecto y renderiza con Vulkan, el audio suena con
FMOD, los tests pasan, y el editor exporta un juego que arranca en Linux. Y que
eso **no se rompa después**: la mayoría del código se seguirá escribiendo en
Windows, así que las guardas importan tanto como el port.

Es el primero de dos subproyectos. macOS va en una spec aparte, con su propia
decisión sobre la física (PhysX 5.8 no soporta macOS, ver `cmake/PhysX.cmake`).

## Alcance

Dentro:

- Capa de plataforma en Core con implementación Windows y POSIX.
- Build de Linux: presets de CMake, `configure.sh`/`build.sh`, FMOD para Linux.
- Exportador con una tabla de plataforma; exportar un juego de Linux desde el
  editor de Linux.
- Test de portabilidad que corre en Windows, y CI de Linux en GitHub Actions.

Fuera:

- macOS (siguiente spec).
- Exportar para una plataforma distinta de la del editor (cross-export).
- D3D12: sigue siendo solo Windows; CMake ya lo apaga fuera de Windows
  (`DTE_ENABLE_D3D12`).
- CI de Windows: Windows se verifica en local, como hasta ahora.

## Estado del código que condiciona el diseño

Medido en `main` (`fb2205a`):

- **Código de Windows fuera de D3D12: 4 ficheros.** `ProjectContext.cpp` y
  `runtime/main.cpp` (`GetModuleFileNameW`, duplicado), `runtime/main.cpp`
  (`MessageBoxW`), `PerformancePanel.cpp` (`GetProcessMemoryInfo`, DXGI, ya
  bajo `#ifdef _WIN32`), `audio_tests.cpp` (`GetCurrentProcessId`).
  `runtime/main.cpp` además mete tres includes de componentes dentro de su
  `#ifdef _WIN32`: hay que comprobar si se usan fuera.
- **Funciones `*_s` de MSVC:** 15 `strncpy_s` en `PropertiesPanel.cpp` (todas
  copian a un buffer fijo de ImGui) y 1 `localtime_s` en `LogPanel.cpp`.
- **Mayúsculas en includes y barras invertidas en rutas: 0 de cada.**
  Comprobado con un script contra el sistema de ficheros.
- **Build solo-Windows:** los `.bat` llaman a `vcvarsall`; los presets son de
  MSVC. Las dependencias van todas por FetchContent y son multiplataforma. Los
  flags de MSVC ya están detrás de `if(MSVC)`.
- **El nombre del runtime está escrito a mano dos veces:**
  `"DonTopoRuntime.exe"` en `EditorUI.cpp:1955` y en el POST_BUILD de
  `runtime/CMakeLists.txt`.
- **FMOD:** ya es opcional (`DT_FMOD_ENABLED`). `FindFMOD.cmake` busca
  `lib/x64`, que es el nombre de Windows; el SDK de Linux usa `lib/x86_64`.
  La copia de la biblioteca junto a los binarios es solo de Windows, y en
  los tests va con una lista de 21 targets escrita a mano.
- **PhysX:** opcional en el código (106 guardas `DT_PHYSX_ENABLED`) y
  `cmake/PhysX.cmake` ya soporta Linux. No se sabe si su build de Linux acepta
  GCC o exige clang.
- **Exportador:** asume Windows de arriba abajo — `gameName + ".exe"`,
  `fmod.dll`, el CRT de MSVC (`msvcp140*`, `vcruntime140*`) y el aviso del CRT
  de depuración.
- **`Sandbox.rc`** es un recurso de Windows y está en la lista de fuentes del
  Sandbox sin condición.

## Diseño

### 1. Capa de plataforma

`engine/include/DonTopo/Core/Platform.h`, namespace `DonTopo::platform`. Solo
lo que el motor usa hoy:

| Función | Windows | Linux |
|---|---|---|
| `std::filesystem::path executableDir()` | `GetModuleFileNameW` | `readlink("/proc/self/exe")` |
| `void showFatalError(title, message)` (UTF-8) | `MessageBoxW` (UTF-8 → UTF-16) | `stderr` |
| `ProcessStats processStats()`: RAM actual, pico, tiempo de CPU | `GetProcessMemoryInfo` + `GetProcessTimes` | `/proc/self/status` (`VmRSS`, `VmHWM`) + `getrusage` |
| `std::optional<GpuMemoryBudget> gpuMemoryBudget()` | DXGI (lo que hace hoy el panel) | `std::nullopt` |
| `std::tm localTime(std::time_t)` | `localtime_s` | `localtime_r` |
| `unsigned long processId()` | `GetCurrentProcessId` | `getpid` |

- Implementaciones en `engine/src/Core/Platform_win.cpp` y
  `engine/src/Core/Platform_posix.cpp`; CMake añade una u otra a DonTopoCore
  según la plataforma. macOS reutilizará la POSIX con `_NSGetExecutablePath`
  para `executableDir`.
- `executableDir()` cae a `std::filesystem::current_path()` si la llamada del
  sistema falla, igual que hoy.
- Llamantes que se migran: `ProjectContext`, `runtime/main.cpp`,
  `PerformancePanel` (pierde todos sus `#ifdef`; si `gpuMemoryBudget()` es
  `nullopt` muestra "no disponible"), `LogPanel`, `audio_tests`.
- Los 15 `strncpy_s` no son de plataforma: pasan a un helper portable
  `copyToBuffer(char (&buf)[N], std::string_view)` que trunca y termina
  siempre en `'\0'`.

Lo que Linux pierde a sabiendas: el presupuesto de VRAM (solo existe vía DXGI)
y la ventana de error fatal del juego exportado. En Linux el error va a
`stderr` y a `game.log`, que el runtime ya escribe.

### 2. Build

- **Presets.** `CMakePresets.json` gana `linux-debug` y `linux-release`
  (condición `hostSystemName == Linux`, Ninja, `build-linux` y
  `build-linux-release`). Los presets actuales se marcan solo-Windows.
- **Scripts.** `configure.sh` y `build.sh` equivalen a los `.bat`. Comprueban
  requisitos antes de configurar y fallan con un mensaje claro si falta
  `glslc`, `ninja` o `cmake` ≥ 3.25 (Ubuntu 22.04 trae 3.22).
- **Requisitos de Ubuntu en el README:** compilador, `ninja-build`, `cmake`,
  `glslc`, el loader y las cabeceras de Vulkan, y las cabeceras de X11 y
  Wayland que pide GLFW.
- **Nombre del runtime en un sitio.** CMake define el nombre del fichero
  (`DonTopoRuntime${CMAKE_EXECUTABLE_SUFFIX}`) y lo pasa a DonTopoEditor como
  definición de compilación. El POST_BUILD que copia el runtime junto al
  Sandbox y `EditorUI` usan ese mismo valor.
- **FMOD.**
  - `FindFMOD.cmake` añade `lib/x86_64` a los sufijos de búsqueda.
  - Una función `dt_copy_fmod_runtime(target)` copia `fmod.dll` o la
    `libfmod.so` (con el nombre de su soname) junto al binario. La usan
    Sandbox, runtime y tests. La lista escrita a mano de 21 targets
    desaparece.
- **rpath.** Los ejecutables de Linux llevan rpath `$ORIGIN` para encontrar
  `libfmod.so` en su carpeta. El juego exportado depende de lo mismo.
- **Varios.** `Sandbox.rc` solo en `WIN32`. Los flags de MSVC no cambian.
- **Riesgo abierto: PhysX en Linux con GCC.** Es lo primero que se comprueba.
  Si su build de Linux exige clang, los presets de Linux usan clang.

### 3. Exportador

Una función pura, `exportPlatformFor(Os)`, describe qué lleva el paquete de
cada plataforma. `writeExportPackage` recorre esa descripción en vez de tener
ramas por plataforma:

| | Windows | Linux |
|---|---|---|
| ejecutable | `<juego>.exe` | `<juego>`, con permiso de ejecución |
| audio | `fmod.dll` | `libfmod.so.N`, con el nombre de su soname |
| biblioteca de C++ | copia `msvcp140*` / `vcruntime140*` | ninguna: `libstdc++` enlazada estática en el runtime |
| aviso de build Debug | CRT de depuración no redistribuible | no aplica |

- Se exporta para la plataforma en la que corre el editor
  (`exportPlatformFor(currentOs())`).
- **`libstdc++` y `libgcc` estáticos en `DonTopoRuntime` en Linux**
  (`-static-libstdc++ -static-libgcc`), para que el juego no dependa de la
  versión de la biblioteca de C++ del jugador.
- **glibc sigue siendo dinámico** y no tiene arreglo: un juego exportado en
  Ubuntu 24.04 no arranca en una distro con glibc más antiguo. El exportador lo
  avisa en `ExportResult::messages`, como hoy avisa del CRT.
- **`libvulkan.so.1` no se empaqueta**, igual que hoy `vulkan-1.dll`: viene
  con el driver del jugador.
- Los avisos de FMOD que faltan junto al editor se mantienen, con el nombre de
  biblioteca de cada plataforma.

### 4. Guardas

**`dt_portability_tests`** (binario nuevo, el 28). Lee el código de
`engine/`, `sandbox/` y `runtime/` y falla con fichero y línea si encuentra:

1. `#include <windows.h>`, una línea de preprocesador que nombre `_WIN32`, o
   una llamada a una API de Win32 de una lista (`GetModuleFileName`,
   `MessageBox`, `GetProcessMemoryInfo`, `GetProcessTimes`,
   `GetCurrentProcessId`, `MultiByteToWideChar`, `QueryPerformanceCounter`…)
   fuera de `engine/src/Core/Platform_win.cpp`, `engine/src/Renderer/D3D12/`
   y `engine/include/DonTopo/Renderer/D3D12/`.
2. Una función `*_s` de MSVC (`strncpy_s`, `strcpy_s`, `localtime_s`,
   `sprintf_s`, `fopen_s`…) fuera de `Platform_win.cpp`.
3. Un `#include "..."` o `#include <DonTopo/...>` que no coincida exactamente,
   mayúsculas incluidas, con un fichero real.

Si no encuentra el árbol (lanzado desde otro directorio de trabajo) falla, no
aprueba en silencio. Es un test de texto: una API de Win32 que no esté en la
lista se le escapa, y un comentario puede dar un falso positivo.

**CI: `.github/workflows/linux.yml`.**

- En cada push y cada PR a `main`, en una máquina `ubuntu-24.04` (trae de
  serie cmake 3.28, `glslc`, Vulkan y las cabeceras de X11/Wayland).
- Configura con `linux-release`, compila todo y pasa los 28 tests desde la
  raíz del repo.
- **Sin FMOD** (su licencia impide descargarlo en CI): obliga a que todo
  compile y pase también sin audio. Si algún test asume FMOD, se protege con
  `DT_FMOD_ENABLED`.
- Caché de dependencias compiladas (PhysX, Assimp, FreeType) para que un push
  no lo recompile todo.
- Sin ventana ni GPU: el editor no se arranca en CI.

## Orden de trabajo

Cada fase deja Windows en verde (todos los tests, Debug y Release):

1. **Guarda primero.** `dt_portability_tests` escrito antes de tocar código:
   tiene que fallar con la lista actual (15 `strncpy_s`, 1 `localtime_s`, los
   `_WIN32` de 4 ficheros y sus APIs).
2. **Capa de plataforma.** `Platform.h` + las dos implementaciones +
   `copyToBuffer`, migración de los llamantes. La guarda pasa a verde; cada
   regla se sabotea de una en una.
3. **Build.** Presets, scripts, `FindFMOD`, nombre del runtime,
   `dt_copy_fmod_runtime`, rpath, `.rc` condicional.
4. **CI.** Workflow de Linux e iteración sobre los errores que solo da GCC
   hasta ver el CI en verde. Es la fase que no se puede estimar: si la lista es
   enorme, se para y se consulta antes de seguir.
5. **Exportador.** `exportPlatformFor(Os)`, libstdc++ estático, aviso de glibc
   y tests de las dos filas.

## Verificación

- **En Windows, en local:** todos los tests en Debug y Release en cada fase;
  sabotaje de uno en uno de cada regla nueva.
- **CI de Linux:** compila y pasa los tests sin FMOD en cada push.
- **Manual, en la máquina Ubuntu del usuario**, al final de la fase 4 y otra
  vez al final de la 5:
  - `./configure.sh && ./build.sh` con el SDK de FMOD para Linux en
    `third_party/fmod`, y los 28 tests.
  - El editor abre un proyecto y renderiza con Vulkan; el audio suena.
  - Exportar un juego, copiarlo a otra carpeta y arrancarlo con doble clic; si
    se puede, también en otra máquina o versión de Ubuntu.

## Riesgos

- **Errores que solo da GCC** (includes que MSVC tolera que falten, código que
  GCC rechaza): de tamaño desconocido hasta el primer build de CI.
- **PhysX con GCC:** puede exigir clang.
- **Rutas de assets en los proyectos del usuario:** Linux distingue mayúsculas
  y la guarda solo cubre el código. Un `.scene` que nombre `Texture.png` para
  un fichero `texture.png` funciona en Windows y no en Linux. Fuera de alcance;
  se anota si aparece al probar.
