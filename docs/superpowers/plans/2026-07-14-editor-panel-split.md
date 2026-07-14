# Editor Panel Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Partir el monolito `EditorUI` (2671 líneas en `EditorUI.cpp`) en una clase por ventana (Scene, Viewport, Properties, Log, Content Browser), dejando `EditorUI` como shell orquestador delgado.

**Architecture:** Se introduce `EditorContext`, un struct de referencias/punteros compartidos (selección, física/renderer/audio/scene/scriptManager, undo, callbacks de log/delete/axis) construido una vez por frame dentro de `EditorUI::draw()` y pasado por referencia al `draw()` de cada panel. Cada panel es dueño exclusivo de su propio estado privado (caches de edición, diálogos IGFD, popups). `EditorUI` conserva menú/toolbar/dockspace/atajo undo-redo/diálogo de escena (cross-cutting, no pertenecen a ninguna ventana) y reenvía la API pública externa (`focusSelected`, `isViewportHovered`, `pushExternalLog`, etc.) a los paneles dueños del estado correspondiente.

**Tech Stack:** C++20, Dear ImGui (docking), ImGuiFileDialog (IGFD), glm, nlohmann::json. Sin framework de tests (repo no usa gtest/ctest) — verificación es build limpio + smoke test manual del Sandbox, mismo patrón que el resto de paneles del editor (ScriptEditorPanel, LogPanel original).

## Global Constraints

- No cambiar comportamiento observable: mismo orden de dibujo de ventanas, mismos atajos, mismos mensajes de log.
- Mover código verbatim (cortar/pegar), no reescribir lógica. Solo cambia dónde vive el estado y cómo se referencia (`m_selected` → `ctx.selected`, etc.).
- Namespace `DonTopo` para todas las clases nuevas.
- Cada panel nuevo sigue el patrón ya usado por `ScriptEditorPanel` (`GetOpenPtr()`, `setLogCallback`-style donde aplique).
- Tras cada tarea: `& .\build.bat` (PowerShell, no Bash — ver [[project_build_commands]]) debe compilar sin error antes de pasar a la siguiente tarea.
- Última tarea incluye smoke test manual: `& .\build-ninja\sandbox\Sandbox.exe`, comprobar que las 5 ventanas + Script Editor abren/cierran desde el menú View, selección/gizmo/undo-redo/log siguen funcionando.

---

## Mapa de extracción (referencia rápida)

Funciones y su destino, con rango de líneas actual en `engine/src/Editor/EditorUI.cpp` (antes de tocar nada — verificar con `grep -n` si el fichero cambió entre tareas):

| Destino | Funciones | Líneas aprox. |
|---|---|---|
| **LogPanel** | `pushLog`, `drawLogPanel` | 394-423 |
| **ScenePanel** | `drawScene`, `drawSceneNode`, `beginRename`, `createBasicShape`, `selectionAxisScale`* | 424-662, 663-674, 804-824, 877-953 |
| **ViewportPanel** | `drawViewport`, `drawSelectionGizmo`, `selectionAxisScale`, `focusSelected` | 954-1116 |
| **PropertiesPanel** | `drawProperties`, `drawBoxColliderSection`, `drawSphereColliderSection`, `drawCapsuleColliderSection`, `drawPlaneColliderSection`, `drawMeshSection`, `drawMeshDialog`, `drawAudioClipSection`, `drawAudioClipDialog`, `drawScriptsSection`, `drawAddComponentButton`, `drawNewScriptPopup`, `loadMeshForSelected`, `loadAudioClipForSelected` | 825-876, 1117-2393 |
| **ContentBrowserPanel** | `drawContentBrowser`, `beginAssetRename`, `updateSceneReferencesForRename`, `countSceneReferences`, `detachSceneReferencesForDelete`, `beginAssetDelete` | 675-800 (helpers), 2394-2671 |
| **EditorUI (shell, se queda)** | `handleUndoRedoShortcut`, `drawMenuBar`, `drawToolbar`, `drawDockSpace`, `reloadSceneFromJson`, `drawSceneDialog` | 239-393, 1998-2074 |

\* `selectionAxisScale` solo la usa `drawSelectionGizmo` → va a `ViewportPanel`, no a `ScenePanel`.

Orden de dibujo actual en `EditorUI::draw()` (línea 221-237) a preservar:
```
handleUndoRedoShortcut(); drawMenuBar(); drawToolbar(); drawDockSpace();
drawScene(sceneRoot); drawSelectionGizmo(); drawViewport(...); drawProperties();
drawLogPanel(); drawMeshDialog(); drawAudioClipDialog(); drawSceneDialog();
drawContentBrowser(sceneRoot); m_scriptEditor->draw();
```
Nuevo orden equivalente (mesh/audio dialogs se pliegan dentro de `PropertiesPanel::draw`, orden relativo entre popups no importa en ImGui — son ventanas/popups independientes):
```
handleUndoRedoShortcut(); drawMenuBar(); drawToolbar(); drawDockSpace();
m_scenePanel.draw(ctx, sceneRoot);
m_viewportPanel.draw(ctx, viewportTexture, cameraView);   // incluye gizmo + dialogs internos
m_propertiesPanel.draw(ctx);                               // incluye drawMeshDialog + drawAudioClipDialog
m_logPanel.draw();
drawSceneDialog();
m_contentBrowserPanel.draw(ctx, sceneRoot);
m_scriptEditor->draw();
```

---

### Task 1: `EditorContext` — struct compartido

**Files:**
- Create: `engine/include/DonTopo/Editor/EditorContext.h`
- Modify: `engine/CMakeLists.txt` (ningún .cpp nuevo aún, es header-only — no toca la lista de fuentes)

**Interfaces:**
- Produces: `struct DonTopo::EditorContext` — consumido por todas las tareas siguientes.

- [ ] **Step 1: Crear el header**

```cpp
#pragma once
#include <functional>
#include <string>
#include <glm/glm.hpp>

namespace DonTopo {

class GameObject;
class PhysicsManager;
class AudioManager;
class Renderer;
class Scene;
class ScriptManager;
class UndoManager;

// Estado compartido entre los paneles del editor, construido de nuevo cada
// frame dentro de EditorUI::draw() y pasado por referencia a cada
// Panel::draw(). `selected` es una referencia real a EditorUI::m_selected:
// un panel que la reasigna (p.ej. ScenePanel al hacer click en un nodo)
// propaga el cambio a los paneles que se dibujan después en el mismo frame
// (Viewport, Properties), igual que hacía el m_selected único de EditorUI.
struct EditorContext {
    GameObject*& selected;
    bool&        isPlaying;

    PhysicsManager* physics       = nullptr;
    Renderer*       renderer      = nullptr;
    AudioManager*   audio         = nullptr;
    Scene*          scene         = nullptr;
    ScriptManager*  scriptManager = nullptr;
    UndoManager*    undo          = nullptr;

    std::function<void(const std::string&)>   pushLog;
    std::function<void(GameObject*)>          onDelete;
    std::function<void(const glm::vec3&)>     onAxisSelected;
};

} // namespace DonTopo
```

- [ ] **Step 2: Build**

```powershell
& .\build.bat
```
Expected: compila sin error (header no usado todavía por nadie, solo debe parsear).

- [ ] **Step 3: Commit**

```bash
git add engine/include/DonTopo/Editor/EditorContext.h
git commit -m "refactor(editor): añadir EditorContext compartido entre paneles"
```

---

### Task 2: `LogPanel`

**Files:**
- Create: `engine/include/DonTopo/Editor/LogPanel.h`
- Create: `engine/src/Editor/LogPanel.cpp`
- Modify: `engine/CMakeLists.txt:6` (añadir `src/Editor/LogPanel.cpp` junto a `src/Editor/EditorUI.cpp`)
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp`

**Interfaces:**
- Consumes: nada de `EditorContext` (el log no toca selección/física/etc.).
- Produces: `LogPanel::push(const std::string&)`, `LogPanel::draw()`, `LogPanel::GetOpenPtr()` — usados por `EditorUI` (Task 7) y por cualquier panel que reciba `ctx.pushLog`.

- [ ] **Step 1: Crear `LogPanel.h`**

```cpp
#pragma once
#include <deque>
#include <string>

namespace DonTopo {

// Ring buffer de acciones de edición confirmadas, más reciente al final.
// Sin persistencia a disco (no hace falta guardar nada).
class LogPanel {
public:
    void push(const std::string& message);
    void draw();
    bool* GetOpenPtr() { return &m_open; }

private:
    static constexpr size_t kLogMaxEntries = 200;
    std::deque<std::string> m_entries;
    // true si el panel ya estaba scrolleado al fondo el frame anterior —
    // evita pelear con el usuario si sube a leer historial mientras llegan
    // más líneas.
    bool m_autoScroll = true;
    bool m_open = true;
};

} // namespace DonTopo
```

- [ ] **Step 2: Crear `LogPanel.cpp`**

Mover el cuerpo exacto de `EditorUI::pushLog` (líneas 394-406) y `EditorUI::drawLogPanel` (líneas 407-423) de `engine/src/Editor/EditorUI.cpp`, renombrando `pushLog`→`push`, `drawLogPanel`→`draw`, y las referencias a `m_logEntries`/`m_logAutoScroll`/`m_logOpen`/`kLogMaxEntries` a `m_entries`/`m_autoScroll`/`m_open`/`kLogMaxEntries` (sin prefijo `m_log`). `draw()` debe envolver su contenido en `if (!m_open) return;` al principio, igual que hacían las demás ventanas vía `m_logOpen` — comprobar cómo lo hacía `drawLogPanel` original (el `ImGui::Begin("Log Console", &m_logOpen)` ya controla visibilidad; mantener esa forma con `&m_open`).

```cpp
#include "DonTopo/Editor/LogPanel.h"
#include <imgui.h>
#include <chrono>
#include <ctime>

namespace DonTopo {

void LogPanel::push(const std::string& message)
{
    // ... cuerpo exacto de EditorUI::pushLog, adaptado a m_entries ...
}

void LogPanel::draw()
{
    // ... cuerpo exacto de EditorUI::drawLogPanel, adaptado a m_open/m_autoScroll/m_entries ...
}

} // namespace DonTopo
```

- [ ] **Step 3: Quitar de `EditorUI.h`**

Borrar declaraciones: `pushLog`, `drawLogPanel`, `kLogMaxEntries`, `m_logEntries`, `m_logAutoScroll`, `m_logOpen`. Añadir:
```cpp
#include "DonTopo/Editor/LogPanel.h"
```
y miembro:
```cpp
LogPanel m_logPanel;
```
`pushExternalLog` pasa a reenviar: `void pushExternalLog(const std::string& message) { m_logPanel.push(message); }`.

- [ ] **Step 4: Actualizar `EditorUI.cpp`**

- Borrar los cuerpos movidos de `pushLog`/`drawLogPanel`.
- Todas las llamadas internas a `pushLog(...)` dentro de `EditorUI.cpp` (fuera del código que se mueve en tareas posteriores) pasan a `m_logPanel.push(...)`. En esta tarea eso afecta solo a las llamadas que aún viven en `EditorUI.cpp` tras esta task (las que se moverán a otros paneles en tareas siguientes se resuelven en esas tareas vía `ctx.pushLog`).
- En `draw()`: sustituir `drawLogPanel();` por `m_logPanel.draw();`.
- En el constructor: `m_scriptEditor->setLogCallback([this](const std::string& msg) { m_logPanel.push(msg); });`.

- [ ] **Step 5: CMakeLists**

Añadir `src/Editor/LogPanel.cpp` en `engine/CMakeLists.txt` junto a la línea de `EditorUI.cpp`.

- [ ] **Step 6: Build**

```powershell
& .\configure.bat
& .\build.bat
```
Expected: compila sin error (nuevo fichero fuente requiere reconfigure).

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/LogPanel.h engine/src/Editor/LogPanel.cpp engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp engine/CMakeLists.txt
git commit -m "refactor(editor): extraer LogPanel de EditorUI"
```

---

### Task 3: `ScenePanel`

**Files:**
- Create: `engine/include/DonTopo/Editor/ScenePanel.h`
- Create: `engine/src/Editor/ScenePanel.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp`

**Interfaces:**
- Consumes: `EditorContext` (Task 1) — usa `ctx.selected`, `ctx.renderer`, `ctx.scene`, `ctx.physics`, `ctx.audio`, `ctx.undo`, `ctx.pushLog`, `ctx.onDelete`.
- Produces: `ScenePanel::draw(EditorContext& ctx, GameObject* sceneRoot)`, `ScenePanel::GetOpenPtr()`.

- [ ] **Step 1: Crear `ScenePanel.h`**

```cpp
#pragma once
#include <memory>
#include <string>

namespace DonTopo {

class GameObject;
class Mesh;
struct EditorContext;

// Ventana "Scene" — árbol jerárquico de GameObjects (hover/click de
// selección, drag&drop de reorder, popup de rename, menú contextual de
// Create/Delete/Basic Shapes).
class ScenePanel {
public:
    void draw(EditorContext& ctx, GameObject* sceneRoot);
    bool* GetOpenPtr() { return &m_open; }

private:
    void drawNode(EditorContext& ctx, GameObject* node);
    void beginRename(GameObject* node);
    void createBasicShape(EditorContext& ctx, GameObject* parent, const std::string& name,
                           std::shared_ptr<Mesh> mesh);

    bool m_open = true;

    // Borrado/reorder diferidos al final del frame: el árbol se recorre con
    // recursión sobre std::vector<unique_ptr<GameObject>>, mutarlo en medio
    // de esa recursión invalidaría los iteradores de los for-range activos.
    GameObject* m_pendingDelete = nullptr;
    GameObject* m_pendingMoveSource = nullptr;
    GameObject* m_pendingMoveTarget = nullptr;

    // Rename — popup modal disparado por "Rename" (click derecho) o F2.
    GameObject* m_renameTarget = nullptr;
    char        m_renameBuffer[128] = {};
    bool        m_openRenamePopup = false;
};

} // namespace DonTopo
```

- [ ] **Step 2: Crear `ScenePanel.cpp`**

Mover verbatim de `EditorUI.cpp`: `drawScene` (424-662, renombrar a `ScenePanel::draw`), `beginRename` (663-674), `createBasicShape` (804-824), `drawSceneNode` (877-953, renombrar a `ScenePanel::drawNode`). Sustituir todas las referencias a `m_selected`→`ctx.selected`, `m_pendingDelete`/`m_pendingMoveSource`/`m_pendingMoveTarget`/`m_renameTarget`/`m_renameBuffer`/`m_openRenamePopup`→ mismos nombres sin cambio (siguen siendo miembros de `ScenePanel`), `m_undoHistory`→`*ctx.undo`, `pushLog(...)`→`ctx.pushLog(...)`, `m_onDelete`→`ctx.onDelete`, `m_renderer`→`ctx.renderer`, `m_physics`/`m_audio`/`m_scene`→`ctx.physics`/`ctx.audio`/`ctx.scene`, `m_scriptManager`→`ctx.scriptManager`, `m_isPlaying`→`ctx.isPlaying`. Añadir `if (!m_open) return;` al inicio de `draw()` si `drawScene` no lo tenía ya vía `ImGui::Begin("Scene", &m_sceneOpen)` — mantener ese patrón con `&m_open`.

```cpp
#include "DonTopo/Editor/ScenePanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Renderer/Renderer.h"
#include <imgui.h>

namespace DonTopo {

void ScenePanel::draw(EditorContext& ctx, GameObject* sceneRoot)
{
    // ... cuerpo exacto de EditorUI::drawScene ...
}

void ScenePanel::drawNode(EditorContext& ctx, GameObject* node)
{
    // ... cuerpo exacto de EditorUI::drawSceneNode ...
}

void ScenePanel::beginRename(GameObject* node)
{
    // ... cuerpo exacto de EditorUI::beginRename ...
}

void ScenePanel::createBasicShape(EditorContext& ctx, GameObject* parent, const std::string& name,
                                   std::shared_ptr<Mesh> mesh)
{
    // ... cuerpo exacto de EditorUI::createBasicShape ...
}

} // namespace DonTopo
```

- [ ] **Step 3: Quitar de `EditorUI.h`/`EditorUI.cpp`**

Borrar declaraciones/cuerpos de `drawScene`, `drawSceneNode`, `beginRename`, `createBasicShape`, y miembros `m_sceneOpen`, `m_pendingDelete`, `m_pendingMoveSource`, `m_pendingMoveTarget`, `m_renameTarget`, `m_renameBuffer`, `m_openRenamePopup`. Añadir `#include "DonTopo/Editor/ScenePanel.h"` y miembro `ScenePanel m_scenePanel;`. En `draw()`, sustituir `drawScene(sceneRoot);` por la construcción de `EditorContext ctx{...}` (ver Task 7 para el listado final — en esta tarea intermedia basta con construir un `ctx` local mínimo que ya incluya `selected`/`renderer`/`scene`/`physics`/`audio`/`scriptManager`/`isPlaying`/`undo`/`pushLog`/`onDelete`, reutilizado por las tareas siguientes) seguido de `m_scenePanel.draw(ctx, sceneRoot);`.

- [ ] **Step 4: CMakeLists** — añadir `src/Editor/ScenePanel.cpp`.

- [ ] **Step 5: Build**

```powershell
& .\configure.bat
& .\build.bat
```

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/ScenePanel.h engine/src/Editor/ScenePanel.cpp engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp engine/CMakeLists.txt
git commit -m "refactor(editor): extraer ScenePanel de EditorUI"
```

---

### Task 4: `ViewportPanel`

**Files:**
- Create: `engine/include/DonTopo/Editor/ViewportPanel.h`
- Create: `engine/src/Editor/ViewportPanel.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp`

**Interfaces:**
- Consumes: `EditorContext` — `ctx.selected`.
- Produces: `ViewportPanel::draw(EditorContext& ctx, VkDescriptorSet viewportTexture, const glm::mat4& cameraView)`, `ViewportPanel::focusSelected(EditorContext& ctx, Camera& camera)`, `ViewportPanel::isHovered() const`, `ViewportPanel::GetOpenPtr()`.

- [ ] **Step 1: Crear `ViewportPanel.h`**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace DonTopo {

class GameObject;
class Camera;
struct EditorContext;

// Ventana "Viewport" — render 3D embebido (textura del Renderer) + gizmo de
// ejes/wireframe de collider sobre la selección activa.
class ViewportPanel {
public:
    void draw(EditorContext& ctx, VkDescriptorSet viewportTexture, const glm::mat4& cameraView);
    // Centra la cámara en ctx.selected (no-op si no hay selección). Usado
    // por el atajo de teclado "F" en main.cpp vía EditorUI::focusSelected.
    void focusSelected(EditorContext& ctx, Camera& camera);
    bool isHovered() const { return m_hovered; }
    bool* GetOpenPtr() { return &m_open; }

private:
    void drawSelectionGizmo(EditorContext& ctx);
    // Longitud de eje proporcional al bbox local del mesh de node (mitad
    // del eje más largo); si node no tiene mesh (o el mesh no tiene
    // vértices), valor fijo de repliegue.
    float selectionAxisScale(GameObject* node) const;

    bool m_open = true;
    bool m_hovered = false;
};

} // namespace DonTopo
```

- [ ] **Step 2: Crear `ViewportPanel.cpp`**

Mover verbatim: `drawViewport` (954-1116 tras renumeración de Tasks 2-3 — reverificar con `grep -n "EditorUI::drawViewport\|EditorUI::drawSelectionGizmo\|EditorUI::selectionAxisScale\|EditorUI::focusSelected" engine/src/Editor/EditorUI.cpp` antes de cortar), `drawSelectionGizmo`, `selectionAxisScale`, `focusSelected`. Sustituir `m_selected`→`ctx.selected`, `m_viewportHovered`→`m_hovered`.

```cpp
#include "DonTopo/Editor/ViewportPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Editor/Gizmos.h"
#include "DonTopo/Renderer/Camera.h"
#include <imgui.h>

namespace DonTopo {

void ViewportPanel::draw(EditorContext& ctx, VkDescriptorSet viewportTexture, const glm::mat4& cameraView)
{
    // ... cuerpo exacto de EditorUI::drawViewport ...
}

void ViewportPanel::drawSelectionGizmo(EditorContext& ctx)
{
    // ... cuerpo exacto de EditorUI::drawSelectionGizmo ...
}

float ViewportPanel::selectionAxisScale(GameObject* node) const
{
    // ... cuerpo exacto de EditorUI::selectionAxisScale ...
}

void ViewportPanel::focusSelected(EditorContext& ctx, Camera& camera)
{
    // ... cuerpo exacto de EditorUI::focusSelected ...
}

} // namespace DonTopo
```

- [ ] **Step 3: Quitar de `EditorUI.h`/`EditorUI.cpp`**

Borrar `drawViewport`, `drawSelectionGizmo`, `selectionAxisScale`, `m_viewportOpen`, `m_viewportHovered`. `focusSelected` y `isViewportHovered` en `EditorUI` pasan a reenviar:
```cpp
void focusSelected(Camera& camera) { m_viewportPanel.focusSelected(m_ctx, camera); }
bool isViewportHovered() const { return m_viewportPanel.isHovered(); }
```
(`m_ctx` — ver nota de Task 3 Step 3; si aún no existe como miembro persistente, construir un `EditorContext` local en `focusSelected`/`isViewportHovered` con los mismos campos que en `draw()`. Preferible: mantener `ctx` como variable local reconstruida en cada llamada pública, no como miembro — evita vida útil ambigua de las referencias).

Añadir `#include "DonTopo/Editor/ViewportPanel.h"` y miembro `ViewportPanel m_viewportPanel;`. En `draw()`: `drawSelectionGizmo(); drawViewport(viewportTexture, cameraView);` → `m_viewportPanel.draw(ctx, viewportTexture, cameraView);`.

- [ ] **Step 4: CMakeLists** — añadir `src/Editor/ViewportPanel.cpp`.

- [ ] **Step 5: Build**

```powershell
& .\configure.bat
& .\build.bat
```

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/ViewportPanel.h engine/src/Editor/ViewportPanel.cpp engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp engine/CMakeLists.txt
git commit -m "refactor(editor): extraer ViewportPanel de EditorUI"
```

---

### Task 5: `PropertiesPanel`

**Files:**
- Create: `engine/include/DonTopo/Editor/PropertiesPanel.h`
- Create: `engine/src/Editor/PropertiesPanel.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp`

Este es el panel más grande (~1500 líneas originales: colliders, mesh, audio, scripts, add-component, new-script popup). Es el que más beneficio de aislamiento aporta.

**Interfaces:**
- Consumes: `EditorContext` — `ctx.selected`, `ctx.renderer`, `ctx.physics`, `ctx.audio`, `ctx.scriptManager`, `ctx.scene`, `ctx.undo`, `ctx.pushLog`.
- Produces: `PropertiesPanel::draw(EditorContext& ctx)`, `PropertiesPanel::GetOpenPtr()`.

- [ ] **Step 1: Crear `PropertiesPanel.h`**

Todos los miembros de cache/edición que hoy viven en `EditorUI.h` líneas 280-364 (Properties, Box/Sphere/Capsule/Plane Collider, mesh/audio dialogs, new-script popup) se mueven tal cual a esta clase, con los mismos nombres (quitando el contexto de "son de EditorUI", ya no hace falta prefijo). Mantener los comentarios explicativos existentes (documentan invariantes no obvias: por qué cachear, cuándo resincronizar).

```cpp
#pragma once
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include "DonTopo/Editor/UndoManager.h" // BoxColliderState, SphereColliderState, CapsuleColliderState, PlaneColliderState

namespace IGFD { class FileDialog; }

namespace DonTopo {

class GameObject;
class BoxCollider;
class SphereCollider;
class CapsuleCollider;
class PlaneCollider;
struct EditorContext;

class PropertiesPanel {
public:
    PropertiesPanel();
    ~PropertiesPanel();
    PropertiesPanel(const PropertiesPanel&) = delete;
    PropertiesPanel& operator=(const PropertiesPanel&) = delete;

    void draw(EditorContext& ctx);
    bool* GetOpenPtr() { return &m_open; }

private:
    void drawBoxColliderSection(EditorContext& ctx);
    void drawSphereColliderSection(EditorContext& ctx);
    void drawCapsuleColliderSection(EditorContext& ctx);
    void drawPlaneColliderSection(EditorContext& ctx);
    void drawMeshSection(EditorContext& ctx);
    void drawMeshDialog(EditorContext& ctx);
    void drawAudioClipSection(EditorContext& ctx);
    void drawAudioClipDialog(EditorContext& ctx);
    void drawScriptsSection(EditorContext& ctx);
    void drawAddComponentButton(EditorContext& ctx);
    void drawNewScriptPopup(EditorContext& ctx);
    void loadMeshForSelected(EditorContext& ctx, const std::string& path);
    void loadAudioClipForSelected(EditorContext& ctx, const std::string& path);

    bool m_open = true;

    // Properties – cache de edición del nodo seleccionado (persiste entre
    // frames para que DragFloat pueda acumular el delta del arrastre; solo
    // se re-sincroniza con localTransform al cambiar de selección).
    GameObject* m_propsCachedFor = nullptr;
    glm::vec3   m_editPosition{0.0f};
    glm::vec3   m_editRotationDeg{0.0f};
    glm::vec3   m_editScale{1.0f};
    bool        m_transformDragActive = false;
    glm::mat4   m_transformBeforeEdit{1.0f};

    BoxCollider* m_colliderCachedFor = nullptr;
    glm::vec3    m_editColliderCenter{0.0f};
    glm::vec3    m_editColliderSize{50.0f};
    bool         m_editUseGravity = false;
    bool         m_colliderDragActive = false;
    BoxColliderState m_boxColliderBeforeEdit{};

    SphereCollider* m_sphereColliderCachedFor = nullptr;
    glm::vec3       m_editSphereCenter{0.0f};
    float           m_editSphereRadius{25.0f};
    bool            m_editSphereUseGravity = false;
    bool            m_sphereColliderDragActive = false;
    SphereColliderState m_sphereColliderBeforeEdit{};

    CapsuleCollider* m_capsuleColliderCachedFor = nullptr;
    glm::vec3        m_editCapsuleCenter{0.0f};
    float            m_editCapsuleRadius{15.0f};
    float            m_editCapsuleHeight{50.0f};
    bool             m_editCapsuleUseGravity = false;
    bool             m_capsuleColliderDragActive = false;
    CapsuleColliderState m_capsuleColliderBeforeEdit{};

    PlaneCollider* m_planeColliderCachedFor = nullptr;
    glm::vec3      m_editPlaneCenter{0.0f};
    bool           m_planeColliderDragActive = false;
    PlaneColliderState m_planeColliderBeforeEdit{};

    bool m_meshDlgOpen = false;
    std::unique_ptr<IGFD::FileDialog> m_meshFileDialog;
    std::string m_meshLoadError;
    GameObject* m_meshAddRequestedFor = nullptr;

    bool m_audioDlgOpen = false;
    std::unique_ptr<IGFD::FileDialog> m_audioFileDialog;
    std::string m_audioLoadError;
    GameObject* m_audioClipAddRequestedFor = nullptr;

    bool        m_openNewScriptPopup = false;
    char        m_newScriptNameBuffer[64] = {};
    std::string m_newScriptError;
    GameObject* m_newScriptTarget = nullptr;
};

} // namespace DonTopo
```

- [ ] **Step 2: Crear `PropertiesPanel.cpp`**

Mover verbatim (tras reverificar líneas con `grep -n` — el fichero ha cambiado en tareas previas): `drawProperties`, `drawBoxColliderSection`, `drawSphereColliderSection`, `drawCapsuleColliderSection`, `drawPlaneColliderSection`, `drawMeshSection`, `drawMeshDialog`, `drawAudioClipSection`, `drawAudioClipDialog`, `drawScriptsSection`, `drawAddComponentButton`, `drawNewScriptPopup`, `loadMeshForSelected`, `loadAudioClipForSelected`. Sustituir `m_selected`→`ctx.selected`, `m_renderer`/`m_physics`/`m_audio`/`m_scriptManager`/`m_scene`→`ctx.*`, `m_undoHistory`→`*ctx.undo`, `pushLog(...)`→`ctx.pushLog(...)`. El constructor inicializa los dos `unique_ptr<IGFD::FileDialog>` igual que hacía `EditorUI::EditorUI()` (líneas 210-217, la parte de `m_meshFileDialog`/`m_audioFileDialog`).

`draw()` debe llamar internamente, en este orden (igual que el `draw()` de `EditorUI` original llamaba a las piezas de Properties + los diálogos que dependían de mesh/audio):
```cpp
void PropertiesPanel::draw(EditorContext& ctx)
{
    if (!m_open) return;
    // ... cuerpo de EditorUI::drawProperties (incluye llamadas internas a
    // drawBoxColliderSection/drawSphereColliderSection/.../drawScriptsSection/
    // drawAddComponentButton, igual que hacía el original) ...
    drawNewScriptPopup(ctx);
    drawMeshDialog(ctx);
    drawAudioClipDialog(ctx);
}
```

- [ ] **Step 3: Quitar de `EditorUI.h`/`EditorUI.cpp`**

Borrar todas las declaraciones/cuerpos movidos y los miembros correspondientes (línea 280-364 y 195-197, 224-234 de `EditorUI.h`: `m_propertiesOpen`, todo el bloque Properties/Box/Sphere/Capsule/Plane Collider, `m_meshDlgOpen`/`m_meshFileDialog`/`m_meshLoadError`/`m_meshAddRequestedFor`, `m_audioDlgOpen`/`m_audioFileDialog`/`m_audioLoadError`/`m_audioClipAddRequestedFor`, `m_openNewScriptPopup`/`m_newScriptNameBuffer`/`m_newScriptError`/`m_newScriptTarget`). Quitar la inicialización de `m_meshFileDialog`/`m_audioFileDialog` del constructor de `EditorUI` (ya no son suyos). Añadir `#include "DonTopo/Editor/PropertiesPanel.h"` y miembro `PropertiesPanel m_propertiesPanel;`. En `draw()`: `drawProperties(); ... drawMeshDialog(); drawAudioClipDialog();` → `m_propertiesPanel.draw(ctx);`.

- [ ] **Step 4: CMakeLists** — añadir `src/Editor/PropertiesPanel.cpp`.

- [ ] **Step 5: Build**

```powershell
& .\configure.bat
& .\build.bat
```

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/PropertiesPanel.h engine/src/Editor/PropertiesPanel.cpp engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp engine/CMakeLists.txt
git commit -m "refactor(editor): extraer PropertiesPanel de EditorUI"
```

---

### Task 6: `ContentBrowserPanel`

**Files:**
- Create: `engine/include/DonTopo/Editor/ContentBrowserPanel.h`
- Create: `engine/src/Editor/ContentBrowserPanel.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp`

**Interfaces:**
- Consumes: `EditorContext` — `ctx.renderer`, `ctx.audio`, `ctx.pushLog`.
- Produces: `ContentBrowserPanel::draw(EditorContext& ctx, GameObject* sceneRoot)`, `ContentBrowserPanel::GetOpenPtr()`.

- [ ] **Step 1: Crear `ContentBrowserPanel.h`**

```cpp
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace DonTopo {

class GameObject;
struct EditorContext;

// Ventana "Content Browser" — explorador de assets del proyecto (mesh,
// audio, scripts), con rename/delete y detección de referencias en la
// escena para desengancharlas antes de borrar/renombrar en disco.
class ContentBrowserPanel {
public:
    void draw(EditorContext& ctx, GameObject* sceneRoot);
    bool* GetOpenPtr() { return &m_open; }

private:
    void beginAssetRename(const std::filesystem::path& path, bool isDir);
    void updateSceneReferencesForRename(EditorContext& ctx, GameObject* sceneRoot,
                                         const std::filesystem::path& oldPath,
                                         const std::filesystem::path& newPath, bool isDir);
    int  countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);
    void detachSceneReferencesForDelete(EditorContext& ctx, GameObject* sceneRoot,
                                         const std::filesystem::path& path, bool isDir);
    void beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir);

    bool m_open = true;
    bool m_dlgOpen = false;
    bool m_scanned = false;
    std::string m_currentDir;
    std::filesystem::path m_projectRoot;
    std::string m_dlgReopenPath;
    std::vector<std::filesystem::path> m_assets;

    std::filesystem::path m_assetRenameTarget;
    bool                   m_assetRenameIsDir = false;
    char                   m_assetRenameBuffer[128] = {};
    std::string            m_assetRenameError;
    bool                   m_openAssetRenamePopup = false;

    std::filesystem::path m_assetDeleteTarget;
    bool                   m_assetDeleteIsDir = false;
    int                    m_assetDeleteAffectedCount = 0;
    bool                   m_openAssetDeletePopup = false;
    std::string            m_assetDeleteError;
};

} // namespace DonTopo
```

- [ ] **Step 2: Crear `ContentBrowserPanel.cpp`**

Mover verbatim `drawContentBrowser`→`draw`, `beginAssetRename`, `updateSceneReferencesForRename`, `countSceneReferences`, `detachSceneReferencesForDelete`, `beginAssetDelete`. Sustituir `m_renderer`/`m_audio`→`ctx.renderer`/`ctx.audio`, `pushLog(...)`→`ctx.pushLog(...)`.

- [ ] **Step 3: Quitar de `EditorUI.h`/`EditorUI.cpp`**

Borrar declaraciones/cuerpos movidos y miembros `m_contentBrowserOpen`, `m_dlgOpen`, `m_scanned`, `m_currentDir`, `m_projectRoot`, `m_dlgReopenPath`, `m_assets`, `m_assetRenameTarget`/`m_assetRenameIsDir`/`m_assetRenameBuffer`/`m_assetRenameError`/`m_openAssetRenamePopup`, `m_assetDeleteTarget`/`m_assetDeleteIsDir`/`m_assetDeleteAffectedCount`/`m_openAssetDeletePopup`/`m_assetDeleteError`. Añadir `#include "DonTopo/Editor/ContentBrowserPanel.h"` y miembro `ContentBrowserPanel m_contentBrowserPanel;`. En `draw()`: `drawContentBrowser(sceneRoot);` → `m_contentBrowserPanel.draw(ctx, sceneRoot);`.

- [ ] **Step 4: CMakeLists** — añadir `src/Editor/ContentBrowserPanel.cpp`.

- [ ] **Step 5: Build**

```powershell
& .\configure.bat
& .\build.bat
```

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp engine/CMakeLists.txt
git commit -m "refactor(editor): extraer ContentBrowserPanel de EditorUI"
```

---

### Task 7: `EditorUI` shell final — menú View, `EditorContext` real, limpieza

**Files:**
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp`

Tras las Tasks 2-6, `EditorUI` debería contener solo: `handleUndoRedoShortcut`, `drawMenuBar`, `drawToolbar`, `drawDockSpace`, `reloadSceneFromJson`, `drawSceneDialog`, los 5 miembros de panel + `m_scriptEditor`, y el estado que de verdad es transversal (`m_selected`, `m_isPlaying`, `m_undoHistory`, `m_physics`/`m_renderer`/`m_audio`/`m_scene`/`m_scriptManager`, `m_onDelete`/`m_onAxisSelected`, diálogo de escena). Esta tarea deja `draw()` en su forma final y corrige el menú View para togglear los `GetOpenPtr()` de los 5 paneles nuevos (antes togglaba `m_sceneOpen` etc. directamente).

**Interfaces:**
- Consumes: `LogPanel`, `ScenePanel`, `ViewportPanel`, `PropertiesPanel`, `ContentBrowserPanel` (Tasks 2-6), `EditorContext` (Task 1).
- Produces: `EditorUI` público sin cambios de firma respecto al original (ver `engine/include/DonTopo/Renderer/Renderer.h:305` y `sandbox/src/main.cpp`, que no deben tocarse).

- [ ] **Step 1: `EditorUI::draw()` final**

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    handleUndoRedoShortcut();
    drawMenuBar();
    drawToolbar();
    drawDockSpace();

    EditorContext ctx{
        m_selected,
        m_isPlaying,
        m_physics,
        m_renderer,
        m_audio,
        m_scene,
        m_scriptManager,
        &m_undoHistory,
        [this](const std::string& msg) { m_logPanel.push(msg); },
        m_onDelete,
        m_onAxisSelected,
    };

    m_scenePanel.draw(ctx, sceneRoot);
    m_viewportPanel.draw(ctx, viewportTexture, cameraView);
    m_propertiesPanel.draw(ctx);
    m_logPanel.draw();
    drawSceneDialog();
    m_contentBrowserPanel.draw(ctx, sceneRoot);
    m_scriptEditor->draw();
}
```

- [ ] **Step 2: `focusSelected` / `isViewportHovered` finales**

```cpp
void EditorUI::focusSelected(Camera& camera)
{
    EditorContext ctx{
        m_selected, m_isPlaying, m_physics, m_renderer, m_audio, m_scene, m_scriptManager,
        &m_undoHistory, [this](const std::string& msg) { m_logPanel.push(msg); },
        m_onDelete, m_onAxisSelected,
    };
    m_viewportPanel.focusSelected(ctx, camera);
}

bool EditorUI::isViewportHovered() const { return m_viewportPanel.isHovered(); }
```

- [ ] **Step 3: Menú View**

En `drawMenuBar()`, sustituir cada `ImGui::MenuItem("X", nullptr, &m_xOpen)` por `ImGui::MenuItem("X", nullptr, m_xPanel.GetOpenPtr())` para los 5 paneles nuevos; el de Script Editor ya usa `m_scriptEditor->GetOpenPtr()` (sin cambio).

- [ ] **Step 4: Limpieza de `EditorUI.h`**

Confirmar que ya no quedan miembros/declaraciones huérfanas de las Tasks 2-6 (`m_sceneOpen`, `m_viewportOpen`, `m_propertiesOpen`, `m_logOpen`, `m_contentBrowserOpen`, `m_viewportHovered`, y todas las demás listadas en esas tareas). El fichero debería quedar por debajo de ~120 líneas.

- [ ] **Step 5: Build**

```powershell
& .\build.bat
```

- [ ] **Step 6: Smoke test manual**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
Checklist:
- Las 6 ventanas (Scene, Viewport, Properties, Log, Content Browser, Script Editor) abren al inicio.
- Cerrar y reabrir cada una desde el menú View conserva su estado (selección, tabs de script, scroll de log).
- Seleccionar un GameObject en Scene → gizmo aparece en Viewport, Properties muestra sus datos.
- Editar Position en Properties, Ctrl+Z deshace, Ctrl+Y rehace, el Log Console muestra ambas entradas.
- Add Mesh / Add Audio Clip desde Properties siguen abriendo su diálogo de fichero.
- Rename/Delete de un asset en Content Browser sigue actualizando referencias en la escena.
- Play/Stop desde el toolbar sigue funcionando (snapshot/restore).

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp
git commit -m "refactor(editor): EditorUI queda como shell delgado sobre los paneles"
```

---

## Self-Review

- **Cobertura:** las 5 ventanas del spec (Scene, Viewport, Properties, Log, Content Browser) tienen tarea dedicada; Script Editor ya estaba separado (no requiere tarea). El shell (menú/toolbar/dockspace/undo-shortcut/scene-dialog) se documenta explícitamente como lo que se queda en `EditorUI` (Task 7), evitando la duda de "¿y esto dónde va?".
- **Placeholders:** los pasos de "mover verbatim" remiten a rangos de línea concretos del fichero actual en vez de reproducir 2000+ líneas ya existentes — es contenido exacto e inequívoco (cortar/pegar), no una instrucción vaga. Los snippets de código nuevo (structs, headers, `draw()` final) están completos.
- **Consistencia de tipos:** `EditorContext::selected` es `GameObject*&` en todas las tareas; `ctx.undo` es `UndoManager*` (no referencia) porque `UndoManager` no es copiable/movible y el original ya lo trataba como miembro fijo — se usa `*ctx.undo` en los paneles, igual que el resto de accesos por puntero (`ctx.renderer`, etc.).
