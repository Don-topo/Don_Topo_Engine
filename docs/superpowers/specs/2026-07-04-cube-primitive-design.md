# Cube primitive — design

## Objetivo
Nueva clase `Cube` que genera geometría de un cubo como `DonTopo::Mesh`, usable igual que los meshes cargados por `ModelLoader`. Sin textura asignada por defecto, pero visible en el render (placeholder checkerboard que ya provee `GpuResources::createTextureImage` cuando `texturePath`/`embeddedTexture` están vacíos).

## API

`engine/include/DonTopo/Cube.h`:

```cpp
#pragma once
#include "DonTopo/Mesh.h"
#include <glm/glm.hpp>

namespace DonTopo
{
    class Cube
    {
        public:
            static Mesh create(float size = 1.0f, glm::vec3 color = {0.8f, 0.8f, 0.8f});
    };
}
```

Factory estática sin estado — mismo patrón que `ModelLoader::load`. No hay clase instanciable: el `Renderer` ya gestiona transform/color por fuera vía `RenderObject`.

## Geometría

- Cubo centrado en origen, lado `size` (caras en ±size/2 por eje).
- 24 vértices (4 por cara × 6 caras) — necesarios para normales planas por cara, correctas para Phong/PBR (normales compartidas por vértice darían sombreado suavizado incorrecto en un cubo).
- 36 índices (2 triángulos por cara, winding consistente con el resto del engine — CCW visto desde fuera, igual que `ModelLoader`/floor en `main.cpp`).
- UV 0..1 por cara, para que si se asigna `texturePath` más adelante se vea correctamente mapeada.
- Tangentes calculados por cara (vector constante a lo largo del edge U), consistente con `Vertex::tangent` usado en el pipeline de normal mapping.
- `color` se asigna a cada vértice de la cara (mismo `glm::vec3` para las 24).
- `texturePath`, `embeddedTexture`, `normalMapPath`, `metallicRoughnessPath` quedan vacíos/por defecto → placeholder checkerboard automático, `metallic=0.0f`, `roughness=0.5f` (defaults de `Mesh`).

## Integración sandbox

En `sandbox/src/main.cpp`, añadir un cubo al vector de meshes antes de `renderer.init`:

```cpp
meshes.push_back(DonTopo::Cube::create(50.0f));
```

Posicionado con un transform razonable tras `renderer.init` (vía `renderer.setTransform(index, ...)`) para que no quede superpuesto con otros objetos de la escena.

## Fuera de alcance
- No se añade UI (ImGui) para editar el cubo.
- No se añade a `SceneNode` jerárquico más allá de lo que ya hace `main.cpp` para floor.
- No hay parámetros de subdivisión ni normales suavizadas.
