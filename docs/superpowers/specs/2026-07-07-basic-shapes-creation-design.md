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

Nuevo método público, mismo patrón que `addSkinnedMesh`:

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
    return (int)m_objects.size() - 1;
}
```

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
// EditorUI.h
void setRenderer(Renderer* renderer) { m_renderer = renderer; }
...
Renderer* m_renderer = nullptr;
```

`Renderer::setSceneRoot` (o `Renderer::init`, donde ya se hace `m_editorUI.setPhysicsManager`) pasa `m_editorUI.setRenderer(this)`.

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
- No se persiste la escena a disco (fuera del alcance de este proyecto por ahora, igual que "Create GameObject").

## Testing

- Verificación manual (no hay suite automatizada de UI en este proyecto): abrir editor, click derecho en área vacía → Basic Shapes → cada una de las 4 opciones aparece con su mesh correcto en el origen. Repetir sobre un nodo existente y confirmar que el shape nace como su hijo. Seleccionar, renombrar, mover con gizmo, añadir/quitar collider (Box/Sphere/Capsule/Plane) y borrar cada shape creado, confirmando que no hay crash ni fugas de recursos GPU (mismo camino que `removeGameObject` ya cubre).
