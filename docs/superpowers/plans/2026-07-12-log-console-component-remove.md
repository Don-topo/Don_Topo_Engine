# Log Console Component Remove Logging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** El botón "x" de cada sección de componente en Properties (Box/Sphere/Capsule/Plane Collider, Mesh, Audio Clip) deja una línea en el Log al quitar el componente, simétrica a la que ya existe al añadirlo.

**Architecture:** Mismo patrón que "añadir componente" (feature Log Console base) — 6 llamadas nuevas a `EditorUI::pushLog(const std::string&)` (ya existente), una dentro de cada bloque `if (removeClicked)` ya presente en `engine/src/EditorUI.cpp`, después de la mutación que quita el componente. Sin estado nuevo, sin tocar `EditorUI.h`.

**Tech Stack:** C++20, Dear ImGui (ya integrado). Sin framework de tests unitarios — verificación por compilación + ejecución manual del editor.

## Global Constraints

- No hay gtest/ctest en el repo — cada tarea se verifica con `build.bat` vía **PowerShell** (no Bash).
- Mensaje: `"Componente <Tipo> quitado de '<name>'"` — mismo patrón invertido que `"Componente <Tipo> añadido a '<name>'"` ya existente.
- 6 puntos de instrumentación: Box/Sphere/Capsule/Plane Collider, Mesh, Audio Clip — los mismos 6 tipos que ya cubre "añadir".
- El `pushLog` va DENTRO del `if (removeClicked ...)` de cada sección, después de la mutación que quita el componente (mismo orden relativo que "añadir"). Para Mesh, el bloque está condicionado también a `m_renderer` — el `pushLog` respeta esa misma condición (si no hay `m_renderer`, el componente no se quita de verdad y no debe loguearse).
- No tocar ningún otro control de esas secciones (Loop/Is 3D del Audio Clip, Play/Stop de previsualización) — fuera de scope.

---

### Task 1: Instrumentar "quitar componente" en las 6 secciones

**Files:**
- Modify: `engine/src/EditorUI.cpp` (`drawBoxColliderSection`, `drawSphereColliderSection`, `drawCapsuleColliderSection`, `drawPlaneColliderSection`, `drawMeshSection`, `drawAudioClipSection`)

**Interfaces:**
- Consumes: `EditorUI::pushLog(const std::string&) -> void` (ya existente).

- [ ] **Step 1: Log al quitar Box Collider**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawBoxColliderSection`, reemplaza:

```cpp
    if (removeClicked)
    {
        m_selected->setBoxCollider(nullptr);
        m_colliderCachedFor = nullptr;
    }
}
```

por:

```cpp
    if (removeClicked)
    {
        m_selected->setBoxCollider(nullptr);
        m_colliderCachedFor = nullptr;
        pushLog("Componente Box Collider quitado de '" + m_selected->name + "'");
    }
}
```

- [ ] **Step 2: Log al quitar Sphere Collider**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawSphereColliderSection`, reemplaza:

```cpp
    if (removeClicked)
    {
        m_selected->setSphereCollider(nullptr);
        m_sphereColliderCachedFor = nullptr;
    }
}
```

por:

```cpp
    if (removeClicked)
    {
        m_selected->setSphereCollider(nullptr);
        m_sphereColliderCachedFor = nullptr;
        pushLog("Componente Sphere Collider quitado de '" + m_selected->name + "'");
    }
}
```

- [ ] **Step 3: Log al quitar Capsule Collider**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawCapsuleColliderSection`, reemplaza:

```cpp
    if (removeClicked)
    {
        m_selected->setCapsuleCollider(nullptr);
        m_capsuleColliderCachedFor = nullptr;
    }
}
```

por:

```cpp
    if (removeClicked)
    {
        m_selected->setCapsuleCollider(nullptr);
        m_capsuleColliderCachedFor = nullptr;
        pushLog("Componente Capsule Collider quitado de '" + m_selected->name + "'");
    }
}
```

- [ ] **Step 4: Log al quitar Plane Collider**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawPlaneColliderSection`, reemplaza:

```cpp
    if (removeClicked)
    {
        m_selected->setPlaneCollider(nullptr);
        m_planeColliderCachedFor = nullptr;
    }
```

por:

```cpp
    if (removeClicked)
    {
        m_selected->setPlaneCollider(nullptr);
        m_planeColliderCachedFor = nullptr;
        pushLog("Componente Plane Collider quitado de '" + m_selected->name + "'");
    }
```

- [ ] **Step 5: Log al quitar Mesh**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawMeshSection`, reemplaza:

```cpp
        if (removeClicked && m_renderer)
        {
            m_renderer->removeMeshComponent(m_selected);
            // Vuelve a ocultar la sección tras quitar el mesh — hay que
            // pulsar "Add > Mesh" de nuevo para reabrirla.
            m_meshAddRequestedFor = nullptr;
        }
```

por:

```cpp
        if (removeClicked && m_renderer)
        {
            m_renderer->removeMeshComponent(m_selected);
            // Vuelve a ocultar la sección tras quitar el mesh — hay que
            // pulsar "Add > Mesh" de nuevo para reabrirla.
            m_meshAddRequestedFor = nullptr;
            pushLog("Componente Mesh quitado de '" + m_selected->name + "'");
        }
```

- [ ] **Step 6: Log al quitar Audio Clip**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawAudioClipSection`, reemplaza:

```cpp
        if (removeClicked)
        {
            m_selected->setAudioClip(nullptr);
            // Vuelve a ocultar la sección tras quitar el clip — hay que
            // pulsar "Add > Audio Clip" de nuevo para reabrirla.
            m_audioClipAddRequestedFor = nullptr;
        }
```

por:

```cpp
        if (removeClicked)
        {
            m_selected->setAudioClip(nullptr);
            // Vuelve a ocultar la sección tras quitar el clip — hay que
            // pulsar "Add > Audio Clip" de nuevo para reabrirla.
            m_audioClipAddRequestedFor = nullptr;
            pushLog("Componente Audio Clip quitado de '" + m_selected->name + "'");
        }
```

- [ ] **Step 7: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 8: Verificación manual completa**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Añadir Box Collider, pulsar "x" pa quitarlo → línea `Componente Box Collider quitado de '<name>'`.
- [ ] Repetir con Sphere/Capsule/Plane Collider → 1 línea cada uno, con el tipo correcto.
- [ ] Añadir Mesh, pulsar "x" → línea `Componente Mesh quitado de '<name>'`.
- [ ] Añadir Audio Clip, pulsar "x" → línea `Componente Audio Clip quitado de '<name>'`.
- [ ] Confirmar que "añadir" sigue funcionando igual que antes (regresión) — añadir cada uno de los 6 tipos sigue dejando su línea `añadido a`.
- [ ] Loop/Is 3D del Audio Clip NO dejan línea en el Log (fuera de scope, sin tocar).

- [ ] **Step 9: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(log): instrumenta quitar componente (colliders, mesh, audio)"
```

---

## Self-Review

**Cobertura del spec:** Tabla de 6 puntos de instrumentación (Box/Sphere/Capsule/Plane Collider, Mesh, Audio Clip) → Steps 1-6, uno por sección. Orden relativo (log después de la mutación, dentro del guard existente) → aplicado en los 6 steps, incluido el guard `&& m_renderer` de Mesh (Step 5). Testing del spec → Step 8. Fuera de scope (Loop/Is 3D) → explícitamente verificado en el checklist de Step 8, sin tocar código de esos controles.

**Placeholders:** ninguno — cada paso trae el código completo before/after, verificado contra el contenido real actual de `EditorUI.cpp` en el momento de escribir este plan.

**Consistencia de tipos:** `pushLog(const std::string&) -> void` usa la misma firma en los 6 call sites nuevos, mismo patrón de concatenación (`"Componente <Tipo> quitado de '" + m_selected->name + "'"`) que el ya existente `"Componente <Tipo> añadido a '" + m_selected->name + "'"`.
