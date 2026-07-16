# Camera Component (Unity-style) — Design

Date: 2026-07-16
Status: Approved (design)

## Goal

Add a `CameraComponent` that turns any `GameObject` into the scene's game camera.
On Play, the `Renderer` renders from that camera instead of the editor's fly camera;
on Stop it returns to the editor camera with its state untouched.

`DonTopo::Camera` (`Core/Camera.h`) is the editor fly camera and is **not modified,
extended or subclassed** by this design. The new component is a separate entity that
never routes through `Camera`.

At most **one camera per scene**, enforced through a single source of truth
(`Scene::findCamera()`), not by convention.

Non-goals for v1 (explicitly out of scope):

- Lua bindings for `CameraComponent`.
- Multiple cameras / render targets / camera priority.
- Letterboxing or fixed aspect ratio (the viewport dictates aspect).
- Refactoring `GameObject` to a generic component system.
- Fixing the audio listener during Play (see Known Limitations).

## Context (verified against the code, not assumed)

Five findings shape this design:

1. **Projection is hardcoded, not driven by `Camera`.** `Renderer.cpp:653`
   (skybox + gizmos) and `Renderer.cpp:1187` (UBO) both build
   `glm::perspective(radians(45.0f), extent.w/extent.h, m_cameraDistance*0.001f, m_cameraDistance*3.0f)`.
   `Camera::getFov()` exists but nothing consumes it. Routing real fov/near/far
   through the renderer means unifying those two duplicated sites.
2. **`setCamera` is called unconditionally every frame from `sandbox/src/main.cpp:268`**,
   which is outside the agreed file scope (`engine/**`). The Play switch must therefore
   live inside `Renderer`, not in `main.cpp`.
3. **`Camera` cannot be constructed from a `mat4`** (its ctor takes pos/yaw/pitch) and
   must not be touched. The component's view matrix is derived from the GameObject's
   `worldTransform` directly, never through a `Camera` instance.
4. **All five serialization paths funnel through `nodeToJson` (`Scene.cpp:54`) /
   `nodeFromJson` (`:209`)** (anonymous namespace in `Scene.cpp`): `toJson` (:552),
   `subtreeToJson` (:451), `fromJson` (:584), `insertFromJson` (:461),
   `cloneGameObject` (:422). One block added to each of those two functions covers all
   five paths.
5. **`cloneGameObject`'s only caller is Lua's `Instantiate`** (`ScriptBindings.cpp:527`) —
   no editor UI clones GameObjects today. Cloning therefore happens *during Play, from a
   script*, which is what makes the clone rule below necessary rather than theoretical.

## Design Decisions

Decisions taken during brainstorming, with their rationale:

| Decision | Choice | Why |
| --- | --- | --- |
| Component name | `CameraComponent` | Matches `AudioClipComponent` / `ScriptComponent`, the repo's other suffixed slots. No clash with `DonTopo::Camera`. |
| Projection modes | Perspective **and** orthographic | Explicitly requested. Enum + `orthographicSize`. |
| Aspect ratio | Not stored; taken from the viewport | Resizing the window must not stretch the image; there is no letterboxing today. One less field to keep coherent. |
| Play with no camera | Fall back to editor camera + Log warning | Play must not break for a scene without a camera; the warning explains why the view did not change. |
| Loading a scene with two cameras | First in pre-order wins, rest discarded + Log warning | Restores the invariant for real instead of leaving the scene incoherent. A hand-edited JSON stays openable, and the user is told what was dropped. |
| Editor-mode projection | Unchanged (45° / `m_cameraDistance`) | Zero visual regression in the editor. Only Play consumes the component. |
| Frustum gizmo | Always visible in edit mode | Shows where the camera points without selecting it. |
| Gizmo contents | Frustum only | `drawSelectionGizmo` already draws `drawAxes` for any selected object; drawing them again would produce two overlapping axis sets of different lengths (`selectionAxisScale`). |
| `sandbox/main.cpp` | Not touched | Keeps the change inside the agreed scope. |

## The One-Camera Invariant

`Scene::findCamera()` is the **single source of truth**. Every gate (Properties "Add",
Scene context menu, Renderer switch, Play warning) asks it; none keeps its own state.

```cpp
// Única fuente de verdad del invariante "como mucho una cámara por escena".
GameObject* findCamera();              // pre-orden desde la raíz; nullptr si ninguna
const GameObject* findCamera() const;
```

It returns the **`GameObject`**, not the component: every consumer needs the transform
*and* the component.

Three paths could break the invariant. All three are closed:

### 1. Loading a JSON with two cameras

`fromJson` keeps the first camera in pre-order and strips the `CameraComponent` from the
rest (the GameObjects themselves are preserved — only the extra components are dropped).

`Scene` is Core and has no Log. It exposes a warning channel that `EditorUI` drains:

```cpp
// Avisos de la última operación que tuvo que corregir el invariante de una
// cámara por escena (carga con varias, clone de una cámara). Core no conoce el
// Log Console: EditorUI los vuelca tras cada carga/clone. Se limpian al inicio
// de cada operación que los pueda rellenar.
const std::vector<std::string>& lastWarnings() const;
```

This keeps the invariant valid headless (so the test can assert it) while still surfacing
the message in the Log Console.

### 2. Undo restoring a deleted camera (the Add/Undo hole)

Properties' "Add" actions do not go through Undo today (colliders, Rigidbody, Mesh and
Audio only `pushLog`). That allows a real two-camera state:

1. GameObject `X` → Add Camera (not in the undo stack).
2. Delete `X` → `DeleteGameObjectCommand` snapshots the subtree **with the camera in it**.
3. `findCamera()` now returns `nullptr`, so Add Camera on `Z` is allowed.
4. Ctrl+Z undoes the delete → `X` returns with its camera → **two cameras**.

Step 3 is invisible to the stack, so stack ordering cannot protect the invariant.

**Fix:** the Add (and Remove) of `CameraComponent` goes through Undo via a new
`CameraComponentCommand`. Once the Add is on the stack, undoing the delete of `X`
requires undoing the Add of `Z` first, and the ordering guarantees the invariant with no
pruning and no data loss. This deliberately deviates from the collider "Add" pattern, as
required by requirement 7 ("the creation of the camera must go through Undo/Redo").

### 3. Cloning a GameObject that has a camera

`cloneGameObject` copies via `nodeToJson` → `nodeFromJson`, so a clone would carry the
camera and produce two. Its only caller is Lua's `Instantiate` (`ScriptBindings.cpp:527`),
so this fires *during Play, from a script* — no editor gate can prevent it.

**Fix:** a clone never keeps the `CameraComponent`, enforced inside
`Scene::cloneGameObject`. Putting the rule in Core covers the Lua path without touching
Scripting (Lua bindings are out of scope). It is deterministic rather than conditional:
when cloning, the source stays alive with its camera, so `findCamera()` is always
non-null, so the clone could never legally keep it.

The drop is recorded in `lastWarnings()`. Note that **this one has no Log consumer
today**: the editor drains `lastWarnings()` after loading a scene, but nothing drains it
after a Lua `Instantiate`. The warning is recorded for the headless test to assert and
for a future consumer; wiring it to the Log from the Lua path is out of scope.

## Components

### `CameraComponent` (engine/include/DonTopo/Core/CameraComponent.h + src/Core/CameraComponent.cpp)

Pure data + math. No Vulkan, no GameObject dependency. Lives in Core next to
`GameObject` and `Camera`.

State (serialized, editable):

- `ProjectionMode mode` — `Perspective` (default) or `Orthographic`
- `float fov` — degrees, perspective only. Default `45.0f`, clamped to `[1, 179]`
- `float orthographicSize` — world-units half-height, orthographic only. Default `100.0f`, clamped `> 0`
- `float nearPlane` — default `1.0f`, clamped `> 0` and `< far`
- `float farPlane` — default `2000.0f`, clamped `> near`

Defaults are chosen for this repo's world scale (primitives are created at 50 units,
`sandbox` places the camera at z=300), not Unity's 0.3/1000.

API:

```cpp
enum class ProjectionMode { Perspective, Orthographic };

ProjectionMode getMode() const;            void setMode(ProjectionMode m);
float getFov() const;                      void setFov(float degrees);
float getOrthographicSize() const;         void setOrthographicSize(float size);
float getNear() const;                     void setNear(float n);
float getFar() const;                      void setFar(float f);

// Proyección del componente con el Y-flip de Vulkan YA aplicado.
glm::mat4 projectionMatrix(float aspect) const;
// View desde el worldTransform del GameObject dueño.
static glm::mat4 viewFromWorld(const glm::mat4& world);
```

**The component builds its own matrices, and that is the load-bearing decision here.**
If the `Renderer` and the frustum gizmo each built the projection their own way, the
drawn frustum would lie about what Play actually renders. One formula, three consumers
(Renderer, gizmo, tests).

Two details worth stating explicitly:

- `projectionMatrix` applies the Vulkan Y-flip (`proj[1][1] *= -1`) internally. Both
  consumers need it (the renderer for the UBO, `Gizmos::drawFrustum` for a viewProj);
  leaving it to callers would duplicate it and invite a forgotten flip.
- `viewFromWorld` normalizes the basis axes before inverting. A scaled GameObject would
  otherwise bake its scale into the view matrix and warp the image.

Setter clamps are enforced in the component itself, so a hand-edited JSON or a Lua
binding added later cannot install a degenerate projection.

### `GameObject` — new slot

```cpp
void setCameraComponent(std::shared_ptr<CameraComponent> c);
const std::shared_ptr<CameraComponent>& getCameraComponent() const;
bool hasCameraComponent() const;
private: std::shared_ptr<CameraComponent> m_cameraComponent;
```

Exact `m_rigidbody` pattern. `GameObject.h` gains `#include "DonTopo/Core/CameraComponent.h"`.

### Serialization

`nodeToJson` writes, `nodeFromJson` reads:

```json
"camera": { "mode": "perspective", "fov": 45.0, "orthographicSize": 100.0,
            "near": 1.0, "far": 2000.0 }
```

`mode` is a string (`"perspective"` / `"orthographic"`), not an int: readable in a
hand-edited `.scene` and stable if the enum ever gains a value. Unknown values fall back
to `"perspective"`. All fields read with `j.value(key, default)`, so a partial block
loads.

**This is additive only.** Scenes saved before this change have no `"camera"` key and
load unchanged; `version` stays at `1`. No existing scene file needs migrating.

### `Renderer` — the Play switch

```cpp
void setScene(Scene* scene) { m_scene = scene; m_editorUI.setScene(scene); }  // + m_scene
float viewportAspect() const;  // m_swapChainExtent.width / height

private:
struct FrameCamera { glm::mat4 view; glm::mat4 proj; glm::vec3 eye; };
// Cámara efectiva del frame: la del CameraComponent en Play, la de vuelo del
// editor en edición.
FrameCamera currentFrameCamera() const;
```

`currentFrameCamera()` returns the component's camera when `isPlaying() && m_scene->findCamera()`,
and the editor's (`m_viewMatrix` + the current hardcoded projection) otherwise. It replaces
the two duplicated projection sites (`:653`, `:1187`).

`eye` is part of the struct because `ubo.viewPos` feeds the specular term: without it,
highlights during Play would still be computed from the editor camera's position.

**`m_camera` and `m_viewMatrix` are never overwritten** — they stay the editor's. That is
what makes "Stop returns to the editor camera with its state intact" fall out for free,
with no save/restore step and no change to `sandbox/main.cpp`.

`viewportAspect()` is public because the gizmo needs the *same* aspect the renderer will
use in Play; any other source would draw a frustum that does not match the result.

### `EditorUI` — Play warning

In the Play-start block (`EditorUI.cpp:~154`), after the snapshot:

```cpp
if (m_scene && !m_scene->findCamera())
    m_logPanel.push("No hay cámara en la escena; usando la del editor");
```

Fires once per Play, not per frame. `EditorUI::reloadSceneFromJson` drains
`Scene::lastWarnings()` into the Log after loading.

### `ViewportPanel` — frustum gizmo

`drawCameraGizmo(ctx)`, called next to `drawSelectionGizmo(ctx)` (both run before
`ImGui::Begin`, so neither depends on window state):

- No-op if `ctx.isPlaying` or `!ctx.scene`.
- `GameObject* cam = ctx.scene->findCamera()`; no-op if null.
- `viewProj = c->projectionMatrix(ctx.renderer->viewportAspect()) * CameraComponent::viewFromWorld(cam->worldTransform)`
- `Gizmos::drawFrustum(viewProj, kCameraGizmoColor)` with cyan `(0,1,1)` — distinct from
  the colliders' yellow.

Axes are not drawn here: `drawSelectionGizmo` already draws them when the camera is
selected.

### `ScenePanel` — context menu

New `createCamera(EditorContext&, GameObject* parent)` helper, mirroring
`createBasicShape` (which already pushes `CreateGameObjectCommand`): creates a
GameObject named `"Camera"`, attaches a default `CameraComponent`, logs, and pushes
`CreateGameObjectCommand` with a `subtreeToJson` snapshot — so undo/redo works and the
snapshot carries the component.

A "Create Camera" item is added to **both** context menus (`BeginPopupContextWindow`
:134 and `BeginPopupContextItem` :403), at the same level as "Create GameObject", and
**visible only when `ctx.scene && !ctx.scene->findCamera()`**.

### `PropertiesPanel`

`drawCameraSection(ctx)`, called alongside the other component sections:

```cpp
if (!ctx.selected || !ctx.selected->hasCameraComponent()) { m_cameraCachedFor = nullptr; return; }
```

**The section is hidden until "Add" is pressed** (firm repo requirement) — identical
early-return to `drawRigidbodySection:850`. The section is reachable only when the
component exists, and the component exists only after Add.

Contents: mode combo, then `fov` **or** `orthographicSize` depending on mode (never both
— the inactive one is meaningless and would suggest it has an effect), `near`, `far`,
plus a "Remove Component" button. Edits go through `PropertyCommand<CameraState>` with an
`applyCameraState` lambda that re-resolves the GameObject by id, exactly like
`applyRbState`.

New in `Command.h`, next to `RigidbodyState`:

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

// Añade (add=true) o quita (add=false) el CameraComponent del GameObject id;
// undo() hace lo contrario. A diferencia de los Add de collider/Rigidbody, el
// de cámara SÍ pasa por el stack: si no, un Undo de Delete podría resucitar una
// cámara borrada estando ya otra en escena (ver spec, "The One-Camera Invariant").
// Resuelve el GameObject por id en cada execute()/undo() (nunca puntero crudo),
// mismo contrato que PropertyCommand. state conserva los valores del componente
// para que un Add-undo-redo no los pierda.
class CameraComponentCommand : public ICommand {
public:
    CameraComponentCommand(Scene& scene, std::string label, uint64_t id,
                           bool add, CameraState state);
    void execute() override;   // aplica add
    void undo() override;      // aplica !add
    std::string label() const override;
private:
    void apply(bool add);
    Scene& m_scene;
    std::string m_label;
    uint64_t m_id;
    bool m_add;
    CameraState m_state;
};
```

Add gate in `drawAddComponentButton`:

```cpp
GameObject* existing = ctx.scene ? ctx.scene->findCamera() : nullptr;
ImGui::BeginDisabled(existing != nullptr);
if (ImGui::Selectable("Camera") && !existing) { /* push CameraComponentCommand(add) */ }
ImGui::EndDisabled();
if (existing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Ya hay una cámara en la escena ('%s')", existing->name.c_str());
```

Disabled (not hidden) matches the majority pattern in that popup (colliders, Mesh, Audio
all use `BeginDisabled`), and the tooltip names the offending GameObject — otherwise a
greyed-out item with no explanation is a dead end for the user.

## Testing

New headless test `engine/tests/camera_tests.cpp` + two entries in
`engine/tests/CMakeLists.txt` (`add_executable`/`target_link_libraries`/`cxx_std_20`,
plus `dt_camera_tests` in the `fmod.dll` copy `foreach`). Plain `main` + `CHECK` asserts,
no framework — matching `physics_tests.cpp`.

**PhysX allows only one `PxFoundation` per process.** A separate executable means a
separate process, so `dt_camera_tests` creates exactly one `PhysicsManager` in its `main`
and passes it by reference to every test — never one per test. `Scene::fromJson` /
`insertFromJson` / `cloneGameObject` require `PhysicsManager&` and `AudioManager&`, which
is the only reason the test needs them at all.

Required coverage:

1. **No second camera** — `fromJson` on a hand-built JSON with two cameras leaves exactly
   one (the first in pre-order), both GameObjects survive, and `lastWarnings()` is
   non-empty. Also: `cloneGameObject` on a camera GameObject produces a clone with
   `hasCameraComponent() == false`.
2. **Serialization round-trip** — `toJson`→`fromJson`, `subtreeToJson`→`insertFromJson`,
   and `cloneGameObject` all preserve mode/fov/orthographicSize/near/far. Covered for
   both projection modes.
3. **`findCamera()` locates it anywhere** — a camera on a grandchild node is found, not
   just a direct child of the root; returns `nullptr` on a camera-less scene.

Additionally: setter clamps reject `near <= 0` and `far <= near`, and a scene JSON with
no `"camera"` key still loads (back-compat).

Existing headless tests (`dt_physics_tests`, `dt_content_browser_tests`) must keep
passing.

## Manual GUI Verification (user)

No subagent has a GUI. **These cannot be claimed as working by any agent** — they are the
user's to confirm by hand:

1. Frustum gizmo appears in cyan, correctly oriented, and follows the GameObject when moved/rotated.
2. Frustum changes shape when editing fov / near / far, and when switching to orthographic.
3. Properties shows no Camera section until "Add" → "Camera" is pressed.
4. With a camera in the scene: "Add" → "Camera" is greyed out on another GameObject, with the tooltip naming the holder.
5. With a camera in the scene: "Create Camera" is absent from both Scene context menus.
6. Play switches to the camera's view; Stop returns to the editor camera exactly where it was.
7. Play with no camera in the scene: falls back to the editor camera and logs the warning.
8. Ctrl+Z undoes both "Create Camera" (context menu) and "Add Camera" (Properties).
9. Frustum gizmo is not drawn during Play.

## Known Limitations

- **Audio listener during Play** follows the editor fly camera, not the game camera
  (`sandbox/src/main.cpp:272` feeds `audio.update` from `camera`). Pre-existing behaviour,
  outside the agreed `engine/**` scope, deliberately left alone.
- **Editor viewport projection** stays at the hardcoded 45° / `m_cameraDistance` near-far.
  The component's values only affect Play (and the gizmo).

## File Scope

- `engine/include/DonTopo/Core/CameraComponent.h` (new)
- `engine/src/Core/CameraComponent.cpp` (new)
- `engine/include/DonTopo/Core/GameObject.h` — slot
- `engine/include/DonTopo/Core/Scene.h` + `engine/src/Core/Scene.cpp` — `findCamera`, `lastWarnings`, serialization, clone rule
- `engine/include/DonTopo/Renderer/Renderer.h` + `engine/src/Renderer/Renderer.cpp` — `m_scene`, `viewportAspect`, `currentFrameCamera`
- `engine/include/DonTopo/Editor/Command.h` + `engine/src/Editor/Command.cpp` — `CameraState`, `CameraComponentCommand`
- `engine/include/DonTopo/Editor/PropertiesPanel.h` + `engine/src/Editor/PropertiesPanel.cpp` — section + Add gate
- `engine/include/DonTopo/Editor/ScenePanel.h` + `engine/src/Editor/ScenePanel.cpp` — `createCamera` + menu items
- `engine/include/DonTopo/Editor/ViewportPanel.h` + `engine/src/Editor/ViewportPanel.cpp` — `drawCameraGizmo`
- `engine/src/Editor/EditorUI.cpp` — Play warning + drain `lastWarnings`
- `engine/tests/camera_tests.cpp` (new) + `engine/tests/CMakeLists.txt`

Not touched: `sandbox/**`, CI, `cmake/`, `DonTopo::Camera`.

## Acceptance Criteria

- `build.bat` compiles clean.
- `dt_physics_tests` and `dt_content_browser_tests` still pass.
- `dt_camera_tests` passes, covering (a) no second camera, (b) serialization round-trip,
  (c) `findCamera()` locates the camera anywhere in the tree.
- Manual GUI verification above confirmed by the user.
