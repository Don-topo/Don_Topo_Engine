# Scene Serialization (File Manager) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persistir la `Scene` completa (árbol de `GameObject` + mesh/colliders/audio) a un fichero JSON y recargarla, para que el editor recupere el estado exacto entre sesiones.

**Architecture:** `FileManager` (nueva clase de funciones estáticas) envuelve I/O de JSON (`writeJson`/`readJson`) sobre `nlohmann::json`, sin conocer `Scene`/`GameObject`. `Scene::save`/`Scene::load` (nuevos métodos) recorren el árbol de `GameObject` recursivamente construyendo/parseando el JSON, y para colliders/audio delegan en las mismas factories que ya usa `EditorUI` (`PhysicsManager::create*ColliderComponent`, `AudioManager::createAudioClipComponent`). `Scene::load` **no** toca `Renderer` (igual que hoy `Scene` no depende de Renderer) — el registro GPU de los meshes reconstruidos (`Renderer::addStaticMesh`) y la liberación GPU de la escena anterior (`Renderer::removeGameObject`) los hace `EditorUI`, que ya es el puente Scene↔Renderer para el resto de componentes (mismo patrón que `loadMeshForSelected`/`createBasicShape`). La UI se expone como dos botones "Save Scene"/"Load Scene" en el toolbar existente, con diálogo `ImGuiFileDialog` propio, mismo patrón que `drawMeshDialog`/`drawAudioClipDialog`.

**Tech Stack:** C++20, CMake+Ninja+FetchContent, [nlohmann/json](https://github.com/nlohmann/json) `v3.11.3` (header-only), sin framework de tests unitarios (verificación por compilación + ejecución manual del editor).

## Global Constraints

- No hay gtest/ctest en el repo — cada tarea se verifica con `build.bat` (compila) vía **PowerShell** (no Bash: `.bat` requiere `vcvarsall.bat` en el PATH del shell). La verificación funcional completa (round-trip guardar/cargar) es manual, en la Tarea 5.
- `Scene::load` recibe `PhysicsManager&`/`AudioManager&` (para recrear colliders/audio) pero **no** `Renderer&` — mantiene la separación de responsabilidades ya establecida en `Scene` (nunca ha dependido de `Renderer`). El registro/liberación GPU de meshes lo hace `EditorUI` en la Tarea 5.
- Referencias de asset (mesh/audio) se guardan como el `sourcePath`/`getPath()` ya almacenado hoy en `Mesh`/`AudioClipComponent` — típicamente relativo (`assets/...`), sin normalización adicional (mismo comportamiento que Content Browser hoy).
- Meshes procedurales (Cube/Sphere/Plane/Capsule) se identifican por `Mesh::name` (`"cube"/"sphere"/"plane"/"capsule"`, ya seteado por cada `create()`) cuando `sourcePath` está vacío — no hace falta un campo `"kind"` nuevo, el dato ya existe.
- Un asset roto (mesh/audio movido o borrado) no aborta la carga completa — el nodo afectado queda sin ese componente, el resto de la escena carga con normalidad (spec §5).
- JSON malformado o `version` incompatible → `Scene::load` devuelve `false` sin modificar la escena (validado **antes** de limpiar `m_root`).

---

### Task 1: Integrar nlohmann/json vía CMake

**Files:**
- Modify: `CMakeLists.txt:142` (tras el bloque ImGuizmo, antes de `# PhysX SDK`)
- Modify: `engine/CMakeLists.txt:33-42` (`target_link_libraries`)

**Interfaces:**
- Produces: target `nlohmann_json::nlohmann_json` disponible para enlazar, header `<nlohmann/json.hpp>` incluible desde `DonTopoEngine`.

- [ ] **Step 1: Añadir `FetchContent_Declare`/`MakeAvailable` en `CMakeLists.txt`**

En `CMakeLists.txt:142`, entre el bloque de `imguizmo` (termina en `target_compile_features(imguizmo PUBLIC cxx_std_17)`, línea 141) y el comentario `# PhysX SDK` (línea 143), inserta:

```cmake
# nlohmann/json — serialización JSON (Scene save/load)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

```

- [ ] **Step 2: Enlazar la librería al target `DonTopoEngine`**

En `engine/CMakeLists.txt:33-42`, añade `nlohmann_json::nlohmann_json` a la lista de `target_link_libraries`:

```cmake
target_link_libraries(DonTopoEngine
    PUBLIC
        Vulkan::Vulkan
        glfw
        glm::glm
        assimp
        imgui_backend
        imgui_filedialog
        imguizmo
        nlohmann_json::nlohmann_json
)
```

- [ ] **Step 3: Reconfigurar y compilar**

```powershell
& .\configure.bat
& .\build.bat
```
Expected: `configure.bat` descarga `nlohmann_json` (log muestra el clone del repo) y termina sin error; `build.bat` compila sin errores (nada usa el header todavía, solo se enlaza el target).

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt engine/CMakeLists.txt
git commit -m "build: añade nlohmann/json vía FetchContent"
```

---

### Task 2: `FileManager` — wrapper de I/O JSON

**Files:**
- Create: `engine/include/DonTopo/FileManager.h`
- Create: `engine/src/FileManager.cpp`
- Modify: `engine/CMakeLists.txt:14` (registrar el nuevo `.cpp`, tras `src/Scene.cpp`)

**Interfaces:**
- Produces: `DonTopo::FileManager::writeJson(const std::string& path, const nlohmann::json& j) -> bool`, `DonTopo::FileManager::readJson(const std::string& path) -> std::optional<nlohmann::json>`.

- [ ] **Step 1: Crear `engine/include/DonTopo/FileManager.h`**

```cpp
#pragma once
#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace DonTopo
{
    // Wrapper de I/O de ficheros JSON, sin estado y sin conocer Scene/GameObject
    // — reutilizable para otros ficheros del motor (config, presets) además
    // de la serialización de escena.
    class FileManager
    {
        public:
            // Escribe j formateado (pretty-print, indent 2) en path. false si
            // el fichero no se pudo abrir/escribir.
            static bool writeJson(const std::string& path, const nlohmann::json& j);

            // Lee y parsea path. std::nullopt si el fichero no existe o el
            // JSON es inválido (nunca lanza excepción hacia el caller).
            static std::optional<nlohmann::json> readJson(const std::string& path);
    };
}
```

- [ ] **Step 2: Crear `engine/src/FileManager.cpp`**

```cpp
#include "DonTopo/FileManager.h"
#include <fstream>

namespace DonTopo
{
    bool FileManager::writeJson(const std::string& path, const nlohmann::json& j)
    {
        std::ofstream out(path);
        if (!out.is_open())
            return false;

        out << j.dump(2);
        return out.good();
    }

    std::optional<nlohmann::json> FileManager::readJson(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open())
            return std::nullopt;

        nlohmann::json j;
        try
        {
            in >> j;
        }
        catch (const nlohmann::json::parse_error&)
        {
            return std::nullopt;
        }
        return j;
    }
}
```

- [ ] **Step 3: Registrar el fuente en `engine/CMakeLists.txt`**

En `engine/CMakeLists.txt:14`, tras `src/Scene.cpp`, añade:

```cmake
    src/Scene.cpp
    src/FileManager.cpp
```

- [ ] **Step 4: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores. Nada usa `FileManager` todavía.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/FileManager.h engine/src/FileManager.cpp engine/CMakeLists.txt
git commit -m "feat(io): añade FileManager (wrapper read/write JSON)"
```

---

### Task 3: `Scene::save` — serialización del árbol a JSON

**Files:**
- Modify: `engine/include/DonTopo/Scene.h` (nueva declaración `save`)
- Modify: `engine/src/Scene.cpp` (nuevas funciones helper + implementación de `save`)

**Interfaces:**
- Consumes: `GameObject::name/localTransform/children/hasMesh()/getMesh()/hasBoxCollider()/getBoxCollider()/hasSphereCollider()/getSphereCollider()/hasCapsuleCollider()/getCapsuleCollider()/hasPlaneCollider()/getPlaneCollider()/hasAudioClip()/getAudioClip()` (`engine/include/DonTopo/GameObject.h`); `BoxCollider::getHalfExtents()/getCenter()/getUseGravity()`; `SphereCollider::getRadius()/getCenter()/getUseGravity()`; `CapsuleCollider::getRadius()/getHalfHeight()/getCenter()/getUseGravity()`; `PlaneCollider::getCenter()`; `AudioClipComponent::getPath()/getLoop()/getIs3D()`; `Mesh::sourcePath/name` (`engine/include/DonTopo/Mesh.h`); `FileManager::writeJson` (Tarea 2).
- Produces: `Scene::save(const std::string& path) const -> bool`.

- [ ] **Step 1: Declarar `save` en `engine/include/DonTopo/Scene.h`**

Reemplaza el contenido actual de `engine/include/DonTopo/Scene.h` por:

```cpp
#pragma once
#include <string>
#include "DonTopo/GameObject.h"

namespace DonTopo
{
    class PhysicsManager;
    class AudioManager;

    class Scene
    {
        public:
            explicit Scene(std::string name = "Scene");

            GameObject& getRoot() { return m_root; }
            const GameObject& getRoot() const { return m_root; }

            GameObject* addGameObject(const std::string& name, GameObject* parent = nullptr);
            void removeGameObject(GameObject* node);

            template <typename Fn>
            void traverse(Fn fn) { m_root.traverse(fn); }

            void update(float dt, PhysicsManager& physics);
            void shutdown(PhysicsManager& physics, AudioManager& audio);

            // Serializa el árbol completo (transforms, mesh, colliders, audio
            // clip) a path en formato JSON. false si la escritura falla.
            bool save(const std::string& path) const;
            // Reemplaza el árbol actual por el contenido de path. Limpia la
            // escena existente (shutdown + children.clear()) SOLO si el
            // fichero es válido — una carga fallida no modifica la escena en
            // memoria. Recrea colliders/audio vía physics/audio (mismas
            // factories que usa EditorUI). No toca Renderer — el caller debe
            // registrar/liberar los meshes en GPU (ver EditorUI::drawSceneDialog).
            bool load(const std::string& path, PhysicsManager& physics, AudioManager& audio);

        private:
            std::string m_name;
            GameObject  m_root;
    };
}
```

- [ ] **Step 2: Añadir helpers de conversión JSON en `engine/src/Scene.cpp`**

Al principio de `engine/src/Scene.cpp` (tras el `#include "DonTopo/Scene.h"` existente), añade:

```cpp
#include "DonTopo/Scene.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/AudioManager.h"
#include "DonTopo/AudioClipComponent.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/Mesh.h"
#include "DonTopo/ModelLoader.h"
#include "DonTopo/Cube.h"
#include "DonTopo/Sphere.h"
#include "DonTopo/Plane.h"
#include "DonTopo/Capsule.h"
#include "DonTopo/FileManager.h"
#include <algorithm>
#include <cctype>
#include <memory>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/type_ptr.hpp>

namespace
{
    using DonTopo::GameObject;

    nlohmann::json mat4ToJson(const glm::mat4& m)
    {
        auto arr = nlohmann::json::array();
        const float* p = glm::value_ptr(m);
        for (int i = 0; i < 16; ++i)
            arr.push_back(p[i]);
        return arr;
    }

    nlohmann::json vec3ToJson(const glm::vec3& v)
    {
        return nlohmann::json::array({ v.x, v.y, v.z });
    }

    nlohmann::json nodeToJson(const GameObject& node)
    {
        nlohmann::json j;
        j["name"] = node.name;
        j["localTransform"] = mat4ToJson(node.localTransform);

        if (node.hasMesh())
        {
            const auto& mesh = node.getMesh();
            j["mesh"] = { {"sourcePath", mesh->sourcePath}, {"name", mesh->name} };
        }
        if (node.hasBoxCollider())
        {
            const auto& c = node.getBoxCollider();
            j["boxCollider"] = { {"halfExtents", vec3ToJson(c->getHalfExtents())},
                                  {"center", vec3ToJson(c->getCenter())},
                                  {"useGravity", c->getUseGravity()} };
        }
        if (node.hasSphereCollider())
        {
            const auto& c = node.getSphereCollider();
            j["sphereCollider"] = { {"radius", c->getRadius()},
                                     {"center", vec3ToJson(c->getCenter())},
                                     {"useGravity", c->getUseGravity()} };
        }
        if (node.hasCapsuleCollider())
        {
            const auto& c = node.getCapsuleCollider();
            j["capsuleCollider"] = { {"radius", c->getRadius()},
                                      {"halfHeight", c->getHalfHeight()},
                                      {"center", vec3ToJson(c->getCenter())},
                                      {"useGravity", c->getUseGravity()} };
        }
        if (node.hasPlaneCollider())
        {
            const auto& c = node.getPlaneCollider();
            j["planeCollider"] = { {"center", vec3ToJson(c->getCenter())} };
        }
        if (node.hasAudioClip())
        {
            const auto& clip = node.getAudioClip();
            j["audioClip"] = { {"path", clip->getPath()},
                                {"loop", clip->getLoop()},
                                {"is3D", clip->getIs3D()} };
        }

        j["children"] = nlohmann::json::array();
        for (const auto& child : node.children)
            j["children"].push_back(nodeToJson(*child));

        return j;
    }
}
```

- [ ] **Step 3: Implementar `Scene::save` en `engine/src/Scene.cpp`**

Al final de `engine/src/Scene.cpp` (dentro del `namespace DonTopo { ... }` existente, tras `Scene::shutdown`), añade:

```cpp
bool Scene::save(const std::string& path) const
{
    nlohmann::json root;
    root["version"] = 1;
    root["root"] = nodeToJson(m_root);
    return FileManager::writeJson(path, root);
}
```

- [ ] **Step 4: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores. Nadie llama `Scene::save` todavía.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Scene.h engine/src/Scene.cpp
git commit -m "feat(scene): añade Scene::save (serialización JSON del árbol)"
```

---

### Task 4: `Scene::load` — reconstrucción del árbol desde JSON

**Files:**
- Modify: `engine/src/Scene.cpp` (nuevos helpers + implementación de `load`)

**Interfaces:**
- Consumes: `PhysicsManager::createBoxColliderComponent/createSphereColliderComponent/createCapsuleColliderComponent/createPlaneColliderComponent` (`engine/include/DonTopo/PhysicsManager.h`); `AudioManager::createAudioClipComponent` (`engine/include/DonTopo/AudioManager.h`); `ModelLoader::load` (`engine/include/DonTopo/ModelLoader.h`); `Cube::create/Sphere::create/Plane::create/Capsule::create` (mismos parámetros que `EditorUI::createBasicShape`); `FileManager::readJson` (Tarea 2); `Scene::shutdown` (ya existente).
- Produces: `Scene::load(const std::string& path, PhysicsManager& physics, AudioManager& audio) -> bool`.

- [ ] **Step 1: Añadir helpers de deserialización en `engine/src/Scene.cpp`**

En el mismo bloque `namespace { ... }` anónimo de la Tarea 3 (tras `nodeToJson`), añade:

```cpp
    glm::mat4 jsonToMat4(const nlohmann::json& j)
    {
        glm::mat4 m(1.0f);
        float* p = glm::value_ptr(m);
        for (int i = 0; i < 16; ++i)
            p[i] = j.at(i).get<float>();
        return m;
    }

    glm::vec3 jsonToVec3(const nlohmann::json& j)
    {
        return glm::vec3(j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>());
    }

    // Crea el Mesh procedural correspondiente a meshName (case-insensitive),
    // con los mismos parámetros fijos que EditorUI::createBasicShape. nullptr
    // si meshName no matchea ninguna de las 4 formas básicas.
    std::shared_ptr<DonTopo::Mesh> proceduralMeshByName(const std::string& meshName)
    {
        std::string lower = meshName;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lower == "cube")    return std::make_shared<DonTopo::Mesh>(DonTopo::Cube::create(50.0f));
        if (lower == "sphere")  return std::make_shared<DonTopo::Mesh>(DonTopo::Sphere::create(50.0f));
        if (lower == "plane")   return std::make_shared<DonTopo::Mesh>(DonTopo::Plane::create(50.0f, 0.0f));
        if (lower == "capsule") return std::make_shared<DonTopo::Mesh>(DonTopo::Capsule::create(25.0f, 50.0f));
        return nullptr;
    }

    // Reconstruye node (ya insertado en el árbol) desde j, y recursivamente
    // sus hijos. parentWorld es el worldTransform ya resuelto del padre —
    // necesario para pasar un worldTransform correcto a las factories de
    // collider (que fijan la pose inicial del actor PhysX a partir de él).
    void nodeFromJson(const nlohmann::json& j, GameObject* node, const glm::mat4& parentWorld,
                       DonTopo::PhysicsManager& physics, DonTopo::AudioManager& audio)
    {
        node->localTransform = jsonToMat4(j.at("localTransform"));
        node->worldTransform = parentWorld * node->localTransform;

        if (j.contains("mesh"))
        {
            std::string sourcePath = j["mesh"].value("sourcePath", "");
            std::string meshName   = j["mesh"].value("name", "");
            try
            {
                if (!sourcePath.empty())
                {
                    auto mesh = std::make_shared<DonTopo::Mesh>(DonTopo::ModelLoader::load(sourcePath));
                    mesh->sourcePath = sourcePath;
                    node->setMesh(std::move(mesh));
                }
                else if (auto mesh = proceduralMeshByName(meshName))
                {
                    node->setMesh(std::move(mesh));
                }
            }
            catch (const std::exception&)
            {
                // Asset roto (movido/borrado) o formato no soportado: node
                // queda sin mesh, el resto de la escena sigue cargando.
            }
        }

        if (j.contains("boxCollider"))
        {
            const auto& c = j["boxCollider"];
            node->setBoxCollider(physics.createBoxColliderComponent(
                jsonToVec3(c.at("halfExtents")), jsonToVec3(c.at("center")),
                node->worldTransform, c.at("useGravity").get<bool>()));
        }
        if (j.contains("sphereCollider"))
        {
            const auto& c = j["sphereCollider"];
            node->setSphereCollider(physics.createSphereColliderComponent(
                c.at("radius").get<float>(), jsonToVec3(c.at("center")),
                node->worldTransform, c.at("useGravity").get<bool>()));
        }
        if (j.contains("capsuleCollider"))
        {
            const auto& c = j["capsuleCollider"];
            node->setCapsuleCollider(physics.createCapsuleColliderComponent(
                c.at("radius").get<float>(), c.at("halfHeight").get<float>(),
                jsonToVec3(c.at("center")), node->worldTransform, c.at("useGravity").get<bool>()));
        }
        if (j.contains("planeCollider"))
        {
            const auto& c = j["planeCollider"];
            node->setPlaneCollider(physics.createPlaneColliderComponent(
                jsonToVec3(c.at("center")), node->worldTransform));
        }
        if (j.contains("audioClip"))
        {
            const auto& c = j["audioClip"];
            auto clip = audio.createAudioClipComponent(
                c.at("path").get<std::string>(), c.at("is3D").get<bool>(), c.at("loop").get<bool>());
            if (clip)
                node->setAudioClip(std::move(clip));
            // clip nullptr (asset roto/formato no soportado): node queda sin
            // audio, el resto de la escena sigue cargando.
        }

        for (const auto& childJson : j.at("children"))
        {
            GameObject* child = node->addChild(childJson.at("name").get<std::string>());
            nodeFromJson(childJson, child, node->worldTransform, physics, audio);
        }
    }
```

- [ ] **Step 2: Implementar `Scene::load` en `engine/src/Scene.cpp`**

Tras `Scene::save` (Tarea 3, Step 3), añade:

```cpp
bool Scene::load(const std::string& path, PhysicsManager& physics, AudioManager& audio)
{
    auto parsed = FileManager::readJson(path);
    if (!parsed)
        return false;

    const nlohmann::json& j = *parsed;
    if (!j.contains("version") || j["version"].get<int>() != 1 || !j.contains("root"))
        return false;

    shutdown(physics, audio);
    m_root.children.clear();

    const nlohmann::json& rootJson = j["root"];
    m_root.name = rootJson.value("name", "root");

    try
    {
        nodeFromJson(rootJson, &m_root, glm::mat4(1.0f), physics, audio);
    }
    catch (const nlohmann::json::exception&)
    {
        // Nodo interno malformado (campo requerido ausente/tipo incorrecto):
        // la escena queda parcialmente reconstruida en vez de crashear. Caso
        // no cubierto por el spec de forma explícita — se prioriza no-crash
        // sobre atomicidad total de la carga.
        return false;
    }

    m_root.updateWorldTransforms();
    return true;
}
```

- [ ] **Step 3: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores. Nadie llama `Scene::load` todavía.

- [ ] **Step 4: Commit**

```bash
git add engine/src/Scene.cpp
git commit -m "feat(scene): añade Scene::load (reconstrucción del árbol desde JSON)"
```

---

### Task 5: UI — File menu (Save/Load Scene) + verificación manual completa

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h` (nuevo método privado, nuevos miembros)
- Modify: `engine/src/EditorUI.cpp` (constructor, `draw()`, `drawToolbar()`, nuevo método `drawSceneDialog()`)

**Interfaces:**
- Consumes: `Scene::save`/`Scene::load` (Tareas 3-4); `Renderer::removeGameObject(GameObject*)`, `Renderer::addStaticMesh(const Mesh&) -> int` (`engine/include/DonTopo/Renderer.h`, ya existentes); `GameObject::hasMesh()/getMesh()/staticRenderIndex` (`engine/include/DonTopo/GameObject.h`).
- Produces: botones "Save Scene"/"Load Scene" en el toolbar, funcionales de extremo a extremo.

- [ ] **Step 1: Añadir método y miembros nuevos en `engine/include/DonTopo/EditorUI.h`**

En `engine/include/DonTopo/EditorUI.h:89`, tras `void drawAudioClipDialog();`, añade:

```cpp
    void drawSceneDialog();
```

En `engine/include/DonTopo/EditorUI.h:172`, tras la declaración de `m_audioFileDialog` (`std::unique_ptr<IGFD::FileDialog> m_audioFileDialog;`), añade:

```cpp

    // Scene save/load — instancia propia de diálogo, mismo motivo que
    // m_meshFileDialog/m_audioFileDialog (Instance() singleton no soporta
    // diálogos concurrentes). Se reusa la misma instancia para Save y Load
    // porque nunca están abiertos a la vez (ambos disparados desde botones
    // secuenciales del toolbar).
    std::unique_ptr<IGFD::FileDialog> m_sceneFileDialog;
    bool        m_sceneDlgOpen = false;
    bool        m_sceneDlgIsSave = false;
    // Último error de guardado/carga de escena (vacío si ninguno pendiente).
    std::string m_sceneIOError;
```

- [ ] **Step 2: Inicializar `m_sceneFileDialog` en el constructor (`engine/src/EditorUI.cpp`)**

En `engine/src/EditorUI.cpp:169-173`, reemplaza:

```cpp
EditorUI::EditorUI()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_audioFileDialog(std::make_unique<IGFD::FileDialog>())
{
}
```

por:

```cpp
EditorUI::EditorUI()
    : m_meshFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_audioFileDialog(std::make_unique<IGFD::FileDialog>())
    , m_sceneFileDialog(std::make_unique<IGFD::FileDialog>())
{
}
```

- [ ] **Step 3: Llamar `drawSceneDialog()` desde `draw()`**

En `engine/src/EditorUI.cpp:177-188`, reemplaza:

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
    drawMeshDialog();
    drawAudioClipDialog();
    drawSceneDialog();
    drawContentBrowser(sceneRoot);
}
```

- [ ] **Step 4: Añadir botones Save/Load al toolbar**

En `engine/src/EditorUI.cpp:190-210`, reemplaza:

```cpp
void EditorUI::drawToolbar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kToolbarHeight));
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("##Toolbar", nullptr, flags);

    bool wireframe = m_renderer && m_renderer->isWireframeMode();
    if (wireframe)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Wireframe") && m_renderer)
        m_renderer->setWireframeMode(!wireframe);
    if (wireframe)
        ImGui::PopStyleColor();

    ImGui::End();
}
```

por:

```cpp
void EditorUI::drawToolbar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kToolbarHeight));
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("##Toolbar", nullptr, flags);

    bool wireframe = m_renderer && m_renderer->isWireframeMode();
    if (wireframe)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Wireframe") && m_renderer)
        m_renderer->setWireframeMode(!wireframe);
    if (wireframe)
        ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Button("Save Scene") && m_scene)
    {
        m_sceneDlgOpen   = true;
        m_sceneDlgIsSave = true;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode |
                    ImGuiFileDialogFlags_ConfirmOverwrite;
        m_sceneFileDialog->OpenDialog("SceneDlg", "Save Scene", ".json", cfg);
    }

    ImGui::SameLine();
    if (ImGui::Button("Load Scene") && m_scene)
    {
        m_sceneDlgOpen   = true;
        m_sceneDlgIsSave = false;
        IGFD::FileDialogConfig cfg;
        cfg.path  = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        m_sceneFileDialog->OpenDialog("SceneDlg", "Load Scene", ".json", cfg);
    }

    if (!m_sceneIOError.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_sceneIOError.c_str());
    }

    ImGui::End();
}
```

- [ ] **Step 5: Implementar `EditorUI::drawSceneDialog()`**

En `engine/src/EditorUI.cpp`, tras `EditorUI::drawAudioClipDialog()` (justo antes de `EditorUI::drawAddComponentButton()`, línea 1399), añade:

```cpp
void EditorUI::drawSceneDialog()
{
    // Mismo motivo que drawMeshDialog/drawAudioClipDialog: se ejecuta cada
    // frame independientemente de m_sceneDlgOpen para drenar el diálogo aunque
    // el usuario lo cierre sin confirmar.
    if (!m_sceneDlgOpen || !m_sceneFileDialog->Display("SceneDlg"))
        return;

    if (m_sceneFileDialog->IsOk())
    {
        std::string path = m_sceneFileDialog->GetFilePathName();

        if (m_sceneDlgIsSave)
        {
            m_sceneIOError = (m_scene && m_scene->save(path)) ? "" : "No se pudo guardar la escena";
        }
        else if (m_scene && m_renderer && m_physics && m_audio)
        {
            // Libera recursos GPU de la escena actual antes de que
            // Scene::load la reemplace — Scene::load solo limpia datos/
            // colliders/audio, no conoce Renderer (ver constraints del plan).
            for (auto& child : m_scene->getRoot().children)
                m_renderer->removeGameObject(child.get());

            if (m_scene->load(path, *m_physics, *m_audio))
            {
                m_scene->traverse([this](GameObject* go) {
                    if (go->hasMesh() && go->staticRenderIndex < 0)
                        go->staticRenderIndex = m_renderer->addStaticMesh(*go->getMesh());
                });
                m_selected = nullptr; // la selección anterior ya no existe
                m_sceneIOError.clear();
            }
            else
            {
                m_sceneIOError = "No se pudo cargar la escena";
            }
        }
    }

    m_sceneFileDialog->Close();
    m_sceneDlgOpen = false;
}
```

- [ ] **Step 6: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores. Primer punto donde todo el flujo Save/Load queda conectado — cualquier error de tipos/firma aparece aquí.

- [ ] **Step 7: Verificación manual (checklist, sin framework de tests automatizado)**

Ejecutar:
```powershell
& .\build-ninja\sandbox\Sandbox.exe
```

Comprobar en la ventana:
- [ ] La app arranca sin crash. El toolbar muestra "Save Scene" y "Load Scene" junto a "Wireframe".
- [ ] Con la escena demo cargada, pulsar "Save Scene", elegir/crear `assets/test_scene.json`: el fichero se crea en disco con contenido JSON legible (`version`, `root`, árbol anidado).
- [ ] Añadir un GameObject nuevo (Basic Shapes > Cube), con Box Collider (`Add > Box Collider`) y Audio Clip si hay FMOD disponible. Guardar de nuevo sobre el mismo fichero (confirmar overwrite).
- [ ] Pulsar "Load Scene", elegir el fichero guardado: la jerarquía se reemplaza por el contenido cargado — el cubo nuevo con su collider aparece, transforms coinciden con las guardadas.
- [ ] El cubo con `useGravity=true` (si se guardó como dinámico) cae y colisiona tras la carga — confirma que el collider se recreó con pose correcta.
- [ ] Cerrar la ventana tras un ciclo save→load no crashea (confirma que `Scene::load` no dejó el árbol en estado inconsistente para `Scene::shutdown` final).
- [ ] Editar a mano el JSON guardado para poner un `sourcePath` de mesh inexistente (ej. `"assets/no_existe.fbx"`) y cargar: la escena carga completa, ese nodo concreto queda sin mesh, sin crash, sin mensaje de error bloqueante.
- [ ] Corromper el JSON (borrar una `}` a mano) y cargar: aparece "No se pudo cargar la escena" en el toolbar, la escena en memoria no cambia, no crashea.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): añade botones Save/Load Scene al toolbar"
```

---

## Self-Review

**Cobertura del spec:** §1 CMake → Tarea 1. §2 `FileManager` → Tarea 2. §3 Modelo de datos → Tareas 3-4 (`nodeToJson`/`nodeFromJson`). §4 `Scene::save`/`Scene::load` → Tareas 3-4. §5 Manejo de errores (asset roto, JSON malformado, escritura fallida) → Tarea 4 Step 2 (`try/catch` en mesh/audio y en `load`), Tarea 5 Step 5 (`m_sceneIOError`). §6 UI File menu → Tarea 5. §7 Testing → Tarea 5 Step 7. Riesgo del spec sobre firmas de factory reutilizables desde `Scene::load` sin pasar por `EditorUI`: confirmado en Tareas 3-4 — `PhysicsManager`/`AudioManager` no dependen de estado propio de `EditorUI`. Riesgo sobre `updateWorldTransforms()` tras colliders kinemáticos: resuelto — `nodeFromJson` calcula `worldTransform` antes de crear cada collider, y la llamada final a `m_root.updateWorldTransforms()` es una pasada de seguridad redundante pero inocua. Riesgo sobre `clearChildren()` vs reusar `removeGameObject`: resuelto usando `shutdown()` + `children.clear()` directo (más simple, evita el recorrido O(n²) de `removeGameObject` en bucle).

**Desviación del spec:** el modelo de datos del spec mencionaba `mesh.kind: "path"|"procedural"` — la implementación deriva ese dato de `sourcePath.empty()` y reutiliza `Mesh::name` (ya seteado por `Cube/Sphere/Plane/Capsule::create`) en vez de añadir un campo nuevo. Mismo contenido semántico, menos redundancia.

**Placeholders:** ninguno — cada paso trae código completo.

**Consistencia de tipos:** `Scene::save(const std::string&) const -> bool` y `Scene::load(const std::string&, PhysicsManager&, AudioManager&) -> bool` usan las mismas firmas en la declaración (Tarea 3 Step 1) y su uso en `EditorUI::drawSceneDialog` (Tarea 5 Step 5). `FileManager::writeJson`/`readJson` firmas consistentes entre Tarea 2 (declaración+implementación) y su uso en `Scene::save`/`Scene::load` (Tareas 3-4).
