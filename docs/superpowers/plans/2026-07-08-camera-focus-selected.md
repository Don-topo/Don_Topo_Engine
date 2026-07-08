# Camera Focus on Selected GameObject (tecla F) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Al pulsar F, la cámara del editor se acerca y centra en el GameObject seleccionado en el panel Scene, salvo que el usuario esté escribiendo en una caja de texto de ImGui.

**Architecture:** `Camera::focusOn(center, radius)` (matemática pura de reposicionamiento) es llamado por `EditorUI::focusSelected(Camera&)` (que conoce `m_selected` y calcula centro/radio a partir del bbox del mesh), expuesto vía passthrough `Renderer::focusSelected(Camera&)`, disparado desde el `glfwSetKeyCallback` existente en `main.cpp` con gate `!ImGui::GetIO().WantTextInput`.

**Tech Stack:** C++20, GLM, GLFW, ImGui, Vulkan (sin relación directa con Vulkan en este feature).

## Global Constraints

- Salto instantáneo, sin animación/lerp (spec: no hay sistema de interpolación temporal en Camera).
- `kFocusDistanceFactor = 2.5f`, `kMinRadius = 5.0f`, `kFallbackRadius = 50.0f` (spec, sección Constantes).
- Gate exacto: `ImGui::GetIO().WantTextInput`, NO `WantCaptureKeyboard` (spec explica por qué).
- Sin gate de `isViewportHovered()` — F funciona con cualquier panel bajo el mouse mientras no sea caja de texto.
- No hay framework de tests unitarios en este repo (motor gráfico C++/Vulkan sin CMake `enable_testing`/gtest). Verificación es: build limpio (`cmake --build --preset debug`) tras cada tarea + checklist manual final (ejecutar el binario) en la última tarea.
- Build: `cmake --build --preset debug` (binaryDir `build-ninja`).

---

### Task 1: `Camera::focusOn`

**Files:**
- Modify: `engine/include/DonTopo/Camera.h`
- Modify: `engine/src/Camera.cpp`

**Interfaces:**
- Produces: `void Camera::focusOn(const glm::vec3& center, float boundingRadius)` — método público. Reposiciona `m_pos`/`m_yaw`/`m_pitch` (vía `lookAlongAxis`, ya existente) pa que la cámara retroceda a lo largo del vector cámara→`center` actual y mire directo a `center`.

- [ ] **Step 1: Añadir declaración del método en `Camera.h`**

En `engine/include/DonTopo/Camera.h`, justo después de `lookAlongAxis`:

```cpp
            void lookAlongAxis(const glm::vec3& axis);
            // Reposiciona la cámara pa encuadrar un objeto: retrocede a lo largo
            // del vector cámara→center actual una distancia proporcional a
            // boundingRadius, y queda mirando directo a center (usado por "F"
            // pa centrar en el GameObject seleccionado).
            void focusOn(const glm::vec3& center, float boundingRadius);
```

- [ ] **Step 2: Implementar en `Camera.cpp`**

En `engine/src/Camera.cpp`, después de `lookAlongAxis`:

```cpp
    void Camera::focusOn(const glm::vec3& center, float boundingRadius)
    {
        constexpr float kFocusDistanceFactor = 2.5f;
        constexpr float kMinRadius           = 5.0f;
        constexpr float kEpsilon             = 0.0001f;

        glm::vec3 dir = m_pos - center;
        if (glm::length(dir) < kEpsilon)
            dir = -m_front;
        dir = glm::normalize(dir);

        float distance = std::max(boundingRadius, kMinRadius) * kFocusDistanceFactor;
        m_pos = center + dir * distance;
        lookAlongAxis(dir);
    }
```

- [ ] **Step 3: Build**

Run: `cmake --build --preset debug`
Expected: build succeeds, sin errores en `Camera.cpp`/`Camera.h`.

- [ ] **Step 4: Commit**

```bash
git add engine/include/DonTopo/Camera.h engine/src/Camera.cpp
git commit -m "feat(camera): añadir Camera::focusOn para encuadrar un punto"
```

---

### Task 2: `EditorUI::focusSelected`

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `Camera::focusOn(const glm::vec3& center, float boundingRadius)` (Task 1). `GameObject::hasMesh()`, `GameObject::getMesh()`, `GameObject::worldTransform` (ya existentes en `GameObject.h`). Miembro privado existente `GameObject* m_selected`.
- Produces: `void EditorUI::focusSelected(Camera& camera)` — método público, no-op si `m_selected == nullptr`.

- [ ] **Step 1: Forward-declarar `Camera` y declarar el método en `EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h`, añadir a la lista de forward-declarations (junto a `class GameObject;` etc., línea ~12-19):

```cpp
class Camera;
```

Y añadir a la sección pública de `EditorUI` (después de `setRenderer`, línea ~44):

```cpp
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
    // Centra la cámara en m_selected (no-op si no hay selección). Usado por
    // el atajo de teclado "F" en main.cpp.
    void focusSelected(Camera& camera);
```

- [ ] **Step 2: Implementar en `EditorUI.cpp`**

En `engine/src/EditorUI.cpp`, añadir `#include "DonTopo/Camera.h"` junto a los demás includes (línea ~9, tras `#include "DonTopo/Renderer.h"`), y añadir la función después de `selectionAxisScale` (línea ~377, antes de `drawSelectionGizmo`):

```cpp
void EditorUI::focusSelected(Camera& camera)
{
    if (!m_selected)
        return;

    constexpr float kFallbackRadius = 50.0f;

    glm::vec3 center = glm::vec3(m_selected->worldTransform[3]);
    float     radius = kFallbackRadius;

    if (m_selected->hasMesh())
    {
        const auto& vertices = m_selected->getMesh()->vertices;
        if (!vertices.empty())
        {
            glm::vec3 bMin = vertices[0].pos;
            glm::vec3 bMax = vertices[0].pos;
            for (const auto& v : vertices)
            {
                bMin = glm::min(bMin, v.pos);
                bMax = glm::max(bMax, v.pos);
            }
            glm::vec3 extent   = bMax - bMin;
            float     maxHalf  = glm::max(extent.x, glm::max(extent.y, extent.z)) * 0.5f;

            glm::vec3 worldScale(
                glm::length(glm::vec3(m_selected->worldTransform[0])),
                glm::length(glm::vec3(m_selected->worldTransform[1])),
                glm::length(glm::vec3(m_selected->worldTransform[2])));
            float maxWorldScale = glm::max(worldScale.x, glm::max(worldScale.y, worldScale.z));

            radius = glm::max(maxHalf, 1.0f) * maxWorldScale;
        }
    }

    camera.focusOn(center, radius);
}
```

- [ ] **Step 3: Build**

Run: `cmake --build --preset debug`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): EditorUI::focusSelected calcula bbox mundo y centra cámara"
```

---

### Task 3: `Renderer::focusSelected` passthrough + integración tecla F

**Files:**
- Modify: `engine/include/DonTopo/Renderer.h`
- Modify: `sandbox/src/main.cpp`

**Interfaces:**
- Consumes: `EditorUI::focusSelected(Camera&)` (Task 2).
- Produces: `void Renderer::focusSelected(Camera& camera)` — passthrough inline.

- [ ] **Step 1: Añadir passthrough en `Renderer.h`**

En `engine/include/DonTopo/Renderer.h`, junto a `setPhysicsManager` (línea ~37):

```cpp
            void setPhysicsManager(PhysicsManager* physics) { m_editorUI.setPhysicsManager(physics); }
            void focusSelected(Camera& camera) { m_editorUI.focusSelected(camera); }
```

- [ ] **Step 2: Enganchar tecla F en `main.cpp`**

En `sandbox/src/main.cpp`, dentro del `glfwSetKeyCallback` existente (línea ~178-182), añadir el chequeo de F junto al de ESCAPE:

```cpp
        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int scancode, int action, int mods) {
            ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods);
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
                glfwSetWindowShouldClose(w, GLFW_TRUE);
            if (key == GLFW_KEY_F && action == GLFW_PRESS && !ImGui::GetIO().WantTextInput)
            {
                auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
                ctx->rnd->focusSelected(*ctx->cam);
            }
        });
```

- [ ] **Step 3: Build**

Run: `cmake --build --preset debug`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add engine/include/DonTopo/Renderer.h sandbox/src/main.cpp
git commit -m "feat(editor): tecla F centra cámara en GameObject seleccionado"
```

---

### Task 4: Verificación manual end-to-end

**Files:** ninguno (solo ejecución del binario ya compilado en Task 3).

- [ ] **Step 1: Lanzar el sandbox**

Run: `build-ninja\sandbox\Debug\sandbox.exe` (o el path de salida que use el preset `debug`; confirmar con `cmake --build --preset debug --target sandbox -v` si el binario no está en esa ruta).

- [ ] **Step 2: Caso básico — objeto con mesh**

Seleccionar un GameObject con mesh (ej. el cubo) en el panel Scene, pulsar `F`.
Expected: la cámara salta y queda mirando directo al objeto, encuadrado (ni pegada ni lejísimos).

- [ ] **Step 3: Caso sin selección**

Deseleccionar (click en vacío del panel Scene o similar), pulsar `F`.
Expected: nada pasa, sin crash.

- [ ] **Step 4: Caso caja de texto activa**

Doble-click o F2 sobre un nodo pa abrir el popup de rename (o poner el cursor en modo edición de un DragFloat de Position en Properties), y con el foco ahí pulsar `F`.
Expected: se escribe una "f" en el campo de texto; la cámara NO se mueve.

- [ ] **Step 5: Caso mouse sobre panel Scene (sin hover en viewport)**

Con un objeto seleccionado, mover el mouse sobre el panel Scene (no el Viewport) y pulsar `F`.
Expected: la cámara SÍ se centra (no hay gate de `isViewportHovered()`).

- [ ] **Step 6: Caso objeto sin mesh**

Si existe o se crea un GameObject vacío (sin mesh, ej. un padre usado solo pa agrupar), seleccionarlo y pulsar `F`.
Expected: la cámara se centra usando el radio fallback (50.0f), sin crash.

- [ ] **Step 7: Commit final (si hubo algún ajuste durante la verificación)**

Solo si Step 2-6 revelaron necesidad de tocar código; en ese caso, repetir build + commit con mensaje describiendo el ajuste. Si todo pasó sin cambios, no hay nada que commitear en este step.
