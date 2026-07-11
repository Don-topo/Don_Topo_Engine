# Log Console Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Panel dockable "Log" que muestra, con timestamp y autoscroll, cada acción de edición significativa realizada durante la sesión (crear/borrar/renombrar GameObject, añadir componente, guardar/cargar escena, Play/Stop, renombrar/borrar asset), con buffer acotado a 200 líneas (FIFO).

**Architecture:** Todo vive en `EditorUI` — es el único punto de entrada de todas las acciones de edición del motor, así que no hace falta un logger global/singleton. Un único método privado `pushLog(const std::string&)` es el punto de escritura del ring buffer (`std::deque<std::string> m_logEntries`, tope `kLogMaxEntries = 200`); un único método privado `drawLogPanel()` lo pinta como ventana ImGui dockable normal (mismo patrón que Content Browser: sin `DockBuilder`, el usuario la dockea a mano la primera vez y `imgui.ini` recuerda la posición). Cada sitio del código que ya realiza una acción de edición (había que localizar cada uno, ya mapeados en el spec) gana una línea `pushLog(...)` justo tras confirmar el efecto.

**Tech Stack:** C++20, Dear ImGui (ya integrado), `<chrono>`/`<ctime>` para el timestamp (`localtime_s`, MSVC). Sin framework de tests unitarios — verificación por compilación + ejecución manual del editor (igual que el resto del motor).

## Global Constraints

- No hay gtest/ctest en el repo — cada tarea se verifica con `build.bat` vía **PowerShell** (no Bash). La verificación funcional se hace ejecutando `Sandbox.exe` y observando el panel Log.
- Buffer del Log acotado a **200 líneas** (`kLogMaxEntries`); al superarlo se descarta la línea más antigua (`pop_front`). Sin persistencia a disco.
- Formato de línea: `"[HH:MM:SS] mensaje"`.
- El panel es una ventana `ImGui::Begin("Log")` dockable normal — sin flags especiales, sin `DockBuilder`, mismo patrón que "Content Browser"/"Scene"/"Properties".
- Solo se loguean acciones de edición significativas (lista cerrada en el spec, sección "Puntos de instrumentación"). No se loguean hover/selección/wireframe toggle/focus de cámara/apertura de paneles.
- Save/Load Scene son el único par que loguea también el caso de error; el resto de fallos (mesh/audio load error, rename/delete de asset fallido) ya tienen su feedback puntual existente en la UI y quedan fuera de esta iteración.

---

### Task 1: Ring buffer + panel `drawLogPanel()`

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Produces: `EditorUI::pushLog(const std::string& message) -> void` (privado, único punto de escritura del log, usado por todas las tareas siguientes). `EditorUI::drawLogPanel() -> void` (privado, llamado desde `draw()`).

- [ ] **Step 1: Añadir `#include <deque>` en `engine/include/DonTopo/EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h:1-9`, reemplaza:

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <filesystem>
#include <functional>
#include <memory>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
```

por:

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
```

- [ ] **Step 2: Declarar `pushLog`/`drawLogPanel` en `engine/include/DonTopo/EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h`, localiza `void drawProperties();` (línea ~85) y reemplaza:

```cpp
    void drawViewport(VkDescriptorSet viewportTexture, const glm::mat4& cameraView);
    void drawProperties();
```

por:

```cpp
    void drawViewport(VkDescriptorSet viewportTexture, const glm::mat4& cameraView);
    void drawProperties();
    // Único punto de escritura del log — añade "[HH:MM:SS] message" al
    // final de m_logEntries y descarta la más antigua si se supera
    // kLogMaxEntries. Llamado desde cada acción de edición confirmada.
    void pushLog(const std::string& message);
    void drawLogPanel();
```

- [ ] **Step 3: Añadir miembros del Log en `engine/include/DonTopo/EditorUI.h`**

Localiza el bloque:

```cpp
    // Viewport
    bool m_viewportHovered = false;

    // Content Browser
```

y reemplaza por:

```cpp
    // Viewport
    bool m_viewportHovered = false;

    // Log — ring buffer de acciones de edición confirmadas, más reciente al
    // final. Sin persistencia a disco (spec: no hace falta guardar nada).
    static constexpr size_t kLogMaxEntries = 200;
    std::deque<std::string> m_logEntries;
    // true si el panel ya estaba scrolleado al fondo el frame anterior —
    // evita pelear con el usuario si sube a leer historial mientras llegan
    // más líneas.
    bool m_logAutoScroll = true;

    // Content Browser
```

- [ ] **Step 4: Añadir includes `<chrono>`/`<ctime>` en `engine/src/EditorUI.cpp`**

En `engine/src/EditorUI.cpp:22-27`, reemplaza:

```cpp
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>
```

por:

```cpp
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
```

- [ ] **Step 5: Implementar `pushLog`/`drawLogPanel` en `engine/src/EditorUI.cpp`**

Localiza el final de `EditorUI::drawDockSpace()`:

```cpp
    ImGui::Begin("##DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void EditorUI::drawScene(GameObject* sceneRoot)
```

y reemplaza por:

```cpp
    ImGui::Begin("##DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void EditorUI::pushLog(const std::string& message)
{
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm     tmBuf{};
    localtime_s(&tmBuf, &t);
    char timeStr[16];
    std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmBuf);

    m_logEntries.push_back(std::string("[") + timeStr + "] " + message);
    if (m_logEntries.size() > kLogMaxEntries)
        m_logEntries.pop_front();
}

void EditorUI::drawLogPanel()
{
    ImGui::Begin("Log");
    for (const auto& line : m_logEntries)
        ImGui::TextUnformatted(line.c_str());

    // Autoscroll: solo si ya estaba al fondo antes de este frame (no pelea
    // con el usuario si sube a revisar historial mientras entran líneas
    // nuevas).
    if (m_logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    m_logAutoScroll = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

    ImGui::End();
}

void EditorUI::drawScene(GameObject* sceneRoot)
```

- [ ] **Step 6: Llamar `drawLogPanel()` desde `draw()`**

En `engine/src/EditorUI.cpp`, reemplaza:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawToolbar();
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawMeshDialog();
    drawAudioClipDialog();
    drawSceneDialog();
    drawContentBrowser(sceneRoot);
}
```

por:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawToolbar();
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawLogPanel();
    drawMeshDialog();
    drawAudioClipDialog();
    drawSceneDialog();
    drawContentBrowser(sceneRoot);
}
```

- [ ] **Step 7: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 8: Verificación manual — el panel existe y dockea**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Aparece una pestaña/ventana "Log" (vacía — aún no hay instrumentación, eso llega en las tareas siguientes).
- [ ] Se puede arrastrar y dockear en la parte inferior del layout junto al resto de paneles.

- [ ] **Step 9: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): añade panel Log con ring buffer de 200 líneas"
```

---

### Task 2: Instrumentar ciclo de vida de GameObject (crear/borrar/renombrar)

**Files:**
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `EditorUI::pushLog(const std::string&) -> void` (Tarea 1).

- [ ] **Step 1: Log en "Create GameObject" del menú contextual raíz (`drawScene`)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawScene`, reemplaza:

```cpp
        if (ImGui::MenuItem("Create GameObject") && sceneRoot)
            sceneRoot->addChild("GameObject");
```

por:

```cpp
        if (ImGui::MenuItem("Create GameObject") && sceneRoot)
        {
            GameObject* created = sceneRoot->addChild("GameObject");
            pushLog("GameObject '" + created->name + "' creado");
        }
```

- [ ] **Step 2: Log en "Create GameObject" del menú contextual por nodo (`drawSceneNode`)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawSceneNode`, reemplaza:

```cpp
        if (ImGui::MenuItem("Create GameObject"))
            node->addChild("GameObject");
```

por:

```cpp
        if (ImGui::MenuItem("Create GameObject"))
        {
            GameObject* created = node->addChild("GameObject");
            pushLog("GameObject '" + created->name + "' creado");
        }
```

- [ ] **Step 3: Log en `createBasicShape` (cubre los 4 Basic Shapes, root y por-nodo)**

En `engine/src/EditorUI.cpp`, reemplaza:

```cpp
void EditorUI::createBasicShape(GameObject* parent, const std::string& name, std::shared_ptr<Mesh> mesh)
{
    if (!parent || !m_renderer || !mesh)
        return;

    GameObject* go = parent->addChild(name);
    go->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
    go->setMesh(std::move(mesh));
}
```

por:

```cpp
void EditorUI::createBasicShape(GameObject* parent, const std::string& name, std::shared_ptr<Mesh> mesh)
{
    if (!parent || !m_renderer || !mesh)
        return;

    GameObject* go = parent->addChild(name);
    go->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
    go->setMesh(std::move(mesh));
    pushLog("GameObject '" + go->name + "' creado");
}
```

- [ ] **Step 4: Log al borrar GameObject (bloque `m_pendingDelete` en `drawScene`)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawScene`, reemplaza:

```cpp
        bool selectionInSubtree = false;
        target->traverse([&](GameObject* go) {
            if (go == m_selected) selectionInSubtree = true;
        });

        if (m_onDelete)
            m_onDelete(target);
```

por:

```cpp
        bool selectionInSubtree = false;
        target->traverse([&](GameObject* go) {
            if (go == m_selected) selectionInSubtree = true;
        });

        pushLog("GameObject '" + target->name + "' eliminado");

        if (m_onDelete)
            m_onDelete(target);
```

- [ ] **Step 5: Log al renombrar GameObject**

En `engine/src/EditorUI.cpp`, dentro del popup "Rename GameObject" de `drawScene`, reemplaza:

```cpp
        if (accept)
        {
            std::string newName = trim(m_renameBuffer);
            if (m_renameTarget && isValidGameObjectName(newName))
                m_renameTarget->name = newName;
            m_renameTarget = nullptr;
            ImGui::CloseCurrentPopup();
        }
```

por:

```cpp
        if (accept)
        {
            std::string newName = trim(m_renameBuffer);
            if (m_renameTarget && isValidGameObjectName(newName))
            {
                std::string oldName  = m_renameTarget->name;
                m_renameTarget->name = newName;
                pushLog("GameObject renombrado: '" + oldName + "' -> '" + newName + "'");
            }
            m_renameTarget = nullptr;
            ImGui::CloseCurrentPopup();
        }
```

- [ ] **Step 6: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 7: Verificación manual**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Click derecho en Scene > "Create GameObject" → línea `GameObject 'GameObject' creado` aparece en el panel Log.
- [ ] Click derecho > Basic Shapes > Cube → línea `GameObject 'Cube' creado`.
- [ ] Seleccionar un GameObject, F2, renombrar a "Test" → línea `GameObject renombrado: '<viejo>' -> 'Test'`.
- [ ] Seleccionar un GameObject, tecla Delete → línea `GameObject 'Test' eliminado`.

- [ ] **Step 8: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(log): instrumenta crear/borrar/renombrar GameObject"
```

---

### Task 3: Instrumentar Add Component (4 colliders, Mesh, Audio Clip)

**Files:**
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `EditorUI::pushLog(const std::string&) -> void` (Tarea 1).

- [ ] **Step 1: Log al añadir Box/Sphere/Capsule/Plane Collider (`drawAddComponentButton`)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawAddComponentButton`, reemplaza:

```cpp
        if (ImGui::Selectable("Box Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setBoxCollider(m_physics->createBoxColliderComponent(
                glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f),
                m_selected->worldTransform, /*useGravity=*/false));
            m_colliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setSphereCollider(m_physics->createSphereColliderComponent(
                25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_sphereColliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setCapsuleCollider(m_physics->createCapsuleColliderComponent(
                15.0f, 25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_capsuleColliderCachedFor = nullptr;
        }

        if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setPlaneCollider(m_physics->createPlaneColliderComponent(
                glm::vec3(0.0f), m_selected->worldTransform));
            m_planeColliderCachedFor = nullptr;
        }
```

por:

```cpp
        if (ImGui::Selectable("Box Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setBoxCollider(m_physics->createBoxColliderComponent(
                glm::vec3(25.0f, 25.0f, 25.0f), glm::vec3(0.0f),
                m_selected->worldTransform, /*useGravity=*/false));
            m_colliderCachedFor = nullptr;
            pushLog("Componente Box Collider añadido a '" + m_selected->name + "'");
        }

        if (ImGui::Selectable("Sphere Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setSphereCollider(m_physics->createSphereColliderComponent(
                25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_sphereColliderCachedFor = nullptr;
            pushLog("Componente Sphere Collider añadido a '" + m_selected->name + "'");
        }

        if (ImGui::Selectable("Capsule Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setCapsuleCollider(m_physics->createCapsuleColliderComponent(
                15.0f, 25.0f, glm::vec3(0.0f), m_selected->worldTransform, /*useGravity=*/false));
            m_capsuleColliderCachedFor = nullptr;
            pushLog("Componente Capsule Collider añadido a '" + m_selected->name + "'");
        }

        if (ImGui::Selectable("Plane Collider") && !alreadyHasAny && m_physics)
        {
            m_selected->setPlaneCollider(m_physics->createPlaneColliderComponent(
                glm::vec3(0.0f), m_selected->worldTransform));
            m_planeColliderCachedFor = nullptr;
            pushLog("Componente Plane Collider añadido a '" + m_selected->name + "'");
        }
```

- [ ] **Step 2: Log al añadir Mesh (`loadMeshForSelected`, rama éxito)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::loadMeshForSelected`, reemplaza:

```cpp
        auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
        m_selected->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
        m_selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
```

por:

```cpp
        auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
        m_selected->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
        m_selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
        pushLog("Componente Mesh añadido a '" + m_selected->name + "'");
```

- [ ] **Step 3: Log al añadir Audio Clip (`loadAudioClipForSelected`, rama éxito)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::loadAudioClipForSelected`, reemplaza:

```cpp
    m_selected->setAudioClip(std::move(clip));
    m_audioLoadError.clear();
```

por:

```cpp
    m_selected->setAudioClip(std::move(clip));
    m_audioLoadError.clear();
    pushLog("Componente Audio Clip añadido a '" + m_selected->name + "'");
```

- [ ] **Step 4: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 5: Verificación manual**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Seleccionar un GameObject, Properties > Add > Box Collider → línea `Componente Box Collider añadido a '<name>'`.
- [ ] Repetir con Sphere/Capsule/Plane Collider → 1 línea cada uno.
- [ ] Add > Mesh, elegir un .fbx válido → línea `Componente Mesh añadido a '<name>'`.
- [ ] Add > Audio Clip, elegir un .wav/.mp3 válido → línea `Componente Audio Clip añadido a '<name>'`.

- [ ] **Step 6: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(log): instrumenta Add Component (colliders, mesh, audio)"
```

---

### Task 4: Instrumentar Save/Load Scene y Play/Stop

**Files:**
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `EditorUI::pushLog(const std::string&) -> void` (Tarea 1).

- [ ] **Step 1: Log en Save/Load Scene (`drawSceneDialog`)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawSceneDialog`, reemplaza:

```cpp
        if (m_sceneDlgIsSave)
        {
            m_sceneIOError = (m_scene && m_scene->save(path)) ? "" : "No se pudo guardar la escena";
        }
        else
        {
            // Valida la estructura básica del JSON ANTES de tocar GPU/Scene:
            // rechaza un fichero top-level corrupto sin tocar nada (fast
            // path, evita el churn de GPU de reloadSceneFromJson). No cubre
            // malformación anidada — para eso, Scene::fromJson es atómico y
            // reloadSceneFromJson cubre ambos desenlaces.
            auto parsed = FileManager::readJson(path);
            bool structureOk = parsed.has_value() &&
                                parsed->contains("version") && (*parsed)["version"].is_number_integer() &&
                                (*parsed)["version"].get<int>() == 1 &&
                                parsed->contains("root") && (*parsed)["root"].is_object();

            m_sceneIOError = (structureOk && reloadSceneFromJson(*parsed)) ? "" : "No se pudo cargar la escena";
        }
```

por:

```cpp
        if (m_sceneDlgIsSave)
        {
            bool saved   = m_scene && m_scene->save(path);
            m_sceneIOError = saved ? "" : "No se pudo guardar la escena";
            pushLog(saved ? ("Escena guardada: " + path) : ("Error al guardar escena: " + path));
        }
        else
        {
            // Valida la estructura básica del JSON ANTES de tocar GPU/Scene:
            // rechaza un fichero top-level corrupto sin tocar nada (fast
            // path, evita el churn de GPU de reloadSceneFromJson). No cubre
            // malformación anidada — para eso, Scene::fromJson es atómico y
            // reloadSceneFromJson cubre ambos desenlaces.
            auto parsed = FileManager::readJson(path);
            bool structureOk = parsed.has_value() &&
                                parsed->contains("version") && (*parsed)["version"].is_number_integer() &&
                                (*parsed)["version"].get<int>() == 1 &&
                                parsed->contains("root") && (*parsed)["root"].is_object();

            bool loaded  = structureOk && reloadSceneFromJson(*parsed);
            m_sceneIOError = loaded ? "" : "No se pudo cargar la escena";
            pushLog(loaded ? ("Escena cargada: " + path) : ("Error al cargar escena: " + path));
        }
```

- [ ] **Step 2: Log en Play/Stop (`drawToolbar`)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawToolbar`, reemplaza:

```cpp
    bool canPlay = m_scene && m_physics && m_audio && m_renderer;
    ImGui::BeginDisabled(!canPlay);
    if (m_isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Stop"))
        {
            m_sceneIOError = reloadSceneFromJson(m_playSnapshot) ? "" : "No se pudo restaurar la escena";
            m_isPlaying = false;
        }
        ImGui::PopStyleColor();
    }
    else
    {
        if (ImGui::Button("Play"))
        {
            m_playSnapshot = m_scene->toJson();
            m_isPlaying = true;
        }
    }
    ImGui::EndDisabled();
```

por:

```cpp
    bool canPlay = m_scene && m_physics && m_audio && m_renderer;
    ImGui::BeginDisabled(!canPlay);
    if (m_isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("Stop"))
        {
            m_sceneIOError = reloadSceneFromJson(m_playSnapshot) ? "" : "No se pudo restaurar la escena";
            m_isPlaying = false;
            pushLog("Play Mode detenido");
        }
        ImGui::PopStyleColor();
    }
    else
    {
        if (ImGui::Button("Play"))
        {
            m_playSnapshot = m_scene->toJson();
            m_isPlaying = true;
            pushLog("Play Mode iniciado");
        }
    }
    ImGui::EndDisabled();
```

- [ ] **Step 3: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 4: Verificación manual**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Save Scene a un path válido → línea `Escena guardada: <path>`.
- [ ] Load Scene del mismo fichero → línea `Escena cargada: <path>`.
- [ ] Play → línea `Play Mode iniciado`; Stop → línea `Play Mode detenido`.

- [ ] **Step 5: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(log): instrumenta Save/Load Scene y Play/Stop"
```

---

### Task 5: Instrumentar rename/delete de asset + verificación final completa

**Files:**
- Modify: `engine/src/EditorUI.cpp`

**Interfaces:**
- Consumes: `EditorUI::pushLog(const std::string&) -> void` (Tarea 1).

- [ ] **Step 1: Log al renombrar asset (Content Browser)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawContentBrowser`, popup "Rename Asset", reemplaza:

```cpp
                        else
                        {
                            updateSceneReferencesForRename(sceneRoot, m_assetRenameTarget, newPath, m_assetRenameIsDir);
                            m_scanned       = false;
                            m_dlgReopenPath = m_currentDir;
                            m_dlgOpen       = false;
                            ImGui::CloseCurrentPopup();
                        }
```

por:

```cpp
                        else
                        {
                            pushLog("Asset renombrado: '" + m_assetRenameTarget.filename().string() +
                                    "' -> '" + newPath.filename().string() + "'");
                            updateSceneReferencesForRename(sceneRoot, m_assetRenameTarget, newPath, m_assetRenameIsDir);
                            m_scanned       = false;
                            m_dlgReopenPath = m_currentDir;
                            m_dlgOpen       = false;
                            ImGui::CloseCurrentPopup();
                        }
```

- [ ] **Step 2: Log al borrar asset (Content Browser)**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawContentBrowser`, popup "Delete Asset", reemplaza:

```cpp
                else
                {
                    detachSceneReferencesForDelete(sceneRoot, m_assetDeleteTarget, m_assetDeleteIsDir);
                    m_scanned       = false;
                    m_dlgReopenPath = m_currentDir;
                    m_dlgOpen       = false;
                    ImGui::CloseCurrentPopup();
                }
```

por:

```cpp
                else
                {
                    pushLog("Asset eliminado: " + m_assetDeleteTarget.string());
                    detachSceneReferencesForDelete(sceneRoot, m_assetDeleteTarget, m_assetDeleteIsDir);
                    m_scanned       = false;
                    m_dlgReopenPath = m_currentDir;
                    m_dlgOpen       = false;
                    ImGui::CloseCurrentPopup();
                }
```

- [ ] **Step 3: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 4: Verificación manual completa (checklist final, sin framework de tests automatizado)**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Renombrar un asset desde Content Browser → línea `Asset renombrado: '<old>' -> '<new>'`.
- [ ] Borrar un asset desde Content Browser → línea `Asset eliminado: <path>`.
- [ ] Repaso end-to-end: crear/borrar/renombrar GameObject, añadir cada tipo de componente, guardar/cargar escena, Play/Stop, renombrar/borrar asset — cada acción deja exactamente 1 línea nueva en Log, en orden cronológico, con timestamp `[HH:MM:SS]`.
- [ ] Forzar un fallo de carga de escena (abrir un .json corrupto o sin la clave `version`) → línea `Error al cargar escena: <path>`, no crashea.
- [ ] Generar más de 200 acciones seguidas (p.ej. crear/borrar un GameObject en bucle) → las líneas más antiguas desaparecen del panel, nunca supera 200 líneas visibles.
- [ ] Arrastrar el panel "Log" y dockearlo en la parte inferior del layout, junto al resto de paneles.
- [ ] Cerrar y reabrir `Sandbox.exe` → la posición dockeada del panel Log persiste (vía `imgui.ini`, mismo comportamiento que Content Browser).
- [ ] Con el panel Log scrolleado manualmente hacia arriba (revisando historial), generar una acción nueva → el panel NO salta solo al fondo (no pelea con el scroll manual del usuario).
- [ ] Con el panel Log al fondo, generar una acción nueva → el panel sí hace autoscroll a la línea nueva.

- [ ] **Step 5: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(log): instrumenta rename/delete de asset"
```

---

## Self-Review

**Cobertura del spec:** §1 (estado, `EditorUI`, sin singleton) → Tarea 1 Steps 1-3. §2 (panel `drawLogPanel`, autoscroll, wiring en `draw()`) → Tarea 1 Steps 5-6. §3 tabla de instrumentación completa → GameObject crear/borrar/renombrar → Tarea 2; colliders/Mesh/Audio → Tarea 3; Save/Load Scene y Play/Stop → Tarea 4; rename/delete asset → Tarea 5. §4 testing → checklist de verificación manual repartida por tarea + checklist final consolidado en Tarea 5 Step 4 (incluye el caso de los 200+ acciones y la persistencia de docking, explícitos en el spec). Riesgo del spec sobre `localtime_s`/MSVC → aplicado en Tarea 1 Step 5. Riesgo sobre autoscroll no peleando con scroll manual → aplicado en Tarea 1 Step 5 y verificado en Tarea 5 Step 4.

**Placeholders:** ninguno — cada paso trae el código completo before/after, verificado contra el contenido real actual de `EditorUI.h`/`EditorUI.cpp` en el momento de escribir este plan.

**Consistencia de tipos:** `EditorUI::pushLog(const std::string& message) -> void` usa la misma firma en la declaración (Tarea 1 Step 2) y en los ~14 call sites repartidos por las Tareas 2-5. `EditorUI::drawLogPanel() -> void` consistente entre declaración (Tarea 1 Step 2), implementación (Tarea 1 Step 5) y su única llamada en `draw()` (Tarea 1 Step 6). `m_logEntries`/`kLogMaxEntries`/`m_logAutoScroll` declarados una única vez (Tarea 1 Step 3) y usados solo dentro de `pushLog`/`drawLogPanel`.
