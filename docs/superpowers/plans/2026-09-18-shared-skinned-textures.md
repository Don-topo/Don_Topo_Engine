# Texturas compartidas entre personajes skinned — plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, elegido por el usuario). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** que los personajes skinned del mismo FBX compartan sus texturas de material en la GPU, en Vulkan y D3D12.

**Architecture:** una caché con recuento de referencias, solo cabecera y sin GPU (`SharedTextureCache<Handle>`), que cada backend usa con su propio tipo de handle. Crear y destruir son callbacks del backend. Los materiales sin textura siguen usando la blanca de relleno y no entran.

**Tech Stack:** C++20, Vulkan, D3D12 + D3D12MA.

**Spec:** `docs/superpowers/specs/2026-09-18-shared-skinned-textures-design.md` (`f8e6ac9`)

## Global Constraints

- Rama `feat/shared-skinned-textures`. `.\build.bat` desde PowerShell; tests desde la raíz.
- CRLF: editar con Edit o Python `newline=''`. OJO: `runtime/main.cpp` está en LF. Commits con `git commit -F -` y el trailer `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- **Commitear antes de cada sabotaje** (restaurar con `git checkout --` sobre un fichero sin commitear se lleva el trabajo).
- Paridad Vulkan/D3D12.

---

### Task 1: la caché (sin GPU)

**Files:** Create `engine/include/DonTopo/Renderer/SharedTextureCache.h`, `engine/tests/shared_texture_cache_tests.cpp`; Modify `engine/tests/CMakeLists.txt` (tras `dt_shared_gpu_mesh_tests`).

**Interfaces — Produces:** `enum class TextureKind : uint8_t { BaseColor, Normal, Orm }`, `std::string makeTextureKey(const std::string&, const std::vector<uint8_t>&, TextureKind)`, `template <typename Handle> class SharedTextureCache` con `acquire(key, create, bool* createdOut = nullptr)`, `release(h, destroy)`, `contains(h)`, `refCount(key)`, `size()`.

- [ ] **Step 1: tests** (`shared_texture_cache_tests.cpp`)

```cpp
// Test headless de la caché de texturas compartidas (sin device). Handles
// falsos con identidad propia: dos objetos solo comparten imagen si la entrada
// NO se ha creado dos veces. Lo que se prueba son las dos formas de romperlo:
// compartir de más (dos texturas distintas en la misma imagen) y liberar de
// más (soltar un personaje y dejar a sus gemelos muestreando memoria libre).
#include "DonTopo/Renderer/SharedTextureCache.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

namespace
{
    int g_next = 1;
    int g_creadas = 0;
    int g_destruidas = 0;
    int crear() { ++g_creadas; return g_next++; }
    void reiniciar() { g_creadas = 0; g_destruidas = 0; }
}

static void test_same_key_creates_once()
{
    reiniciar();
    SharedTextureCache<int> c;
    const std::string k = makeTextureKey("a.png", {}, TextureKind::BaseColor);
    bool creada1 = false, creada2 = true;
    const int h1 = c.acquire(k, crear, &creada1);
    const int h2 = c.acquire(k, crear, &creada2);
    CHECK(h1 == h2);
    CHECK(g_creadas == 1);
    CHECK(creada1 && !creada2);
    CHECK(c.refCount(k) == 2);
}

static void test_same_path_other_kind_is_other_image()
{
    reiniciar();
    SharedTextureCache<int> c;
    const int a = c.acquire(makeTextureKey("a.png", {}, TextureKind::BaseColor), crear);
    const int b = c.acquire(makeTextureKey("a.png", {}, TextureKind::Normal), crear);
    CHECK(a != b);
    CHECK(g_creadas == 2);
    CHECK(c.size() == 2u);
}

static void test_release_destroys_at_zero()
{
    reiniciar();
    SharedTextureCache<int> c;
    const std::string k = makeTextureKey("a.png", {}, TextureKind::Orm);
    const int h = c.acquire(k, crear);
    c.acquire(k, crear);
    auto destruir = [](const int&) { ++g_destruidas; };
    c.release(h, destruir);
    CHECK(g_destruidas == 0);
    CHECK(c.contains(h));
    c.release(h, destruir);
    CHECK(g_destruidas == 1);
    CHECK(!c.contains(h));
    c.release(h, destruir);                 // ya no está: no-op
    CHECK(g_destruidas == 1);
}

static void test_empty_key_is_not_cached()
{
    reiniciar();
    SharedTextureCache<int> c;
    const std::string k = makeTextureKey("", {}, TextureKind::BaseColor);
    CHECK(k.empty());
    const int h1 = c.acquire(k, crear);
    const int h2 = c.acquire(k, crear);
    CHECK(g_creadas == 2);
    CHECK(h1 != h2);
    CHECK(c.size() == 0u);
    c.release(h1, [](const int&) { ++g_destruidas; });
    CHECK(g_destruidas == 0);               // no era suya: el llamante sigue su camino
}

// Si crear falla (handle vacío), no queda una entrada nula que devolver al
// siguiente: se vuelve a intentar.
static void test_failed_create_is_not_cached()
{
    reiniciar();
    SharedTextureCache<int> c;
    const std::string k = makeTextureKey("roto.png", {}, TextureKind::BaseColor);
    const int h = c.acquire(k, [] { ++g_creadas; return 0; });
    CHECK(h == 0);
    CHECK(c.size() == 0u);
    c.acquire(k, crear);
    CHECK(g_creadas == 2);
}

static void test_embedded_key_by_content()
{
    const std::vector<uint8_t> a = { 1, 2, 3, 4 };
    const std::vector<uint8_t> b = { 1, 2, 3, 4 };
    const std::vector<uint8_t> d = { 1, 2, 3, 5 };
    CHECK(makeTextureKey("x.fbx", a, TextureKind::BaseColor) == makeTextureKey("y.fbx", b, TextureKind::BaseColor));
    CHECK(makeTextureKey("x.fbx", a, TextureKind::BaseColor) != makeTextureKey("x.fbx", d, TextureKind::BaseColor));
    CHECK(makeTextureKey("x.fbx", a, TextureKind::BaseColor) != makeTextureKey("x.fbx", a, TextureKind::Normal));
}

int main()
{
    test_same_key_creates_once();
    test_same_path_other_kind_is_other_image();
    test_release_destroys_at_zero();
    test_empty_key_is_not_cached();
    test_failed_create_is_not_cached();
    test_embedded_key_by_content();
    if (g_failures == 0) std::printf("shared_texture_cache_tests OK\n");
    else                 std::printf("shared_texture_cache_tests FAILED: %d checks\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

`engine/tests/CMakeLists.txt`, tras `dt_add_test(dt_shared_gpu_mesh_tests ...)`: `dt_add_test(dt_shared_texture_cache_tests shared_texture_cache_tests.cpp DonTopoCore)`.

- [ ] **Step 2: la caché** (`SharedTextureCache.h`)

```cpp
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DonTopo
{
    // Qué se sube: el mismo fichero como color (sRGB) o como normal/ORM
    // (lineal) son imágenes distintas.
    enum class TextureKind : uint8_t { BaseColor, Normal, Orm };

    // Clave de contenido de una textura de material. De fichero: la ruta. Embebida
    // en el FBX: tamaño + FNV-1a de los bytes, así que dos FBX con la misma
    // textura dentro comparten imagen, y la ruta no cuenta. Vacía si no hay
    // textura: esos materiales usan la blanca de relleno, que ya se presta por
    // su cuenta (GpuResources::releaseMaterialImage) y NO debe entrar aquí.
    inline std::string makeTextureKey(const std::string& path, const std::vector<uint8_t>& embedded,
                                      TextureKind kind)
    {
        if (path.empty() && embedded.empty()) return {};
        const char tipo = kind == TextureKind::BaseColor ? 'c' : (kind == TextureKind::Normal ? 'n' : 'o');
        if (!embedded.empty())
        {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : embedded) { h ^= b; h *= 1099511628211ull; }
            return std::string(1, tipo) + ":emb:" + std::to_string(embedded.size()) + ":" + std::to_string(h);
        }
        return std::string(1, tipo) + ":" + path;
    }

    // Texturas de material compartidas con recuento de referencias. No sabe nada
    // de la GPU: crear y destruir son del backend, que es lo que la deja probar
    // sin device (shared_texture_cache_tests). Pocas entradas por escena, así
    // que un vector lineal basta.
    template <typename Handle>
    class SharedTextureCache
    {
    public:
        // El handle de `key`, creándolo con `create` solo la primera vez. Con
        // clave vacía, o si `create` devuelve un handle vacío (Handle{}: no se
        // pudo crear), no se guarda nada: no es de la caché.
        Handle acquire(const std::string& key, const std::function<Handle()>& create,
                       bool* createdOut = nullptr)
        {
            if (!key.empty())
                for (auto& e : m_entries)
                    if (e.key == key)
                    {
                        ++e.refs;
                        if (createdOut) *createdOut = false;
                        return e.handle;
                    }
            if (createdOut) *createdOut = true;
            Handle h = create();
            if (!key.empty() && !(h == Handle{})) m_entries.push_back({ key, h, 1 });
            return h;
        }

        // Una referencia menos; a cero, fuera de la tabla y `destroy`. Un handle
        // que no está (el relleno, o ya soltado) es un no-op: quien lo pidió
        // sigue su camino de siempre.
        void release(const Handle& h, const std::function<void(const Handle&)>& destroy)
        {
            for (size_t i = 0; i < m_entries.size(); i++)
            {
                if (!(m_entries[i].handle == h)) continue;
                if (--m_entries[i].refs > 0) return;
                const Handle copia = m_entries[i].handle;
                m_entries.erase(m_entries.begin() + i);
                destroy(copia);
                return;
            }
        }

        bool contains(const Handle& h) const
        {
            for (const auto& e : m_entries) if (e.handle == h) return true;
            return false;
        }
        int refCount(const std::string& key) const
        {
            for (const auto& e : m_entries) if (e.key == key) return e.refs;
            return 0;
        }
        size_t size() const { return m_entries.size(); }

    private:
        struct Entry { std::string key; Handle handle; int refs; };
        std::vector<Entry> m_entries;
    };
}
```

- [ ] **Step 3:** `configure.bat` (test nuevo), build, `dt_shared_texture_cache_tests` OK y suite completa 29/29.
- [ ] **Step 4:** CRLF y commit `feat(renderer): cache de texturas compartidas con recuento de referencias`.
- [ ] **Step 5: sabotajes** (uno a uno, en `SharedTextureCache.h`):

| # | Cambio | Debe caer |
|---|---|---|
| T1 | en `acquire`, `if (!key.empty())` del bucle de búsqueda → `if (false)` | `same_key_creates_once` |
| T2 | `if (--m_entries[i].refs > 0) return;` → borrar | `release_destroys_at_zero` |
| T3 | `if (!key.empty() && !(h == Handle{})) m_entries.push_back` → `if (!(h == Handle{})) m_entries.push_back` | `empty_key_is_not_cached` |
| T3b | `&& !(h == Handle{})` → borrar | `failed_create_is_not_cached` |
| T4 | en `makeTextureKey`, `std::string(1, tipo) + ":emb:"` → `"emb:"` | `embedded_key_by_content` |
| T5 | `h ^= b;` → borrar | `embedded_key_by_content` |

---

### Task 2: Vulkan

**Files:** Modify `engine/include/DonTopo/Renderer/Renderer.h` (miembro), `engine/src/Renderer/Renderer.cpp` (`initSkinnedRenderObject` texturas ~:3805, destrucción ~:3640, `shutdown` antes de `destroySharedPlaceholders` ~:724).

- [ ] **Step 1:** en `Renderer.h`, `#include "DonTopo/Renderer/SharedTextureCache.h"` y, junto a `m_sharedMeshes`:

```cpp
            // Imagen de material de un personaje skinned, compartida entre los
            // que salen del mismo FBX (antes, 218 MB por personaje: la VRAM se
            // agotaba entre 33 y 40). Las vistas siguen siendo por personaje.
            struct MaterialImage
            {
                VkImage        image = VK_NULL_HANDLE;
                VkDeviceMemory mem   = VK_NULL_HANDLE;
                bool operator==(const MaterialImage& o) const { return image == o.image && mem == o.mem; }
            };
            SharedTextureCache<MaterialImage> m_skinnedTextures;
```

- [ ] **Step 2:** en el bloque de texturas por material, sustituir las creaciones de color, normal y ORM-con-mapa por `acquire`:

```cpp
            // Color, normal y ORM, compartidos entre personajes del mismo FBX
            // (m_skinnedTextures). Sin textura, la blanca de relleno de siempre,
            // que no entra en la caché.
            auto pedir = [&](const std::string& ruta, const std::vector<uint8_t>& emb, TextureKind tipo,
                             VkImage& img, VkDeviceMemory& mem) {
                const MaterialImage m = m_skinnedTextures.acquire(makeTextureKey(ruta, emb, tipo), [&] {
                    MaterialImage nueva;
                    if (tipo == TextureKind::BaseColor)
                        m_res.createTextureImage(ruta, emb, nueva.image, nueva.mem, batch);
                    else
                        m_res.createNormalMapImage(ruta, emb, nueva.image, nueva.mem, batch);
                    return nueva;
                });
                img = m.image;
                mem = m.mem;
            };

            // Diffuse
            pedir(smat.texturePath, smat.embeddedTexture, TextureKind::BaseColor, mgfx.textureImage, mgfx.textureMem);
            m_res.createTextureImageView(mgfx.textureImage, mgfx.textureView);
            mgfx.sampler = m_res.sharedMaterialSampler();

            // Normal map
            pedir(smat.normalMapPath, smat.embeddedNormalMap, TextureKind::Normal, mgfx.normalImage, mgfx.normalMem);
            m_res.createTextureImageView(mgfx.normalImage, mgfx.normalView, VK_FORMAT_R8G8B8A8_UNORM);
            mgfx.normalSampler = m_res.sharedMaterialSampler();

            // ORM
            if (!smat.metallicRoughnessPath.empty() || !smat.embeddedMetallicRoughness.empty())
            {
                pedir(smat.metallicRoughnessPath, smat.embeddedMetallicRoughness, TextureKind::Orm,
                      mgfx.ormImage, mgfx.ormMem);
```

(el `else` con `sharedWhiteOrm` y el resto no cambian).

- [ ] **Step 3:** destrucción (las tres llamadas `m_res.releaseMaterialImage(mgfx.X, mgfx.XMem)` del bucle `for (auto& mgfx : obj.matGfx)`, NO las de objetos estáticos de ~:3377) pasan a `soltarMaterial(mgfx.X, mgfx.XMem);` con, antes del bucle:

```cpp
        // Compartida: se destruye cuando la suelta el último personaje. Si no es
        // de la caché (la blanca de relleno), releaseMaterialImage sabe que es
        // prestada y no la destruye.
        auto soltarMaterial = [&](VkImage img, VkDeviceMemory mem) {
            const MaterialImage m{ img, mem };
            if (m_skinnedTextures.contains(m))
                m_skinnedTextures.release(m, [&](const MaterialImage& h) { m_res.releaseMaterialImage(h.image, h.mem); });
            else
                m_res.releaseMaterialImage(img, mem);
        };
```

- [ ] **Step 4:** en `shutdown`, justo antes de `m_res.destroySharedPlaceholders();`:

```cpp
        // Todos los personajes se soltaron arriba: la caché tiene que estar
        // vacía. Si no, alguien se saltó el release; se avisa y no se destruye
        // nada a ciegas (mismo criterio que H79).
        if (m_skinnedTextures.size() != 0)
            fprintf(stderr, "[Renderer] %zu texturas de personaje sin soltar al cerrar\n", m_skinnedTextures.size());
```

- [ ] **Step 5:** build, suite. Medida con el instrumental temporal de `GpuResources` (`VRAMIMG`, ver el commit de la spec) en `build-ninja/sandbox` con `rm1` y `rm10`: la diferencia de imágenes entre 1 y 10 debe ser ~0. Escena de 80 (`rm80.scene`) en Debug Vulkan: carga sin error y el proceso cierra limpio con `CloseMainWindow` (stderr sin `has not been destroyed` de texturas). Revertir instrumental. Commit `feat(vulkan): personajes skinned comparten sus texturas de material`.

- [ ] **Step 6: sabotajes** (commit hecho antes):

| # | Cambio | Debe caer |
|---|---|---|
| V1 | `pedir` usa `makeTextureKey("", {}, tipo)` como clave | la medida: imágenes por personaje vuelven a ~218 MB |
| V2 | `soltarMaterial` llama siempre a `releaseMaterialImage` directo | rm10 al cerrar: validación con imágenes destruidas dos veces, o el aviso de caché no vacía |

---

### Task 3: D3D12

**Files:** Modify `engine/src/Renderer/D3D12/D3D12Renderer.cpp` (`Impl`, `createSkinnedObject` ~:3445-3470, liberaciones ~:3545 y ~:8238).

- [ ] **Step 1:** `#include "DonTopo/Renderer/SharedTextureCache.h"` y en `Impl`, junto a `skinnedObjects`: `SharedTextureCache<D3D12MA::Allocation*> skinnedTextures;` con el comentario de Vulkan.

- [ ] **Step 2:** en `createSkinnedObject`, un helper antes del bucle de submallas:

```cpp
    // Textura de material compartida entre personajes del mismo FBX. En fallo
    // de caché sube y escribe la vista (uploadMaterialTexture); en acierto solo
    // escribe la vista de este personaje sobre la imagen ya subida, con el
    // mismo formato. nullptr = sin textura: el llamante pone su relleno.
    auto pedirTextura = [&](const std::string& ruta, const std::vector<uint8_t>& emb, TextureKind tipo,
                            UINT srvIndex) -> D3D12MA::Allocation* {
        const bool srgb = tipo == TextureKind::BaseColor;
        bool creada = false;
        D3D12MA::Allocation* a = skinnedTextures.acquire(makeTextureKey(ruta, emb, tipo),
            [&] { return uploadMaterialTexture(ruta, emb, srgb, srvIndex); }, &creada);
        if (a && !creada)
            createTexture2DSrv(a->GetResource(),
                               srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM, srvIndex);
        return a;
    };
```

y las tres llamadas `uploadMaterialTexture(range.material->X, ..., srgb, slot + n)` pasan a `pedirTextura(range.material->X, range.material->embeddedX, TextureKind::BaseColor|Normal|Orm, slot + n)`.

Un `nullptr` de `uploadMaterialTexture` (sin textura o fichero ilegible) no se guarda en la caché (`Handle{}`, Task 1), así que el llamante pone su relleno como hoy.

- [ ] **Step 3:** en las dos liberaciones, `texture->Release()` pasa a:

```cpp
        for (D3D12MA::Allocation* texture : character.textures)
            if (texture)
                skinnedTextures.release(texture, [](D3D12MA::Allocation* const& a) { a->Release(); });
```

y en la liberación total (~:3545), tras vaciar `skinnedObjects`, el mismo aviso que en Vulkan si `skinnedTextures.size() != 0`.

- [ ] **Step 4:** build, suite; `rm80` con `game.cfg` en `DirectX 12` carga y cierra limpio; `d3d12_diag.log` sin líneas nuevas de la capa. Commit `feat(d3d12): personajes skinned comparten sus texturas de material`.

---

### Task 4: audit, memoria y verificación manual

- [ ] `docs/animation-audit.md`: B6 (lado GPU) pasa a medido y cerrado para las texturas; anotar 218 → ~0 MB por personaje adicional y que los buffers (20 MB) siguen por personaje.
- [ ] Memoria `runtime_headless_perf_harness.md`: el límite de personajes ya no es la VRAM de texturas.
- [ ] Verificación manual (usuario), los dos backends: varios personajes del mismo FBX con sus texturas; borrar uno y deshacer; cambiar la textura de uno en el inspector y que los demás no cambien.
