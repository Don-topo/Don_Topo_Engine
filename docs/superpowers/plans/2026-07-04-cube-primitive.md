# Cube Primitive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Cube` class that generates cube geometry as a `DonTopo::Mesh`, usable like any mesh from `ModelLoader`, and wire one instance into the sandbox so it renders (with the engine's existing checkerboard placeholder since no texture is assigned).

**Architecture:** Static factory `Cube::create(size, color)` returns a `Mesh` with 24 vertices (per-face normals/UVs/tangents) and 36 indices — no engine/Renderer changes needed, since `GpuResources::createTextureImage` already falls back to a checkerboard placeholder when `Mesh::texturePath` and `Mesh::embeddedTexture` are empty (`engine/src/GpuResources.cpp:183-214`).

**Tech Stack:** C++20, GLM, existing `DonTopo::Mesh`/`Vertex` types. No new dependencies. No unit test framework in this repo — verification is build success + visual check via `sandbox` executable (matches existing project convention: no test targets in any `CMakeLists.txt`).

## Global Constraints

- Vertex winding must be CCW seen from outside the face, consistent with `ModelLoader.cpp` and the floor mesh in `sandbox/src/main.cpp:35-50`.
- `Vertex` fields per `engine/include/DonTopo/Vertex.h`: `pos, color, uv, normal, tangent` (all `glm::vec3` except `uv` which is `glm::vec2`).
- `Mesh` fields per `engine/include/DonTopo/Mesh.h`: leave `texturePath`, `embeddedTexture`, `normalMapPath`, `embeddedNormalMap`, `metallicRoughnessPath`, `embeddedMetallicRoughness` at their defaults (empty/unset) so the placeholder checkerboard texture applies; leave `metallic = 0.0f`, `roughness = 0.5f` (`Mesh` defaults).
- New source file must be added to `engine/CMakeLists.txt`'s `add_library(DonTopoEngine STATIC ...)` list to be compiled.

---

### Task 1: Cube geometry class

**Files:**
- Create: `engine/include/DonTopo/Cube.h`
- Create: `engine/src/Cube.cpp`
- Modify: `engine/CMakeLists.txt:1-13` (add `src/Cube.cpp` to the source list)

**Interfaces:**
- Produces: `DonTopo::Cube::create(float size = 1.0f, glm::vec3 color = {0.8f, 0.8f, 0.8f}) -> DonTopo::Mesh` — used by Task 2 (`sandbox/src/main.cpp`).

- [ ] **Step 1: Write `Cube.h`**

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

- [ ] **Step 2: Write `Cube.cpp`**

```cpp
#include "DonTopo/Cube.h"

namespace DonTopo
{
    Mesh Cube::create(float size, glm::vec3 color)
    {
        Mesh mesh;
        mesh.name = "cube";

        const float h = size * 0.5f;

        struct FaceDef {
            glm::vec3 normal;
            glm::vec3 tangent;
            glm::vec3 v0, v1, v2, v3; // CCW seen from outside
        };

        const std::array<FaceDef, 6> faces = { {
            // +X
            { { 1, 0, 0}, {0, 0,-1}, { h,-h,-h}, { h,-h, h}, { h, h, h}, { h, h,-h} },
            // -X
            { {-1, 0, 0}, {0, 0, 1}, {-h,-h, h}, {-h,-h,-h}, {-h, h,-h}, {-h, h, h} },
            // +Y
            { { 0, 1, 0}, {1, 0, 0}, {-h, h,-h}, { h, h,-h}, { h, h, h}, {-h, h, h} },
            // -Y
            { { 0,-1, 0}, {1, 0, 0}, {-h,-h, h}, { h,-h, h}, { h,-h,-h}, {-h,-h,-h} },
            // +Z
            { { 0, 0, 1}, {1, 0, 0}, {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h} },
            // -Z
            { { 0, 0,-1}, {-1, 0, 0}, { h,-h,-h}, {-h,-h,-h}, {-h, h,-h}, { h, h,-h} },
        } };

        for (const auto& face : faces)
        {
            uint32_t base = static_cast<uint32_t>(mesh.vertices.size());

            Vertex v0{}, v1{}, v2{}, v3{};
            v0.pos = face.v0; v1.pos = face.v1; v2.pos = face.v2; v3.pos = face.v3;
            v0.uv = {0, 1}; v1.uv = {1, 1}; v2.uv = {1, 0}; v3.uv = {0, 0};

            for (Vertex* v : { &v0, &v1, &v2, &v3 })
            {
                v->color   = color;
                v->normal  = face.normal;
                v->tangent = face.tangent;
            }

            mesh.vertices.insert(mesh.vertices.end(), { v0, v1, v2, v3 });
            mesh.indices.insert(mesh.indices.end(), {
                base + 0, base + 1, base + 2,
                base + 0, base + 2, base + 3
            });
        }

        return mesh;
    }
}
```

- [ ] **Step 3: Add `array` include used by `Cube.cpp`**

`Cube.cpp` uses `std::array` — add `#include <array>` under the existing `#include "DonTopo/Cube.h"` line.

- [ ] **Step 4: Register the new source file in the build**

In `engine/CMakeLists.txt`, add `src/Cube.cpp` to the `add_library(DonTopoEngine STATIC ...)` list (after `src/ModelLoader.cpp`):

```cmake
add_library(DonTopoEngine STATIC
    src/Engine.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/EditorUI.cpp
    src/Skybox.cpp
    src/ModelLoader.cpp
    src/Cube.cpp
    src/Camera.cpp
    src/SceneNode.cpp
    src/AudioManager.cpp
    src/GpuDevice.cpp
    src/GpuResources.cpp
)
```

- [ ] **Step 5: Build to verify it compiles**

Run: `build.bat`
Expected: build succeeds, no errors referencing `Cube.h`/`Cube.cpp`.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Cube.h engine/src/Cube.cpp engine/CMakeLists.txt
git commit -m "feat(engine): add Cube primitive mesh generator"
```

---

### Task 2: Wire a cube into the sandbox

**Files:**
- Modify: `sandbox/src/main.cpp:1-6` (include), `:24-51` (mesh vector construction), `:63-77` (post-init transform)

**Interfaces:**
- Consumes: `DonTopo::Cube::create(float size = 1.0f, glm::vec3 color = {0.8f, 0.8f, 0.8f}) -> DonTopo::Mesh` (Task 1).
- Consumes: `Renderer::setTransform(size_t objectIndex, const glm::mat4& transform)` (existing, `engine/include/DonTopo/Renderer.h:34-38`).

- [ ] **Step 1: Include the header**

In `sandbox/src/main.cpp`, add after the `#include "DonTopo/ModelLoader.h"` line:

```cpp
#include "DonTopo/Cube.h"
```

- [ ] **Step 2: Add the cube mesh and remember its index**

In `sandbox/src/main.cpp`, right after the floor block (currently ending at line 51 with `meshes.push_back(floor);`), add:

```cpp
        // Cubo de prueba (sin textura -> placeholder checkerboard)
        size_t cubeIndex = meshes.size();
        meshes.push_back(DonTopo::Cube::create(50.0f));
```

- [ ] **Step 3: Position the cube after `renderer.init`**

Right after the existing `renderer.init(window, meshes);` call, add:

```cpp
        renderer.setTransform(cubeIndex,
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 50.0f, -200.0f)));
```

(Y = 50 lifts the 50-unit cube so its base sits near the floor at y=0; Z = -200 keeps it clear of the two loaded models, which sit at x=±200.)

- [ ] **Step 4: Build**

Run: `build.bat`
Expected: build succeeds.

- [ ] **Step 5: Run and visually verify**

Run: `build-ninja\sandbox\Sandbox.exe`
Expected: a gray/checkerboard-textured cube visible in the viewport near the floor, distinct from the two FBX models and not overlapping them.

- [ ] **Step 6: Commit**

```bash
git add sandbox/src/main.cpp
git commit -m "feat(sandbox): render a Cube primitive instance"
```
