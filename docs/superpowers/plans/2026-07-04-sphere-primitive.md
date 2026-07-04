# Sphere Primitive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Sphere` class that generates UV-sphere geometry as a `DonTopo::Mesh`, usable like `Cube`/`ModelLoader` meshes, and wire one instance into the sandbox so it renders (checkerboard placeholder, no texture assigned).

**Architecture:** Static factory `Sphere::create(radius, segments, rings, color)` returns a `Mesh` built from a `(rings+1) x (segments+1)` theta/phi grid — smooth per-vertex normals (`normalize(pos)`), unlike `Cube`'s flat per-face normals. No engine/Renderer changes needed; texture fields stay empty so `GpuResources::createTextureImage` (`engine/src/GpuResources.cpp:183-214`) falls back to its checkerboard placeholder.

**Tech Stack:** C++20, GLM, existing `DonTopo::Mesh`/`Vertex` types. No new dependencies. No unit test framework in this repo (confirmed: no test targets in any `CMakeLists.txt`) — verification is build success + visual check via the `sandbox` executable.

## Global Constraints

- Vertex winding must be CCW seen from outside, matching the engine's `VK_FRONT_FACE_COUNTER_CLOCKWISE` + `VK_CULL_MODE_BACK_BIT` pipeline (`engine/src/Renderer.cpp:761-762`). **This exact invariant caused a critical bug in the sibling `Cube` feature** (4 of 6 faces wound backwards, silently culled) — the vertex/triangle order given in Task 1 below has already been verified analytically (cross-product of a generic grid cell against the expected outward normal); do not "simplify" or re-derive the index order without re-verifying winding the same way.
- `Vertex` fields per `engine/include/DonTopo/Vertex.h`: `pos, color, uv, normal, tangent` (all `glm::vec3` except `uv` which is `glm::vec2`).
- `Mesh` fields per `engine/include/DonTopo/Mesh.h`: leave `texturePath`, `embeddedTexture`, `normalMapPath`, `embeddedNormalMap`, `metallicRoughnessPath`, `embeddedMetallicRoughness` at their defaults (empty/unset); leave `metallic = 0.0f`, `roughness = 0.5f`.
- New source file must be added to `engine/CMakeLists.txt`'s `add_library(DonTopoEngine STATIC ...)` list to be compiled.
- Tangent must point along the +U (longitude) direction at each vertex — `normalize(d(pos)/dphi)` — consistent with the fix applied to `Cube` after its own tangent regression (do not default to an arbitrary constant tangent; it must vary with `phi` per vertex, unlike `Cube`'s per-face-constant tangent).

---

### Task 1: Sphere geometry class

**Files:**
- Create: `engine/include/DonTopo/Sphere.h`
- Create: `engine/src/Sphere.cpp`
- Modify: `engine/CMakeLists.txt:1-14` (add `src/Sphere.cpp` to the source list, after `src/Cube.cpp`)

**Interfaces:**
- Produces: `DonTopo::Sphere::create(float radius = 0.5f, uint32_t segments = 32, uint32_t rings = 16, glm::vec3 color = {0.8f, 0.8f, 0.8f}) -> DonTopo::Mesh` — used by Task 2 (`sandbox/src/main.cpp`).

- [ ] **Step 1: Write `Sphere.h`**

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

- [ ] **Step 2: Write `Sphere.cpp`**

```cpp
#include "DonTopo/Sphere.h"
#include <cmath>

namespace DonTopo
{
    Mesh Sphere::create(float radius, uint32_t segments, uint32_t rings, glm::vec3 color)
    {
        Mesh mesh;
        mesh.name = "sphere";

        const float PI = 3.14159265358979323846f;

        for (uint32_t r = 0; r <= rings; ++r)
        {
            float theta = static_cast<float>(r) * PI / static_cast<float>(rings);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            for (uint32_t c = 0; c <= segments; ++c)
            {
                float phi = static_cast<float>(c) * 2.0f * PI / static_cast<float>(segments);
                float sinPhi = std::sin(phi);
                float cosPhi = std::cos(phi);

                glm::vec3 dir{ sinTheta * cosPhi, cosTheta, sinTheta * sinPhi };

                Vertex v{};
                v.pos     = radius * dir;
                v.normal  = dir;
                v.color   = color;
                v.uv      = { static_cast<float>(c) / static_cast<float>(segments),
                               static_cast<float>(r) / static_cast<float>(rings) };
                v.tangent = { -sinPhi, 0.0f, cosPhi };

                mesh.vertices.push_back(v);
            }
        }

        const uint32_t cols = segments + 1;
        for (uint32_t r = 0; r < rings; ++r)
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

- [ ] **Step 3: Register the new source file in the build**

In `engine/CMakeLists.txt`, add `src/Sphere.cpp` to the `add_library(DonTopoEngine STATIC ...)` list, after `src/Cube.cpp`:

```cmake
add_library(DonTopoEngine STATIC
    src/Engine.cpp
    src/Window.cpp
    src/Renderer.cpp
    src/EditorUI.cpp
    src/Skybox.cpp
    src/ModelLoader.cpp
    src/Cube.cpp
    src/Sphere.cpp
    src/Camera.cpp
    src/SceneNode.cpp
    src/AudioManager.cpp
    src/GpuDevice.cpp
    src/GpuResources.cpp
)
```

- [ ] **Step 4: Build to verify it compiles**

Run: `build.bat`
Expected: build succeeds, no errors referencing `Sphere.h`/`Sphere.cpp`.

- [ ] **Step 5: Verify winding is CCW from outside (required — this exact class of bug broke `Cube`)**

By hand or in your own scratch calculation, confirm that for a generic interior grid cell (not a pole row), `cross(v1.pos - v0.pos, v2.pos - v0.pos)` points in the same direction as `v0.normal`, where `v0 = mesh.vertices[i0]`, `v1 = mesh.vertices[i1]`, `v2 = mesh.vertices[i2]` for some `r` in the middle of the range (e.g. `r = rings/2`) and `c = 0`. Show this arithmetic in your report — do not skip it or assume the plan's code is correct without checking, since the sibling `Cube` task's plan code had exactly this bug and it was only caught in review.

Expected: the cross product is a positive scalar multiple of the normal direction (not the reverse).

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Sphere.h engine/src/Sphere.cpp engine/CMakeLists.txt
git commit -m "feat(engine): add Sphere primitive mesh generator"
```

---

### Task 2: Wire a sphere into the sandbox

**Files:**
- Modify: `sandbox/src/main.cpp:5` (include), `:54-56` (mesh vector construction, right after the existing cube block), `:70-71` (post-init transform, right after the existing cube transform)

**Interfaces:**
- Consumes: `DonTopo::Sphere::create(float radius = 0.5f, uint32_t segments = 32, uint32_t rings = 16, glm::vec3 color = {0.8f, 0.8f, 0.8f}) -> DonTopo::Mesh` (Task 1).
- Consumes: `Renderer::setTransform(size_t objectIndex, const glm::mat4& transform)` (existing, `engine/include/DonTopo/Renderer.h:34-38`).

- [ ] **Step 1: Include the header**

In `sandbox/src/main.cpp`, add after the existing `#include "DonTopo/Cube.h"` line (line 5):

```cpp
#include "DonTopo/Sphere.h"
```

- [ ] **Step 2: Add the sphere mesh and remember its index**

In `sandbox/src/main.cpp`, right after the existing cube block (currently ending at line 56 with `meshes.push_back(DonTopo::Cube::create(50.0f));`), add:

```cpp
        // Esfera de prueba (sin textura -> placeholder checkerboard)
        size_t sphereIndex = meshes.size();
        meshes.push_back(DonTopo::Sphere::create(50.0f));
```

- [ ] **Step 3: Position the sphere after `renderer.init`**

Right after the existing cube's `renderer.setTransform(cubeIndex, ...)` call (currently lines 70-71), add:

```cpp
        renderer.setTransform(sphereIndex,
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 50.0f, 200.0f)));
```

(Y = 50 lifts the radius-50 sphere so its base sits near the floor at y=0, same convention as the existing cube; Z = +200 puts it on the opposite side of the scene from the cube (z=-200), clear of both FBX models at x=±200 and the cube.)

- [ ] **Step 4: Build**

Run: `build.bat`
Expected: build succeeds.

- [ ] **Step 5: Smoke-test the executable**

Run: `build-ninja\sandbox\Sandbox.exe` in the background, wait ~5 seconds, confirm the process is still running (did not crash/throw on startup), then terminate it. You cannot visually confirm the sphere's on-screen appearance from this environment (no screenshot tooling for the native Vulkan window) — do not claim visual confirmation; only claim what the smoke test establishes.

- [ ] **Step 6: Commit**

```bash
git add sandbox/src/main.cpp
git commit -m "feat(sandbox): render a Sphere primitive instance"
```
