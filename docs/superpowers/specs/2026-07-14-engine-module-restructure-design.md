# Reorganización de `engine/` en módulos por subsistema

**Fecha:** 2026-07-14
**Estado:** Aprobado

## Problema

`engine/include/DonTopo/` y `engine/src/` son planos: 38 headers y 33 `.cpp` mezclados sin
agrupación. Localizar el fichero correcto de un subsistema (renderizado, audio, física, editor,
scripting) requiere buscar por nombre en una lista larga sin estructura.

## Objetivo

Agrupar los ficheros existentes en subcarpetas por módulo, sin cambiar ningún comportamiento.
Refactor puramente mecánico: mover ficheros, actualizar rutas de `#include` y `CMakeLists.txt`.

## Estructura final

Se mantiene la separación actual `include/` vs `src/` en la raíz de `engine/` (no se fusionan
en carpetas de módulo autocontenidas). Dentro de cada una, una subcarpeta por módulo:

```
engine/
  include/DonTopo/
    Core/
      Engine.h, Window.h, GameObject.h, Scene.h, Camera.h, Input.h
    Renderer/
      Renderer.h, GpuDevice.h, GpuResources.h, Material.h, Mesh.h, SkinnedMesh.h,
      Vertex.h, UniformBufferObject.h, ModelLoader.h, Skybox.h,
      Cube.h, Sphere.h, Plane.h, Capsule.h
    Audio/
      AudioManager.h, AudioClipComponent.h
    Physics/
      PhysicsManager.h
      Colliders/
        BoxCollider.h, SphereCollider.h, CapsuleCollider.h, PlaneCollider.h
    Files/
      FileManager.h
    Editor/
      EditorUI.h, Command.h, UndoManager.h, Gizmos.h, ScriptEditorPanel.h
    Scripting/
      ScriptComponent.h, ScriptManager.h, ScriptBindings.h,
      LuaSyntaxCheck.h, LuaApiReference.h
  src/
    Core/       Engine.cpp, Window.cpp, GameObject.cpp, Scene.cpp, Camera.cpp, Input.cpp
    Renderer/   Renderer.cpp, GpuDevice.cpp, GpuResources.cpp, ModelLoader.cpp, Skybox.cpp,
                Cube.cpp, Sphere.cpp, Plane.cpp, Capsule.cpp
                (Material.h, Mesh.h, SkinnedMesh.h, Vertex.h, UniformBufferObject.h son
                header-only, sin .cpp)
    Audio/      AudioManager.cpp, AudioClipComponent.cpp
    Physics/    PhysicsManager.cpp
                Colliders/ BoxCollider.cpp, SphereCollider.cpp, CapsuleCollider.cpp,
                           PlaneCollider.cpp
    Files/      FileManager.cpp
    Editor/     EditorUI.cpp, Command.cpp, UndoManager.cpp, Gizmos.cpp, ScriptEditorPanel.cpp
    Scripting/  ScriptComponent.cpp, ScriptManager.cpp, ScriptBindings.cpp,
                LuaSyntaxCheck.cpp, LuaApiReference.cpp
```

### Decisiones de encaje (resueltas con el usuario)

- **Core** es una carpeta nueva, no pedida explícitamente pero necesaria: agrupa motor base
  (`Engine`, `Window`, `GameObject`, `Scene`, `Camera`, `Input`) que no pertenece a ningún
  subsistema de los 6 originales.
- **Primitivas geométricas** (`Cube`, `Sphere`, `Plane`, `Capsule`, `Mesh`, `SkinnedMesh`,
  `Vertex`) van a **Renderer**: son geometría GPU-ready acoplada al pipeline de dibujado, no
  datos de escena.
- **Colliders** van en subcarpeta propia `Physics/Colliders/`, separados de `PhysicsManager`.
- **Gizmos** va en **Editor**: solo se usa en modo edición, no en runtime/Play.
- **Command/UndoManager** van en **Editor**: hoy solo los consume `EditorUI`.
- **ScriptEditorPanel** va en **Editor** (es un panel de UI más, como `EditorUI`), no en
  Scripting — aunque edite scripts Lua, no es lógica de ejecución de scripts.

### Convención de include

Ruta completa desde `include/`, reflejando la subcarpeta de módulo:

```cpp
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Editor/UndoManager.h"
```

## Migración

1. `git mv` fichero a fichero (preserva historial de `git blame`/`git log`), no
   copiar+borrar.
2. Reescribir todas las ocurrencias de `#include "DonTopo/X.h"` → `#include "DonTopo/Modulo/X.h"`
   en los ficheros que los consumen: `engine/src/**`, `engine/include/DonTopo/**`,
   `sandbox/src/main.cpp` (~149 ocurrencias en 47 ficheros, medido antes del refactor).
3. Actualizar `engine/CMakeLists.txt`: cada entrada `src/X.cpp` → `src/Modulo/X.cpp`. El flag
   `/bigobj` de `ScriptBindings.cpp` (línea 72-74 actual) se mueve con el fichero.
4. `target_include_directories` sigue apuntando a `include/` (raíz), no cambia — los includes
   ya usan ruta completa `DonTopo/Modulo/X.h`.

## Fuera de scope

- `sandbox/`, `Scripts/`, `shaders/`, `cmake/`: no se tocan (sandbox solo actualiza su único
  `#include`).
- No se divide `EditorUI` en paneles más pequeños ni se cambia ninguna lógica — 0 cambios de
  comportamiento, solo de ubicación de ficheros.
- No se reorganiza `build-ninja/` (directorio de build, no versionado como código fuente).

## Riesgos y verificación

- **Build stale tras mover headers**: ninja puede quedarse con dependencias de headers viejas
  y producir crashes de puntero basura que parecen bugs de código nuevo (visto antes en este
  repo). Mitigación: borrar `.obj` propios del engine y hacer rebuild limpio antes de dar el
  refactor por bueno, no solo incremental.
- **Criterio de éxito**: build limpio vía `configure.bat`/`build.bat` sin errores. No hay
  cambio de comportamiento que verificar visualmente — es reorganización de ficheros.
