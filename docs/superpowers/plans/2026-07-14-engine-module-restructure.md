# Reorganización de engine/ en módulos por subsistema Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganizar `engine/include/DonTopo/` y `engine/src/` (planos, 38 headers + 33 cpp) en subcarpetas por módulo (`Core`, `Renderer`, `Audio`, `Physics`, `Files`, `Editor`, `Scripting`), sin cambiar ningún comportamiento.

**Architecture:** Refactor puramente mecánico en 5 tareas secuenciales: mover headers, mover cpp, reescribir rutas de `#include`, actualizar `CMakeLists.txt`, verificar con build limpio. Las tareas 1-4 dejan el repo en estado no compilable (es inherente a un rename repo-wide con dependencias cruzadas entre casi todos los módulos); la tarea 5 es la que restaura un build verde. Usar `git mv` (no copiar+borrar) para preservar historial.

**Tech Stack:** C++20, CMake + Ninja, MSVC (vcvarsall), Git Bash (`git mv`, `sed`) para las reescrituras de texto — más fiable que PowerShell aquí porque preserva UTF-8 sin BOM y CRLF sin tocarlos (verificado: ficheros fuente son UTF-8 sin BOM, CRLF).

## Global Constraints

- Ningún cambio de comportamiento — 0 líneas de lógica tocadas, solo rutas de fichero e includes.
- Preservar historial de git: usar `git mv`, nunca borrar+recrear.
- `sandbox/src/main.cpp` y cualquier otro consumidor externo de `DonTopo/*.h` también se actualiza (no solo `engine/`).
- Tras el refactor, build limpio vía `configure.bat`/`build.bat` (ver [[project_build_commands]]) debe compilar sin errores — es el único criterio de éxito, no hay tests unitarios para este cambio.
- Antes de dar el build por bueno, borrar artefactos `.obj` propios del engine y hacer rebuild limpio (no incremental) — build stale con headers movidos produce crashes de puntero basura que parecen bugs nuevos (ver [[ninja_stale_header_deps]]).

---

### Task 1: Mover headers a subcarpetas por módulo

**Files:**
- Move (git mv): 38 headers en `engine/include/DonTopo/*.h` → `engine/include/DonTopo/<Modulo>/*.h`

**Interfaces:**
- Consumes: nada (primer paso).
- Produces: nueva ubicación física de cada header. Los `#include` que los referencian siguen apuntando a las rutas viejas hasta la Task 3 — el build **no compilará** entre esta tarea y la Task 4. Esto es esperado.

- [ ] **Step 1: Crear subcarpetas y mover headers con git mv**

```bash
cd engine/include/DonTopo
mkdir -p Core Renderer Audio Physics/Colliders Files Editor Scripting

git mv Engine.h Core/Engine.h
git mv Window.h Core/Window.h
git mv GameObject.h Core/GameObject.h
git mv Scene.h Core/Scene.h
git mv Camera.h Core/Camera.h
git mv Input.h Core/Input.h

git mv Renderer.h Renderer/Renderer.h
git mv GpuDevice.h Renderer/GpuDevice.h
git mv GpuResources.h Renderer/GpuResources.h
git mv Material.h Renderer/Material.h
git mv Mesh.h Renderer/Mesh.h
git mv SkinnedMesh.h Renderer/SkinnedMesh.h
git mv Vertex.h Renderer/Vertex.h
git mv UniformBufferObject.h Renderer/UniformBufferObject.h
git mv ModelLoader.h Renderer/ModelLoader.h
git mv Skybox.h Renderer/Skybox.h
git mv Cube.h Renderer/Cube.h
git mv Sphere.h Renderer/Sphere.h
git mv Plane.h Renderer/Plane.h
git mv Capsule.h Renderer/Capsule.h

git mv AudioManager.h Audio/AudioManager.h
git mv AudioClipComponent.h Audio/AudioClipComponent.h

git mv PhysicsManager.h Physics/PhysicsManager.h
git mv BoxCollider.h Physics/Colliders/BoxCollider.h
git mv SphereCollider.h Physics/Colliders/SphereCollider.h
git mv CapsuleCollider.h Physics/Colliders/CapsuleCollider.h
git mv PlaneCollider.h Physics/Colliders/PlaneCollider.h

git mv FileManager.h Files/FileManager.h

git mv EditorUI.h Editor/EditorUI.h
git mv Command.h Editor/Command.h
git mv UndoManager.h Editor/UndoManager.h
git mv Gizmos.h Editor/Gizmos.h
git mv ScriptEditorPanel.h Editor/ScriptEditorPanel.h

git mv ScriptComponent.h Scripting/ScriptComponent.h
git mv ScriptManager.h Scripting/ScriptManager.h
git mv ScriptBindings.h Scripting/ScriptBindings.h
git mv LuaSyntaxCheck.h Scripting/LuaSyntaxCheck.h
git mv LuaApiReference.h Scripting/LuaApiReference.h

cd ../../..
```

- [ ] **Step 2: Verificar que no queda ningún .h suelto en la raíz de DonTopo/**

Run: `find engine/include/DonTopo -maxdepth 1 -type f -name "*.h"`
Expected: sin salida (0 ficheros, solo queda `.gitkeep` si existía)

- [ ] **Step 3: Commit**

```bash
git add engine/include/DonTopo
git commit -m "refactor(engine): mover headers a subcarpetas por módulo

Core/Renderer/Audio/Physics(+Colliders)/Files/Editor/Scripting. Build roto
hasta que Task 3-4 actualicen los #include y CMakeLists.txt."
```

---

### Task 2: Mover ficheros .cpp a subcarpetas por módulo

**Files:**
- Move (git mv): 33 ficheros `engine/src/*.cpp` → `engine/src/<Modulo>/*.cpp`

**Interfaces:**
- Consumes: nada (independiente de Task 1, mismo tipo de operación).
- Produces: nueva ubicación física de cada `.cpp`. `engine/CMakeLists.txt` sigue listando las rutas viejas hasta la Task 4 — el build sigue roto.

- [ ] **Step 1: Crear subcarpetas y mover cpp con git mv**

```bash
cd engine/src
mkdir -p Core Renderer Audio Physics/Colliders Files Editor Scripting

git mv Engine.cpp Core/Engine.cpp
git mv Window.cpp Core/Window.cpp
git mv GameObject.cpp Core/GameObject.cpp
git mv Scene.cpp Core/Scene.cpp
git mv Camera.cpp Core/Camera.cpp
git mv Input.cpp Core/Input.cpp

git mv Renderer.cpp Renderer/Renderer.cpp
git mv GpuDevice.cpp Renderer/GpuDevice.cpp
git mv GpuResources.cpp Renderer/GpuResources.cpp
git mv ModelLoader.cpp Renderer/ModelLoader.cpp
git mv Skybox.cpp Renderer/Skybox.cpp
git mv Cube.cpp Renderer/Cube.cpp
git mv Sphere.cpp Renderer/Sphere.cpp
git mv Plane.cpp Renderer/Plane.cpp
git mv Capsule.cpp Renderer/Capsule.cpp

git mv AudioManager.cpp Audio/AudioManager.cpp
git mv AudioClipComponent.cpp Audio/AudioClipComponent.cpp

git mv PhysicsManager.cpp Physics/PhysicsManager.cpp
git mv BoxCollider.cpp Physics/Colliders/BoxCollider.cpp
git mv SphereCollider.cpp Physics/Colliders/SphereCollider.cpp
git mv CapsuleCollider.cpp Physics/Colliders/CapsuleCollider.cpp
git mv PlaneCollider.cpp Physics/Colliders/PlaneCollider.cpp

git mv FileManager.cpp Files/FileManager.cpp

git mv EditorUI.cpp Editor/EditorUI.cpp
git mv Command.cpp Editor/Command.cpp
git mv UndoManager.cpp Editor/UndoManager.cpp
git mv Gizmos.cpp Editor/Gizmos.cpp
git mv ScriptEditorPanel.cpp Editor/ScriptEditorPanel.cpp

git mv ScriptComponent.cpp Scripting/ScriptComponent.cpp
git mv ScriptManager.cpp Scripting/ScriptManager.cpp
git mv ScriptBindings.cpp Scripting/ScriptBindings.cpp
git mv LuaSyntaxCheck.cpp Scripting/LuaSyntaxCheck.cpp
git mv LuaApiReference.cpp Scripting/LuaApiReference.cpp

cd ../..
```

- [ ] **Step 2: Verificar que no queda ningún .cpp suelto en la raíz de src/**

Run: `find engine/src -maxdepth 1 -type f -name "*.cpp"`
Expected: sin salida (0 ficheros)

- [ ] **Step 3: Commit**

```bash
git add engine/src
git commit -m "refactor(engine): mover .cpp a subcarpetas por módulo

Mismo mapeo de módulos que los headers (Task anterior). Build sigue roto
hasta actualizar CMakeLists.txt (Task 4)."
```

---

### Task 3: Reescribir rutas de #include a la nueva estructura

**Files:**
- Modify: todos los `.h`/`.cpp` bajo `engine/include/DonTopo/`, `engine/src/`, y `sandbox/src/main.cpp` que contengan `#include "DonTopo/<Nombre>.h"`

**Interfaces:**
- Consumes: mapeo módulo↔fichero fijado en Task 1/2 (no hay filenames duplicados entre módulos, así que la sustitución es no ambigua).
- Produces: todos los includes en formato `#include "DonTopo/<Modulo>/<Nombre>.h"` (o `DonTopo/Physics/Colliders/<Nombre>.h` pa colliders). Deja el repo listo pa que Task 4 (CMakeLists) complete el build.

- [ ] **Step 1: Medir el estado antes de tocar nada (para comparar después)**

Run: `grep -rho '#include "DonTopo/[A-Za-z]*\.h"' engine sandbox --include=*.cpp --include=*.h | sort -u | wc -l`
Expected: `38` (una línea de include distinta por cada uno de los 38 headers — el número de patrones únicos a reemplazar)

- [ ] **Step 2: Aplicar la sustitución con sed sobre todos los ficheros fuente**

```bash
FILES=$(grep -rl '#include "DonTopo/' engine sandbox --include=*.cpp --include=*.h)

for f in $FILES; do
  sed -i \
    -e 's#DonTopo/Engine\.h#DonTopo/Core/Engine.h#g' \
    -e 's#DonTopo/Window\.h#DonTopo/Core/Window.h#g' \
    -e 's#DonTopo/GameObject\.h#DonTopo/Core/GameObject.h#g' \
    -e 's#DonTopo/Scene\.h#DonTopo/Core/Scene.h#g' \
    -e 's#DonTopo/Camera\.h#DonTopo/Core/Camera.h#g' \
    -e 's#DonTopo/Input\.h#DonTopo/Core/Input.h#g' \
    -e 's#DonTopo/Renderer\.h#DonTopo/Renderer/Renderer.h#g' \
    -e 's#DonTopo/GpuDevice\.h#DonTopo/Renderer/GpuDevice.h#g' \
    -e 's#DonTopo/GpuResources\.h#DonTopo/Renderer/GpuResources.h#g' \
    -e 's#DonTopo/Material\.h#DonTopo/Renderer/Material.h#g' \
    -e 's#DonTopo/Mesh\.h#DonTopo/Renderer/Mesh.h#g' \
    -e 's#DonTopo/SkinnedMesh\.h#DonTopo/Renderer/SkinnedMesh.h#g' \
    -e 's#DonTopo/Vertex\.h#DonTopo/Renderer/Vertex.h#g' \
    -e 's#DonTopo/UniformBufferObject\.h#DonTopo/Renderer/UniformBufferObject.h#g' \
    -e 's#DonTopo/ModelLoader\.h#DonTopo/Renderer/ModelLoader.h#g' \
    -e 's#DonTopo/Skybox\.h#DonTopo/Renderer/Skybox.h#g' \
    -e 's#DonTopo/Cube\.h#DonTopo/Renderer/Cube.h#g' \
    -e 's#DonTopo/Sphere\.h#DonTopo/Renderer/Sphere.h#g' \
    -e 's#DonTopo/Plane\.h#DonTopo/Renderer/Plane.h#g' \
    -e 's#DonTopo/Capsule\.h#DonTopo/Renderer/Capsule.h#g' \
    -e 's#DonTopo/AudioManager\.h#DonTopo/Audio/AudioManager.h#g' \
    -e 's#DonTopo/AudioClipComponent\.h#DonTopo/Audio/AudioClipComponent.h#g' \
    -e 's#DonTopo/PhysicsManager\.h#DonTopo/Physics/PhysicsManager.h#g' \
    -e 's#DonTopo/BoxCollider\.h#DonTopo/Physics/Colliders/BoxCollider.h#g' \
    -e 's#DonTopo/SphereCollider\.h#DonTopo/Physics/Colliders/SphereCollider.h#g' \
    -e 's#DonTopo/CapsuleCollider\.h#DonTopo/Physics/Colliders/CapsuleCollider.h#g' \
    -e 's#DonTopo/PlaneCollider\.h#DonTopo/Physics/Colliders/PlaneCollider.h#g' \
    -e 's#DonTopo/FileManager\.h#DonTopo/Files/FileManager.h#g' \
    -e 's#DonTopo/EditorUI\.h#DonTopo/Editor/EditorUI.h#g' \
    -e 's#DonTopo/Command\.h#DonTopo/Editor/Command.h#g' \
    -e 's#DonTopo/UndoManager\.h#DonTopo/Editor/UndoManager.h#g' \
    -e 's#DonTopo/Gizmos\.h#DonTopo/Editor/Gizmos.h#g' \
    -e 's#DonTopo/ScriptEditorPanel\.h#DonTopo/Editor/ScriptEditorPanel.h#g' \
    -e 's#DonTopo/ScriptComponent\.h#DonTopo/Scripting/ScriptComponent.h#g' \
    -e 's#DonTopo/ScriptManager\.h#DonTopo/Scripting/ScriptManager.h#g' \
    -e 's#DonTopo/ScriptBindings\.h#DonTopo/Scripting/ScriptBindings.h#g' \
    -e 's#DonTopo/LuaSyntaxCheck\.h#DonTopo/Scripting/LuaSyntaxCheck.h#g' \
    -e 's#DonTopo/LuaApiReference\.h#DonTopo/Scripting/LuaApiReference.h#g' \
    "$f"
done
```

**Nota de orden:** cada patrón matchea el nombre de fichero completo seguido de `.h` (p.ej. `DonTopo/Command\.h`), y ningún nombre de fichero es substring de otro en esta lista (verificado: 38 basenames únicos), así que el orden de las 38 sustituciones no importa — no hay riesgo de doble-reemplazo.

- [ ] **Step 3: Verificar que no queda ningún include con la ruta vieja (plana)**

Run: `grep -rn '#include "DonTopo/[A-Za-z]*\.h"' engine sandbox --include=*.cpp --include=*.h`
Expected: sin salida (0 coincidencias — todo include ahora tiene un segmento de módulo entre `DonTopo/` y el nombre de fichero)

- [ ] **Step 4: Verificar que el total de includes reescritos con módulo coincide con lo esperado**

Run: `grep -rho '#include "DonTopo/[A-Za-z]*/' engine sandbox --include=*.cpp --include=*.h | sort | uniq -c`
Expected: conteo por módulo (`Core/`, `Renderer/`, `Audio/`, `Physics/`, `Files/`, `Editor/`, `Scripting/`) — ninguna línea con 0 ni con un nombre de módulo fuera de esta lista

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo engine/src sandbox/src
git commit -m "refactor(engine): actualizar rutas de #include a la nueva estructura de módulos

Sustitución mecánica DonTopo/X.h -> DonTopo/<Modulo>/X.h en todo el repo.
Build sigue roto hasta actualizar CMakeLists.txt (Task 4)."
```

---

### Task 4: Actualizar engine/CMakeLists.txt

**Files:**
- Modify: `engine/CMakeLists.txt:1-35` (bloque `add_library(DonTopoEngine STATIC ...)`), y línea 72-74 (`set_source_files_properties` de `ScriptBindings.cpp`)

**Interfaces:**
- Consumes: rutas nuevas de `.cpp` fijadas en Task 2.
- Produces: `CMakeLists.txt` apuntando a las rutas correctas — habilita que Task 5 compile.

- [ ] **Step 1: Reemplazar el bloque add_library con las rutas nuevas**

Reemplazar líneas 1-35 de `engine/CMakeLists.txt`:

```cmake
add_library(DonTopoEngine STATIC
    src/Core/Engine.cpp
    src/Core/Window.cpp
    src/Renderer/Renderer.cpp
    src/Editor/EditorUI.cpp
    src/Editor/Command.cpp
    src/Editor/UndoManager.cpp
    src/Renderer/Skybox.cpp
    src/Renderer/ModelLoader.cpp
    src/Renderer/Cube.cpp
    src/Renderer/Sphere.cpp
    src/Renderer/Plane.cpp
    src/Renderer/Capsule.cpp
    src/Core/Camera.cpp
    src/Core/GameObject.cpp
    src/Core/Scene.cpp
    src/Files/FileManager.cpp
    src/Audio/AudioManager.cpp
    src/Audio/AudioClipComponent.cpp
    src/Scripting/ScriptComponent.cpp
    src/Scripting/ScriptManager.cpp
    src/Core/Input.cpp
    src/Scripting/ScriptBindings.cpp
    src/Physics/PhysicsManager.cpp
    src/Physics/Colliders/BoxCollider.cpp
    src/Physics/Colliders/SphereCollider.cpp
    src/Physics/Colliders/CapsuleCollider.cpp
    src/Physics/Colliders/PlaneCollider.cpp
    src/Renderer/GpuDevice.cpp
    src/Renderer/GpuResources.cpp
    src/Editor/Gizmos.cpp
    src/Editor/ScriptEditorPanel.cpp
    src/Scripting/LuaSyntaxCheck.cpp
    src/Scripting/LuaApiReference.cpp
)
```

- [ ] **Step 2: Actualizar la ruta de ScriptBindings.cpp en set_source_files_properties**

Buscar en `engine/CMakeLists.txt` (línea ~73):

```cmake
    set_source_files_properties(src/ScriptBindings.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
```

Reemplazar por:

```cmake
    set_source_files_properties(src/Scripting/ScriptBindings.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
```

- [ ] **Step 3: Verificar que no queda ninguna ruta `src/` sin módulo en el fichero**

Run: `grep -n 'src/[A-Za-z]*\.cpp' engine/CMakeLists.txt`
Expected: sin salida (0 coincidencias — todas las rutas `src/...` tienen ahora un segmento de módulo, p.ej. `src/Core/Engine.cpp`)

- [ ] **Step 4: Commit**

```bash
git add engine/CMakeLists.txt
git commit -m "refactor(engine): actualizar rutas de CMakeLists.txt a la nueva estructura de módulos

Última pieza del refactor mecánico — a partir de aquí el build debe volver
a compilar (verificación en la siguiente tarea)."
```

---

### Task 5: Build limpio y verificación final

**Files:**
- None (solo verificación; fixes puntuales si el build revela algo no cubierto por Tasks 1-4, p.ej. un include con ruta relativa `../X.h` en vez de `DonTopo/X.h` que el grep de Task 3 no habría cazado)

**Interfaces:**
- Consumes: repo completo tras Task 4.
- Produces: build verde — criterio de éxito final del plan.

- [ ] **Step 1: Borrar artefactos de build propios del engine (evitar dependencias de header stale)**

En PowerShell (ver [[project_build_commands]]):

```powershell
Remove-Item -Recurse -Force build-ninja\CMakeFiles\DonTopoEngine.dir -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force build-ninja\CMakeFiles\Sandbox.dir -ErrorAction SilentlyContinue
```

(Ajustar nombres de directorio si difieren — comprobar con `Get-ChildItem build-ninja\CMakeFiles` antes de borrar.)

- [ ] **Step 2: Reconfigurar y compilar**

```powershell
.\configure.bat
.\build.bat
```

Expected: build termina sin errores (`0 Error(s)` o equivalente en la salida de MSVC/Ninja).

- [ ] **Step 3: Si hay errores, diagnosticar y arreglar**

Errores esperables y su causa:
- `fatal error: 'DonTopo/X.h' file not found` → algún include no capturado por el sed de Task 3 (p.ej. ruta relativa `#include "X.h"` sin prefijo `DonTopo/`, o un include dentro de un fichero fuera de `engine/`/`sandbox/` no cubierto). Localizar con `grep -rn '#include "X.h"' --include=*.cpp --include=*.h .` (sustituir `X` por el nombre del header que falta) y corregir la ruta a mano.
- `LNK2019 unresolved external symbol` → ruta de `.cpp` mal escrita en `CMakeLists.txt` (Task 4) — el símbolo existe pero el `.cpp` no se está compilando. Revisar `engine/CMakeLists.txt` contra la lista de Task 2.

Si se necesitan fixes, aplicarlos y volver a Step 2 hasta build limpio.

- [ ] **Step 4: Confirmar que git status está limpio (todo commiteado) salvo build artifacts**

Run: `git status`
Expected: solo ficheros dentro de `build-ninja/` (ignorados o no) como no-trackeados; ningún fichero fuente (`engine/`, `sandbox/`) sin commitear.

- [ ] **Step 5: Commit final (solo si Step 3 requirió fixes)**

```bash
git add -A engine sandbox
git commit -m "fix(engine): corregir includes residuales tras reorganización de módulos"
```

(Omitir este step si Task 4 ya dejó el build verde sin necesidad de fixes.)
