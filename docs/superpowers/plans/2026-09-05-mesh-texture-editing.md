# Edición de texturas del Mesh desde Properties — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Asignar, cambiar y quitar las texturas (Albedo, Normal, Metallic/Roughness) de un GameObject con Mesh desde el panel Properties, con efecto inmediato en el viewport en los dos backends y persistencia en el .scene.

**Architecture:** Los overrides de textura viven en el `GameObject`, no en el `Material`, y se aplican encima del material que trae el FBX; el valor original se guarda al lado para que Clear tenga a qué volver en caliente. La re-subida a GPU pasa por un `rebuildStaticMesh(index, mesh)` nuevo, simétrico al `rebuildSkinnedMesh` que ya existe, que conserva el índice de render. La serialización añade un bloque `materials` al JSON del mesh, con rutas relativas a la raíz del proyecto.

**Tech Stack:** C++20, Vulkan, D3D12, nlohmann::json, ImGui, CMake + Ninja.

**Spec:** `docs/superpowers/specs/2026-09-05-mesh-texture-editing-design.md`

## Global Constraints

- Los ficheros del repo van en **CRLF** con comentarios acentuados en UTF-8: editar SIEMPRE con la herramienta Edit. Nunca `sed -i`, nunca `Get-Content`/`Set-Content` de PowerShell, nunca reescrituras por script.
- Build por PowerShell con `configure.bat`/`build.bat` (Ninja + vcvarsall), nunca cmake crudo. Release: `configure-release.bat` + `build-release.bat` → `build-ninja-release`.
- Los tests se ejecutan **desde la raíz del repo**: desde `build-ninja/engine/tests` fallan 5 de 25 porque `assets/` no resuelve.
- Paridad Vulkan y D3D12 obligatoria: una feature que solo va en un backend no está terminada.
- Solo lo pedido: no refactorizar `Mesh`/`Material`, no partir clases, no añadir abstracciones, no unificar las dos copias de `materialsOf()`.
- Estilo de test del repo: `main()` plano con `#define CHECK(cond)`, sin framework.
- Sabotaje de guardas **de una en una** (parchear → compilar → ejecutar → revertir), nunca en bloque.
- Si el commit incluye `imgui.ini` modificado, va dentro: es la config del editor del usuario.

---

### Task 1: Overrides de textura en el GameObject

**Files:**
- Modify: `engine/include/DonTopo/Core/GameObject.h`
- Modify: `engine/src/Core/GameObject.cpp`
- Test: `engine/tests/material_texture_tests.cpp` (crear)
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `DonTopo::Mesh`, `DonTopo::SkinnedMesh`, `DonTopo::Material` (ya existen).
- Produces:
  - `struct DonTopo::MaterialTextureOverride { int index; std::string albedo, normal, orm, baseAlbedo, baseNormal, baseOrm; };`
  - `std::vector<MaterialTextureOverride> GameObject::materialOverrides;` (miembro público, como `meshVisible`)
  - `void DonTopo::applyMaterialOverrides(GameObject& go);` (función libre, declarada en GameObject.h)
  - `std::vector<Material*> DonTopo::materialsOfMesh(GameObject& go);` (función libre: los N materiales editables del objeto, en el orden en que los indexan los overrides)

**Semántica que fija esta tarea (la usan todas las demás):**
- `materialsOfMesh` devuelve `SkinnedMesh::materials` si el objeto es skinned y ese vector no está vacío; en cualquier otro caso, un vector con un solo puntero a `Mesh::material`.
- Un override con `index` fuera de rango se ignora sin tocar nada.
- Un slot con string vacío = sin override: no se escribe nada en el material.
- La primera vez que un slot se pisa, el valor que había en el material se copia a su `base*`. Las veces siguientes NO se vuelve a copiar: el baseline es el del FBX, no el del override anterior.

- [ ] **Step 1: Escribir el test que falla**

Crear `engine/tests/material_texture_tests.cpp`:

```cpp
// Test headless de los overrides de textura del Mesh (sin GPU). Plain main +
// asserts, sin framework — mismo patrón que content_browser_tests.cpp.
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <cstdio>
#include <memory>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Estático con una textura de albedo venida del FBX.
static std::unique_ptr<GameObject> makeStaticFixture()
{
    auto go = std::make_unique<GameObject>("Estatico");
    auto mesh = std::make_shared<Mesh>();
    mesh->name = "cubo";
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    return go;
}

// Skinned con TRES materiales por submalla, cada uno con su albedo del FBX.
static std::unique_ptr<GameObject> makeSkinnedFixture()
{
    auto go = std::make_unique<GameObject>("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->name = "heroe";
    mesh->materials.resize(3);
    mesh->materials[0].texturePath = "assets/cuerpo.png";
    mesh->materials[1].texturePath = "assets/ropa.png";
    mesh->materials[2].texturePath = "assets/pelo.png";
    go->setMesh(std::move(mesh));
    return go;
}

// Un estático expone UN material: el heredado. El vector materials de un
// SkinnedMesh vacío no cuenta.
static void test_materials_of_static_mesh()
{
    auto go = makeStaticFixture();
    std::vector<Material*> mats = materialsOfMesh(*go);
    CHECK(mats.size() == 1);
    if (mats.size() == 1)
        CHECK(mats[0]->texturePath == "assets/fbx_albedo.png");
}

// Un skinned con submallas expone SUS materiales, no el heredado.
static void test_materials_of_skinned_mesh()
{
    auto go = makeSkinnedFixture();
    std::vector<Material*> mats = materialsOfMesh(*go);
    CHECK(mats.size() == 3);
    if (mats.size() == 3)
        CHECK(mats[2]->texturePath == "assets/pelo.png");
}

// Asignar pisa el material y guarda como baseline lo que traía el FBX.
static void test_override_writes_material_and_captures_baseline()
{
    auto go = makeStaticFixture();
    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");
}

// Cambiar de textura NO mueve el baseline: sigue siendo el del FBX, no el
// override intermedio. Sin esto, un Clear tras dos cambios devolvería la
// primera textura que puso el usuario en vez de la del modelo.
static void test_second_override_keeps_original_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialTextureOverride{});
    go->materialOverrides[0].index  = 0;
    go->materialOverrides[0].albedo = "assets/primera.png";
    applyMaterialOverrides(*go);

    go->materialOverrides[0].albedo = "assets/segunda.png";
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/segunda.png");
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");
}

// Clear = vaciar el override. El material vuelve al baseline capturado.
static void test_clear_restores_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialTextureOverride{});
    go->materialOverrides[0].index  = 0;
    go->materialOverrides[0].albedo = "assets/mia.png";
    applyMaterialOverrides(*go);

    go->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/fbx_albedo.png");
}

// El override de una submalla no toca a las vecinas.
static void test_skinned_override_touches_only_its_index()
{
    auto go = makeSkinnedFixture();
    MaterialTextureOverride ov;
    ov.index  = 2;
    ov.albedo = "assets/pelo_rubio.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    SkinnedMesh* sm = go->getSkinnedMesh();
    CHECK(sm->materials[2].texturePath == "assets/pelo_rubio.png");
    CHECK(sm->materials[0].texturePath == "assets/cuerpo.png");
    CHECK(sm->materials[1].texturePath == "assets/ropa.png");
}

// Índice que ya no existe (FBX reexportado con menos submallas): se ignora sin
// tocar nada y sin crash.
static void test_out_of_range_index_is_ignored()
{
    auto go = makeSkinnedFixture();
    MaterialTextureOverride ov;
    ov.index  = 7;
    ov.albedo = "assets/fantasma.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    SkinnedMesh* sm = go->getSkinnedMesh();
    CHECK(sm->materials[0].texturePath == "assets/cuerpo.png");
    CHECK(sm->materials[1].texturePath == "assets/ropa.png");
    CHECK(sm->materials[2].texturePath == "assets/pelo.png");
}

// Los tres slots son independientes: pisar el albedo no toca normal ni ORM.
static void test_slots_are_independent()
{
    auto go = makeStaticFixture();
    go->getMesh()->material.normalMapPath          = "assets/fbx_normal.png";
    go->getMesh()->material.metallicRoughnessPath  = "assets/fbx_orm.png";

    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.normalMapPath == "assets/fbx_normal.png");
    CHECK(go->getMesh()->material.metallicRoughnessPath == "assets/fbx_orm.png");
}

// Un GameObject sin mesh no revienta.
static void test_no_mesh_is_noop()
{
    GameObject go("Vacio");
    go.materialOverrides.push_back(MaterialTextureOverride{});
    applyMaterialOverrides(go);
    CHECK(materialsOfMesh(go).empty());
}

int main()
{
    test_materials_of_static_mesh();
    test_materials_of_skinned_mesh();
    test_override_writes_material_and_captures_baseline();
    test_second_override_keeps_original_baseline();
    test_clear_restores_baseline();
    test_skinned_override_touches_only_its_index();
    test_out_of_range_index_is_ignored();
    test_slots_are_independent();
    test_no_mesh_is_noop();
    if (g_failures == 0) std::printf("ALL MATERIAL TEXTURE TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

Añadir al final de `engine/tests/CMakeLists.txt` (con Edit, respetando el CRLF del fichero):

```cmake
# DonTopoEditor y no DonTopoCore: en la Task 8 este mismo binario cubre
# MaterialTextureCommand, que vive en la libreria del editor. Mismo enlace que
# dt_audio_tests, y por el mismo motivo.
add_executable(dt_material_texture_tests material_texture_tests.cpp)
target_link_libraries(dt_material_texture_tests PRIVATE DonTopoEditor)
target_compile_features(dt_material_texture_tests PRIVATE cxx_std_20)
```

- [ ] **Step 2: Compilar y ver que falla**

```powershell
.\build.bat
```

Esperado: FALLA al compilar el test — `MaterialTextureOverride`, `applyMaterialOverrides` y `materialsOfMesh` no existen.

- [ ] **Step 3: Implementar**

En `engine/include/DonTopo/Core/GameObject.h`, ANTES de `class GameObject` (necesita estar declarado para el miembro), junto a las forward declarations de `Mesh`/`SkinnedMesh`:

```cpp
    // Rutas de textura que el usuario ha puesto a mano desde Properties, por
    // encima de lo que trajera el FBX.
    //
    // Viven aqui y no en Material a proposito: Material es lo que leen los
    // uploaders de los dos backends y lo que entra en la clave de dedup de
    // SharedGpuMesh, y no sabe distinguir "esto lo puso el modelo" de "esto lo
    // puso el usuario". Sin esa distincion no hay Clear posible.
    //
    // Los base* son lo que habia en el material la PRIMERA vez que se piso ese
    // slot: es a lo que vuelve Clear en caliente. No se serializan — al cargar
    // la escena el material se re-deriva del FBX y el baseline se vuelve a
    // capturar solo.
    struct MaterialTextureOverride
    {
        int         index = 0;   // indice en SkinnedMesh::materials; 0 = Mesh::material
        std::string albedo, normal, orm;
        std::string baseAlbedo, baseNormal, baseOrm;
        // "Ya se tomo el baseline de este slot". No se puede deducir de que
        // base* este vacio: un baseline legitimamente vacio (mesh procedural
        // sin textura) seria indistinguible de "aun no tomado", y el Clear
        // dejaria puesta la textura del usuario en vez de quitarla.
        bool        baseAlbedoTaken = false, baseNormalTaken = false, baseOrmTaken = false;
    };
```

Dentro de `class GameObject`, como miembro público junto a `meshVisible`:

```cpp
            // Vacio = el material es tal cual lo trajo el FBX. Ver
            // MaterialTextureOverride.
            std::vector<MaterialTextureOverride> materialOverrides;
```

Al final del namespace en el mismo header, tras la clase:

```cpp
    // Los materiales EDITABLES de un objeto, en el orden que indexan los
    // overrides: los de submalla si es un skinned que los trae, y si no el
    // heredado de Mesh. Vacio si no hay mesh.
    //
    // Fuera de linea: el dynamic_cast necesita SkinnedMesh completo, mismo
    // motivo que isSkinned().
    std::vector<Material*> materialsOfMesh(GameObject& go);

    // Escribe los overrides sobre los materiales, capturando el baseline la
    // primera vez que pisa cada slot. Idempotente: llamarla dos veces seguidas
    // deja lo mismo.
    void applyMaterialOverrides(GameObject& go);
```

`Material` hay que declararlo arriba con los demás: `struct Material;`.

En `engine/src/Core/GameObject.cpp`, tras `getSkinnedMesh()`:

```cpp
    std::vector<Material*> materialsOfMesh(GameObject& go)
    {
        std::vector<Material*> out;
        if (!go.hasMesh()) return out;

        // Mismo criterio que materialsOf() del Content Browser: en un skinned
        // con submallas, el Material heredado no lo mira nadie.
        if (SkinnedMesh* sm = go.getSkinnedMesh(); sm && !sm->materials.empty())
        {
            out.reserve(sm->materials.size());
            for (Material& m : sm->materials) out.push_back(&m);
            return out;
        }

        out.push_back(&go.getMesh()->material);
        return out;
    }

    void applyMaterialOverrides(GameObject& go)
    {
        std::vector<Material*> mats = materialsOfMesh(go);

        for (MaterialTextureOverride& ov : go.materialOverrides)
        {
            // Indice que ya no existe: el FBX se reexporto con menos submallas.
            // Se ignora en silencio aqui; el aviso lo da el lector de escena,
            // que es quien tiene canal para darlo.
            if (ov.index < 0 || ov.index >= (int)mats.size()) continue;
            Material& mat = *mats[(size_t)ov.index];

            // El baseline se captura UNA vez por slot, la primera que se pisa:
            // si se recapturase en cada pasada, el segundo cambio de textura
            // guardaria como "original" el override anterior y el Clear
            // devolveria una textura del usuario en vez de la del modelo.
            auto aplica = [](const std::string& override_, std::string& base,
                             bool& baseTomado, std::string& destino)
            {
                if (override_.empty())
                {
                    // Sin override: si alguna vez lo hubo, se vuelve al
                    // baseline. Si nunca lo hubo, no se toca nada — escribir el
                    // base vacio aqui borraria la ruta que trae el FBX.
                    if (baseTomado) destino = base;
                    return;
                }
                if (!baseTomado)
                {
                    base       = destino;
                    baseTomado = true;
                }
                destino = override_;
            };

            // baseTomado se deriva de que el slot tenga override o baseline no
            // vacios... y eso NO basta: un baseline legitimamente vacio (mesh
            // procedural sin textura) seria indistinguible de "aun no tomado".
            // De ahi los tres flags explicitos.
            aplica(ov.albedo, ov.baseAlbedo, ov.baseAlbedoTaken, mat.texturePath);
            aplica(ov.normal, ov.baseNormal, ov.baseNormalTaken, mat.normalMapPath);
            aplica(ov.orm,    ov.baseOrm,    ov.baseOrmTaken,    mat.metallicRoughnessPath);
        }
    }
```

Y por tanto el struct del header lleva también los tres flags:

```cpp
        bool        baseAlbedoTaken = false, baseNormalTaken = false, baseOrmTaken = false;
```

- [ ] **Step 4: Compilar y ejecutar**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_material_texture_tests.exe
```

Esperado: `ALL MATERIAL TEXTURE TESTS PASSED`.

- [ ] **Step 5: Commit**

```powershell
git add engine/include/DonTopo/Core/GameObject.h engine/src/Core/GameObject.cpp engine/tests/material_texture_tests.cpp engine/tests/CMakeLists.txt
git commit -F <fichero de mensaje escrito con Write>
```

Mensaje: `feat(core): overrides de textura por material en el GameObject`.

---

### Task 2: La ruta explícita gana a la textura embebida

**Files:**
- Create: `engine/include/DonTopo/Renderer/MaterialTextureSource.h`
- Modify: `engine/src/Renderer/GpuResources.cpp:273-299`
- Modify: `engine/src/Renderer/D3D12/D3D12Renderer.cpp:2550-2562`
- Modify: `engine/src/Renderer/AsyncAssetLoader.cpp` (`decodeSlot`)
- Test: `engine/tests/material_texture_tests.cpp` (añadir casos)

**Interfaces:**
- Consumes: nada de tareas anteriores.
- Produces: `enum class DonTopo::TextureSource { None, Path, Embedded };` y
  `inline TextureSource chooseTextureSource(const std::string& path, const std::vector<uint8_t>& embedded);`

**Por qué existe el header:** la regla de precedencia se aplica en tres sitios (Vulkan, D3D12 y el decodificador del worker) y no hay ningún fichero que los tres compilen. Un `inline` en un header propio es lo que permite probarla una vez en vez de tres, y lo que impide que se desincronicen.

- [ ] **Step 1: Escribir el test que falla**

Añadir a `engine/tests/material_texture_tests.cpp` (y el `#include "DonTopo/Renderer/MaterialTextureSource.h"` arriba):

```cpp
// La ruta explicita GANA a los bytes embebidos. Es lo contrario de lo que hacia
// el motor antes de esta feature, y es lo que hace posible el Clear: si ganara
// la embebida, asignar una textura a mano exigiria destruir los bytes del FBX y
// no habria a que volver.
static void test_path_wins_over_embedded()
{
    const std::vector<uint8_t> bytes{1, 2, 3};
    CHECK(chooseTextureSource("assets/x.png", bytes) == TextureSource::Path);
}

// Sin ruta, la embebida.
static void test_embedded_when_no_path()
{
    const std::vector<uint8_t> bytes{1, 2, 3};
    CHECK(chooseTextureSource("", bytes) == TextureSource::Embedded);
}

// Sin nada, nada: el caller pone su relleno (blanca compartida en Vulkan,
// neutro global en D3D12).
static void test_none_when_empty()
{
    CHECK(chooseTextureSource("", {}) == TextureSource::None);
}

// Solo ruta.
static void test_path_when_no_embedded()
{
    CHECK(chooseTextureSource("assets/x.png", {}) == TextureSource::Path);
}
```

Registrar las cuatro en `main()`.

- [ ] **Step 2: Compilar y ver que falla**

```powershell
.\build.bat
```

Esperado: FALLA — no existe `MaterialTextureSource.h`.

- [ ] **Step 3: Implementar**

Crear `engine/include/DonTopo/Renderer/MaterialTextureSource.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    // De donde salen los pixeles de un slot de material.
    enum class TextureSource { None, Path, Embedded };

    // La RUTA gana a los bytes embebidos.
    //
    // Antes era al reves, y el motor nunca veia los dos campos llenos a la vez
    // porque ambos los rellenaba Assimp, uno u otro. Desde que Properties deja
    // poner una ruta a mano si pueden estarlo, y entonces la ruta es lo que el
    // usuario acaba de pedir: tiene que ganar. Ademas es lo que permite que
    // Clear vuelva a la embebida — si esta ganara, asignar exigiria destruirla.
    //
    // NO hay fallback de Path a Embedded si el fichero no se puede leer: una
    // ruta rota tiene que notarse (damero), no taparse con la textura del FBX.
    inline TextureSource chooseTextureSource(const std::string& path,
                                             const std::vector<uint8_t>& embedded)
    {
        if (!path.empty())     return TextureSource::Path;
        if (!embedded.empty()) return TextureSource::Embedded;
        return TextureSource::None;
    }
}
```

En `engine/src/Renderer/GpuResources.cpp`, sustituir el bloque de selección (líneas 293-299) por:

```cpp
    switch (chooseTextureSource(path, embedded)) {
        case TextureSource::Path:
            pixels  = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
            fromStb = (pixels != nullptr);
            break;
        case TextureSource::Embedded:
            pixels  = stbi_load_from_memory(embedded.data(), (int)embedded.size(), &w, &h, &channels, STBI_rgb_alpha);
            fromStb = (pixels != nullptr);
            break;
        case TextureSource::None:
            break;  // el early-return de arriba ya cubre este caso
    }
```

En `engine/src/Renderer/D3D12/D3D12Renderer.cpp:2555-2559`, el mismo switch con las mismas dos llamadas a stbi.

En `engine/src/Renderer/AsyncAssetLoader.cpp`, dentro de `decodeSlot`, la misma inversión: la ruta primero.

Los tres ficheros incluyen `"DonTopo/Renderer/MaterialTextureSource.h"`.

- [ ] **Step 4: Compilar y ejecutar**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_material_texture_tests.exe
```

Esperado: `ALL MATERIAL TEXTURE TESTS PASSED`.

- [ ] **Step 5: Commit**

Mensaje: `fix(render): la ruta de textura gana a la embebida en los tres uploaders`.

---

### Task 3: Re-clave de una entrada de SharedGpuMeshCache

**Files:**
- Modify: `engine/include/DonTopo/Renderer/SharedGpuMesh.h`
- Modify: `engine/src/Renderer/SharedGpuMesh.cpp`
- Test: `engine/tests/shared_gpu_mesh_tests.cpp`

**Interfaces:**
- Consumes: nada.
- Produces: `bool SharedGpuMeshCache::rekey(int index, const std::string& newKey);`

**Contrato:** cambia la clave con la que se encuentra la entrada `index`, sin tocar refs, ni el slot, ni los handles. Devuelve `false` si el índice no está vivo o si la clave nueva ya está ocupada por OTRA entrada (dos entradas con la misma clave dejarían el mapa apuntando a una y la otra inalcanzable, o sea una fuga). `true` si la clave nueva es la que ya tenía (no-op).

Lo necesita el camino rápido de Vulkan de la Task 4: cuando una entrada con `refs == 1` cambia de textura, su contenido deja de corresponder a su clave, y sin re-clave el siguiente objeto que pidiera la clave vieja recibiría la malla con la textura nueva.

- [ ] **Step 1: Escribir el test que falla**

Añadir a `engine/tests/shared_gpu_mesh_tests.cpp` (leer antes su `CHECK` y sus helpers, y reusarlos):

```cpp
// Tras re-clavear, la entrada se encuentra por la clave nueva y la vieja queda
// libre para una entrada distinta.
static void test_rekey_moves_the_entry()
{
    SharedGpuMeshCache cache;
    int creadas = 0;
    auto crear  = [&](SharedGpuMesh&) { ++creadas; };

    const int idx = cache.acquire("vieja", crear);
    CHECK(cache.rekey(idx, "nueva"));

    // La clave nueva devuelve la MISMA entrada, sin crear otra.
    CHECK(cache.acquire("nueva", crear) == idx);
    CHECK(creadas == 1);
    // Y la vieja ya no la encuentra: crea una entrada distinta.
    CHECK(cache.acquire("vieja", crear) != idx);
    CHECK(creadas == 2);
}

// Re-clavear a una clave que ya tiene OTRA entrada se rechaza: dejaria una de
// las dos inalcanzable en el mapa, o sea una fuga de recursos GPU.
static void test_rekey_rejects_collision()
{
    SharedGpuMeshCache cache;
    auto crear = [](SharedGpuMesh&) {};
    const int a = cache.acquire("a", crear);
    const int b = cache.acquire("b", crear);
    CHECK(a != b);
    CHECK(!cache.rekey(a, "b"));
    // Y la de a sigue encontrandose por su clave de siempre.
    CHECK(cache.acquire("a", crear) == a);
}

// Indice muerto: no hace nada y lo dice.
static void test_rekey_on_dead_index()
{
    SharedGpuMeshCache cache;
    CHECK(!cache.rekey(0, "loquesea"));
    CHECK(!cache.rekey(-1, "loquesea"));
}

// Re-clavear a la clave que ya tenia es un no-op que devuelve true.
static void test_rekey_to_same_key()
{
    SharedGpuMeshCache cache;
    auto crear = [](SharedGpuMesh&) {};
    const int idx = cache.acquire("k", crear);
    CHECK(cache.rekey(idx, "k"));
    CHECK(cache.acquire("k", crear) == idx);
}

// El refcount no lo toca: dos duenos antes, dos duenos despues.
static void test_rekey_preserves_refcount()
{
    SharedGpuMeshCache cache;
    auto crear = [](SharedGpuMesh&) {};
    const int idx = cache.acquire("k", crear);
    cache.acquire("k", crear);
    CHECK(cache.refCount(idx) == 2);
    CHECK(cache.rekey(idx, "otra"));
    CHECK(cache.refCount(idx) == 2);
}
```

Registrarlas en el `main()` de ese fichero.

- [ ] **Step 2: Compilar y ver que falla**

```powershell
.\build.bat
```

Esperado: FALLA — `rekey` no existe.

- [ ] **Step 3: Implementar**

En `SharedGpuMesh.h`, dentro de `class SharedGpuMeshCache`, tras `release`:

```cpp
            // Cambia la clave con la que se encuentra la entrada `index`, sin
            // tocar refs ni handles. false si el indice no esta vivo o si la
            // clave nueva ya es de OTRA entrada — dos entradas con la misma
            // clave dejarian una inalcanzable en el mapa, o sea una fuga de
            // recursos GPU que nadie liberaria nunca.
            //
            // La necesita el cambio de textura en caliente: cuando una entrada
            // con un solo dueno cambia de material, su contenido deja de
            // corresponder a su clave, y sin re-clavear el siguiente objeto que
            // pidiera la clave vieja recibiria la malla con la textura nueva.
            bool rekey(int index, const std::string& newKey);
```

En `SharedGpuMesh.cpp`:

```cpp
    bool SharedGpuMeshCache::rekey(int index, const std::string& newKey)
    {
        if (index < 0 || index >= (int)m_entries.size()) return false;
        Entry& e = m_entries[(size_t)index];
        if (!e.live) return false;
        if (e.key == newKey) return true;

        auto choque = m_byKey.find(newKey);
        if (choque != m_byKey.end() && choque->second != index) return false;

        m_byKey.erase(e.key);
        e.key = newKey;
        m_byKey[newKey] = index;
        return true;
    }
```

- [ ] **Step 4: Compilar y ejecutar**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_shared_gpu_mesh_tests.exe
```

Esperado: todos los tests de ese binario en verde (comprobar el nombre real del ejecutable en `engine/tests/CMakeLists.txt` antes de lanzarlo).

- [ ] **Step 5: Commit**

Mensaje: `feat(render): rekey de una entrada de SharedGpuMeshCache`.

---

### Task 4: rebuildStaticMesh en Vulkan

**Files:**
- Modify: `engine/include/DonTopo/Renderer/EditorRenderer.h` (junto a `rebuildSkinnedMesh`)
- Modify: `engine/include/DonTopo/Renderer/Renderer.h:432`
- Modify: `engine/src/Renderer/Renderer.cpp` (junto a `replaceStaticTextureWithMissing`, línea 4131)

**Interfaces:**
- Consumes: `SharedGpuMeshCache::rekey` (Task 3), `chooseTextureSource` (Task 2).
- Produces: `virtual void EditorRenderer::rebuildStaticMesh(int index, const Mesh& mesh) = 0;`

**Contrato (va en el comentario del header, es lo que lee el otro backend):** el índice de render no se mueve; transform, visibilidad y SSR se conservan; los objetos que compartían malla+material con este NO cambian de aspecto.

**Sin test automatizado:** exige un device. `EditorRenderer` son 73 virtuales puras y no hay fake en el repo (ver el comentario de `camera_tests.cpp:781`). Se verifica compilando y en GUI, como el resto del camino de GPU. Lo que sí está cubierto por test es la pieza que se puede aislar: el `rekey` de la Task 3.

- [ ] **Step 1: Declarar en la interfaz**

En `EditorRenderer.h`, junto a `rebuildSkinnedMesh`:

```cpp
            // Rehace los recursos de GPU de un mesh ESTATICO ya registrado, sin
            // moverle el indice de render: transform, visibilidad y SSR se
            // conservan, y los comandos de undo que guardan ese indice siguen
            // valiendo. Es lo que usa el cambio de textura desde Properties.
            //
            // Los objetos que compartian malla y material con este NO cambian
            // de aspecto: al cambiar el material cambia la clave de dedup, asi
            // que este objeto se separa del grupo.
            virtual void rebuildStaticMesh(int index, const Mesh& mesh) = 0;
```

En `Renderer.h:432`, junto a `rebuildSkinnedMesh`: `void rebuildStaticMesh(int index, const Mesh& mesh);`.

- [ ] **Step 2: Implementar en Renderer.cpp**

Tras `replaceStaticTextureWithMissing`. Reutiliza sus mismas cautelas, que ya están comentadas allí y son las que hay que respetar: encolar los handles viejos en `m_deferredDeletes` (un command buffer en vuelo aún los referencia), preguntar `m_res.isSharedPlaceholder(oldImage)` ANTES de encolar (una imagen de relleno prestada no es de esta malla y destruirla se lleva las de todas), no destruir el sampler (es el compartido), y `vkDeviceWaitIdle` antes de escribir los descriptor sets (no piden `UPDATE_AFTER_BIND`).

```cpp
    void Renderer::rebuildStaticMesh(int index, const Mesh& mesh)
    {
        if (index < 0 || index >= (int)m_objects.size()) return;
        RenderObject& obj = m_objects[index];

        const std::string nuevaClave = makeSharedMeshKey(mesh);
        const int         viejo      = obj.sharedIndex;

        SharedGpuMesh* gpuPtr = m_sharedMeshes.get(viejo);
        if (!gpuPtr) return;

        // Camino LENTO: la entrada tiene mas de un dueno, asi que este objeto se
        // separa del grupo. No se puede mutar in situ — cambiaria la textura de
        // los demas, que no han pedido nada.
        if (m_sharedMeshes.refCount(viejo) > 1)
        {
            if (!m_pendingBatch)
                m_pendingBatch = std::make_unique<TransferBatch>(m_gpu);

            bool creada = false;
            const int nuevo = m_sharedMeshes.acquire(
                nuevaClave,
                [&](SharedGpuMesh& gpu) { createSharedGpuMesh(mesh, gpu, m_pendingBatch.get(), nullptr); },
                &creada);

            m_sharedMeshes.release(viejo, [this](const SharedGpuMesh& gpu) {
                destroySharedGpuMesh(gpu);
            });
            obj.sharedIndex = nuevo;
            // Descriptor sets de la entrada recien creada: los aloja el mismo
            // camino que usa addStaticMesh cuando `created` sale true.
            if (creada)
                createDescriptorSetsFor(nuevo);
            return;
        }

        // Camino RAPIDO: dueno unico. Se sustituyen las tres imagenes en su
        // sitio y se reescriben los descriptores, sin re-subir geometria.
        SharedGpuMesh& gpu = *gpuPtr;

        struct SlotRefs { VkImage* img; VkDeviceMemory* mem; VkImageView* view;
                          VkSampler* sampler; uint32_t binding; };
        const SlotRefs slots[3] = {
            { &gpu.textureImage, &gpu.textureMem, &gpu.textureView, &gpu.sampler,       1 },
            { &gpu.normalImage,  &gpu.normalMem,  &gpu.normalView,  &gpu.normalSampler, 2 },
            { &gpu.ormImage,     &gpu.ormMem,     &gpu.ormView,     &gpu.ormSampler,    4 },
        };

        for (const SlotRefs& s : slots)
        {
            // Los tres handles viejos siguen referenciados por un descriptor set
            // que un command buffer en vuelo puede tener bindeado: se encolan
            // POR VALOR (capturar los punteros seria leer los nuevos cuando el
            // lambda corriera). El sampler no entra: es el compartido.
            const VkImage        oldImage = *s.img;
            const VkDeviceMemory oldMem   = *s.mem;
            const VkImageView    oldView  = *s.view;
            const bool           prestada = m_res.isSharedPlaceholder(oldImage);
            m_deferredDeletes.push([oldImage, oldMem, oldView, prestada](VkDevice dev) {
                vkDestroyImageView(dev, oldView, nullptr);
                if (prestada) return;
                vkDestroyImage(dev, oldImage, nullptr);
                vkFreeMemory(dev,   oldMem,   nullptr);
            });
        }

        // Las tres imagenes nuevas por el mismo camino que createSharedGpuMesh,
        // con los mismos formatos: SRGB en el color, UNORM en normal y ORM.
        m_res.createTextureImage(mesh.material.texturePath, mesh.material.embeddedTexture,
                                 gpu.textureImage, gpu.textureMem);
        m_res.createTextureImageView(gpu.textureImage, gpu.textureView);
        gpu.sampler = m_res.sharedMaterialSampler();

        m_res.createNormalMapImage(mesh.material.normalMapPath, mesh.material.embeddedNormalMap,
                                   gpu.normalImage, gpu.normalMem);
        m_res.createTextureImageView(gpu.normalImage, gpu.normalView, VK_FORMAT_R8G8B8A8_UNORM);
        gpu.normalSampler = m_res.sharedMaterialSampler();

        if (chooseTextureSource(mesh.material.metallicRoughnessPath,
                                mesh.material.embeddedMetallicRoughness) != TextureSource::None)
        {
            m_res.createNormalMapImage(mesh.material.metallicRoughnessPath,
                                       mesh.material.embeddedMetallicRoughness,
                                       gpu.ormImage, gpu.ormMem);
            gpu.metallic  = 1.0f;
            gpu.roughness = 1.0f;
        }
        else
        {
            m_res.sharedWhiteOrm(gpu.ormImage, gpu.ormMem);
            gpu.metallic  = mesh.material.metallic;
            gpu.roughness = mesh.material.roughness;
        }
        m_res.createTextureImageView(gpu.ormImage, gpu.ormView, VK_FORMAT_R8G8B8A8_UNORM);
        gpu.ormSampler = m_res.sharedMaterialSampler();

        // El wait protege la ESCRITURA del set, no la creacion de las imagenes.
        vkDeviceWaitIdle(m_gpu.device());

        for (int i = 0; i < MAX_FRAMES; i++)
            for (const SlotRefs& s : slots)
            {
                VkDescriptorImageInfo imageInfo{};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView   = *s.view;
                imageInfo.sampler     = *s.sampler;

                VkWriteDescriptorSet write{};
                write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet          = gpu.descriptorSets[i];
                write.dstBinding      = s.binding;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.descriptorCount = 1;
                write.pImageInfo      = &imageInfo;
                vkUpdateDescriptorSets(m_gpu.device(), 1, &write, 0, nullptr);
            }

        // Lo ultimo: el contenido ya no corresponde a la clave vieja. Si el
        // rekey se rechaza (colision con una entrada que ya tiene esta clave
        // exacta), se suelta esta y se adquiere aquella — dejar dos entradas
        // con la misma clave seria una fuga.
        if (!m_sharedMeshes.rekey(viejo, nuevaClave))
        {
            bool creada = false;
            const int nuevo = m_sharedMeshes.acquire(
                nuevaClave,
                [&](SharedGpuMesh& g) { createSharedGpuMesh(mesh, g, nullptr, nullptr); },
                &creada);
            m_sharedMeshes.release(viejo, [this](const SharedGpuMesh& g) { destroySharedGpuMesh(g); });
            obj.sharedIndex = nuevo;
            if (creada) createDescriptorSetsFor(nuevo);
        }
    }
```

**Nota para el implementador:** `createDescriptorSetsFor(int sharedIndex)` es el nombre que se usa arriba para el camino que ya existe dentro de `addStaticMesh` cuando `created` sale true. Antes de escribir esto, leer `Renderer::addStaticMesh` (Renderer.cpp:3292 en adelante) y llamar **exactamente** a lo que llame él; si ese camino está inline dentro de `addStaticMesh`, extraerlo a un método privado con este nombre y llamarlo desde los dos sitios. Mismo criterio con `m_deferredDeletes.push` y con la firma de `createTextureImage` sin `TransferBatch` (la versión síncrona), que es la que usa `replaceStaticTextureWithMissing`.

- [ ] **Step 3: Compilar**

```powershell
.\build.bat
```

Esperado: compila. La suite entera debe seguir en verde:

```powershell
Get-ChildItem build-ninja\engine\tests\dt_*_tests.exe | ForEach-Object { & $_.FullName }
```

- [ ] **Step 4: Commit**

Mensaje: `feat(render): rebuildStaticMesh en Vulkan`.

---

### Task 5: rebuildStaticMesh en D3D12

**Files:**
- Modify: `engine/include/DonTopo/Renderer/D3D12/D3D12Renderer.h:170` (junto a `rebuildSkinnedMesh`)
- Modify: `engine/src/Renderer/D3D12/D3D12Renderer.cpp` (junto a `replaceStaticTextureWithMissing`, línea 10104)

**Interfaces:**
- Consumes: `rebuildStaticMesh` declarada en `EditorRenderer` (Task 4), `chooseTextureSource` (Task 2).
- Produces: nada nuevo.

**Aquí no hay entrada compartida que re-clavear:** cada objeto tiene su bloque propio de descriptores (`srvBase`) y sus propias allocations, así que el cambio es siempre in situ y nunca toca geometría.

- [ ] **Step 1: Implementar**

```cpp
void D3D12Renderer::rebuildStaticMesh(int index, const Mesh& mesh)
{
    Impl& d = *m_impl;
    if (index < 0 || index >= static_cast<int>(d.objects.size()))
        return;

    Impl::StaticObject& object = d.objects[index];
    if (object.srvBase == kSrvBaseColor)
        return;  // sin bloque propio: dibuja con los neutros globales

    // Su bloque deja de decir lo mismo que el de los que comparten esta malla,
    // asi que deja de poder compartir draw con ellos.
    ++object.materialVariant;
    d.drawGroupsDirty = true;

    // Los recursos viejos pueden estar en uso por el ultimo frame presentado.
    d.waitForGpu();

    // Soltar SOLO lo que es de este objeto. Una allocation que sea del dueno de
    // una malla compartida no se toca aqui: la comparticion se decide en
    // addStaticMesh (rama `reusa`), donde este objeto copio los punteros del
    // dueno sin quedarse con su propiedad.
    auto sueltaSiPropia = [&](D3D12MA::Allocation*& propia) {
        if (propia && object.ownsGpu)
            propia->Release();
        propia = nullptr;
    };

    sueltaSiPropia(object.baseColorAllocation);
    sueltaSiPropia(object.normalMapAllocation);
    sueltaSiPropia(object.metalRoughAllocation);

    const UINT slot = object.srvBase;

    object.baseColorAllocation = d.uploadMaterialTexture(
        mesh.material.texturePath, mesh.material.embeddedTexture, true, slot + 0);
    if (!object.baseColorAllocation) {
        // La pidio y no se pudo leer: damero, que se note. No la pidio: blanco.
        const bool sePidio = chooseTextureSource(mesh.material.texturePath,
                                                 mesh.material.embeddedTexture) != TextureSource::None;
        ID3D12Resource* relleno = (sePidio && d.missingTextureAllocation)
                                      ? d.missingTextureAllocation->GetResource()
                                      : d.baseColorAllocation->GetResource();
        d.createTexture2DSrv(relleno, DXGI_FORMAT_R8G8B8A8_UNORM, slot + 0);
    }

    object.normalMapAllocation = d.uploadMaterialTexture(
        mesh.material.normalMapPath, mesh.material.embeddedNormalMap, false, slot + 1);
    if (!object.normalMapAllocation)
        d.createTexture2DSrv(d.normalMapAllocation->GetResource(),
                             DXGI_FORMAT_R8G8B8A8_UNORM, slot + 1);

    object.metalRoughAllocation = d.uploadMaterialTexture(
        mesh.material.metallicRoughnessPath, mesh.material.embeddedMetallicRoughness,
        false, slot + 3);
    if (!object.metalRoughAllocation)
        d.createTexture2DSrv(d.metalRoughAllocation->GetResource(),
                             DXGI_FORMAT_R8G8B8A8_UNORM, slot + 3);

    object.metallic  = mesh.material.metallic;
    object.roughness = mesh.material.roughness;
}
```

**Nota para el implementador — verificar antes de escribir, no asumir:**
1. Leer `addStaticMesh` (D3D12Renderer.cpp:9686-9745) y confirmar qué significa exactamente `ownsGpu` y si cubre también las allocations de textura o solo los buffers de geometría. Si la propiedad de las texturas se decide con otro campo, usar ese. Si NO hay ningún campo que lo diga, **parar y preguntar**: soltar una allocation prestada es un doble free mudo, y aquí ya ha pasado antes.
2. Confirmar los offsets del bloque: color en `+0`, normal en `+1`, ORM en `+3` (t3..t7 los rellena `fillSharedSlots`, y el ORM propio pisa el neutro que deja).
3. Confirmar que `materialVariant` existe como campo de `StaticObject` (lo usa `replaceStaticTextureWithMissing`).

- [ ] **Step 2: Compilar y pasar la suite**

```powershell
.\build.bat
Get-ChildItem build-ninja\engine\tests\dt_*_tests.exe | ForEach-Object { & $_.FullName }
```

- [ ] **Step 3: Commit**

Mensaje: `feat(render): rebuildStaticMesh en D3D12`.

---

### Task 6: Serialización de los overrides con rutas relativas

**Files:**
- Modify: `engine/include/DonTopo/Core/Scene.h`
- Modify: `engine/src/Core/Scene.cpp` (`nodeToJson`:670-706, `nodeFromJson`:1472+)
- Modify: `engine/src/Editor/EditorUI.cpp:619` (fijar la raíz al aplicar el proyecto)
- Test: `engine/tests/material_texture_tests.cpp`

**Interfaces:**
- Consumes: `MaterialTextureOverride`, `GameObject::materialOverrides` (Task 1).
- Produces:
  - `void Scene::setAssetRoot(std::string root);` y `const std::string& Scene::assetRoot() const;`
  - Bloque JSON `mesh.materials`: array de `{ "index": int, "albedo": str?, "normal": str?, "orm": str? }`.

`nodeToJson` y `nodeFromJson` son funciones libres en el anónimo de Scene.cpp: hay que pasarles la raíz como parámetro nuevo (`const std::string& assetRoot`), y los tres callers de `nodeFromJson` (`fromJson`, `insertFromJson`, `cloneGameObject`) le pasan `m_assetRoot`.

- [ ] **Step 1: Escribir el test que falla**

Añadir a `engine/tests/material_texture_tests.cpp` (con `#include "DonTopo/Core/Scene.h"`, `PhysicsManager`, `AudioManager` y `<filesystem>`; copiar el arranque de esos dos managers de `engine/tests/audio_tests.cpp`, que ya los levanta):

```cpp
// Round-trip: los overrides de TODOS los indices sobreviven a guardar y cargar.
static void test_overrides_survive_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->sourcePath = "assets/hero.fbx";
    mesh->materials.resize(3);
    go->setMesh(std::move(mesh));

    MaterialTextureOverride a; a.index = 0; a.albedo = "assets/cuerpo.png";
    MaterialTextureOverride b; b.index = 2; b.albedo = "assets/pelo.png";
                               b.normal = "assets/pelo_n.png";
    go->materialOverrides = {a, b};

    const nlohmann::json j = scene.toJson();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Personaje") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 2);
    if (leido->materialOverrides.size() != 2) return;
    CHECK(leido->materialOverrides[0].index  == 0);
    CHECK(leido->materialOverrides[0].albedo == "assets/cuerpo.png");
    CHECK(leido->materialOverrides[1].index  == 2);
    CHECK(leido->materialOverrides[1].normal == "assets/pelo_n.png");
    // El baseline NO viaja: se recaptura al aplicar sobre el material recien
    // derivado del FBX.
    CHECK(leido->materialOverrides[0].baseAlbedo.empty());
}

// Un objeto sin overrides no escribe la clave: las escenas viejas y las nuevas
// sin texturas tocadas son byte a byte iguales.
static void test_no_overrides_writes_no_key()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));

    const nlohmann::json j = scene.toJson();
    // El nodo raiz cuelga de "root"; localizar el hijo por nombre en vez de
    // asumir el indice.
    CHECK(!j.dump().empty());
    CHECK(j.dump().find("\"materials\"") == std::string::npos);
}

// Escena SIN la clave materials: carga exactamente igual que hoy, sin overrides
// y sin avisos.
static void test_scene_without_materials_key_loads_clean(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    nlohmann::json j = scene.toJson();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido) CHECK(leido->materialOverrides.empty());
}

// Con raiz de proyecto fijada, una ruta bajo ella se guarda RELATIVA con "/".
static void test_path_under_root_is_stored_relative(PhysicsManager& pm, AudioManager& am)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dt_mat_root";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = (root / "assets" / "x.png").string();
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("assets/x.png") != std::string::npos);
    CHECK(texto.find(root.string()) == std::string::npos);

    // Y al leerla con la misma raiz vuelve absoluta.
    Scene cargada("Vacia");
    cargada.setAssetRoot(root.string());
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido && leido->materialOverrides.size() == 1)
        CHECK(fs::path(leido->materialOverrides[0].albedo) == (root / "assets" / "x.png"));
}

// Una ruta FUERA de la raiz se guarda absoluta tal cual: relativizarla daria
// una ristra de ".." que no sobrevive a mover el proyecto.
static void test_path_outside_root_stays_absolute()
{
    namespace fs = std::filesystem;
    const fs::path root  = fs::temp_directory_path() / "dt_mat_root";
    const fs::path fuera = fs::temp_directory_path() / "dt_otro" / "y.png";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = fuera.string();
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("y.png") != std::string::npos);
    CHECK(texto.find("..") == std::string::npos);
}

// Sin raiz fijada (tests, runtime headless), la ruta va y vuelve IDENTICA.
static void test_without_root_path_is_verbatim(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/tal/cual.png";
    go->materialOverrides.push_back(ov);

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    if (leido && leido->materialOverrides.size() == 1)
        CHECK(leido->materialOverrides[0].albedo == "assets/tal/cual.png");
}

// Un bloque "materials" corrupto no tumba la carga: avisa y sigue.
static void test_corrupt_materials_block_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    nlohmann::json j = scene.toJson();

    // Inyectar basura donde iria el bloque: un objeto en vez de un array.
    // Localizar el nodo del cubo recorriendo el JSON por nombre.
    // (El implementador escribe aqui el recorrido concreto segun la forma real
    // de toJson(): raiz -> "children" -> el que tenga "name" == "Cubo".)

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    CHECK(!cargada.lastWarnings().empty());
}
```

- [ ] **Step 2: Compilar y ver que falla**

```powershell
.\build.bat
```

Esperado: FALLA — `setAssetRoot` no existe.

- [ ] **Step 3: Implementar**

En `Scene.h`, público:

```cpp
            // Raiz del proyecto contra la que se relativizan las rutas de
            // textura al guardar y se resuelven al cargar. Vacia = las rutas van
            // y vuelven tal cual, que es lo que hacen los tests y cualquier
            // caller que no la fije (su directorio de trabajo ya es la raiz).
            //
            // Vive aqui y no se saca de ProjectContext porque Scene esta en
            // Core, y Core no puede depender del Editor.
            void setAssetRoot(std::string root) { m_assetRoot = std::move(root); }
            const std::string& assetRoot() const { return m_assetRoot; }
```

Privado: `std::string m_assetRoot;`.

En `Scene.cpp`, dos helpers en el anónimo, junto a los demás:

```cpp
    // Ruta que va al fichero: relativa con "/" si cae bajo la raiz, y absoluta
    // tal cual si no. Fuera de la raiz una relativa seria una ristra de ".."
    // que no sobrevive a mover el proyecto de sitio.
    std::string toStoredPath(const std::string& path, const std::string& assetRoot)
    {
        if (path.empty() || assetRoot.empty()) return path;
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(path, assetRoot, ec);
        if (ec || rel.empty() || *rel.begin() == "..") return path;
        return rel.generic_string();
    }

    // La inversa. Una ruta ya absoluta se devuelve tal cual.
    std::string fromStoredPath(const std::string& stored, const std::string& assetRoot)
    {
        if (stored.empty() || assetRoot.empty()) return stored;
        std::filesystem::path p(stored);
        if (p.is_absolute()) return stored;
        return (std::filesystem::path(assetRoot) / p).string();
    }
```

En `nodeToJson`, dentro del `if (node.hasMesh())` y antes de `j["mesh"] = std::move(meshJson);`:

```cpp
            // Rutas de textura puestas a mano desde Properties. Solo los
            // materiales con algo que decir, y dentro de cada uno solo las
            // claves no vacias: un objeto sin overrides no escribe la clave, y
            // las escenas viejas siguen siendo validas sin tocarlas.
            nlohmann::json mats = nlohmann::json::array();
            for (const MaterialTextureOverride& ov : node.materialOverrides)
            {
                if (ov.albedo.empty() && ov.normal.empty() && ov.orm.empty()) continue;
                nlohmann::json entry = { {"index", ov.index} };
                if (!ov.albedo.empty()) entry["albedo"] = toStoredPath(ov.albedo, assetRoot);
                if (!ov.normal.empty()) entry["normal"] = toStoredPath(ov.normal, assetRoot);
                if (!ov.orm.empty())    entry["orm"]    = toStoredPath(ov.orm,    assetRoot);
                mats.push_back(std::move(entry));
            }
            if (!mats.empty())
                meshJson["materials"] = std::move(mats);
```

En `nodeFromJson`, dentro de la rama que ya lee `j["mesh"]`, tras resolver `sourcePath`:

```cpp
            // Overrides de textura. Un bloque que no sea array, o una entrada
            // sin "index" numerico, se descarta con aviso: media configuracion
            // es peor que ninguna, mismo criterio que jsonToMat4 con la matriz.
            if (j["mesh"].contains("materials"))
            {
                const nlohmann::json& mats = j["mesh"]["materials"];
                if (!mats.is_array())
                {
                    if (warnings)
                        warnings->push_back("mesh de '" + node->name + "'.materials: no es una lista, "
                                            "las texturas asignadas a mano se descartan");
                }
                else
                {
                    for (const auto& entry : mats)
                    {
                        if (!entry.is_object() || !entry.contains("index")
                            || !entry["index"].is_number_integer())
                        {
                            if (warnings)
                                warnings->push_back("mesh de '" + node->name + "'.materials: entrada sin "
                                                    "index valido, se descarta");
                            continue;
                        }
                        MaterialTextureOverride ov;
                        ov.index  = entry["index"].get<int>();
                        ov.albedo = fromStoredPath(entry.value("albedo", ""), assetRoot);
                        ov.normal = fromStoredPath(entry.value("normal", ""), assetRoot);
                        ov.orm    = fromStoredPath(entry.value("orm",    ""), assetRoot);
                        node->materialOverrides.push_back(std::move(ov));
                    }
                }
            }
```

En `EditorUI.cpp:619`, donde se aplica el proyecto y ya se sabe que es válido, fijar la raíz en la escena viva:

```cpp
    if (m_scene)
        m_scene->setAssetRoot(m_project->root().string());
```

- [ ] **Step 4: Compilar y ejecutar**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_material_texture_tests.exe
```

- [ ] **Step 5: Commit**

Mensaje: `feat(scene): serializa las rutas de textura del material, relativas al proyecto`.

---

### Task 7: Aplicar los overrides en las rutas de carga

**Files:**
- Modify: `engine/src/Renderer/AsyncAssetLoader.cpp` (`applyLoadedMesh`:312-352)
- Modify: `engine/src/Core/Scene.cpp` (`nodeFromJson`, tras cada `node->setMesh(...)`)
- Test: `engine/tests/material_texture_tests.cpp`

**Interfaces:**
- Consumes: `applyMaterialOverrides` (Task 1), `materialOverrides` cargados (Task 6).
- Produces: nada nuevo.

**El orden importa:** en `applyLoadedMesh` la llamada va **después de `setMesh` pero antes de registrar en el renderer**. La malla que llega trae el material del FBX y hay que pisarlo antes de subirlo a GPU; registrar primero subiría la textura del modelo y la del usuario no se vería hasta el siguiente rebuild.

Hoy `applyLoadedMesh` registra antes de `setMesh` (a propósito: si el registro lanza, el GameObject queda intacto). Se conserva ese orden y se aplica el override sobre `*r.mesh` ANTES del registro, sin depender de `setMesh`.

- [ ] **Step 1: Escribir el test que falla**

```cpp
// El override se aplica sobre la malla que llega, no despues: si se aplicara
// tras registrar en el renderer, la GPU subiria la textura del FBX y la del
// usuario no se veria hasta el siguiente rebuild.
static void test_overrides_applied_to_incoming_mesh()
{
    auto go = makeStaticFixture();
    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    // La malla que "llega" trae lo del FBX.
    auto llegada = std::make_shared<Mesh>();
    llegada->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(llegada);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");
}
```

(La punta del hilo —que `applyLoadedMesh` llame a esto en el orden correcto— no se puede probar sin un `EditorRenderer`, que son 73 virtuales puras: se verifica en GUI, igual que `test_remove_notifies_listener` en camera_tests.cpp.)

- [ ] **Step 2: Compilar y ejecutar el test**

Debe PASAR ya con lo de la Task 1: es el mecanismo. Sirve de red para el paso siguiente.

- [ ] **Step 3: Implementar**

En `applyLoadedMesh`, dentro del `try`, antes de las llamadas a `addSkinnedMesh`/`addStaticMesh`:

```cpp
            // Las rutas que el usuario puso a mano pisan lo que trae el FBX, y
            // tienen que hacerlo ANTES de subir: registrar primero subiria la
            // textura del modelo y la del usuario no se veria hasta el siguiente
            // rebuild. setMesh sigue yendo despues del registro, como estaba.
            target->setMesh(r.mesh);
            applyMaterialOverrides(*target);
```

y reordenar el bloque para que quede: `setMesh` → `applyMaterialOverrides` → registro. Si el registro lanza, se deshace el `setMesh` (`target->setMesh(nullptr)`) para conservar la garantía que documenta el comentario de esa función: el GameObject queda intacto y el reintento funciona.

En `Scene.cpp`, `nodeFromJson`: tras cada `node->setMesh(...)` de la rama de mesh (son tres: skinned, cacheada/estática y procedural), una sola llamada al final de la rama:

```cpp
                // Los overrides ya se leyeron arriba: se aplican con la malla
                // puesta, sea cual sea el camino por el que haya llegado. En el
                // camino ASINCRONO no hay malla todavia y no se aplica aqui —
                // lo hace applyLoadedMesh cuando el worker entregue.
                if (node->hasMesh())
                    applyMaterialOverrides(*node);
```

- [ ] **Step 4: Compilar y pasar la suite entera**

```powershell
.\build.bat
Get-ChildItem build-ninja\engine\tests\dt_*_tests.exe | ForEach-Object { & $_.FullName }
```

- [ ] **Step 5: Commit**

Mensaje: `feat(core): aplica los overrides de textura al cargar la malla`.

---

### Task 8: Undo/redo del cambio de textura

**Files:**
- Modify: `engine/include/DonTopo/Editor/Command.h`
- Modify: `engine/src/Editor/Command.cpp`
- Test: `engine/tests/material_texture_tests.cpp`

**Interfaces:**
- Consumes: `applyMaterialOverrides` (Task 1), `rebuildStaticMesh` (Tasks 4-5), `Scene::findById`.
- Produces:
  - `enum class DonTopo::MaterialTextureSlot { Albedo, Normal, Orm };`
  - `class MaterialTextureCommand : public ICommand;` con constructor
    `MaterialTextureCommand(Scene& scene, EditorRenderer* renderer, std::string label, uint64_t id, int materialIndex, MaterialTextureSlot slot, std::string before, std::string after);`
  - `void DonTopo::setMaterialTextureOverride(GameObject& go, int materialIndex, MaterialTextureSlot slot, const std::string& path);` — helper que crea la entrada si no existe, escribe el slot y llama a `applyMaterialOverrides`. Lo usan el comando y el panel: es lo olvidable metido dentro de lo que no se olvida.

`EditorRenderer* renderer` es **puntero**, nullable, exactamente como `AnimationSourceCommand::m_renderer` (Command.h:684). Es lo que hace el comando instanciable en un test sin GPU.

- [ ] **Step 1: Escribir el test que falla**

```cpp
// Undo/redo de una asignacion, con el renderer a nullptr (sin GPU): lo que se
// prueba es el dato, que es lo unico que sobrevive al ciclo.
static void test_command_undo_redo_assignment(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialTextureCommand cmd(scene, nullptr, "Textura de 'Cubo'", id, 0,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");
    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");

    cmd.undo();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/fbx_albedo.png");

    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");
    (void)pm; (void)am;
}

// Undo de un Clear: vuelve a poner la ruta que el usuario habia asignado.
static void test_command_undo_of_clear(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    setMaterialTextureOverride(*go, 0, MaterialTextureSlot::Albedo, "assets/mia.png");

    MaterialTextureCommand clear(scene, nullptr, "Quitar textura", id, 0,
                                  MaterialTextureSlot::Albedo, "assets/mia.png", "");
    clear.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/fbx_albedo.png");

    clear.undo();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");
    (void)pm; (void)am;
}

// El comando resuelve por id en CADA aplicacion: un puntero guardado quedaria
// colgando tras un undo de Delete que reconstruya el objeto.
static void test_command_survives_object_rebuild(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialTextureCommand cmd(scene, nullptr, "Textura", id, 0,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");

    // El objeto desaparece: el comando no puede reventar ni escribir en memoria
    // liberada, solo no hacer nada.
    scene.removeGameObject(scene.findById(id));
    cmd.execute();
    CHECK(scene.findById(id) == nullptr);
    (void)pm; (void)am;
}
```

- [ ] **Step 2: Compilar y ver que falla**

Esperado: FALLA — no existen `MaterialTextureCommand` ni `setMaterialTextureOverride`.

- [ ] **Step 3: Implementar**

En `Command.h`, junto a los demás comandos de componente:

```cpp
enum class MaterialTextureSlot { Albedo, Normal, Orm };

// Escribe UNA ruta de textura en el override del material `materialIndex` y la
// aplica al Material. Crea la entrada del override si no existe.
//
// Existe para que "escribir el override" y "aplicarlo" no puedan ir por
// separado: son dos pasos, se olvidaria el segundo, y el sintoma seria que el
// panel enseña la ruta nueva y el viewport la vieja.
void setMaterialTextureOverride(GameObject& go, int materialIndex,
                                 MaterialTextureSlot slot, const std::string& path);

// Cambio de UNA textura de UN material, por el stack de undo.
//
// El renderer es PUNTERO y puede ser nullptr (tests headless): mismo patron que
// AnimationSourceCommand. Resuelve el GameObject por id en cada aplicacion,
// nunca por puntero, para sobrevivir a un undo de Delete que lo reconstruya.
class MaterialTextureCommand : public ICommand {
public:
    MaterialTextureCommand(Scene& scene, EditorRenderer* renderer, std::string label,
                            uint64_t id, int materialIndex, MaterialTextureSlot slot,
                            std::string before, std::string after);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(const std::string& path);

    Scene&              m_scene;
    EditorRenderer*     m_renderer;
    std::string         m_label;
    uint64_t            m_id;
    int                 m_materialIndex;
    MaterialTextureSlot m_slot;
    std::string         m_before;
    std::string         m_after;
};
```

En `Command.cpp`:

```cpp
void setMaterialTextureOverride(GameObject& go, int materialIndex,
                                 MaterialTextureSlot slot, const std::string& path)
{
    MaterialTextureOverride* ov = nullptr;
    for (auto& candidato : go.materialOverrides)
        if (candidato.index == materialIndex) { ov = &candidato; break; }

    if (!ov)
    {
        MaterialTextureOverride nuevo;
        nuevo.index = materialIndex;
        go.materialOverrides.push_back(nuevo);
        ov = &go.materialOverrides.back();
    }

    switch (slot)
    {
        case MaterialTextureSlot::Albedo: ov->albedo = path; break;
        case MaterialTextureSlot::Normal: ov->normal = path; break;
        case MaterialTextureSlot::Orm:    ov->orm    = path; break;
    }

    applyMaterialOverrides(go);
}

MaterialTextureCommand::MaterialTextureCommand(Scene& scene, EditorRenderer* renderer,
                                                std::string label, uint64_t id,
                                                int materialIndex, MaterialTextureSlot slot,
                                                std::string before, std::string after)
    : m_scene(scene), m_renderer(renderer), m_label(std::move(label)), m_id(id),
      m_materialIndex(materialIndex), m_slot(slot),
      m_before(std::move(before)), m_after(std::move(after)) {}

void MaterialTextureCommand::execute() { apply(m_after); }
void MaterialTextureCommand::undo()    { apply(m_before); }

void MaterialTextureCommand::apply(const std::string& path)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go || !go->hasMesh()) return;

    setMaterialTextureOverride(*go, m_materialIndex, m_slot, path);

    if (!m_renderer) return;

    // Skinned y estatico van por caminos distintos porque los recursos de GPU
    // lo son: el personaje se reconstruye entero (es lo unico que hay), el
    // estatico solo cambia de material.
    if (SkinnedMesh* sm = go->getSkinnedMesh(); sm && go->skinnedRenderIndex >= 0)
        m_renderer->rebuildSkinnedMesh(go->skinnedRenderIndex, *sm);
    else if (go->staticRenderIndex >= 0)
        m_renderer->rebuildStaticMesh(go->staticRenderIndex, *go->getMesh());
}
```

- [ ] **Step 4: Compilar y ejecutar**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_material_texture_tests.exe
```

- [ ] **Step 5: Commit**

Mensaje: `feat(editor): undo/redo del cambio de textura de material`.

---

### Task 9: Sección Textures en el panel Properties

**Files:**
- Modify: `engine/include/DonTopo/Editor/PropertiesPanel.h`
- Modify: `engine/src/Editor/PropertiesPanel.cpp` (`drawMeshSection`, ~7734-7800)

**Interfaces:**
- Consumes: `materialsOfMesh` (Task 1), `MaterialTextureCommand` y `setMaterialTextureOverride` (Task 8), `drawAssetDropBox` (ya existe, PropertiesPanel.cpp:347).
- Produces: `void PropertiesPanel::drawTexturesSection(EditorContext& ctx);` (privado) y `void PropertiesPanel::assignMaterialTexture(EditorContext& ctx, int materialIndex, MaterialTextureSlot slot, const std::string& path);` (privado; `path` vacío = Clear).

**Sin test automatizado:** es ImGui dentro de un frame. Se verifica en GUI. Lo que sí está probado es todo lo que hay debajo (override, comando, serialización).

- [ ] **Step 1: Declarar en el header**

En `PropertiesPanel.h`, junto a `drawMeshSection` (línea 116):

```cpp
    // Las tres texturas de cada material del mesh. Va DENTRO de la seccion
    // Mesh, sin Add-gate propio: no es un componente nuevo, es parte del que ya
    // esta puesto.
    void drawTexturesSection(EditorContext& ctx);
    // path vacio = Clear. Un solo sitio del que salen las seis llamadas
    // (tres slots x drop y browse) y el unico que apila el comando.
    void assignMaterialTexture(EditorContext& ctx, int materialIndex,
                                MaterialTextureSlot slot, const std::string& path);
```

Miembros nuevos, junto a `m_meshLoadError`:

```cpp
    std::string m_textureLoadError;
    // Dialogo de Browse por slot: hay que saber a que material y a que slot
    // vuelve el resultado cuando el modal se cierre, varios frames despues.
    bool                m_textureDlgOpen      = false;
    int                 m_textureDlgMaterial  = 0;
    MaterialTextureSlot m_textureDlgSlot      = MaterialTextureSlot::Albedo;
```

- [ ] **Step 2: Implementar la sección**

En `drawMeshSection`, dentro del `if (sectionOpen)`, tras el checkbox `Visible` y antes del `ImGui::TreePop()`:

```cpp
            drawTexturesSection(ctx);
```

Y la función nueva:

```cpp
void PropertiesPanel::drawTexturesSection(EditorContext& ctx)
{
    if (!ctx.selected || !ctx.selected->hasMesh()) return;

    std::vector<Material*> mats = materialsOfMesh(*ctx.selected);
    if (mats.empty()) return;

    if (!ImGui::CollapsingHeader("Textures")) return;

    struct SlotDesc { const char* nombre; MaterialTextureSlot slot; };
    static const SlotDesc kSlots[3] = {
        { "Albedo",             MaterialTextureSlot::Albedo },
        { "Normal Map",         MaterialTextureSlot::Normal },
        { "Metallic/Roughness", MaterialTextureSlot::Orm    },
    };

    for (int m = 0; m < (int)mats.size(); ++m)
    {
        // PushID por material: CollapsingHeader NO abre scope de ID propio, asi
        // que sin esto los tres slots del material 0 y los del 1 colisionan
        // entre si y el drop de uno se lo come el otro.
        ImGui::PushID(m);
        if (mats.size() > 1)
            ImGui::Text("Material %d", m);

        for (const SlotDesc& d : kSlots)
        {
            ImGui::PushID(d.nombre);

            const std::string actual = currentTexturePath(*mats[(size_t)m], d.slot);
            ImGui::Text("%s: %s", d.nombre,
                        actual.empty() ? "None"
                                       : std::filesystem::path(actual).filename().string().c_str());

            const int materialIndex = m;
            const MaterialTextureSlot slot = d.slot;
            drawAssetDropBox(
                ctx, d.nombre, "Drop image here",
                [this, materialIndex, slot]() {
                    m_textureDlgOpen     = true;
                    m_textureDlgMaterial = materialIndex;
                    m_textureDlgSlot     = slot;
                    IGFD::FileDialogConfig cfg;
                    cfg.path  = "assets";
                    cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                                ImGuiFileDialogFlags_HideColumnDate |
                                ImGuiFileDialogFlags_DisableThumbnailMode |
                                ImGuiFileDialogFlags_DisablePlaceMode;
                    // Key sin "##": Display() construye el nombre de la ventana
                    // como titulo+"##"+key, y un "###" ahi rompe el ID que
                    // guarda el layout (ver el comentario de AddMeshDlg).
                    m_textureFileDialog->OpenDialog("PickTextureDlg", "Choose image",
                                                     ".png,.jpg,.jpeg,.bmp,.tga", cfg);
                },
                [this, &ctx, materialIndex, slot](const std::string& path) {
                    assignMaterialTexture(ctx, materialIndex, slot, path);
                });

            ImGui::BeginDisabled(ctx.editingLocked || !hasOverride(*ctx.selected, materialIndex, slot));
            if (ImGui::Button("Clear"))
                assignMaterialTexture(ctx, materialIndex, slot, "");
            ImGui::EndDisabled();

            ImGui::PopID();
        }
        ImGui::PopID();
    }

    if (!m_textureLoadError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_textureLoadError.c_str());
}

void PropertiesPanel::assignMaterialTexture(EditorContext& ctx, int materialIndex,
                                             MaterialTextureSlot slot, const std::string& path)
{
    if (!ctx.selected || !ctx.selected->hasMesh() || ctx.editingLocked) return;

    // Clear entra con path vacio y sin comprobar extension: lo que se valida es
    // lo que se asigna.
    if (!path.empty())
    {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        static const std::set<std::string> kImagenes =
            { ".png", ".jpg", ".jpeg", ".bmp", ".tga" };
        if (!kImagenes.count(ext))
        {
            m_textureLoadError = "Formato no soportado: " + ext;
            return;
        }
    }

    const std::string antes = currentOverride(*ctx.selected, materialIndex, slot);
    m_textureLoadError.clear();

    auto cmd = std::make_unique<MaterialTextureCommand>(
        *ctx.scene, ctx.renderer,
        (path.empty() ? "Quitar textura de '" : "Textura de '") + ctx.selected->name + "'",
        ctx.selected->id, materialIndex, slot, antes, path);
    cmd->execute();
    if (ctx.undo) ctx.undo->push(std::move(cmd));

    ctx.pushLog((path.empty() ? "Textura quitada de '" : "Textura asignada a '")
                + ctx.selected->name + "'");
}
```

Helpers estáticos en el anónimo del mismo .cpp:

```cpp
// La ruta que hay AHORA en el material para ese slot (override aplicado o lo
// que trajo el FBX): es lo que se enseña.
static std::string currentTexturePath(const Material& mat, MaterialTextureSlot slot)
{
    switch (slot)
    {
        case MaterialTextureSlot::Albedo: return mat.texturePath;
        case MaterialTextureSlot::Normal: return mat.normalMapPath;
        case MaterialTextureSlot::Orm:    return mat.metallicRoughnessPath;
    }
    return {};
}

// La ruta del OVERRIDE, que no es lo mismo: vacia significa "esto es del FBX",
// y es lo que decide si Clear tiene algo que hacer.
static std::string currentOverride(const GameObject& go, int materialIndex,
                                    MaterialTextureSlot slot)
{
    for (const MaterialTextureOverride& ov : go.materialOverrides)
        if (ov.index == materialIndex)
            switch (slot)
            {
                case MaterialTextureSlot::Albedo: return ov.albedo;
                case MaterialTextureSlot::Normal: return ov.normal;
                case MaterialTextureSlot::Orm:    return ov.orm;
            }
    return {};
}

static bool hasOverride(const GameObject& go, int materialIndex, MaterialTextureSlot slot)
{
    return !currentOverride(go, materialIndex, slot).empty();
}
```

- [ ] **Step 3: Drenar el diálogo**

En `drawMeshDialog` (o en una función hermana llamada desde el mismo sitio, cada frame e independientemente de la selección — si no se drena, cambiar de selección con el modal abierto deja `m_textureDlgOpen` atascado en true para siempre):

```cpp
    if (m_textureDlgOpen && m_textureFileDialog->Display("PickTextureDlg"))
    {
        if (m_textureFileDialog->IsOk() && assetAllowed(ctx, m_textureFileDialog->GetFilePathName()))
            assignMaterialTexture(ctx, m_textureDlgMaterial, m_textureDlgSlot,
                                   m_textureFileDialog->GetFilePathName());
        m_textureFileDialog->Close();
        m_textureDlgOpen = false;
    }
```

`m_textureFileDialog` es una instancia PROPIA de `IGFD::FileDialog` (no compartida con `m_meshFileDialog` ni con `m_audioFileDialog`): redimensionar un popup toca el estado interno del diálogo que lo dibuja.

- [ ] **Step 4: Compilar y pasar la suite**

```powershell
.\build.bat
Get-ChildItem build-ninja\engine\tests\dt_*_tests.exe | ForEach-Object { & $_.FullName }
```

- [ ] **Step 5: Commit**

Mensaje: `feat(editor): editar las texturas del Mesh desde Properties`.

---

### Task 10: Cerrar la limitación documentada y verificar

**Files:**
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp:202-215, 282-310`
- Modify: `README.md` (sección de features del editor, si menciona las limitaciones del Mesh)

**Interfaces:** ninguna nueva.

Cuatro comentarios de ese fichero dicen que `nodeToJson` no serializa `texturePath`/`normalMapPath`/`metallicRoughnessPath` y que por eso el material se re-deriva del FBX al recargar. Desde la Task 6 eso es **falso**, y un comentario que miente es peor que ninguno: el siguiente que lea "no es cuestión de durabilidad entre sesiones" decidirá mal.

- [ ] **Step 1: Reescribir los comentarios**

En cada uno de los cuatro sitios, sustituir la afirmación por la situación real: las rutas del material que puso el usuario **sí** se serializan (bloque `mesh.materials`), así que reescribirlas en memoria al renombrar un asset **sí** tiene efecto entre sesiones. Lo que se re-deriva del FBX es lo que el usuario no ha tocado.

- [ ] **Step 2: Build en Debug y Release**

```powershell
.\configure.bat
.\build.bat
.\configure-release.bat
.\build-release.bat
```

Los dos tienen que terminar sin errores.

- [ ] **Step 3: Suite completa desde la raíz del repo**

```powershell
Get-ChildItem build-ninja\engine\tests\dt_*_tests.exe | ForEach-Object { Write-Host $_.Name; & $_.FullName }
```

Ejecutar **desde la raíz del repo**: desde `build-ninja/engine/tests` fallan 5 de 25 porque `assets/` no resuelve, y dos revientan con 139 mudo. Pegar la salida entera en el informe.

- [ ] **Step 4: Sabotaje una a una**

Guardas nuevas a sabotear, cada una por separado (parchear → compilar → ejecutar → revertir), anotando qué test cae y con qué mensaje:

1. `applyMaterialOverrides`: quitar la comprobación de índice fuera de rango → cae `test_out_of_range_index_is_ignored` (o revienta, que también vale como prueba de que la guarda es la que sostiene el caso).
2. `applyMaterialOverrides`: recapturar el baseline en cada pasada → cae `test_second_override_keeps_original_baseline`.
3. `applyMaterialOverrides`: escribir el baseline aunque nunca se tomara → cae `test_clear_restores_baseline`.
4. `materialsOfMesh`: devolver siempre `Mesh::material` → cae `test_materials_of_skinned_mesh`.
5. `chooseTextureSource`: devolver `Embedded` cuando hay ambos → cae `test_path_wins_over_embedded`.
6. `SharedGpuMeshCache::rekey`: quitar el rechazo por colisión → cae `test_rekey_rejects_collision`.
7. `SharedGpuMeshCache::rekey`: no borrar la clave vieja del mapa → cae `test_rekey_moves_the_entry`.
8. `toStoredPath`: relativizar también fuera de la raíz → cae `test_path_outside_root_stays_absolute`.
9. `fromStoredPath`: no resolver contra la raíz → cae `test_path_under_root_is_stored_relative`.
10. Lector de `materials`: aceptar una entrada sin `index` → cae `test_corrupt_materials_block_warns`.
11. `MaterialTextureCommand::apply`: guardar el puntero en vez de resolver por id → cae `test_command_survives_object_rebuild`.

- [ ] **Step 5: Verificación en los dos backends**

El backend con el que arranca el editor sale del `project.json` del último proyecto: para probar el otro hay que cambiarlo y **abrir el proyecto por la GUI** (abrir el .exe a secas no abre el proyecto y no verifica nada).

En cada backend: asignar una textura a un objeto estático, cambiarla, quitarla; lo mismo en un personaje con varios materiales; guardar, cerrar, reabrir y comprobar que se mantiene.

- [ ] **Step 6: Pedir al usuario la verificación visual**

No darla por hecha. Pedirla explícitamente, diciendo qué mirar.

- [ ] **Step 7: Commit final**

Incluir `imgui.ini` si sale modificado: es la config del editor del usuario, no se descarta.

Mensaje: `docs(editor): las rutas de material ya se serializan`.

---

## Self-review

**Cobertura de la spec:**

| Requisito de la spec | Tarea |
|---|---|
| Overrides en el GameObject con baseline | T1 |
| `materialsOfMesh` y semántica del índice | T1 |
| Precedencia ruta > embebida en los 3 uploaders | T2 |
| Re-clave de `SharedGpuMeshCache` | T3 |
| `rebuildStaticMesh` Vulkan (fast path + release/acquire) | T4 |
| `rebuildStaticMesh` D3D12 (in situ) | T5 |
| Skinned vía `rebuildSkinnedMesh` | T8 (`apply`) |
| Bloque JSON `materials` + compat hacia atrás | T6 |
| Rutas relativas a la raíz del proyecto | T6 |
| Aplicación en las rutas de carga (async y síncrona) | T7 |
| Undo/redo | T8 |
| UI: 3 slots por material, drop + Browse + Clear | T9 |
| Comentarios obsoletos del Content Browser | T10 |
| Build Debug+Release, suite, sabotaje, dos backends | T10 |

**Consistencia de tipos:** `MaterialTextureSlot` (T8) es distinto de `EditorRenderer::TextureSlot` (que ya existe y usa `replaceStaticTextureWithMissing`) y de `TextureSource` (T2). Son tres cosas distintas a propósito: el slot del override es un concepto del editor, el de `EditorRenderer` es de la API de render, y `TextureSource` dice de dónde salen los píxeles. El implementador NO debe fusionarlos.

**Dependencias entre tareas:** T2 y T3 son independientes entre sí y de T1. T4 necesita T2 y T3. T5 necesita T2 y la declaración de T4. T6 necesita T1. T7 necesita T1 y T6. T8 necesita T1, T4 y T5. T9 necesita T8.
