# Basic Shapes creation en el editor

## Objetivo

Añadir al panel Scene la capacidad de crear GameObjects primitivos (Cube, Sphere, Plane, Capsule) desde el menú contextual de botón derecho, con el mismo comportamiento de selección/borrado/física que cualquier otro GameObject.

## Contexto actual

- `EditorUI::drawScene` (área vacía) y `EditorUI::drawSceneNode` (sobre un nodo existente) tienen cada uno un `BeginPopupContextWindow`/`BeginPopupContextItem` con `MenuItem("Create GameObject")`, que crea un `GameObject` vacío (sin mesh) como hijo de `sceneRoot` o del nodo clicado respectivamente.
- `GameObject` guarda `staticRenderIndex` (índice en `Renderer::m_objects`) y `m_mesh` (`shared_ptr<Mesh>`).
- Todas las mallas estáticas se registran hoy únicamente al arrancar, en `main.cpp`, vía `renderer.init(window, meshes)` (`Renderer::m_objects.resize(meshes.size())` + `buildRenderObject` por cada una).
- Ya existe un precedente de alta **en caliente** de geometría: `Renderer::addSkinnedMesh(const SkinnedMesh&) -> int`, que hace `m_skinnedObjects.emplace_back()` y construye los recursos GPU de ese único mesh, sin reutilizar huecos liberados.
- El borrado ya soporta esto: `Renderer::removeGameObject(node)` recorre el subárbol y llama a `removeStaticObject(index)` / `removeSkinnedObject(index)`, dejando el slot vacío (`vertexBuffer == VK_NULL_HANDLE`).
- Existen clases de geometría estática `Cube::create(size, color)`, `Sphere::create(radius, segments, rings, color)`, `Plane::create(size, y, color, uvScale)` (`engine/include/DonTopo/{Cube,Sphere,Plane}.h`). **No existe** una clase `Capsule` de geometría visual (solo `CapsuleCollider`, que es física, sin mesh renderizable).
- El panel Properties ya tiene un flujo genérico "Add" (botón + popup) para añadir Box/Sphere/Capsule/Plane Collider a cualquier GameObject seleccionado, mutuamente excluyentes vía `GameObject::hasAnyCollider()`. Este flujo no necesita cambios.
- `EditorUI` ya sostiene un puntero no-propietario a un sistema externo (`PhysicsManager* m_physics`, inyectado vía `setPhysicsManager`) para poder crear colliders directamente desde el panel sin pasar por callbacks. Se reutiliza el mismo patrón para el Renderer.

## Diseño

### 1. `Renderer::addStaticMesh`

`buildRenderObject(mesh, obj)` solo crea buffers/texturas — **no** aloca `obj.descriptorSets`. Eso lo hace hoy `createDescriptorSets()`, en un único paso ejecutado una vez al final de `init()` que itera todo `m_objects`. Para poder añadir un objeto en caliente hace falta extraer esa alocación+escritura de descriptor set a un método privado reutilizable por objeto:

```cpp
// Renderer.h (sección privada, junto a buildRenderObject/destroyRenderObject)
void allocateObjectDescriptorSet(RenderObject& obj);
```

```cpp
// Renderer.cpp — cuerpo extraído literal del bucle `for(auto& obj : m_objects)`
// de createDescriptorSets() (líneas 1085-1161 actuales), parametrizado sobre un solo obj.
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

`createDescriptorSets()` pasa a ser un simple `for(auto& obj : m_objects) allocateObjectDescriptorSet(obj);` (mismo comportamiento, cero regresión).

Nuevo método público:

```cpp
// Renderer.h (sección pública)
int addStaticMesh(const Mesh& mesh);
```

```cpp
// Renderer.cpp
int Renderer::addStaticMesh(const Mesh& mesh)
{
    m_objects.emplace_back();
    RenderObject& obj = m_objects.back();
    buildRenderObject(mesh, obj);
    allocateObjectDescriptorSet(obj);
    return (int)m_objects.size() - 1;
}
```

**Por qué es seguro sin sincronización explícita:** `recordCommandBuffer` se vuelve a grabar entero cada `drawFrame` (no hay comandos en vuelo que referencien buffers todavía no creados), y el pool de descriptores ya se crea con margen para esto: `createDescriptorPool()` usa `n = (m_objects.size() + 128) * MAX_FRAMES` como `maxSets` — 128 slots de sobra ya reservados en `init()`, pensados exactamente para altas en caliente como ésta (mismo pool, sin necesidad de recrearlo). El límite práctico de este pass: no crear más de 128 shapes nuevos en una sesión sin reiniciar — no se valida ni se avisa (fuera de alcance, ver más abajo).

No reutiliza slots liberados por `removeStaticObject` (igual que el precedente skinned) — consistente y más simple; el hueco vacío en el vector no se dibuja porque `recordCommandBuffer` ya salta objetos con `vertexBuffer == VK_NULL_HANDLE` (mismo chequeo que usa `recordComputePass` para skinned).

### 2. `Capsule` — nueva clase de geometría visual

`engine/include/DonTopo/Capsule.h` + `engine/src/Capsule.cpp`, mismo patrón estático que `Sphere`:

```cpp
class Capsule
{
public:
    static Mesh create(float radius = 25.0f, float height = 50.0f,
                        uint32_t segments = 16, uint32_t rings = 8,
                        glm::vec3 color = {0.8f, 0.8f, 0.8f});
};
```

Geometría: cilindro de altura `height` entre dos semiesferas de radio `radius` en los extremos (sin solape, altura total = `height + 2*radius`), generado por revolución igual que `Sphere::create` pero partiendo la banda de anillos en cilindro central + dos casquetes. Normales suaves por vértice. `height`/`radius` en las mismas unidades que `CapsuleCollider` para que a futuro un collider añadido desde Properties encaje visualmente con el mesh (aunque no se auto-vinculan: ver sección Alcance).

### 3. `EditorUI` — puntero a Renderer + submenú Basic Shapes

```cpp
// EditorUI.h — junto a las demás forward declarations
class Renderer;
...
void setRenderer(Renderer* renderer) { m_renderer = renderer; }
...
Renderer* m_renderer = nullptr;
```

A diferencia de `PhysicsManager` (que vive en `main.cpp`, fuera de `Renderer`, y por eso necesita `Renderer::setPhysicsManager` reenviando desde fuera), el `Renderer` es dueño directo de `m_editorUI` — puede pasarse `this` sin exponer API nueva a `main.cpp`. Basta con añadir una línea a `Renderer::setSceneRoot` (ya existe, `Renderer.cpp`):

```cpp
void Renderer::setSceneRoot(GameObject* root)
{
    m_sceneRoot = root;
    m_editorUI.setOnDelete([this](GameObject* node) { removeGameObject(node); });
    m_editorUI.setRenderer(this);
}
```

`EditorUI.cpp` incluye `"DonTopo/Renderer.h"` para poder llamar `m_renderer->addStaticMesh(...)` (mismo patrón circular ya resuelto hoy: `Renderer.h` incluye `EditorUI.h` para el miembro `m_editorUI`, y `EditorUI.h` solo forward-declara `Renderer`; `#pragma once` en ambos evita el ciclo en tiempo de compilación).

Nuevo helper privado en `EditorUI`:

```cpp
void EditorUI::createBasicShape(GameObject* parent, ShapeKind kind)
{
    if (!parent || !m_renderer) return;
    std::shared_ptr<Mesh> mesh;
    const char* name;
    switch (kind) {
        case ShapeKind::Cube:    mesh = std::make_shared<Mesh>(Cube::create(50.0f));    name = "Cube";    break;
        case ShapeKind::Sphere:  mesh = std::make_shared<Mesh>(Sphere::create(50.0f));  name = "Sphere";  break;
        case ShapeKind::Plane:   mesh = std::make_shared<Mesh>(Plane::create(50.0f, 0.0f)); name = "Plane"; break;
        case ShapeKind::Capsule: mesh = std::make_shared<Mesh>(Capsule::create(25.0f, 50.0f)); name = "Capsule"; break;
    }
    GameObject* go = parent->addChild(name);
    go->setMesh(mesh);
    go->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
}
```

`ShapeKind` es un `enum class` simple definido junto a la clase (o local al .cpp). `localTransform` queda identidad (`{1.0f}` por defecto en `GameObject`) → el shape aparece en el origen local del padre, tal como "Create GameObject".

En los dos sitios existentes (`drawScene` para área vacía, `drawSceneNode` para nodo), justo después de `MenuItem("Create GameObject")`:

```cpp
if (ImGui::BeginMenu("Basic Shapes"))
{
    if (ImGui::MenuItem("Cube"))    createBasicShape(sceneRoot, ShapeKind::Cube);
    if (ImGui::MenuItem("Sphere"))  createBasicShape(sceneRoot, ShapeKind::Sphere);
    if (ImGui::MenuItem("Plane"))   createBasicShape(sceneRoot, ShapeKind::Plane);
    if (ImGui::MenuItem("Capsule")) createBasicShape(sceneRoot, ShapeKind::Capsule);
    ImGui::EndMenu();
}
```

(en `drawSceneNode` el parent pasado es `node` en vez de `sceneRoot`, igual que ya distingue "Create GameObject" en cada sitio).

### 4. Nombre, selección, borrado, física — sin cambios de código

- Nombre = nombre fijo del shape ("Cube"/"Sphere"/"Plane"/"Capsule"); duplicados entre hermanos permitidos, igual que ya ocurre con "GameObject" repetido.
- Selección (`m_selected`), rename (F2/menú), drag&drop, borrado (`m_pendingDelete` → `m_onDelete` → `Renderer::removeGameObject`) ya operan sobre cualquier `GameObject` con `staticRenderIndex` válido — no requieren cambios.
- El shape nace **sin collider**. Añadir física se hace igual que con cualquier otro GameObject: panel Properties → "Add" → Box/Sphere/Capsule/Plane Collider.

## Alcance / fuera de alcance

- No se auto-asigna collider al crear el shape (decisión explícita: consistente con el resto del editor).
- No se intenta que el tamaño del collider añadido después coincida automáticamente con el tamaño del mesh — eso ya es el comportamiento actual para cualquier GameObject (el usuario ajusta el collider manualmente desde Properties).
- No se añade un mecanismo de reciclado de slots libres en `m_objects`; se acepta el mismo comportamiento que `addSkinnedMesh` (crecimiento del vector, huecos no reutilizados).
- No se valida el límite de 128 objetos estáticos añadidos en caliente (headroom del descriptor pool, ver sección 1) — pasado ese número `vkAllocateDescriptorSets` fallaría con `VK_ERROR_OUT_OF_POOL_MEMORY` y lanzaría excepción; aceptable para este pass (uso manual desde el editor).
- No se persiste la escena a disco (fuera del alcance de este proyecto por ahora, igual que "Create GameObject").

## Testing

- Verificación manual (no hay suite automatizada de UI en este proyecto): abrir editor, click derecho en área vacía → Basic Shapes → cada una de las 4 opciones aparece con su mesh correcto en el origen. Repetir sobre un nodo existente y confirmar que el shape nace como su hijo. Seleccionar, renombrar, mover con gizmo, añadir/quitar collider (Box/Sphere/Capsule/Plane) y borrar cada shape creado, confirmando que no hay crash ni fugas de recursos GPU (mismo camino que `removeGameObject` ya cubre).
