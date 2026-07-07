# Ejes de coordenadas al seleccionar GameObject — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Al seleccionar un GameObject en el panel Scene, mostrar sus ejes RGB (`Gizmos::drawAxes`) en su posición/orientación; al deseleccionar o cambiar de selección, dejan de mostrarse.

**Architecture:** Todo el cambio vive en `EditorUI` (ya dueño de `m_selected`). Nuevo método privado `drawSelectionGizmo()` llamado desde `EditorUI::draw()` justo después de `drawScene()` (punto donde selección/borrado ya están resueltos ese frame). Escala de los ejes calculada por `selectionAxisScale()`: bbox local del mesh si tiene, si no valor fijo 50. Se elimina además la llamada de demo `Gizmos::drawAxes(cube->worldTransform, 40.0f)` de `sandbox/src/main.cpp`, redundante con este feature.

**Tech Stack:** C++20, Dear ImGui, GLM. Reutiliza `DonTopo::Gizmos::drawAxes` (ya implementado y en producción).

## Global Constraints

- Spec de referencia: `docs/superpowers/specs/2026-07-07-selection-axes-gizmo-design.md`.
- Sin getter nuevo en `EditorUI`, sin tocar `Renderer` ni el contrato de `Gizmos`.
- Escala: `max(maxHalf, 1.0f) * 1.3f`, donde `maxHalf` es la mitad del eje más largo del bbox local del mesh (si `hasMesh()` y `vertices` no vacío) o, en cualquier otro caso, `50.0f`.
- Sin caché de la escala — se recalcula cada frame solo para el objeto seleccionado.
- Sin framework de tests unitarios en este repo; verificación es build limpio (`.\build.bat`) + prueba manual descrita en el spec.

---

### Task 1: Ejes de selección en `EditorUI` + limpieza de la demo redundante

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h:42-52` (declarar 2 métodos privados nuevos)
- Modify: `engine/src/EditorUI.cpp:1-7` (include), `:96-103` (`draw()`), tras `drawSceneNode()` (nuevas implementaciones)
- Modify: `sandbox/src/main.cpp:260-261` (quitar la línea de demo `drawAxes` redundante)

**Interfaces:**
- Consumes: `DonTopo::Gizmos::drawAxes(const glm::mat4& transform, float scale = 1.0f)` (`engine/include/DonTopo/Gizmos.h`, ya existente) y `GameObject::hasMesh()`/`getMesh()` (`engine/include/DonTopo/GameObject.h`, ya existentes, `getMesh()` retorna `const std::shared_ptr<Mesh>&`, `Mesh::vertices` es `std::vector<Vertex>`, `Vertex::pos` es `glm::vec3`).
- Produces: nada — tarea única, hoja del árbol de dependencias.

- [ ] **Step 1: Añadir el include de Gizmos en `EditorUI.cpp`**

En `engine/src/EditorUI.cpp`, tras `#include "DonTopo/PlaneCollider.h"` (línea 7):

```cpp
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/Gizmos.h"
```

- [ ] **Step 2: Declarar los 2 métodos privados nuevos en `EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h`, dentro de la sección `private:` de métodos (tras `void drawSceneNode(GameObject* node);`, línea 42):

```cpp
    void drawSceneNode(GameObject* node);
    // Dibuja los ejes RGB de Gizmos sobre m_selected (si hay selección),
    // cada frame — desaparecen solos cuando m_selected pasa a nullptr.
    void drawSelectionGizmo();
    // Longitud de eje proporcional al bbox local del mesh de node (mitad
    // del eje más largo); si node no tiene mesh (o el mesh no tiene
    // vértices), valor fijo de repliegue.
    float selectionAxisScale(GameObject* node) const;
```

- [ ] **Step 3: Implementar ambos métodos en `EditorUI.cpp`**

En `engine/src/EditorUI.cpp`, justo después del cierre de `EditorUI::drawSceneNode(...)` (la función que termina con el `}` tras el `for` de `child : node->children` y `ImGui::TreePop();`, antes de `void EditorUI::drawViewport(...)`), añadir:

```cpp
float EditorUI::selectionAxisScale(GameObject* node) const
{
    constexpr float kFallback = 50.0f;
    constexpr float kFactor   = 1.3f;

    if (!node->hasMesh())
        return kFallback;

    const auto& vertices = node->getMesh()->vertices;
    if (vertices.empty())
        return kFallback;

    glm::vec3 bMin = vertices[0].pos;
    glm::vec3 bMax = vertices[0].pos;
    for (const auto& v : vertices)
    {
        bMin = glm::min(bMin, v.pos);
        bMax = glm::max(bMax, v.pos);
    }

    glm::vec3 extent  = bMax - bMin;
    float     maxHalf = glm::max(extent.x, glm::max(extent.y, extent.z)) * 0.5f;
    return glm::max(maxHalf, 1.0f) * kFactor;
}

void EditorUI::drawSelectionGizmo()
{
    if (!m_selected)
        return;
    Gizmos::drawAxes(m_selected->worldTransform, selectionAxisScale(m_selected));
}
```

- [ ] **Step 4: Llamar a `drawSelectionGizmo()` desde `draw()`**

En `engine/src/EditorUI.cpp`, la función `EditorUI::draw(...)` (línea 96-103) pasa de:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawDockSpace();
    drawScene(sceneRoot);
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawContentBrowser();
}
```

a:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawContentBrowser();
}
```

(`drawScene()` ya resuelve ese mismo frame cualquier borrado/deselección pendiente antes de retornar, así que `m_selected` es seguro de leer aquí.)

- [ ] **Step 5: Quitar la llamada de demo redundante en `sandbox/src/main.cpp`**

En `sandbox/src/main.cpp`, dentro del bloque `// --- Gizmos: demo de depuración visual ---` (líneas 260-262), quitar la línea de `drawAxes` y su comentario de sección se actualiza para reflejar que ya no incluye ejes fijos:

```cpp
            // --- Gizmos: demo de depuración visual (ejes, bbox, ray, frustum) ---
            DonTopo::Gizmos::drawAxes(cube->worldTransform, 40.0f);

            if (cube->hasBoxCollider())
```

pasa a:

```cpp
            // --- Gizmos: demo de depuración visual (bbox, ray, frustum) ---
            // Los ejes ya no se dibujan fijos aquí: EditorUI::drawSelectionGizmo()
            // los muestra automáticamente sobre cualquier GameObject seleccionado.
            if (cube->hasBoxCollider())
```

- [ ] **Step 6: Build**

Run: `.\build.bat`
Expected: build succeeds sin errores.

- [ ] **Step 7: Verificación manual**

Ejecutar `Sandbox.exe` y confirmar:

1. Seleccionar `cube` en el panel Scene → aparecen 3 líneas RGB desde su centro, con longitud proporcional a su tamaño (mesh de 50 unidades de lado → ejes visiblemente más cortos que los del `floor`).
2. Seleccionar `floor` → los ejes son notablemente más largos que los del `cube`.
3. Click en zona vacía del panel Scene → los ejes desaparecen.
4. Cambiar de un objeto a otro → nunca se ven los ejes de dos objetos a la vez.
5. Crear un GameObject vacío ("Create GameObject", sin mesh ni collider) y seleccionarlo → aparecen ejes de longitud fija (repliegue 50), ni cero ni gigantes.
6. Borrar el objeto seleccionado (tecla Delete) → los ejes desaparecen sin crash.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp sandbox/src/main.cpp
git commit -m "feat(gizmos): mostrar ejes de coordenadas al seleccionar GameObject en el editor"
```
