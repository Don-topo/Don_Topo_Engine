# Camera Component Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `CameraComponent` slot to `GameObject` so any object can be the scene's game camera, rendered from on Play and drawn as a frustum gizmo in edit mode, with at most one camera per scene enforced through `Scene::findCamera()`.

**Architecture:** The component is pure data + math in Core (no Vulkan, no `GameObject` dependency) that builds its own view/projection matrices, so the `Renderer` (Play) and the frustum gizmo (edit) cannot disagree about the framing. The GameObject's `worldTransform` supplies position/orientation; the viewport supplies aspect. `Renderer::currentFrameCamera()` picks the component's camera during Play and the editor fly camera otherwise, never overwriting `m_camera` — so Stop restores the editor view for free.

**Tech Stack:** C++20, glm, nlohmann::json, Dear ImGui, Vulkan, PhysX (tests only), CMake + Ninja + MSVC.

**Spec:** `docs/superpowers/specs/2026-07-16-camera-component-design.md`

## Global Constraints

- **Comments in Castilian Spanish, explaining the WHY**, matching the density and tone of `GameObject.h` / `Scene.h`. Code identifiers stay in English.
- **File scope:** `engine/include/DonTopo/**`, `engine/src/**`, `engine/tests/**`. **Do NOT touch** `sandbox/**`, CI, or `cmake/`. The only build file touched is `engine/tests/CMakeLists.txt` (Task 1), already approved.
- **Do NOT modify `DonTopo::Camera`** (`engine/include/DonTopo/Core/Camera.h`). It is the editor fly camera; the component never routes through it.
- **Do NOT refactor** `GameObject` to a generic component system. Follow the existing `std::shared_ptr<T> m_x` + `setX`/`getX`/`hasX` slot pattern exactly.
- **No Lua bindings** for `CameraComponent`.
- **Serialization is additive:** scene `version` stays at `1`; scenes without a `"camera"` key must keep loading unchanged.
- **Build:** `.\configure.bat` then `.\build.bat` from PowerShell (vcvarsall + Ninja). Never invoke raw `cmake` from Bash.
- **PhysX allows one `PxFoundation` per process.** In tests, create exactly one `PhysicsManager` in `main` and pass it by reference. Never one per test.
- **Defaults** (repo world scale, not Unity's): `fov=45.0f`, `orthographicSize=100.0f`, `near=1.0f`, `far=2000.0f`.

## File Structure

| File | Responsibility |
| --- | --- |
| `engine/include/DonTopo/Core/CameraComponent.h` (new) | Component state + matrix API |
| `engine/src/Core/CameraComponent.cpp` (new) | Clamps, `projectionMatrix`, `viewFromWorld` |
| `engine/tests/camera_tests.cpp` (new) | Headless tests (Core only) |
| `engine/tests/CMakeLists.txt` | Register `dt_camera_tests` |
| `engine/include/DonTopo/Core/GameObject.h` | Component slot |
| `engine/include/DonTopo/Core/Scene.h` + `engine/src/Core/Scene.cpp` | `findCamera`, `lastWarnings`, serialization, prune, clone rule |
| `engine/include/DonTopo/Renderer/Renderer.h` + `.cpp` | `m_scene`, `viewportAspect`, `currentFrameCamera` |
| `engine/include/DonTopo/Editor/Command.h` + `engine/src/Editor/Command.cpp` | `CameraState`, `CameraComponentCommand` |
| `engine/include/DonTopo/Editor/PropertiesPanel.h` + `.cpp` | Camera section + Add gate |
| `engine/include/DonTopo/Editor/ScenePanel.h` + `.cpp` | `createCamera` + context menu items |
| `engine/include/DonTopo/Editor/ViewportPanel.h` + `.cpp` | Frustum gizmo |
| `engine/src/Editor/EditorUI.cpp` | Play warning + drain `lastWarnings` |

**Testability boundary — read this before starting.** Only Core is testable headless (Tasks 1-4, 6). Tasks 5 and 7-10 touch Vulkan/ImGui, which need a GPU and a window: **their only automated gate is that `build.bat` compiles clean**, and their real verification is Task 11, done by hand by the user. Do not write "tests" that fake ImGui, and **never claim a gizmo, a menu or the Play switch "works"** — no agent has a GUI.

---

### Task 1: CameraComponent + test harness

**Files:**
- Create: `engine/include/DonTopo/Core/CameraComponent.h`
- Create: `engine/src/Core/CameraComponent.cpp`
- Create: `engine/tests/camera_tests.cpp`
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `DonTopo::CameraComponent` with `enum class ProjectionMode { Perspective, Orthographic }`; `getMode()/setMode(ProjectionMode)`, `getFov()/setFov(float)`, `getOrthographicSize()/setOrthographicSize(float)`, `getNear()/setNear(float)`, `getFar()/setFar(float)`, `glm::mat4 projectionMatrix(float aspect) const`, `static glm::mat4 viewFromWorld(const glm::mat4& world)`. Test executable `dt_camera_tests`.

**Note on `engine/src/Core/CameraComponent.cpp`:** the engine target globs its sources (`engine/src/**`), so no CMake edit is needed for the new `.cpp` — only `engine/tests/CMakeLists.txt` for the new test executable. Verify the glob assumption by building; if the file is not picked up, stop and report rather than editing the engine's CMakeLists.

- [ ] **Step 1: Write the failing test**

Create `engine/tests/camera_tests.cpp`:

```cpp
// Test headless del CameraComponent y de la serialización/invariante de cámara
// en Scene (sin GUI). Plain main + asserts, sin framework — coherente con
// physics_tests.cpp.
//
// PhysX sólo admite UNA PxFoundation por proceso (crearla dos veces, aunque se
// libere entremedias, crashea). Por eso se crea un único PhysicsManager en
// main() y se pasa por referencia: aquí sólo hace falta porque Scene::fromJson/
// insertFromJson/cloneGameObject lo exigen en su firma para recrear colliders,
// no porque estos tests simulen física.
#include "DonTopo/Core/CameraComponent.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// Defaults a la escala de este repo, no a los de Unity.
static void test_defaults()
{
    CameraComponent c;
    CHECK(c.getMode() == CameraComponent::ProjectionMode::Perspective);
    CHECK(nearlyEqual(c.getFov(), 45.0f));
    CHECK(nearlyEqual(c.getOrthographicSize(), 100.0f));
    CHECK(nearlyEqual(c.getNear(), 1.0f));
    CHECK(nearlyEqual(c.getFar(), 2000.0f));
}

// Los clamps viven en el componente: un JSON editado a mano no puede instalar
// una proyección degenerada.
static void test_clamps()
{
    CameraComponent c;
    c.setFov(0.0f);      CHECK(c.getFov() >= 1.0f);
    c.setFov(500.0f);    CHECK(c.getFov() <= 179.0f);
    c.setOrthographicSize(-5.0f); CHECK(c.getOrthographicSize() > 0.0f);
    c.setNear(-5.0f);    CHECK(c.getNear() > 0.0f);
    // far nunca queda por debajo de near.
    c.setNear(10.0f);
    c.setFar(5.0f);
    CHECK(c.getFar() > c.getNear());
    // near nunca sobrepasa far, y hacerlo no debe mover far.
    CameraComponent d;
    d.setFar(100.0f);
    d.setNear(500.0f);
    CHECK(d.getNear() < d.getFar());
    CHECK(nearlyEqual(d.getFar(), 100.0f));
}

// El Y-flip de Vulkan va DENTRO de projectionMatrix: sus dos consumidores (UBO
// del Renderer y Gizmos::drawFrustum) lo necesitan.
static void test_projection_has_vulkan_y_flip()
{
    CameraComponent c;
    glm::mat4 p = c.projectionMatrix(16.0f / 9.0f);
    CHECK(p[1][1] < 0.0f);
}

// Perspectiva y ortográfica no pueden dar la misma matriz.
static void test_projection_modes_differ()
{
    CameraComponent c;
    glm::mat4 persp = c.projectionMatrix(1.0f);
    c.setMode(CameraComponent::ProjectionMode::Orthographic);
    glm::mat4 ortho = c.projectionMatrix(1.0f);
    CHECK(persp != ortho);
    // En ortográfica, w del punto proyectado es 1 (sin división perspectiva).
    glm::vec4 clip = ortho * glm::vec4(0.0f, 0.0f, -50.0f, 1.0f);
    CHECK(nearlyEqual(clip.w, 1.0f));
}

// Un aspect degenerado (viewport de ancho 0 al minimizar) no debe producir NaN.
static void test_projection_degenerate_aspect()
{
    CameraComponent c;
    glm::mat4 p = c.projectionMatrix(0.0f);
    CHECK(!std::isnan(p[0][0]));
}

// La cámara mira a -Z local (convención de glm/lookAt y de DonTopo::Camera,
// cuyo yaw por defecto de -90° da front = (0,0,-1)).
static void test_view_from_world_translation()
{
    glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
    glm::mat4 view  = CameraComponent::viewFromWorld(world);
    // El origen del mundo queda 10 unidades delante de la cámara, o sea en -Z.
    glm::vec4 p = view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(nearlyEqual(p.x, 0.0f));
    CHECK(nearlyEqual(p.y, 0.0f));
    CHECK(nearlyEqual(p.z, -10.0f));
}

// La escala del GameObject NO debe entrar en la view (deformaría la imagen).
static void test_view_from_world_ignores_scale()
{
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
    glm::mat4 unscaled = CameraComponent::viewFromWorld(t);
    glm::mat4 scaled   = CameraComponent::viewFromWorld(t * glm::scale(glm::mat4(1.0f), glm::vec3(5.0f)));
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK(nearlyEqual(unscaled[col][row], scaled[col][row]));
}

int main()
{
    test_defaults();
    test_clamps();
    test_projection_has_vulkan_y_flip();
    test_projection_modes_differ();
    test_projection_degenerate_aspect();
    test_view_from_world_translation();
    test_view_from_world_ignores_scale();
    if (g_failures == 0) std::printf("ALL CAMERA TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

Register it in `engine/tests/CMakeLists.txt` — add after the `dt_content_browser_tests` block:

```cmake
add_executable(dt_camera_tests camera_tests.cpp)
target_link_libraries(dt_camera_tests PRIVATE DonTopoEngine)
target_compile_features(dt_camera_tests PRIVATE cxx_std_20)
```

and add `dt_camera_tests` to the existing fmod.dll `foreach` list, which becomes:

```cmake
        foreach(_dt_test_target dt_physics_tests dt_content_browser_tests dt_camera_tests)
```

- [ ] **Step 2: Run the build to verify the test fails to compile**

```powershell
.\configure.bat
.\build.bat
```

Expected: FAIL — `Cannot open include file: 'DonTopo/Core/CameraComponent.h'`.

- [ ] **Step 3: Write the implementation**

Create `engine/include/DonTopo/Core/CameraComponent.h`:

```cpp
#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    // Componente de cámara de juego (equivalente a Unity Camera). NO guarda
    // posición ni orientación: las da el worldTransform del GameObject dueño,
    // así que mover el objeto mueve la cámara. Tampoco guarda aspect ratio —
    // lo dicta el viewport (Renderer::viewportAspect), pa que redimensionar la
    // ventana no deforme la imagen (no hay letterboxing hoy).
    //
    // Data pura: sin Vulkan y sin conocer GameObject, misma regla que Rigidbody
    // (la dependencia va Core -> resto, nunca al revés).
    //
    // El componente construye sus propias matrices a propósito: el Renderer (en
    // Play) y el gizmo de frustum (en edición) tienen que coincidir, o el
    // wireframe dibujado mentiría sobre lo que se ve al dar a Play.
    class CameraComponent
    {
        public:
            enum class ProjectionMode { Perspective, Orthographic };

            CameraComponent() = default;

            ProjectionMode getMode() const { return m_mode; }
            void setMode(ProjectionMode mode) { m_mode = mode; }

            // Los clamps viven aquí (y no en la UI) pa que un .scene editado a
            // mano tampoco pueda instalar una proyección degenerada.
            float getFov() const { return m_fov; }              // grados, solo perspectiva
            void  setFov(float degrees);
            float getOrthographicSize() const { return m_orthographicSize; } // semi-altura mundo, solo ortográfica
            void  setOrthographicSize(float size);
            float getNear() const { return m_near; }
            void  setNear(float n);
            float getFar() const { return m_far; }
            void  setFar(float f);

            // Proyección pa el aspect dado, con el Y-flip de Vulkan YA aplicado
            // (proj[1][1] *= -1): sus dos consumidores lo necesitan, y dejárselo
            // al caller invitaba a que uno de los dos se lo olvidara.
            glm::mat4 projectionMatrix(float aspect) const;

            // View de una cámara colocada en world, mirando a -Z local (misma
            // convención que glm::lookAt y que DonTopo::Camera, cuyo yaw por
            // defecto de -90° da front = (0,0,-1)). Normaliza los ejes antes de
            // invertir: un GameObject escalado hornearía su escala en la view y
            // deformaría la imagen. static porque no depende del estado del
            // componente — la comparten Renderer, gizmo y tests.
            static glm::mat4 viewFromWorld(const glm::mat4& world);

        private:
            ProjectionMode m_mode = ProjectionMode::Perspective;
            // Defaults a la escala de este repo (primitivas de 50 unidades,
            // sandbox coloca la cámara a z=300), no a los de Unity (0.3/1000).
            float m_fov              = 45.0f;
            float m_orthographicSize = 100.0f;
            float m_near             = 1.0f;
            float m_far              = 2000.0f;
    };
}
```

Create `engine/src/Core/CameraComponent.cpp`:

```cpp
#include "DonTopo/Core/CameraComponent.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace DonTopo
{
    namespace
    {
        // Separación mínima entre near y far (y mínimo absoluto de near):
        // glm::perspective diverge con near == 0 y con near == far.
        constexpr float kMinNear = 0.001f;
    }

    void CameraComponent::setFov(float degrees)
    {
        m_fov = glm::clamp(degrees, 1.0f, 179.0f);
    }

    void CameraComponent::setOrthographicSize(float size)
    {
        m_orthographicSize = std::max(size, kMinNear);
    }

    void CameraComponent::setNear(float n)
    {
        // Se clampa contra el far actual en vez de empujar far: un setter no
        // debe cambiar el otro campo a espaldas del caller. Ojo al cargar de
        // JSON: hay que llamar a setFar ANTES que a setNear (ver Scene.cpp).
        m_near = glm::clamp(n, kMinNear, m_far - kMinNear);
    }

    void CameraComponent::setFar(float f)
    {
        m_far = std::max(f, m_near + kMinNear);
    }

    glm::mat4 CameraComponent::projectionMatrix(float aspect) const
    {
        // Aspect degenerado (viewport de ancho/alto 0 al minimizar la ventana)
        // metería NaN en la matriz; 1.0 es un repliegue inocuo pa ese frame.
        if (!(aspect > 0.0f))
            aspect = 1.0f;

        glm::mat4 proj;
        if (m_mode == ProjectionMode::Orthographic)
        {
            const float halfHeight = m_orthographicSize;
            const float halfWidth  = halfHeight * aspect;
            proj = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, m_near, m_far);
        }
        else
        {
            proj = glm::perspective(glm::radians(m_fov), aspect, m_near, m_far);
        }
        proj[1][1] *= -1.0f; // Vulkan Y flip
        return proj;
    }

    glm::mat4 CameraComponent::viewFromWorld(const glm::mat4& world)
    {
        const glm::vec3 right   = glm::vec3(world[0]);
        const glm::vec3 up      = glm::vec3(world[1]);
        const glm::vec3 forward = glm::vec3(world[2]);

        // Base degenerada (algún eje con escala 0): invertir daría NaN y
        // ensuciaría todo el frame. La identidad al menos deja ver algo.
        if (glm::length(right) < 1e-6f || glm::length(up) < 1e-6f || glm::length(forward) < 1e-6f)
            return glm::mat4(1.0f);

        glm::mat4 unscaled(1.0f);
        unscaled[0] = glm::vec4(glm::normalize(right), 0.0f);
        unscaled[1] = glm::vec4(glm::normalize(up), 0.0f);
        unscaled[2] = glm::vec4(glm::normalize(forward), 0.0f);
        unscaled[3] = world[3]; // posición intacta
        return glm::inverse(unscaled);
    }
}
```

- [ ] **Step 4: Build and run the test**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_camera_tests.exe
```

Expected: `ALL CAMERA TESTS PASSED`, exit code 0.

- [ ] **Step 5: Verify the existing tests still pass**

```powershell
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
```

Expected: `ALL PHYSICS TESTS PASSED` and the content browser suite's pass line.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Core/CameraComponent.h engine/src/Core/CameraComponent.cpp engine/tests/camera_tests.cpp engine/tests/CMakeLists.txt
git commit -m "feat(core): add CameraComponent with projection and view matrices

The component builds its own matrices so the Renderer and the frustum gizmo
cannot disagree about the framing. Clamps live in the setters so a hand-edited
scene cannot install a degenerate projection.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: GameObject slot + Scene::findCamera()

**Files:**
- Modify: `engine/include/DonTopo/Core/GameObject.h`
- Modify: `engine/include/DonTopo/Core/Scene.h`
- Modify: `engine/src/Core/Scene.cpp`
- Test: `engine/tests/camera_tests.cpp`

**Interfaces:**
- Consumes: `CameraComponent` (Task 1).
- Produces: `GameObject::setCameraComponent(std::shared_ptr<CameraComponent>)`, `GameObject::getCameraComponent() const` (returns `const std::shared_ptr<CameraComponent>&`), `GameObject::hasCameraComponent() const`; `Scene::findCamera()` returning `GameObject*` and a `const` overload returning `const GameObject*`.

- [ ] **Step 1: Write the failing test**

Add to `engine/tests/camera_tests.cpp`, before `main`:

```cpp
// findCamera() es la ÚNICA fuente de verdad del invariante "una cámara por
// escena": tiene que encontrarla esté donde esté, no solo colgando de la raíz.
static void test_find_camera_at_any_depth()
{
    Scene scene("Test");
    CHECK(scene.findCamera() == nullptr);

    GameObject* parent = scene.addGameObject("Parent");
    GameObject* child  = scene.addGameObject("Child", parent);
    GameObject* nieto  = scene.addGameObject("Nieto", child);
    nieto->setCameraComponent(std::make_shared<CameraComponent>());

    CHECK(scene.findCamera() == nieto);
}

// La cámara puede vivir en CUALQUIER GameObject, no solo en uno llamado
// "Camera".
static void test_find_camera_ignores_name()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("CualquierNombre");
    go->setCameraComponent(std::make_shared<CameraComponent>());
    CHECK(scene.findCamera() == go);
    CHECK(scene.findCamera()->hasCameraComponent());
}

// Pre-orden: gana la primera en el recorrido, no una cualquiera.
static void test_find_camera_returns_first_in_preorder()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    a->setCameraComponent(std::make_shared<CameraComponent>());
    b->setCameraComponent(std::make_shared<CameraComponent>());
    CHECK(scene.findCamera() == a);
}
```

Add the include at the top of `camera_tests.cpp` (next to the existing one):

```cpp
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include <memory>
```

And register them in `main`, before the final `if`:

```cpp
    test_find_camera_at_any_depth();
    test_find_camera_ignores_name();
    test_find_camera_returns_first_in_preorder();
```

- [ ] **Step 2: Build to verify it fails**

```powershell
.\build.bat
```

Expected: FAIL — `'setCameraComponent': is not a member of 'DonTopo::GameObject'` and `'findCamera': is not a member of 'DonTopo::Scene'`.

- [ ] **Step 3: Add the GameObject slot**

In `engine/include/DonTopo/Core/GameObject.h`, add the include next to the other component includes:

```cpp
#include "DonTopo/Core/CameraComponent.h"
```

Add the accessors after the `getAudioClip` block (before the scripts block):

```cpp
            // Cámara de juego: al dar a Play el Renderer renderiza desde este
            // GameObject (su worldTransform da posición y orientación). Como
            // mucho una por escena — el invariante lo impone Scene::findCamera,
            // no esta clase, igual que la exclusividad de colliders la impone
            // el editor.
            void setCameraComponent(std::shared_ptr<CameraComponent> camera) { m_cameraComponent = std::move(camera); }
            const std::shared_ptr<CameraComponent>& getCameraComponent() const { return m_cameraComponent; }
            bool hasCameraComponent() const { return m_cameraComponent != nullptr; }
```

Add the member next to `m_audioClip`:

```cpp
            std::shared_ptr<CameraComponent> m_cameraComponent;
```

- [ ] **Step 4: Add Scene::findCamera()**

In `engine/include/DonTopo/Core/Scene.h`, declare it after `findById`:

```cpp
            // Única fuente de verdad del invariante "como mucho una cámara por
            // escena": la buscan el gate de "Add" de Properties, el menú
            // contextual del panel Scene, el switch de cámara del Renderer y el
            // aviso al dar a Play — ninguno guarda estado propio. Pre-orden
            // desde la raíz (gana la primera), nullptr si no hay ninguna. O(n)
            // sobre el árbol, igual que findById.
            GameObject* findCamera();
            const GameObject* findCamera() const;
```

In `engine/src/Core/Scene.cpp`, implement next to `findById`:

```cpp
    GameObject* Scene::findCamera()
    {
        GameObject* found = nullptr;
        // traverse es pre-orden (fn(this) antes que los hijos) y no permite
        // early-exit: el guard de !found deja ganar a la primera igualmente.
        m_root.traverse([&](GameObject* n) {
            if (!found && n->hasCameraComponent()) found = n;
        });
        return found;
    }

    const GameObject* Scene::findCamera() const
    {
        // traverse es non-const (template en GameObject); el const_cast se
        // queda contenido aquí y la versión const no muta nada.
        return const_cast<Scene*>(this)->findCamera();
    }
```

- [ ] **Step 5: Build and run the tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_camera_tests.exe
```

Expected: `ALL CAMERA TESTS PASSED`, exit code 0.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Core/GameObject.h engine/include/DonTopo/Core/Scene.h engine/src/Core/Scene.cpp engine/tests/camera_tests.cpp
git commit -m "feat(core): add CameraComponent slot and Scene::findCamera

findCamera is the single source of truth for the one-camera-per-scene
invariant: every gate asks it instead of keeping its own state.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Serialization round-trip

**Files:**
- Modify: `engine/src/Core/Scene.cpp` (`nodeToJson` ~:54, `nodeFromJson` ~:209)
- Test: `engine/tests/camera_tests.cpp`

**Interfaces:**
- Consumes: `CameraComponent` (Task 1), `GameObject::setCameraComponent`/`getCameraComponent`/`hasCameraComponent`, `Scene::findCamera` (Task 2).
- Produces: a `"camera"` JSON block read/written by `nodeToJson`/`nodeFromJson`, which covers all five paths (`toJson`, `subtreeToJson`, `fromJson`, `insertFromJson`, `cloneGameObject`).

**Why one block covers five paths:** `toJson` (:552), `subtreeToJson` (:451), `fromJson` (:584), `insertFromJson` (:461) and `cloneGameObject` (:422) all funnel through these two functions. Verified — do not add per-path code.

- [ ] **Step 1: Write the failing test**

Add to `engine/tests/camera_tests.cpp`, before `main`:

```cpp
// Round-trip completo por toJson/fromJson. Los valores NO son los defaults a
// propósito: unos defaults se "preservarían" solos aunque el bloque no se
// serializara. near/far grandes cubren además el orden de carga (setNear clampa
// contra el far actual, así que far tiene que cargarse antes).
static void test_serialization_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Observador");
    auto cam = std::make_shared<CameraComponent>();
    cam->setMode(CameraComponent::ProjectionMode::Orthographic);
    cam->setFar(8000.0f);
    cam->setNear(3000.0f);
    cam->setFov(70.0f);
    cam->setOrthographicSize(250.0f);
    go->setCameraComponent(cam);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findCamera();
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Observador");
    const auto& c = found->getCameraComponent();
    CHECK(c->getMode() == CameraComponent::ProjectionMode::Orthographic);
    CHECK(nearlyEqual(c->getFov(), 70.0f));
    CHECK(nearlyEqual(c->getOrthographicSize(), 250.0f));
    CHECK(nearlyEqual(c->getNear(), 3000.0f));
    CHECK(nearlyEqual(c->getFar(), 8000.0f));
}

// Camino de subtreeToJson/insertFromJson — el que usan los comandos de
// Undo/Redo. Sin él, un Undo de Delete devolvería el GameObject sin su cámara.
static void test_subtree_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("ConCamara");
    auto cam = std::make_shared<CameraComponent>();
    cam->setFov(33.0f);
    go->setCameraComponent(cam);

    nlohmann::json snapshot = scene.subtreeToJson(go);
    scene.removeGameObject(go);
    CHECK(scene.findCamera() == nullptr);

    GameObject* restored = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(restored != nullptr);
    if (!restored) return;
    CHECK(restored->hasCameraComponent());
    CHECK(nearlyEqual(restored->getCameraComponent()->getFov(), 33.0f));
}

// Back-compat: las escenas guardadas antes de este cambio no traen bloque
// "camera" y tienen que cargar igual (version sigue en 1).
static void test_scene_without_camera_block_still_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    scene.addGameObject("Cubo");
    nlohmann::json j = scene.toJson();
    CHECK(!j["root"]["children"][0].contains("camera"));

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.findCamera() == nullptr);
    CHECK(loaded.getRoot().children.size() == 1);
}
```

Add these includes at the top of `camera_tests.cpp`:

```cpp
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include <nlohmann/json.hpp>
```

Change `main` to create the single shared managers and pass them by reference:

```cpp
int main()
{
    // Una sola PxFoundation por proceso: un único PhysicsManager compartido
    // por todos los tests, nunca uno por test. Aquí physics/audio solo hacen
    // falta porque Scene::fromJson/insertFromJson/cloneGameObject los exigen
    // en su firma pa recrear colliders y clips — estos tests no simulan nada.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    test_defaults();
    test_clamps();
    test_projection_has_vulkan_y_flip();
    test_projection_modes_differ();
    test_projection_degenerate_aspect();
    test_view_from_world_translation();
    test_view_from_world_ignores_scale();
    test_find_camera_at_any_depth();
    test_find_camera_ignores_name();
    test_find_camera_returns_first_in_preorder();
    test_serialization_round_trip(pm, am);
    test_subtree_round_trip(pm, am);
    test_scene_without_camera_block_still_loads(pm, am);

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL CAMERA TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Build and run to verify it fails**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_camera_tests.exe
```

Expected: FAIL — `test_serialization_round_trip` reports `found != nullptr` failing (the component is not serialized yet).

- [ ] **Step 3: Write the serialization**

In `engine/src/Core/Scene.cpp`, add the include next to the other component includes:

```cpp
#include "DonTopo/Core/CameraComponent.h"
```

Add `using DonTopo::CameraComponent;` next to the existing `using DonTopo::Rigidbody;` in the anonymous namespace.

In `nodeToJson`, after the `rigidbody` block:

```cpp
        if (node.hasCameraComponent())
        {
            const auto& c = node.getCameraComponent();
            // "mode" como string y no como int del enum: legible en un .scene
            // editado a mano y estable si el enum crece por el medio.
            j["camera"] = { {"mode", c->getMode() == CameraComponent::ProjectionMode::Orthographic
                                         ? "orthographic" : "perspective"},
                            {"fov", c->getFov()},
                            {"orthographicSize", c->getOrthographicSize()},
                            {"near", c->getNear()},
                            {"far", c->getFar()} };
        }
```

In `nodeFromJson`, after the rigidbody block and before the `audioClip` block:

```cpp
        // Bloque aditivo: las escenas guardadas antes de este campo no lo traen
        // y cargan igual (version sigue en 1). Valor de "mode" desconocido ->
        // perspective.
        if (j.contains("camera"))
        {
            const auto& c = j["camera"];
            auto cam = std::make_shared<CameraComponent>();
            cam->setMode(c.value("mode", std::string("perspective")) == "orthographic"
                             ? CameraComponent::ProjectionMode::Orthographic
                             : CameraComponent::ProjectionMode::Perspective);
            // far ANTES que near: setNear clampa contra el far ACTUAL, así que
            // cargarlos al revés recortaría un near grande contra el far por
            // defecto (2000) y lo dejaría mal.
            cam->setFar(c.value("far", 2000.0f));
            cam->setNear(c.value("near", 1.0f));
            cam->setFov(c.value("fov", 45.0f));
            cam->setOrthographicSize(c.value("orthographicSize", 100.0f));
            node->setCameraComponent(cam);
        }
```

- [ ] **Step 4: Build and run the tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_camera_tests.exe
```

Expected: `ALL CAMERA TESTS PASSED`, exit code 0.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/camera_tests.cpp
git commit -m "feat(core): serialize CameraComponent

nodeToJson/nodeFromJson feed all five paths (toJson, subtreeToJson, fromJson,
insertFromJson, cloneGameObject), so one block covers them all. Additive:
version stays at 1 and scenes without the key load unchanged.

far is loaded before near because setNear clamps against the current far.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Enforce the invariant on load and clone

**Files:**
- Modify: `engine/include/DonTopo/Core/Scene.h`
- Modify: `engine/src/Core/Scene.cpp` (`fromJson` ~:565, `cloneGameObject` ~:416)
- Test: `engine/tests/camera_tests.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: `Scene::lastWarnings()` returning `const std::vector<std::string>&`; private `Scene::pruneExtraCameras()`; `fromJson` keeps only the first camera; `cloneGameObject` never keeps a `CameraComponent`.

- [ ] **Step 1: Write the failing test**

Add to `engine/tests/camera_tests.cpp`, before `main`:

```cpp
// Escena con DOS cámaras (JSON editado a mano): gana la primera en pre-orden,
// la otra pierde SOLO el componente (su GameObject se conserva) y queda aviso.
// Así un .scene recuperable se abre igual, en vez de fallar la carga.
static void test_load_with_two_cameras_keeps_first(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("Primera");
    GameObject* b = scene.addGameObject("Segunda");
    a->setCameraComponent(std::make_shared<CameraComponent>());
    b->setCameraComponent(std::make_shared<CameraComponent>());
    // toJson serializa las dos: el invariante lo impone la carga, que es donde
    // puede llegar un fichero editado a mano.
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    int cameraCount = 0;
    loaded.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++cameraCount; });
    CHECK(cameraCount == 1);

    GameObject* cam = loaded.findCamera();
    CHECK(cam != nullptr);
    if (cam) CHECK(cam->name == "Primera");
    // Los dos GameObjects siguen ahí: solo se cae el componente sobrante.
    CHECK(loaded.getRoot().children.size() == 2);
    CHECK(!loaded.lastWarnings().empty());
}

// Una escena con UNA cámara no genera avisos (el prune no es un falso positivo).
static void test_load_with_one_camera_has_no_warnings(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    scene.addGameObject("Solo")->setCameraComponent(std::make_shared<CameraComponent>());
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.findCamera() != nullptr);
    CHECK(loaded.lastWarnings().empty());
}

// Clonar un GameObject con cámara NO puede dar dos cámaras. Su único caller es
// Instantiate de Lua, que corre en Play: ningún gate de UI puede evitarlo, así
// que la regla vive en Scene.
static void test_clone_never_keeps_camera(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Camara");
    go->setCameraComponent(std::make_shared<CameraComponent>());

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(!clone->hasCameraComponent());
    CHECK(!scene.lastWarnings().empty());
    // El original conserva la suya y sigue siendo LA cámara de la escena.
    CHECK(go->hasCameraComponent());
    CHECK(scene.findCamera() == go);

    int cameraCount = 0;
    scene.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++cameraCount; });
    CHECK(cameraCount == 1);
}

// La cámara puede estar en un descendiente del subárbol clonado, no solo en su
// raíz.
static void test_clone_strips_camera_from_descendant(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* parent = scene.addGameObject("Padre");
    GameObject* child  = scene.addGameObject("Hijo", parent);
    child->setCameraComponent(std::make_shared<CameraComponent>());

    GameObject* clone = scene.cloneGameObject(parent, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    int cameraCount = 0;
    scene.traverse([&](GameObject* n) { if (n->hasCameraComponent()) ++cameraCount; });
    CHECK(cameraCount == 1);
}
```

Register them in `main` after `test_scene_without_camera_block_still_loads(pm, am);`:

```cpp
    test_load_with_two_cameras_keeps_first(pm, am);
    test_load_with_one_camera_has_no_warnings(pm, am);
    test_clone_never_keeps_camera(pm, am);
    test_clone_strips_camera_from_descendant(pm, am);
```

- [ ] **Step 2: Build and run to verify it fails**

```powershell
.\build.bat
```

Expected: FAIL — `'lastWarnings': is not a member of 'DonTopo::Scene'`.

- [ ] **Step 3: Add the warning channel and the prune declaration**

In `engine/include/DonTopo/Core/Scene.h`, add `#include <vector>` at the top, then declare after `findCamera`:

```cpp
            // Avisos de la última operación que tuvo que corregir el invariante
            // de una cámara por escena (fromJson con varias, cloneGameObject de
            // una cámara). Core no conoce el Log Console: EditorUI los vuelca
            // tras cargar. Se limpian al principio de cada operación que los
            // pueda rellenar, así que nunca crecen sin control.
            //
            // OJO: hoy solo los drena el editor tras cargar escena. El aviso del
            // clone (Instantiate de Lua, en Play) no tiene consumidor de Log —
            // queda registrado pa los tests y pa un futuro consumidor.
            const std::vector<std::string>& lastWarnings() const { return m_warnings; }
```

In the `private:` section, after `GameObject m_root;`:

```cpp
            // Impone el invariante de una cámara por escena tras reconstruir el
            // árbol: se queda con la primera en pre-orden y le quita el
            // CameraComponent al resto (el GameObject se conserva — solo se cae
            // el componente). Así un .scene editado a mano con dos cámaras se
            // abre igual, con aviso, en vez de fallar la carga o quedar en un
            // estado donde findCamera() decide sobre una escena incoherente.
            void pruneExtraCameras();

            std::vector<std::string> m_warnings;
```

- [ ] **Step 4: Implement the prune and wire it into fromJson**

In `engine/src/Core/Scene.cpp`, add next to `findCamera`:

```cpp
    void Scene::pruneExtraCameras()
    {
        GameObject* first = nullptr;
        m_root.traverse([&](GameObject* n) {
            if (!n->hasCameraComponent()) return;
            if (!first) { first = n; return; }
            m_warnings.push_back("Escena con más de una cámara: se descarta la de '" + n->name +
                                  "' (se conserva la de '" + first->name + "')");
            n->setCameraComponent(nullptr);
        });
    }
```

In `Scene::fromJson`, clear the warnings at the very top of the function (before the `version`/`root` validation, so a rejected load does not leave stale warnings around):

```cpp
        m_warnings.clear();
```

Then, at the end of the function — **after** `m_root = std::move(newRoot);` and after the existing re-parent fix-up, and **before** the final `return true;` — add:

```cpp
        // Tras reconstruir: el fichero puede traer dos cámaras (editado a mano).
        pruneExtraCameras();
```

**Important:** locate the existing `return true;` at the end of `fromJson` and place the call immediately before it. Read the surrounding code before editing — do not guess the line number.

- [ ] **Step 5: Implement the clone rule**

In `Scene::cloneGameObject` (~:435), replace the existing render-index traverse:

```cpp
        clone->traverse([](GameObject* n) {
            n->staticRenderIndex  = -1;
            n->skinnedRenderIndex = -1;
        });
```

with:

```cpp
        m_warnings.clear();
        clone->traverse([&](GameObject* n) {
            n->staticRenderIndex  = -1;
            n->skinnedRenderIndex = -1;
            // El clon nunca se lleva el CameraComponent: al clonar, el original
            // sigue vivo con su cámara, así que findCamera() ya es no-nulo y el
            // clon rompería el invariante. Determinista, no condicional. Su
            // único caller es Instantiate de Lua (ScriptBindings.cpp), que corre
            // en Play — ningún gate de la UI puede evitarlo, por eso la regla
            // vive aquí.
            if (n->hasCameraComponent())
            {
                n->setCameraComponent(nullptr);
                m_warnings.push_back("Clone de '" + n->name +
                                      "': se descarta el CameraComponent (ya hay una cámara en la escena)");
            }
        });
```

- [ ] **Step 6: Build and run the tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
```

Expected: all three pass.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Core/Scene.h engine/src/Core/Scene.cpp engine/tests/camera_tests.cpp
git commit -m "feat(core): enforce one camera per scene on load and clone

A hand-edited scene with two cameras keeps the first in pre-order and drops the
extra component (the GameObject survives), with a warning, instead of failing
the load. Clones never keep the component: the source stays alive with its
camera, so a clone could never legally keep it. Lua's Instantiate is the only
caller and it runs during Play, so the rule belongs in Scene, not in a UI gate.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Renderer switches camera on Play

**Files:**
- Modify: `engine/include/DonTopo/Renderer/Renderer.h`
- Modify: `engine/src/Renderer/Renderer.cpp` (`recordCommandBuffer` ~:653, `updateUniformBuffer` ~:1184)

**Interfaces:**
- Consumes: `CameraComponent::viewFromWorld`, `CameraComponent::projectionMatrix` (Task 1), `Scene::findCamera` (Task 2).
- Produces: `Renderer::viewportAspect() const` returning `float` (public — the gizmo in Task 9 needs it); private `Renderer::FrameCamera { glm::mat4 view; glm::mat4 proj; glm::vec3 eye; }` and `Renderer::currentFrameCamera() const`.

**No headless test exists for this task** (it needs a Vulkan device and a window). Its gate is a clean compile; behaviour is verified by the user in Task 11. Do not claim the Play switch works.

- [ ] **Step 1: Add the members and the declarations**

In `engine/include/DonTopo/Renderer/Renderer.h`, add the include next to the other Core include:

```cpp
#include "DonTopo/Core/CameraComponent.h"
```

Replace the existing `setScene` (line ~45):

```cpp
            void setScene(Scene* scene) { m_editorUI.setScene(scene); }
```

with:

```cpp
            // Guarda la escena además del passthrough al EditorUI:
            // currentFrameCamera() necesita preguntarle por su cámara cada
            // frame (Scene::findCamera es la única fuente de verdad).
            void setScene(Scene* scene) { m_scene = scene; m_editorUI.setScene(scene); }

            // Aspect del render target. Público porque el gizmo de frustum
            // (ViewportPanel) tiene que usar EXACTAMENTE el mismo que usará la
            // proyección de Play, o dibujaría un encuadre que no se corresponde.
            float viewportAspect() const
            {
                return m_swapChainExtent.height > 0
                    ? (float)m_swapChainExtent.width / (float)m_swapChainExtent.height
                    : 1.0f;
            }
```

In the `private:` section, next to the other struct definitions:

```cpp
            // Cámara efectiva de un frame. eye va aquí porque ubo.viewPos
            // alimenta el specular: sin él, en Play los brillos se calcularían
            // desde la posición de la cámara del editor.
            struct FrameCamera {
                glm::mat4 view;
                glm::mat4 proj;
                glm::vec3 eye;
            };

            // La del CameraComponent en Play (si la escena tiene una), la de
            // vuelo del editor en cualquier otro caso. Único sitio donde se
            // decide: antes la proyección estaba duplicada a pelo en
            // recordCommandBuffer y updateUniformBuffer.
            FrameCamera currentFrameCamera() const;
```

Add the member next to `m_sceneRoot`:

```cpp
            Scene* m_scene = nullptr;
```

- [ ] **Step 2: Implement currentFrameCamera**

In `engine/src/Renderer/Renderer.cpp`, make sure these includes are present at the top (add only the missing ones):

```cpp
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/CameraComponent.h"
```

Add the implementation right after `Renderer::setCamera` (~:250):

```cpp
    Renderer::FrameCamera Renderer::currentFrameCamera() const
    {
        const float aspect = viewportAspect();

        // Edición: la proyección de siempre (45° fijos + near/far derivados de
        // m_cameraDistance). No se toca a propósito — el componente solo manda
        // en Play, así que el editor no cambia de look.
        FrameCamera fc{ m_viewMatrix,
                        glm::perspective(glm::radians(45.0f), aspect,
                                          m_cameraDistance * 0.001f, m_cameraDistance * 3.0f),
                        m_camera.getPos() };
        fc.proj[1][1] *= -1.0f; // Vulkan Y flip

        // Play con cámara en escena: manda el CameraComponent. m_camera y
        // m_viewMatrix NO se tocan nunca — siguen siendo los del editor, así que
        // al parar Play la vista vuelve sola, sin guardar ni restaurar estado
        // (y sin que main.cpp, que llama a setCamera cada frame, se entere).
        // Sin cámara en escena se cae al repliegue de arriba; el aviso al Log lo
        // da EditorUI al arrancar Play, no aquí (esto corre cada frame).
        if (m_editorUI.isPlaying() && m_scene)
        {
            if (GameObject* cam = m_scene->findCamera())
            {
                const auto& c = cam->getCameraComponent();
                fc.view = CameraComponent::viewFromWorld(cam->worldTransform);
                fc.proj = c->projectionMatrix(aspect);
                fc.eye  = glm::vec3(cam->worldTransform[3]);
            }
        }
        return fc;
    }
```

- [ ] **Step 3: Use it in recordCommandBuffer**

In `recordCommandBuffer` (~:652), replace:

```cpp
            // Proyección compartida por skybox y gizmos (mismo pass, misma cámara).
            glm::mat4 proj = glm::perspective(
                glm::radians(45.0f),
                (float)m_swapChainExtent.width / (float)m_swapChainExtent.height,
                m_cameraDistance * 0.001f, m_cameraDistance * 3.0f);
            proj[1][1] *= -1.0f;
```

with:

```cpp
            // Proyección compartida por skybox y gizmos (mismo pass, misma
            // cámara). El Y-flip ya viene aplicado desde currentFrameCamera().
            const FrameCamera fc = currentFrameCamera();
            const glm::mat4 proj = fc.proj;
```

Then replace the **two** remaining `m_viewMatrix` uses in that block. The skybox (~:662):

```cpp
                glm::mat4 rotView    = glm::mat4(glm::mat3(m_viewMatrix)); // sin traslación
```

becomes:

```cpp
                glm::mat4 rotView    = glm::mat4(glm::mat3(fc.view)); // sin traslación
```

and the Gizmos call (~:669):

```cpp
                Gizmos::draw(m_commandBuffers[m_currentFrame], proj * m_viewMatrix, m_currentFrame);
```

becomes:

```cpp
                Gizmos::draw(m_commandBuffers[m_currentFrame], proj * fc.view, m_currentFrame);
```

Both matter: skipping the skybox one would leave the sky rotating with the editor camera during Play while the scene renders from the game camera.

- [ ] **Step 4: Use it in updateUniformBuffer**

In `updateUniformBuffer` (~:1184), replace:

```cpp
        UniformBufferObject ubo{};
        ubo.view = m_viewMatrix;        

        ubo.proj = glm::perspective(
            glm::radians(45.0f),
            (float)m_swapChainExtent.width / (float)m_swapChainExtent.height,
            m_cameraDistance * 0.001f,
            m_cameraDistance * 3.0f);
        ubo.proj[1][1] *= -1.0f;    // Vulkan Y flip
```

with:

```cpp
        // Cámara del CameraComponent en Play, la del editor en edición. El
        // Y-flip de Vulkan ya viene aplicado desde currentFrameCamera().
        const FrameCamera fc = currentFrameCamera();
        UniformBufferObject ubo{};
        ubo.view = fc.view;
        ubo.proj = fc.proj;
```

And replace the `viewPos` line (~:1199):

```cpp
        ubo.viewPos  = glm::vec4(m_camera.getPos(), 1.0f);
```

with:

```cpp
        ubo.viewPos  = glm::vec4(fc.eye, 1.0f);
```

- [ ] **Step 5: Build**

```powershell
.\build.bat
```

Expected: compiles clean, no warnings about the removed `proj[1][1]` flips.

- [ ] **Step 6: Verify the headless tests still pass**

```powershell
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
```

Expected: all three pass.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp
git commit -m "feat(renderer): render from the scene camera during Play

currentFrameCamera() unifies the two duplicated projection sites and picks the
CameraComponent's camera while playing. m_camera/m_viewMatrix are never
overwritten, so Stop restores the editor view with no save/restore step and
sandbox/main.cpp stays untouched.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: CameraState + CameraComponentCommand

**Files:**
- Modify: `engine/include/DonTopo/Editor/Command.h`
- Modify: `engine/src/Editor/Command.cpp`
- Test: `engine/tests/camera_tests.cpp`

**Interfaces:**
- Consumes: `CameraComponent` (Task 1), `Scene::findById` (existing), `GameObject::setCameraComponent` (Task 2).
- Produces: `struct CameraState { CameraComponent::ProjectionMode mode; float fov; float orthographicSize; float nearPlane; float farPlane; };` and `class CameraComponentCommand : public ICommand` with ctor `(Scene& scene, std::string label, uint64_t id, bool add, CameraState state)`.

**Why this command exists** (do not "simplify" it away): Properties' Add actions for colliders/Rigidbody do not go through Undo. That allows a real two-camera state — Add camera to `X`, delete `X` (snapshot keeps the camera), add camera to `Z` (allowed, `findCamera()` is null), Ctrl+Z restores `X` **with its camera**. Putting the Add on the stack makes the ordering enforce the invariant.

- [ ] **Step 1: Write the failing test**

Add to `engine/tests/camera_tests.cpp`, before `main`:

```cpp
// El Add/Remove de cámara pasa por el stack de Undo (a diferencia de los Add de
// collider/Rigidbody): si no, un Undo de Delete podría resucitar una cámara
// borrada estando ya otra en escena. Ver spec, "The One-Camera Invariant".
static void test_camera_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Objetivo");

    CameraState st{ CameraComponent::ProjectionMode::Orthographic, 60.0f, 300.0f, 2.0f, 900.0f };
    CameraComponentCommand cmd(scene, "Add Camera", go->id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasCameraComponent());
    CHECK(scene.findCamera() == go);

    cmd.undo();
    CHECK(!go->hasCameraComponent());
    CHECK(scene.findCamera() == nullptr);

    // Redo: los valores del state se conservan, no vuelve a los defaults.
    cmd.execute();
    CHECK(go->hasCameraComponent());
    const auto& c = go->getCameraComponent();
    CHECK(c->getMode() == CameraComponent::ProjectionMode::Orthographic);
    CHECK(nearlyEqual(c->getFov(), 60.0f));
    CHECK(nearlyEqual(c->getOrthographicSize(), 300.0f));
    CHECK(nearlyEqual(c->getNear(), 2.0f));
    CHECK(nearlyEqual(c->getFar(), 900.0f));
}

// add=false invierte el sentido: execute quita, undo devuelve.
static void test_camera_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Objetivo");
    go->setCameraComponent(std::make_shared<CameraComponent>());

    CameraState st{ CameraComponent::ProjectionMode::Perspective, 45.0f, 100.0f, 1.0f, 2000.0f };
    CameraComponentCommand cmd(scene, "Remove Camera", go->id, /*add=*/false, st);

    cmd.execute();
    CHECK(!go->hasCameraComponent());
    cmd.undo();
    CHECK(go->hasCameraComponent());
}

// El comando resuelve el GameObject por id en cada execute()/undo(), nunca
// guarda un puntero crudo: sobrevive a que el objeto se reconstruya entretanto.
static void test_camera_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Objetivo");
    uint64_t id = go->id;
    CameraState st{ CameraComponent::ProjectionMode::Perspective, 45.0f, 100.0f, 1.0f, 2000.0f };
    CameraComponentCommand cmd(scene, "Add Camera", id, /*add=*/true, st);

    scene.removeGameObject(go);
    cmd.execute(); // no debe crashear: findById devuelve nullptr y sale
    CHECK(scene.findCamera() == nullptr);
}
```

Add the include at the top of `camera_tests.cpp`:

```cpp
#include "DonTopo/Editor/Command.h"
```

Register in `main` after the clone tests:

```cpp
    test_camera_command_add_undo_redo();
    test_camera_command_remove();
    test_camera_command_survives_missing_target();
```

- [ ] **Step 2: Build to verify it fails**

```powershell
.\build.bat
```

Expected: FAIL — `'CameraState': undeclared identifier`.

- [ ] **Step 3: Declare CameraState and the command**

In `engine/include/DonTopo/Editor/Command.h`, add the include at the top:

```cpp
#include "DonTopo/Core/CameraComponent.h"
```

After the `RigidbodyState` struct:

```cpp
// Snapshot value-type del CameraComponent — T de PropertyCommand<T> en la
// sección Camera del panel Properties.
struct CameraState {
    CameraComponent::ProjectionMode mode;
    float fov;
    float orthographicSize;
    float nearPlane;
    float farPlane;
};
```

After the `CreateGameObjectCommand` class:

```cpp
// Añade (add=true) o quita (add=false) el CameraComponent del GameObject id;
// undo() hace lo contrario.
//
// A diferencia de los Add de collider/Rigidbody (que no pasan por el stack),
// el de cámara SÍ: sin esto se puede llegar a dos cámaras en escena — Add a X,
// Delete X (el snapshot se lleva la cámara), Add a Z (permitido, findCamera()
// es nullptr), Ctrl+Z resucita X CON su cámara. Con el Add en el stack, para
// deshacer el Delete de X hay que deshacer antes el Add de Z, y el orden impone
// el invariante sin descartar nada.
//
// Resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// mismo contrato que PropertyCommand. m_state conserva los valores pa que un
// Add-undo-redo no los devuelva a los defaults.
class CameraComponentCommand : public ICommand {
public:
    CameraComponentCommand(Scene& scene, std::string label, uint64_t id,
                            bool add, CameraState state);
    void execute() override;
    void undo() override;
    std::string label() const override { return m_label; }

private:
    void apply(bool add);

    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CameraState m_state;
};
```

- [ ] **Step 4: Implement the command**

In `engine/src/Editor/Command.cpp`, add at the end, before the closing namespace brace:

```cpp
CameraComponentCommand::CameraComponentCommand(Scene& scene, std::string label, uint64_t id,
                                                bool add, CameraState state)
    : m_scene(scene), m_label(std::move(label)), m_id(id), m_add(add), m_state(state) {}

void CameraComponentCommand::execute() { apply(m_add); }
void CameraComponentCommand::undo()    { apply(!m_add); }

void CameraComponentCommand::apply(bool add)
{
    GameObject* go = m_scene.findById(m_id);
    if (!go) return;
    if (!add)
    {
        go->setCameraComponent(nullptr);
        return;
    }

    auto cam = std::make_shared<CameraComponent>();
    cam->setMode(m_state.mode);
    // far antes que near: setNear clampa contra el far actual (ver
    // CameraComponent::setNear).
    cam->setFar(m_state.farPlane);
    cam->setNear(m_state.nearPlane);
    cam->setFov(m_state.fov);
    cam->setOrthographicSize(m_state.orthographicSize);
    go->setCameraComponent(cam);
}
```

- [ ] **Step 5: Build and run the tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_camera_tests.exe
```

Expected: `ALL CAMERA TESTS PASSED`, exit code 0.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/Command.h engine/src/Editor/Command.cpp engine/tests/camera_tests.cpp
git commit -m "feat(editor): add CameraComponentCommand for undoable Add/Remove

Unlike the collider Adds, the camera Add goes on the undo stack: without it,
Add to X -> Delete X -> Add to Z -> Ctrl+Z resurrects X with its camera and the
scene ends up with two.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: Properties — Camera section + Add gate

**Files:**
- Modify: `engine/include/DonTopo/Editor/PropertiesPanel.h`
- Modify: `engine/src/Editor/PropertiesPanel.cpp` (`draw` ~:323, `drawAddComponentButton` ~:1326)

**Interfaces:**
- Consumes: `CameraComponent` (Task 1), `Scene::findCamera` (Task 2), `CameraState` + `CameraComponentCommand` (Task 6).
- Produces: `PropertiesPanel::drawCameraSection(EditorContext&)` (private).

**No headless test** (ImGui). Gate: clean compile. Verified by the user in Task 11.

**Firm repo requirement:** the section stays **hidden until "Add" is pressed** — the early-return on `hasCameraComponent()` is what implements this. Do not add a `m_cameraAddRequestedFor`-style reveal.

- [ ] **Step 1: Add the members**

In `engine/include/DonTopo/Editor/PropertiesPanel.h`, add the include:

```cpp
#include "DonTopo/Core/CameraComponent.h"
```

Declare the method next to `drawRigidbodySection`:

```cpp
    void drawCameraSection(EditorContext& ctx);
```

Add the cache members after the Rigidbody block:

```cpp
    // Camera – mismo patrón de cache que Rigidbody. Los DragFloat (fov/size/
    // near/far) usan begin/commit con m_cameraBeforeEdit pa empujar un único
    // PropertyCommand<CameraState> al soltar; el combo de modo empuja comando
    // inmediato.
    const void* m_cameraCachedFor = nullptr;
    CameraComponent::ProjectionMode m_editCamMode = CameraComponent::ProjectionMode::Perspective;
    float       m_editCamFov = 45.0f;
    float       m_editCamOrthoSize = 100.0f;
    float       m_editCamNear = 1.0f;
    float       m_editCamFar = 2000.0f;
    bool        m_cameraDragActive = false;
    CameraState m_cameraBeforeEdit{};
```

- [ ] **Step 2: Implement drawCameraSection**

In `engine/src/Editor/PropertiesPanel.cpp`, add the include at the top:

```cpp
#include "DonTopo/Core/CameraComponent.h"
```

Add the call in `draw`, after `drawRigidbodySection(ctx);`:

```cpp
            drawCameraSection(ctx);
```

Add the implementation after `drawRigidbodySection` (after its closing brace, ~:988):

```cpp
void PropertiesPanel::drawCameraSection(EditorContext& ctx)
{
    // Oculta hasta que se pulse Add: la sección solo existe si el componente
    // existe, y el componente solo existe tras Add (mismo early-return que
    // drawRigidbodySection).
    if (!ctx.selected || !ctx.selected->hasCameraComponent()) { m_cameraCachedFor = nullptr; return; }
    CameraComponent* cam = ctx.selected->getCameraComponent().get();
    if (m_cameraCachedFor != cam)
    {
        m_editCamMode      = cam->getMode();
        m_editCamFov       = cam->getFov();
        m_editCamOrthoSize = cam->getOrthographicSize();
        m_editCamNear      = cam->getNear();
        m_editCamFar       = cam->getFar();
        m_cameraCachedFor  = cam;
    }

    ImGui::Separator();
    if (!ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen))
        return;

    Scene*   scene = ctx.scene;
    uint64_t id    = ctx.selected->id;

    // Aplica un CameraState al GameObject resuelto por id (sobrevive a
    // undo-de-delete). Mismo patrón que applyRbState.
    auto applyCamState = [scene, id](const CameraState& s) {
        GameObject* go = scene->findById(id);
        if (!go || !go->hasCameraComponent()) return;
        auto c = go->getCameraComponent();
        c->setMode(s.mode);
        // far antes que near: setNear clampa contra el far actual.
        c->setFar(s.farPlane);
        c->setNear(s.nearPlane);
        c->setFov(s.fov);
        c->setOrthographicSize(s.orthographicSize);
    };
    auto currentState = [&]() {
        return CameraState{ m_editCamMode, m_editCamFov, m_editCamOrthoSize, m_editCamNear, m_editCamFar };
    };

    // --- Combo de modo: comando inmediato con before/after ---
    {
        CameraState before = currentState();
        const char* modes[] = { "Perspective", "Orthographic" };
        int modeIdx = (m_editCamMode == CameraComponent::ProjectionMode::Orthographic) ? 1 : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::Combo("Projection", &modeIdx, modes, 2))
        {
            m_editCamMode = (modeIdx == 1) ? CameraComponent::ProjectionMode::Orthographic
                                            : CameraComponent::ProjectionMode::Perspective;
            applyCamState(currentState());
            ctx.pushLog(std::string("Projection de '") + ctx.selected->name + "' (Camera): " + modes[modeIdx]);
            if (ctx.scene)
                ctx.undo->push(std::make_unique<PropertyCommand<CameraState>>(
                    "Projection de '" + ctx.selected->name + "' (Camera)", before, currentState(), applyCamState));
        }
    }

    // --- Drag floats: snapshot al activar CUALQUIERA, comando al soltar
    // CUALQUIERA (mismo patrón acumulativo que Rigidbody).
    auto snapshotBefore = [&]() {
        if (!m_cameraDragActive)
        {
            m_cameraDragActive = true;
            m_cameraBeforeEdit = CameraState{ cam->getMode(), cam->getFov(), cam->getOrthographicSize(),
                                               cam->getNear(), cam->getFar() };
        }
    };
    bool floatChanged = false;
    bool floatCommitted = false;

    // Solo se muestra el campo del modo activo: enseñar el otro sugeriría que
    // hace algo, y no hace nada.
    if (m_editCamMode == CameraComponent::ProjectionMode::Orthographic)
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        floatChanged |= ImGui::DragFloat("Size", &m_editCamOrthoSize, 1.0f, 0.001f, FLT_MAX, "%.3f");
        if (ImGui::IsItemActivated()) snapshotBefore();
        floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();
    }
    else
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        floatChanged |= ImGui::DragFloat("Field of View", &m_editCamFov, 0.5f, 1.0f, 179.0f, "%.1f");
        if (ImGui::IsItemActivated()) snapshotBefore();
        floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Near", &m_editCamNear, 0.1f, 0.001f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated()) snapshotBefore();
    floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    floatChanged |= ImGui::DragFloat("Far", &m_editCamFar, 1.0f, 0.001f, FLT_MAX, "%.3f");
    if (ImGui::IsItemActivated()) snapshotBefore();
    floatCommitted |= ImGui::IsItemDeactivatedAfterEdit();

    if (floatChanged)
    {
        applyCamState(currentState());
        // Los clamps del componente pueden haber corregido el valor (p.ej. near
        // por encima de far): se re-sincroniza el cache pa que el widget enseñe
        // lo que de verdad quedó guardado, no lo que se arrastró.
        m_editCamFov       = cam->getFov();
        m_editCamOrthoSize = cam->getOrthographicSize();
        m_editCamNear      = cam->getNear();
        m_editCamFar       = cam->getFar();
    }
    if (m_cameraDragActive && floatCommitted)
    {
        m_cameraDragActive = false;
        if (ctx.scene)
            ctx.undo->push(std::make_unique<PropertyCommand<CameraState>>(
                "Camera de '" + ctx.selected->name + "'", m_cameraBeforeEdit, currentState(), applyCamState));
    }

    if (ImGui::Button("Remove Camera"))
    {
        // Pasa por el stack igual que el Add (ver CameraComponentCommand): si el
        // Remove no fuera deshacible, quitar la cámara sería una pérdida
        // irreversible.
        CameraState st = currentState();
        m_cameraCachedFor = nullptr;
        ctx.pushLog("Componente Camera quitado de '" + ctx.selected->name + "'");
        if (ctx.scene && ctx.undo)
        {
            auto cmd = std::make_unique<CameraComponentCommand>(
                *ctx.scene, "Quitar Camera de '" + ctx.selected->name + "'", id, /*add=*/false, st);
            cmd->execute();
            ctx.undo->push(std::move(cmd));
        }
        else
        {
            ctx.selected->setCameraComponent(nullptr);
        }
        ImGui::TreePop();
        return;
    }

    ImGui::TreePop();
}
```

**Note on the `Remove` early return:** the component is gone after `execute()`, so continuing to draw the section would dereference a dangling `cam`. `TreePop()` is called before returning to keep the ImGui stack balanced.

- [ ] **Step 3: Add the gate to the Add popup**

In `drawAddComponentButton`, after the `Audio Clip` block and before the `Script` menu:

```cpp
        // Cámara: como mucho una por escena, y el gate pregunta a la única
        // fuente de verdad (Scene::findCamera), no a un flag propio. Deshabilitado
        // y no oculto porque es lo que hacen los demás items de este popup — y el
        // tooltip dice QUIÉN la tiene ya, que si no un item gris sin explicación
        // es un callejón sin salida.
        GameObject* existingCamera = ctx.scene ? ctx.scene->findCamera() : nullptr;
        ImGui::BeginDisabled(existingCamera != nullptr);
        if (ImGui::Selectable("Camera") && !existingCamera && ctx.scene && ctx.undo)
        {
            CameraComponent defaults;
            CameraState st{ defaults.getMode(), defaults.getFov(), defaults.getOrthographicSize(),
                            defaults.getNear(), defaults.getFar() };
            auto cmd = std::make_unique<CameraComponentCommand>(
                *ctx.scene, "Añadir Camera a '" + ctx.selected->name + "'", ctx.selected->id, /*add=*/true, st);
            cmd->execute();
            ctx.undo->push(std::move(cmd));
            m_cameraCachedFor = nullptr;
            ctx.pushLog("Componente Camera añadido a '" + ctx.selected->name + "'");
        }
        ImGui::EndDisabled();
        if (existingCamera && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Ya hay una cámara en la escena ('%s')", existingCamera->name.c_str());
```

- [ ] **Step 4: Build**

```powershell
.\build.bat
```

Expected: compiles clean.

- [ ] **Step 5: Verify the headless tests still pass**

```powershell
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
```

Expected: all three pass.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/PropertiesPanel.h engine/src/Editor/PropertiesPanel.cpp
git commit -m "feat(editor): add Camera section and Add gate to Properties

The section stays hidden until Add is pressed, like the colliders. The Add gate
asks Scene::findCamera instead of keeping its own state, and the tooltip names
the GameObject that already holds the camera.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: Scene panel — Create Camera

**Files:**
- Modify: `engine/include/DonTopo/Editor/ScenePanel.h`
- Modify: `engine/src/Editor/ScenePanel.cpp` (`draw` popup ~:134, `drawNode` popup ~:403, next to `createBasicShape` ~:349)

**Interfaces:**
- Consumes: `CameraComponent` (Task 1), `Scene::findCamera` (Task 2), `Scene::subtreeToJson` + `CreateGameObjectCommand` (existing).
- Produces: `ScenePanel::createCamera(EditorContext&, GameObject* parent)` (private).

**No headless test** (ImGui). Gate: clean compile. Verified by the user in Task 11.

- [ ] **Step 1: Declare the helper**

In `engine/include/DonTopo/Editor/ScenePanel.h`, declare next to `createBasicShape`:

```cpp
    // Crea un GameObject con CameraComponent de un tirón, pasando por el stack
    // de Undo igual que createBasicShape. El caller comprueba que no haya ya
    // una cámara (Scene::findCamera) antes de ofrecer la acción.
    void createCamera(EditorContext& ctx, GameObject* parent);
```

- [ ] **Step 2: Implement it**

In `engine/src/Editor/ScenePanel.cpp`, add the includes at the top:

```cpp
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/Scene.h"
```

(`Scene.h` may already be included — check before adding.)

Add the implementation right after `createBasicShape` (~:369):

```cpp
void ScenePanel::createCamera(EditorContext& ctx, GameObject* parent)
{
    if (!parent || !ctx.scene) return;

    GameObject* go = parent->addChild("Camera");
    go->setCameraComponent(std::make_shared<CameraComponent>());
    ctx.pushLog("GameObject '" + go->name + "' con Camera creado");

    // Mismo patrón que createBasicShape: el snapshot se toma DESPUÉS de montar
    // el componente, así que el Undo/Redo lo reconstruye entero.
    if (ctx.physics && ctx.audio && ctx.renderer && ctx.undo)
    {
        uint64_t parentId = parent->id;
        size_t index = parent->children.size() - 1;
        nlohmann::json snapshot = ctx.scene->subtreeToJson(go);
        ctx.undo->push(std::make_unique<CreateGameObjectCommand>(
            *ctx.scene, *ctx.physics, *ctx.audio, *ctx.renderer,
            "Crear '" + go->name + "'", parentId, index, std::move(snapshot)));
    }
}
```

- [ ] **Step 3: Add the item to the window context menu**

In `draw` (~:134), inside `BeginPopupContextWindow`, after the "Create GameObject" block and before `BeginMenu("Basic Shapes")`:

```cpp
        // Visible solo si no hay ya una cámara en la escena — el invariante lo
        // decide Scene::findCamera, no un flag de este panel.
        if (ctx.scene && !ctx.scene->findCamera())
        {
            if (ImGui::MenuItem("Create Camera") && sceneRoot)
                createCamera(ctx, sceneRoot);
        }
```

- [ ] **Step 4: Add the item to the node context menu**

In `drawNode` (~:403), inside `BeginPopupContextItem`, after its "Create GameObject" block and before `BeginMenu("Basic Shapes")`:

```cpp
        // Mismo gate que el menú de la ventana: la cámara puede colgar de
        // cualquier nodo, pero solo puede haber una.
        if (ctx.scene && !ctx.scene->findCamera())
        {
            if (ImGui::MenuItem("Create Camera"))
                createCamera(ctx, node);
        }
```

- [ ] **Step 5: Build**

```powershell
.\build.bat
```

Expected: compiles clean.

- [ ] **Step 6: Verify the headless tests still pass**

```powershell
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
```

Expected: all three pass.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Editor/ScenePanel.h engine/src/Editor/ScenePanel.cpp
git commit -m "feat(editor): add Create Camera to the Scene context menus

Goes through CreateGameObjectCommand like createBasicShape, and is only offered
when Scene::findCamera reports no camera in the scene.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 9: Viewport — frustum gizmo

**Files:**
- Modify: `engine/include/DonTopo/Editor/ViewportPanel.h`
- Modify: `engine/src/Editor/ViewportPanel.cpp` (`draw` ~:113)

**Interfaces:**
- Consumes: `CameraComponent::projectionMatrix`/`viewFromWorld` (Task 1), `Scene::findCamera` (Task 2), `Renderer::viewportAspect` (Task 5), `Gizmos::drawFrustum` (existing).
- Produces: `ViewportPanel::drawCameraGizmo(EditorContext&)` (private).

**No headless test** (Vulkan/ImGui). Gate: clean compile. Verified by the user in Task 11 — **do not claim the gizmo renders correctly.**

- [ ] **Step 1: Declare the method**

In `engine/include/DonTopo/Editor/ViewportPanel.h`, declare next to `drawSelectionGizmo`:

```cpp
    // Wireframe del frustum de la cámara de la escena, siempre visible en
    // edición (no solo al seleccionarla). Solo el frustum: los ejes del
    // transform ya los dibuja drawSelectionGizmo al seleccionar cualquier
    // objeto, y repetirlos aquí daría dos juegos de ejes superpuestos de
    // distinta longitud.
    void drawCameraGizmo(EditorContext& ctx);
```

- [ ] **Step 2: Implement it**

In `engine/src/Editor/ViewportPanel.cpp`, add the includes at the top:

```cpp
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Renderer/Renderer.h"
```

(Some may already be present — check before adding.)

Add the implementation after `drawSelectionGizmo` (~:111):

```cpp
void ViewportPanel::drawCameraGizmo(EditorContext& ctx)
{
    // Solo en edición: en Play ya se está mirando POR esa cámara, dibujar su
    // propio frustum no aporta nada (y taparía la vista desde dentro).
    if (ctx.isPlaying || !ctx.scene || !ctx.renderer)
        return;

    GameObject* cam = ctx.scene->findCamera();
    if (!cam) return;

    // El aspect sale del Renderer (el del render target), no del tamaño de esta
    // ventana ImGui: tiene que ser EXACTAMENTE el que usará la proyección al
    // dar a Play, o el wireframe dibujaría un encuadre que luego no se cumple.
    const glm::mat4 viewProj =
        cam->getCameraComponent()->projectionMatrix(ctx.renderer->viewportAspect()) *
        CameraComponent::viewFromWorld(cam->worldTransform);

    // Cian: distinto del amarillo de los colliders, pa no confundirlos.
    const glm::vec3 kCameraGizmoColor(0.0f, 1.0f, 1.0f);
    Gizmos::drawFrustum(viewProj, kCameraGizmoColor);
}
```

- [ ] **Step 3: Call it from draw**

In `ViewportPanel::draw` (~:115), right after `drawSelectionGizmo(ctx);`:

```cpp
    drawCameraGizmo(ctx);
```

Both run before `if (!m_open) return;` and before `ImGui::Begin`, so neither depends on window state — same as the existing selection gizmo.

- [ ] **Step 4: Build**

```powershell
.\build.bat
```

Expected: compiles clean.

- [ ] **Step 5: Verify the headless tests still pass**

```powershell
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
```

Expected: all three pass.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/ViewportPanel.h engine/src/Editor/ViewportPanel.cpp
git commit -m "feat(editor): draw the scene camera frustum gizmo in edit mode

Uses the component's own matrices and the renderer's aspect, so the wireframe
matches exactly what Play will render.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 10: EditorUI — Play warning + drain warnings

**Files:**
- Modify: `engine/src/Editor/EditorUI.cpp` (Play start block ~:154, `reloadSceneFromJson`)

**Interfaces:**
- Consumes: `Scene::findCamera` (Task 2), `Scene::lastWarnings` (Task 4).
- Produces: nothing consumed by later tasks.

**No headless test** (ImGui). Gate: clean compile. Verified by the user in Task 11.

- [ ] **Step 1: Add the Play warning**

In `engine/src/Editor/EditorUI.cpp`, in the Play-start block (the `else` branch that sets `m_isPlaying = true`, ~:154), after `m_playSnapshot = m_scene->toJson();` and before `m_isPlaying = true;`:

```cpp
            // Aviso una sola vez al arrancar Play (no cada frame: el Renderer
            // consulta findCamera() en todos, y loguear ahí inundaría la
            // consola). Sin cámara, Play arranca igual con la del editor — que
            // se pueda iterar sin cámara importa más que forzar disciplina.
            if (!m_scene->findCamera())
                m_logPanel.push("No hay cámara en la escena; usando la del editor");
```

- [ ] **Step 2: Drain the load warnings**

In `EditorUI::reloadSceneFromJson` (~:254), the success branch currently reads:

```cpp
    if (loaded)
        m_selected = nullptr; // la selección anterior ya no existe
    m_undoHistory.clear();
```

Replace it with:

```cpp
    if (loaded)
    {
        m_selected = nullptr; // la selección anterior ya no existe
        // Avisos de la carga (p.ej. escena con dos cámaras, donde fromJson se
        // queda con la primera): Core no conoce el Log Console, así que los
        // vuelca aquí quien sí lo conoce. Solo si loaded — una carga fallida
        // no modifica la escena y sus avisos no aplican.
        for (const auto& w : m_scene->lastWarnings())
            m_logPanel.push(w);
    }
    m_undoHistory.clear();
```

This is the only drain point needed: the "Open Scene" dialog (~:321) routes through `reloadSceneFromJson`, and so does Stop (~:144, restoring `m_playSnapshot`). No other caller of `fromJson` exists in `EditorUI.cpp` — verified.

- [ ] **Step 3: Build**

```powershell
.\build.bat
```

Expected: compiles clean.

- [ ] **Step 4: Verify the headless tests still pass**

```powershell
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
```

Expected: all three pass.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Editor/EditorUI.cpp
git commit -m "feat(editor): warn on Play without a camera and surface load warnings

The Play warning fires once per Play, not per frame. Scene's warnings (two
cameras in a hand-edited file) reach the Log Console through the editor, since
Core does not know about it.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 11: Manual GUI verification (USER)

**Files:** none — this task is executed by the user, not by an agent.

**No subagent has a GUI.** Tasks 5 and 7-10 have no automated coverage beyond compiling. This is the only gate that proves the feature works. **No agent may check these boxes or claim any of them passes.**

- [ ] **Step 1: Build and launch**

```powershell
.\build.bat
.\build-ninja\sandbox\DonTopoSandbox.exe
```

(If the executable path differs, locate it under `build-ninja\sandbox\`.)

- [ ] **Step 2: Report the results of this checklist**

Confirm each, or report what actually happened:

1. Right-click the Scene panel → **"Create Camera"** appears → creates a GameObject named "Camera".
2. The **cyan frustum wireframe** is visible in the viewport without selecting anything, correctly oriented (pointing along the object's -Z).
3. Moving/rotating the camera GameObject moves the frustum with it.
4. Properties on the camera → the **Camera section is visible**, with Projection / Field of View / Near / Far.
5. Editing **Field of View / Near / Far** reshapes the frustum live.
6. Switching Projection to **Orthographic** → "Field of View" is replaced by "Size", and the frustum becomes a box.
7. On a *different* GameObject: Properties → **"Add" → "Camera" is greyed out**, and hovering shows the tooltip naming the GameObject that has it.
8. Right-click the Scene panel again → **"Create Camera" is gone** (a camera already exists).
9. On a GameObject *without* a camera: Properties → **no Camera section** until "Add" → "Camera" is pressed.
10. **Play** → the view switches to the camera's viewpoint, with its fov/near/far.
11. **Stop** → returns to the editor fly camera, exactly where it was before Play.
12. The **frustum gizmo is not drawn during Play**.
13. **Ctrl+Z** after "Create Camera" removes the GameObject; **Ctrl+Z** after "Add" → "Camera" removes just the component.
14. Delete the camera → **Play** → falls back to the editor camera and the Log shows `No hay cámara en la escena; usando la del editor`.
15. Save the scene, reload it → the camera and its values survive.

- [ ] **Step 3: Report the outcome**

Report anything that misbehaves. Do not merge until this checklist passes.

---

## Notes for the implementer

- **Line numbers in this plan are approximate** and will drift as tasks land. Always read the surrounding code and match on content, not on line number.
- **`m_camera` / `m_viewMatrix` in `Renderer` must never be overwritten** with the game camera. The whole "Stop restores the editor camera" behaviour depends on it, and so does not touching `sandbox/main.cpp`.
- **`setFar` before `setNear`** everywhere a camera is built from stored values (`nodeFromJson`, `CameraComponentCommand::apply`, `applyCamState`). `setNear` clamps against the *current* `far`.
- If a task's assumption turns out to be wrong (a function is not where the plan says, an include is missing, the engine CMake does not glob `engine/src/**`), **stop and report** rather than improvising a workaround.
