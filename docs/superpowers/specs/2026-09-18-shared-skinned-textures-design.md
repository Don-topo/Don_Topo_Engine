# Texturas compartidas entre personajes skinned — diseño

## Problema, medido

Cada personaje skinned sube su propia copia de las texturas de sus materiales, en los dos backends (Vulkan `Renderer::addSkinnedMesh`, D3D12 `Impl::createSkinnedObject`), aunque vengan del mismo FBX. Medido en Vulkan el 2026-09-18 con `assets/modelAnimation.fbx` (diferencia entre escenas de 1 y 10 personajes):

| por personaje | |
|---|---|
| imágenes (texturas) | **218 MB** (tres de 4096² y una de 2048², sin mips) |
| buffers | 20 MB |

En una GPU de 8 GB la VRAM se agota entre 33 y 40 personajes según lo libre que haya (`failed to allocate image memory`). Compartir las texturas deja cada personaje en ~20 MB. D3D12 hace lo mismo (`uploadMaterialTexture` por personaje); cargó 40 sin agotarse, pero el crecimiento es igual de lineal.

## Decisión

Una caché de texturas con recuento de referencias, en cada backend, para las texturas de material de los personajes skinned. Las vistas y los descriptor sets siguen siendo por personaje. Los buffers no se tocan: son el 9 % y muchos son por instancia de verdad (salida del skinning, matrices).

## Pieza común: `SharedTextureCache<Handle>` (`Renderer/SharedTextureCache.h`, solo cabecera, sin GPU)

```cpp
enum class TextureKind : uint8_t { BaseColor, Normal, Orm };   // formato distinto = imagen distinta

// Clave de contenido: ruta tal cual si viene de fichero; si va embebida,
// "emb:" + tamaño + FNV-1a de los bytes (dos texturas embebidas iguales en
// FBX distintos son la misma imagen). Más el tipo. Vacía si no hay textura.
std::string makeTextureKey(const std::string& path, const std::vector<uint8_t>& embedded, TextureKind kind);

template <typename Handle>
class SharedTextureCache
{
public:
    // Devuelve el handle de `key`; lo crea con `create` solo la primera vez.
    // createdOut dice cuál de los dos casos ha sido.
    Handle acquire(const std::string& key, const std::function<Handle()>& create, bool* createdOut = nullptr);
    // Resta una referencia al handle; al llegar a 0 lo saca y llama a `destroy`.
    // No-op si el handle no está en la caché.
    void release(const Handle& h, const std::function<void(const Handle&)>& destroy);
    bool contains(const Handle& h) const;
    int  refCount(const std::string& key) const;   // 0 si no está
    size_t size() const;
};
```

El `Handle` necesita `operator==` y ser copiable: en Vulkan `{VkImage, VkDeviceMemory}`, en D3D12 `D3D12MA::Allocation*`.

**Qué NO entra en la caché:** una clave vacía (material sin textura). Esos ya usan la blanca de relleno compartida, que tiene su propio mecanismo de préstamo (`GpuResources::releaseMaterialImage`, H79). Mezclar los dos préstamos en un mismo handle es exactamente la situación que produjo aquel doble free. `acquire` con clave vacía llama a `create` sin guardar nada, y `release` de un handle que no está es un no-op, así que el llamante sigue su camino de siempre.

Una textura que se pide y no se puede leer (damero) **sí** entra, con su clave de ruta: sigue siendo una imagen propia creada por `create`.

## Vulkan

- `Renderer` gana `SharedTextureCache<MaterialImage> m_skinnedTextures`, con `struct MaterialImage { VkImage image; VkDeviceMemory mem; bool operator==(...) }`.
- `addSkinnedMesh`: cada una de las tres texturas se pide con `acquire(makeTextureKey(ruta, embebida, tipo), [&]{ crear con createTextureImage/createNormalMapImage; return {img, mem}; })`. La vista se sigue creando por personaje sobre la imagen devuelta.
- Borrado del personaje (`Renderer.cpp` ~:3640): en lugar de `m_res.releaseMaterialImage(img, mem)`, `m_skinnedTextures.release({img, mem}, [&](auto& h){ m_res.releaseMaterialImage(h.image, h.mem); })`. Si el handle no estaba en la caché (el relleno blanco), se llama a `releaseMaterialImage` directamente, que ya sabe no destruirlo.
- **Subidas en vuelo:** una textura creada en un batch aún no completado se comparte con un personaje registrado después. Sirve porque ese personaje tiene su propio ticket en un batch igual o posterior y los batches se completan en orden en la misma cola; su `uploadTicket` no llega a 0 antes que el de la textura.
- **Desmontaje:** antes de `destroySharedPlaceholders`, `m_skinnedTextures.size()` tiene que ser 0; si no, se avisa por stderr (no se destruye nada a ciegas).

## D3D12

- `Impl` gana `SharedTextureCache<D3D12MA::Allocation*> skinnedTextures`.
- `createSkinnedObject`: en fallo de caché, `uploadMaterialTexture(..., slot)` como hoy, que sube y escribe la vista. En acierto, solo `createTexture2DSrv(alloc->GetResource(), formato, slot)`, con el mismo formato que usa `uploadMaterialTexture` (sRGB para el color base, UNORM para normal y ORM). `uploadMaterialTexture` devuelve `nullptr` si no hay textura: eso no se guarda, igual que en Vulkan.
- Liberación (`:3545`, `:8238`): `skinnedTextures.release(alloc, [](auto a){ a->Release(); })` para cada una de `character.textures`, en vez de `Release()` directo.
- `character.textures` sigue guardando los punteros (con repetidos entre personajes): es lo que se suelta al borrar.

## Tests (`engine/tests/shared_texture_cache_tests.cpp`, nuevo, sin GPU)

1. La misma clave dos veces llama a `create` una sola vez y devuelve el mismo handle; `refCount` es 2.
2. La misma ruta con tipo distinto son dos entradas.
3. `release` no destruye hasta que el contador llega a 0, y entonces destruye una vez.
4. Clave vacía: `create` se llama siempre y no se guarda (`size() == 0`); `release` de ese handle no llama a `destroy`.
5. `makeTextureKey` con bytes embebidos iguales en vectores distintos da la misma clave; con un byte distinto, otra; la ruta no influye cuando hay bytes embebidos.

Cada test con su sabotaje.

**Medida:** la misma instrumentación temporal de hoy en `GpuResources` (bytes de imagen por personaje, diferencia 1 → 10): de ~218 MB a ~0 MB de imágenes por personaje adicional. **Carga:** la escena de 80 personajes, que hoy agota la VRAM, carga en Vulkan y en D3D12. **Manual:** varios personajes del mismo FBX se ven con sus texturas en los dos backends; borrar uno (y deshacer) no deja a los demás sin textura; cambiar el material de uno no cambia el de los otros.

## Fuera de alcance

- Mipmaps para las texturas de material (hoy ninguna tiene; cada 4K ocupa 64 MB).
- Compartir texturas entre mallas estáticas y skinned (las estáticas ya comparten la malla entera por `SharedGpuMesh`).
- Compartir buffers de solo lectura entre personajes (vértices de entrada, claves de animación): el 9 %.

## Riesgo comprobado

**Cambiar el material de un personaje en el editor** podría cambiar el de todos los que comparten la textura si se modificara en sitio. No es el caso: en un skinned, la textura solo cambia por `rebuildSkinnedMesh` (`Renderer.cpp:3934`), que destruye el objeto y lo vuelve a crear, y por tanto pasa por `release` y `acquire`. Los factores metallic/roughness ya son por personaje. El test manual lo confirma igualmente.
