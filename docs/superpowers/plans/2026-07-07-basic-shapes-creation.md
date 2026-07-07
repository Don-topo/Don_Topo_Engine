# Basic Shapes Creation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Añadir al menú contextual del panel Scene una entrada "Basic Shapes" (Cube/Sphere/Plane/Capsule) que crea un GameObject con el mesh correspondiente, con la misma selección/borrado/física que cualquier otro GameObject.

**Architecture:** `Renderer` gana `addStaticMesh(const Mesh&) -> int`, alta en caliente de un objeto renderizable (mismo patrón que el `addSkinnedMesh` existente), apoyado en un método privado extraído `allocateObjectDescriptorSet` (hoy inline dentro de `createDescriptorSets`). Nueva clase de geometría `Capsule` (cilindro + 2 semiesferas), mismo patrón estático que `Sphere`. `EditorUI` gana un puntero no-propietario a `Renderer` (mismo patrón que ya tiene con `PhysicsManager`) y un método privado `createBasicShape` invocado desde los dos menús contextuales existentes del panel Scene.

**Tech Stack:** C++20, Vulkan, ImGui, CMake + Ninja (preset `debug`).

## Global Constraints

- No hay framework de tests en el repo (sin gtest/ctest). Verificación = build (`cmake --build --preset debug`) + ejecutar `build-ninja/sandbox/DonTopoSandbox.exe` + revisar visualmente, igual que el resto del proyecto.
- El pool de descriptores ya tiene 128 slots de margen (`createDescriptorPool`, `n = (m_objects.size()+128)*MAX_FRAMES`) — no se toca, no se valida el límite (fuera de alcance, ver spec).
- El shape nuevo nace **sin collider** — el flujo "Add" del panel Properties (ya existente) no se modifica.
- Nombre del GameObject = nombre fijo del shape ("Cube"/"Sphere"/"Plane"/"Capsule"), duplicados entre hermanos permitidos.
- Spawn en `localTransform` identidad (origen local del padre clicado).
- Spec completo: `docs/superpowers/specs/2026-07-07-basic-shapes-creation-design.md`.

---

### Task 1: `Renderer::addStaticMesh` — alta en caliente de mallas estáticas

**Files:**
- Modify: `engine/include/DonTopo/Renderer.h`
- Modify: `engine/src/Renderer.cpp:1081-1162` (refactor `createDescriptorSets`), `engine/src/Renderer.cpp:1246-1275` (junto a `buildRenderObject`)

**Interfaces:**
- Produces: `int Renderer::addStaticMesh(const Mesh& mesh)` — construye buffers/texturas/descriptor set de un mesh nuevo, lo añade a `m_objects` y devuelve su índice (para asignar a `GameObject::staticRenderIndex`).

- [ ] **Step 1: Declarar los dos métodos nuevos en `Renderer.h`**

En la sección `public:` (junto a `addSkinnedMesh`, `engine/include/DonTopo/Renderer.h:50`):

```cpp
            int addSkinnedMesh(const SkinnedMesh& mesh);
            // Añade un mesh estático nuevo (buffers + texturas + descriptor set) y lo
            // registra en m_objects. Devuelve el índice para GameObject::staticRenderIndex.
            int addStaticMesh(const Mesh& mesh);
```

En la sección `private:` (junto a `buildRenderObject`/`destroyRenderObject`, `engine/include/DonTopo/Renderer.h:188-189`):

```cpp
            void buildRenderObject(const Mesh& mesh, RenderObject& obj);
            void allocateObjectDescriptorSet(RenderObject& obj);
            void destroyRenderObject(RenderObject& obj);
```

- [ ] **Step 2: Extraer `allocateObjectDescriptorSet` del cuerpo de `createDescriptorSets`**

En `engine/src/Renderer.cpp`, reemplazar la función `createDescriptorSets` completa (líneas 1081-1162) por:

```cpp
    void Renderer::createDescriptorSets()
    {
        for (auto& obj : m_objects)
            allocateObjectDescriptorSet(obj);
    }

    void Renderer::allocateObjectDescriptorSet(RenderObject& obj)
    {
        VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_descriptorSetLayout, m_descriptorSetLayout };

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_descriptorPool;
        allocInfo.descriptorSetCount = MAX_FRAMES;
        allocInfo.pSetLayouts        = layouts;

        if (vkAllocateDescriptorSets(m_gpu.device(), &allocInfo, obj.descriptorSets) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate descriptor sets!");

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = m_uniformBuffers[i];
            bufferInfo.offset = 0;
            bufferInfo.range  = sizeof(UniformBufferObject);

            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView   = obj.textureView;
            imageInfo.sampler     = obj.sampler;

            VkDescriptorImageInfo normalInfo{};
            normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            normalInfo.imageView   = obj.normalView;
            normalInfo.sampler     = obj.normalSampler;

            VkDescriptorImageInfo shadowInfo{};
            shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            shadowInfo.imageView   = m_shadowView;
            shadowInfo.sampler     = m_shadowSampler;

            VkDescriptorImageInfo ormInfo{};
            ormInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ormInfo.imageView   = obj.ormView;
            ormInfo.sampler     = obj.ormSampler;

            VkWriteDescriptorSet writes[5]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = obj.descriptorSets[i];
            writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1; writes[0].pBufferInfo = &bufferInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = obj.descriptorSets[i];
            writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1; writes[1].pImageInfo = &imageInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = obj.descriptorSets[i];
            writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].descriptorCount = 1; writes[2].pImageInfo = &normalInfo;

            writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[3].dstSet = obj.descriptorSets[i];
            writes[3].dstBinding = 3; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3].descriptorCount = 1; writes[3].pImageInfo = &shadowInfo;

            writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[4].dstSet = obj.descriptorSets[i];
            writes[4].dstBinding = 4; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[4].descriptorCount = 1; writes[4].pImageInfo = &ormInfo;

            vkUpdateDescriptorSets(m_gpu.device(), 5, writes, 0, nullptr);
        }
    }
```

Esto es un refactor puro (mismo comportamiento, cero cambio funcional) — el cuerpo es literal al que ya existía dentro del `for`.

- [ ] **Step 3: Añadir `addStaticMesh`, junto a `buildRenderObject`**

En `engine/src/Renderer.cpp`, justo después del cierre de `buildRenderObject` (después de la línea `engine/src/Renderer.cpp:1275`, antes de `destroyRenderObject`):

```cpp
    int Renderer::addStaticMesh(const Mesh& mesh)
    {
        m_objects.emplace_back();
        RenderObject& obj = m_objects.back();
        buildRenderObject(mesh, obj);
        allocateObjectDescriptorSet(obj);
        return (int)m_objects.size() - 1;
    }
```

- [ ] **Step 4: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 5: Verificar que no hay regresión visual**

Run: `build-ninja/sandbox/DonTopoSandbox.exe`
Expected: la escena arranca igual que antes del refactor — soldier, model, floor, cube, sphere, soldier_animado visibles y texturizados correctamente (el refactor de `createDescriptorSets` no cambia el resultado, solo la estructura del código).

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer.h engine/src/Renderer.cpp
git commit -m "feat(renderer): añadir addStaticMesh para alta en caliente de mallas estáticas"
```

---

### Task 2: `Capsule` — geometría visual (cilindro + 2 semiesferas)

**Files:**
- Create: `engine/include/DonTopo/Capsule.h`
- Create: `engine/src/Capsule.cpp`
- Modify: `engine/CMakeLists.txt:10` (añadir `src/Capsule.cpp` tras `src/Plane.cpp`)

**Interfaces:**
- Produces: `DonTopo::Mesh Capsule::create(float radius = 0.5f, float height = 1.0f, uint32_t segments = 32, uint32_t capRings = 8, glm::vec3 color = {0.8f, 0.8f, 0.8f})`. `height` es la longitud del cilindro central (distancia entre los centros de las dos semiesferas, igual semántica que `CapsuleCollider::halfHeight * 2`); la altura total del mesh es `height + 2*radius`.

- [ ] **Step 1: Crear `Capsule.h`**

```cpp
// engine/include/DonTopo/Capsule.h
#pragma once
#include "DonTopo/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo
{
    class Capsule
    {
        public:
            // height = longitud del cilindro central (sin contar las semiesferas);
            // altura total del mesh = height + 2*radius. Mismo eje (Y) y misma
            // semántica de radio que CapsuleCollider.
            static Mesh create(float radius = 0.5f, float height = 1.0f,
                                uint32_t segments = 32, uint32_t capRings = 8,
                                glm::vec3 color = {0.8f, 0.8f, 0.8f});
    };
}
```

- [ ] **Step 2: Crear `Capsule.cpp`**

```cpp
// engine/src/Capsule.cpp
#include "DonTopo/Capsule.h"
#include <cmath>
#include <vector>

namespace DonTopo
{
    Mesh Capsule::create(float radius, float height, uint32_t segments, uint32_t capRings, glm::vec3 color)
    {
        Mesh mesh;
        mesh.name = "capsule";

        const float PI = 3.14159265358979323846f;
        const float halfHeight = height * 0.5f;

        // Perfil (r, y, normal_r, normal_y) de abajo a arriba: casquete inferior
        // (capRings+1 anillos, polo incluido), cilindro (2 anillos, mismo radio),
        // casquete superior (capRings+1 anillos, polo incluido). Se revoluciona
        // este perfil alrededor del eje Y (mismo enfoque que Sphere::create pero
        // con un perfil no-circular).
        struct ProfilePoint { float r, y, nr, ny; };
        std::vector<ProfilePoint> profile;

        for (uint32_t i = 0; i <= capRings; ++i)
        {
            float theta = PI - (float)i / (float)capRings * (PI * 0.5f); // PI -> PI/2
            float s = std::sin(theta), c = std::cos(theta);
            profile.push_back({ radius * s, -halfHeight + radius * c, s, c });
        }
        profile.push_back({ radius, -halfHeight, 1.0f, 0.0f });
        profile.push_back({ radius,  halfHeight, 1.0f, 0.0f });
        for (uint32_t i = 0; i <= capRings; ++i)
        {
            float theta = PI * 0.5f - (float)i / (float)capRings * (PI * 0.5f); // PI/2 -> 0
            float s = std::sin(theta), c = std::cos(theta);
            profile.push_back({ radius * s, halfHeight + radius * c, s, c });
        }

        const uint32_t ringsTotal = (uint32_t)profile.size();
        for (uint32_t r = 0; r < ringsTotal; ++r)
        {
            const ProfilePoint& p = profile[r];
            for (uint32_t c = 0; c <= segments; ++c)
            {
                float phi = (float)c / (float)segments * 2.0f * PI;
                float sinPhi = std::sin(phi), cosPhi = std::cos(phi);

                Vertex v{};
                v.pos     = { p.r * cosPhi, p.y, p.r * sinPhi };
                v.normal  = { p.nr * cosPhi, p.ny, p.nr * sinPhi };
                v.color   = color;
                v.uv      = { (float)c / (float)segments, (float)r / (float)(ringsTotal - 1) };
                v.tangent = { -sinPhi, 0.0f, cosPhi };

                mesh.vertices.push_back(v);
            }
        }

        const uint32_t cols = segments + 1;
        for (uint32_t r = 0; r < ringsTotal - 1; ++r)
        {
            for (uint32_t c = 0; c < segments; ++c)
            {
                uint32_t i0 = r * cols + c;
                uint32_t i1 = r * cols + c + 1;
                uint32_t i2 = (r + 1) * cols + c + 1;
                uint32_t i3 = (r + 1) * cols + c;

                mesh.indices.insert(mesh.indices.end(), { i0, i1, i2, i0, i2, i3 });
            }
        }

        return mesh;
    }
}
```

- [ ] **Step 3: Registrar `Capsule.cpp` en el build**

En `engine/CMakeLists.txt`, tras `src/Plane.cpp` (línea 10):

```cmake
    src/Cube.cpp
    src/Sphere.cpp
    src/Plane.cpp
    src/Capsule.cpp
    src/Camera.cpp
```

- [ ] **Step 4: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error, sin referencias rotas a `Capsule.h`/`Capsule.cpp`. (Sin verificación visual en este task — `Capsule::create` no se invoca todavía desde ningún sitio; se verifica visualmente en Task 3.)

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Capsule.h engine/src/Capsule.cpp engine/CMakeLists.txt
git commit -m "feat(geometry): añadir Capsule::create (cilindro + 2 semiesferas)"
```

---

### Task 3: Menú "Basic Shapes" en el panel Scene

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h`
- Modify: `engine/src/EditorUI.cpp:161-171` (`drawScene`, menú de área vacía), `engine/src/EditorUI.cpp:292-302` (`drawSceneNode`, menú sobre un nodo)
- Modify: `engine/src/Renderer.cpp:2004-2008` (`setSceneRoot`)

**Interfaces:**
- Consumes: `Renderer::addStaticMesh(const Mesh&) -> int` (Task 1), `Capsule::create(...)` (Task 2), `Cube::create`, `Sphere::create`, `Plane::create` (ya existentes), `GameObject::addChild(std::string) -> GameObject*`, `GameObject::setMesh(std::shared_ptr<Mesh>)`, `GameObject::staticRenderIndex`.

- [ ] **Step 1: `EditorUI.h` — forward declarations, `setRenderer` y `createBasicShape`**

Añadir `class Mesh;` y `class Renderer;` junto a las demás forward declarations (`engine/include/DonTopo/EditorUI.h:9-16`):

```cpp
class GameObject;
class Mesh;
class PhysicsManager;
class BoxCollider;
class SphereCollider;
class CapsuleCollider;
class PlaneCollider;
class Renderer;
```

Añadir `#include <memory>` junto a los demás includes (`engine/include/DonTopo/EditorUI.h:1-7`, se usa `std::shared_ptr<Mesh>` en la firma nueva).

Añadir el setter público, junto a `setPhysicsManager` (`engine/include/DonTopo/EditorUI.h:37`):

```cpp
    void setPhysicsManager(PhysicsManager* physics) { m_physics = physics; }
    // Puntero no-propietario: Renderer es dueño de este EditorUI y se pasa a sí
    // mismo desde setSceneRoot. Necesario para registrar el mesh GPU (addStaticMesh)
    // al crear un shape desde el menú "Basic Shapes".
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
```

Añadir el método privado, junto a `drawAddComponentButton` (`engine/include/DonTopo/EditorUI.h:58`):

```cpp
    void drawAddComponentButton();
    // Crea un GameObject hijo de parent con el mesh dado, lo registra en el
    // Renderer (staticRenderIndex) y lo deja sin collider. No-op si parent o
    // m_renderer son nullptr.
    void createBasicShape(GameObject* parent, const std::string& name, std::shared_ptr<Mesh> mesh);
    void drawContentBrowser();
```

Añadir el miembro, junto a `m_physics` (`engine/include/DonTopo/EditorUI.h:133`):

```cpp
    PhysicsManager* m_physics = nullptr;
    Renderer*       m_renderer = nullptr;
```

- [ ] **Step 2: `EditorUI.cpp` — includes y `createBasicShape`**

Añadir includes junto a los existentes (`engine/src/EditorUI.cpp:1-8`):

```cpp
#include "DonTopo/EditorUI.h"
#include "DonTopo/GameObject.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include "DonTopo/Gizmos.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/Cube.h"
#include "DonTopo/Sphere.h"
#include "DonTopo/Plane.h"
#include "DonTopo/Capsule.h"
```

Añadir la implementación, junto a `beginRename` (después de `engine/src/EditorUI.cpp:258`, antes de `drawSceneNode`):

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

- [ ] **Step 3: Menú en el área vacía (`drawScene`)**

En `engine/src/EditorUI.cpp:161-171`, reemplazar:

```cpp
    if (ImGui::BeginPopupContextWindow("##SceneContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("Create GameObject") && sceneRoot)
            sceneRoot->addChild("GameObject");
        if (ImGui::MenuItem("Rename", nullptr, false, canRename))
            beginRename(m_selected);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canDelete))
            m_pendingDelete = m_selected;
        ImGui::EndPopup();
    }
```

por:

```cpp
    if (ImGui::BeginPopupContextWindow("##SceneContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("Create GameObject") && sceneRoot)
            sceneRoot->addChild("GameObject");
        if (ImGui::BeginMenu("Basic Shapes"))
        {
            if (ImGui::MenuItem("Cube"))
                createBasicShape(sceneRoot, "Cube", std::make_shared<Mesh>(Cube::create(50.0f)));
            if (ImGui::MenuItem("Sphere"))
                createBasicShape(sceneRoot, "Sphere", std::make_shared<Mesh>(Sphere::create(50.0f)));
            if (ImGui::MenuItem("Plane"))
                createBasicShape(sceneRoot, "Plane", std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)));
            if (ImGui::MenuItem("Capsule"))
                createBasicShape(sceneRoot, "Capsule", std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)));
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Rename", nullptr, false, canRename))
            beginRename(m_selected);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canDelete))
            m_pendingDelete = m_selected;
        ImGui::EndPopup();
    }
```

- [ ] **Step 4: Menú sobre un nodo existente (`drawSceneNode`)**

En `engine/src/EditorUI.cpp:292-302`, reemplazar:

```cpp
    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Create GameObject"))
            node->addChild("GameObject");
        bool canModify = node->parent != nullptr;
        if (ImGui::MenuItem("Rename", nullptr, false, canModify))
            beginRename(node);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canModify))
            m_pendingDelete = node;
        ImGui::EndPopup();
    }
```

por:

```cpp
    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Create GameObject"))
            node->addChild("GameObject");
        if (ImGui::BeginMenu("Basic Shapes"))
        {
            if (ImGui::MenuItem("Cube"))
                createBasicShape(node, "Cube", std::make_shared<Mesh>(Cube::create(50.0f)));
            if (ImGui::MenuItem("Sphere"))
                createBasicShape(node, "Sphere", std::make_shared<Mesh>(Sphere::create(50.0f)));
            if (ImGui::MenuItem("Plane"))
                createBasicShape(node, "Plane", std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)));
            if (ImGui::MenuItem("Capsule"))
                createBasicShape(node, "Capsule", std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)));
            ImGui::EndMenu();
        }
        bool canModify = node->parent != nullptr;
        if (ImGui::MenuItem("Rename", nullptr, false, canModify))
            beginRename(node);
        if (ImGui::MenuItem("Delete GameObject", nullptr, false, canModify))
            m_pendingDelete = node;
        ImGui::EndPopup();
    }
```

- [ ] **Step 5: Wiring — `Renderer::setSceneRoot` pasa `this` a `EditorUI`**

En `engine/src/Renderer.cpp:2004-2008`, reemplazar:

```cpp
    void Renderer::setSceneRoot(GameObject* root)
    {
        m_sceneRoot = root;
        m_editorUI.setOnDelete([this](GameObject* node) { removeGameObject(node); });
    }
```

por:

```cpp
    void Renderer::setSceneRoot(GameObject* root)
    {
        m_sceneRoot = root;
        m_editorUI.setOnDelete([this](GameObject* node) { removeGameObject(node); });
        m_editorUI.setRenderer(this);
    }
```

- [ ] **Step 6: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 7: Verificación manual completa**

Run: `build-ninja/sandbox/DonTopoSandbox.exe`

Verificar en el editor:
1. Click derecho en área vacía del panel Scene → "Basic Shapes" despliega Cube/Sphere/Plane/Capsule.
2. Click en cada una de las 4 → aparece un GameObject nuevo en el árbol (nombre correcto) y su mesh se renderiza en el origen (0,0,0) del mundo, con la geometría esperada (capsule = cilindro con dos tapas redondeadas, sin huecos ni triángulos volteados).
3. Click derecho sobre un GameObject existente (p.ej. "cube") → "Basic Shapes" → crear un shape → confirmar que aparece como hijo de ese nodo en el árbol (indentado bajo él), no bajo root.
4. Seleccionar cada shape creado (click en el árbol) → gizmo de ejes aparece sobre él; mover con drag de posición en Properties confirma que el mesh se mueve.
5. Rename (F2) sobre un shape creado funciona igual que sobre cualquier GameObject.
6. Panel Properties → "Add" → añadir el collider correspondiente (Box para Cube, Sphere para Sphere, Plane para Plane, Capsule para Capsule) → confirmar que no crashea y el collider se puede editar/quitar igual que en GameObjects existentes.
7. Borrar (Delete key o menú) cada shape creado → desaparece del árbol y de la escena renderizada, sin crash ni warning de validation layers de Vulkan en consola.
8. Crear más de un shape del mismo tipo (p.ej. dos Cube) → ambos coexisten con el mismo nombre, ambos seleccionables/borrables independientemente.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp engine/src/Renderer.cpp
git commit -m "feat(editor): menú Basic Shapes para crear Cube/Sphere/Plane/Capsule desde Scene"
```
