# Sphere primitive — design

## Objetivo
Nueva clase `Sphere` que genera geometría de una esfera (UV sphere) como `DonTopo::Mesh`, siguiendo el mismo patrón de factory estática que `Cube` (ver `docs/superpowers/specs/2026-07-04-cube-primitive-design.md` y `engine/src/Cube.cpp`). Sin textura asignada por defecto, visible vía el placeholder checkerboard existente (`GpuResources::createTextureImage`, `engine/src/GpuResources.cpp:183-214`).

## API

`engine/include/DonTopo/Sphere.h`:

```cpp
#pragma once
#include "DonTopo/Mesh.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo
{
    class Sphere
    {
        public:
            static Mesh create(float radius = 0.5f, uint32_t segments = 32, uint32_t rings = 16, glm::vec3 color = {0.8f, 0.8f, 0.8f});
    };
}
```

Factory estática sin estado — mismo patrón que `Cube::create` / `ModelLoader::load`. `segments` = divisiones de longitud (alrededor del eje Y), `rings` = divisiones de latitud (polo a polo).

## Geometría — UV sphere

Grid de `(rings+1) × (segments+1)` vértices, indexado por fila `r ∈ [0, rings]` (ángulo polar `θ = r·π/rings`, `θ=0` en el polo norte) y columna `c ∈ [0, segments]` (ángulo azimutal `φ = c·2π/segments`). La columna `segments` duplica la columna `0` (costura) para que el UV envuelva correctamente — mismo motivo por el que un `Mesh` con textura futura necesita esta duplicación en lugar de vértices compartidos en la costura.

Por vértice `(r, c)`:
- `pos = radius · (sin θ·cos φ, cos θ, sin θ·sin φ)`
- `normal = normalize(pos)` — dirección radial; sombreado **suave** (a diferencia de `Cube`, que usa normales planas por cara — una esfera necesita continuidad de normal entre vértices adyacentes para verse curva bajo Phong/PBR).
- `uv = (c / segments, r / rings)`
- `tangent = normalize(d(pos)/dφ) = (-sin φ, 0, cos φ)` — apunta en dirección +U, consistente con la convención de `Cube` (ver hallazgo de code review en la feature de Cube: el tangente debe alinearse con `v1-v0` en UV).
- `color` = mismo `glm::vec3` para todos los vértices.

**Triangulación e índices** (por celda `r ∈ [0, rings-1]`, `c ∈ [0, segments-1]`):
```
i0 = r*(segments+1) + c         // top-left
i1 = r*(segments+1) + c+1       // top-right
i2 = (r+1)*(segments+1) + c+1   // bottom-right
i3 = (r+1)*(segments+1) + c     // bottom-left
triángulos: (i0, i1, i2), (i0, i2, i3)
```
Mismo patrón de triangulación que usa `Cube.cpp` (`base+0,base+1,base+2` / `base+0,base+2,base+3`). Winding verificado analíticamente (producto cruzado `cross(v1-v0, v2-v0)` en un punto genérico del ecuador) como CCW visto desde fuera, consistente con `VK_FRONT_FACE_COUNTER_CLOCKWISE` + `VK_CULL_MODE_BACK_BIT` del pipeline (`engine/src/Renderer.cpp:761-762`) — el mismo invariante que causó un bug crítico en la feature de `Cube` y que aquí se deja pre-verificado en el plan de implementación, no descubierto en review.

**Polos:** en `r=0` (polo norte) y `r=rings` (polo sur), todos los vértices de esa fila colapsan al mismo punto físico (radio·sin(0)=0), pero mantienen UVs distintos por columna. Esto genera un triángulo degenerado (área cero) por celda en la fila polar — técnica estándar, no requiere manejo especial ni fan de triángulos alternativo.

`texturePath`, `embeddedTexture`, `normalMapPath`, `embeddedNormalMap`, `metallicRoughnessPath`, `embeddedMetallicRoughness` quedan vacíos/por defecto → placeholder checkerboard automático; `metallic=0.0f`, `roughness=0.5f` (defaults de `Mesh`).

## Integración sandbox

En `sandbox/src/main.cpp`, añadir una esfera junto al cubo ya existente (de la feature anterior), con un transform que no se solape con el cubo, los modelos FBX (x=±200) ni el suelo:

```cpp
size_t sphereIndex = meshes.size();
meshes.push_back(DonTopo::Sphere::create(50.0f));
```
posicionada tras `renderer.init` vía `renderer.setTransform(sphereIndex, ...)`.

## Fuera de alcance
- No hay UI para editar parámetros de la esfera.
- No se añade a `SceneNode` más allá de lo que ya hace `main.cpp` para floor/cube.
- No hay LOD ni icosphere — solo UV sphere estándar.
