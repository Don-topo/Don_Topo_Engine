# Don Topo Engine

A Vulkan-based game engine written in C++20.

## Features

- PBR rendering (Cook-Torrance GGX, ACES tonemapping)
- GPU skeletal animation (compute shader skinning: bone eval → hierarchy → skinning)
- Shadow mapping (PCF 3×3)
- Normal maps + tangent space
- Cubemap skybox (fullscreen quad, inverse view-projection)
- Wireframe render mode
- 3D spatial audio (FMOD): `AudioClipComponent` (loop, 3D/2D toggle)
- Dockable ImGui editor with offscreen viewport
- Scene graph (hierarchical transforms), GameObject hierarchy panel (create/delete/rename, drag-drop reorder)
- Basic shapes menu (Cube/Sphere/Plane/Capsule), Content Browser (asset browsing, rename/delete)
- ImGuizmo transform gizmo (translate/rotate/scale, camera-oriented axis gizmo), debug-draw gizmos, collider gizmos
- **Camera component**: any GameObject can be the scene camera (perspective/orthographic, fov, near/far); frustum gizmo in edit mode, renders from it on Play
- **Animator component**: Unity-style animation state graph (node = clip, link = transition; `bool`/`trigger`/`animation finished` conditions), edited in a node panel; instant-cut transitions (no blending), driven from Lua
- Physics (PhysX): Box/Sphere/Capsule/Plane colliders (shape only) + `Rigidbody` (mass, gravity, drag, kinematic, 6-axis constraints, forces/impulses), raycasting
- Scene serialization (JSON save/load, full GameObject tree incl. mesh/colliders/audio/scripts)
- Play Mode (edit/play toggle, snapshot restore, physics gated to Play)
- Log Console panel (edit-action history, live value editing)
- **Lua scripting**: `ScriptComponent` (multiple per GameObject), Unity-style lifecycle (Awake/Start/Update/FixedUpdate/LateUpdate/OnDestroy), Entity/Transform/Scene/Input/Audio API, hot reload, auto-generated property UI
- FBX / OBJ model loading (embedded textures supported)

## Tech Stack

| Component | Library | Source |
| --- | --- | --- |
| Graphics | Vulkan | System SDK |
| Window / Input | GLFW 3.4 | Auto-fetched |
| Math | GLM 1.0.1 | Auto-fetched |
| 3D Model loading | Assimp | Auto-fetched |
| Image loading | stb_image | Auto-fetched |
| Editor UI | Dear ImGui | Auto-fetched |
| File dialog | ImGuiFileDialog | Auto-fetched |
| Transform gizmo | ImGuizmo | Auto-fetched |
| Node graph UI | imgui-node-editor (thedmd) | Auto-fetched |
| Physics | NVIDIA PhysX 5.8.0 | Auto-fetched |
| Audio | FMOD Studio (optional) | Manual install |
| Scene serialization | nlohmann/json 3.11.3 | Auto-fetched |
| Scripting | Lua 5.4.7 + sol2 3.3.0 | Auto-fetched |
| Build | CMake 3.25+ | — |
| Language | C++20 | — |

## Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| CMake | 3.25+ | Required |
| Vulkan SDK | 1.3+ | Required — includes `glslc` shader compiler |
| MSVC | 2022+ | Required on Windows |
| FMOD Studio API | Latest | Optional — audio disabled if not found |

GLFW, GLM, Assimp, stb_image, ImGui, ImGuiFileDialog, ImGuizmo, imgui-node-editor, PhysX, nlohmann/json, Lua and sol2 are downloaded and built automatically by CMake.

## Build (Windows)

```batch
# Configure + build (release)
configure.bat
build-release.bat

# Run
build-ninja-release\sandbox\Sandbox.exe
```

Or via VS Code: `Ctrl+Shift+B` → **Build Release**.

Shaders are compiled from `shaders/*.{vert,frag,comp}` to SPIR-V automatically during build and copied to both the executable directory and `shaders/`.

## Project Structure

```
Don_Topo_Engine/
├── assets/         # Runtime assets (models, textures, audio, skybox)
├── Scripts/        # Lua gameplay scripts (Scripts/<Name>.lua defines global table <Name>)
├── cmake/          # Custom Find modules (PhysX, Lua, FMOD)
├── docs/           # Design specs and implementation plans (superpowers/)
├── engine/         # Core engine (static library: DonTopoEngine)
│   ├── include/    # Public headers, mirroring the module layout (DonTopo/<Module>/)
│   ├── src/        # Implementation, split into seven modules:
│   │   ├── Core/       # Engine loop, Window, Input, Scene, GameObject, Camera
│   │   ├── Renderer/   # Vulkan device, meshes, materials, model loading, skybox
│   │   ├── Physics/    # PhysX integration, Rigidbody, Colliders/
│   │   ├── Audio/      # FMOD wrapper, AudioClipComponent
│   │   ├── Scripting/  # Lua/sol2 bindings, ScriptManager, syntax check
│   │   ├── Editor/     # ImGui panels, gizmos, undo/redo
│   │   └── Files/      # Filesystem helpers
│   └── tests/      # Headless unit tests (GoogleTest)
├── sandbox/        # Test playground executable (Sandbox)
└── shaders/        # GLSL sources + compiled SPIR-V
```

## Camera

Any GameObject can be the scene's camera — via **Properties → Add → Camera**, or in one
click with **right-click → Create Camera** in the Scene panel. The GameObject's transform
supplies position and orientation; the component supplies only the projection (perspective
or orthographic, fov / orthographic size, near, far). Aspect ratio comes from the viewport,
so resizing never stretches the image.

**At most one camera per scene**, enforced through `Scene::findCamera()` as the single
source of truth: **Add → Camera** greys out (the tooltip names the GameObject that already
holds one) and **Create Camera** disappears once a camera exists. Loading a hand-edited
scene that contains two keeps the first in pre-order, drops the extra component (the
GameObject survives) and says so in the Log.

In edit mode a cyan wireframe draws the camera's frustum, built from the component's own
matrices — the same ones the renderer uses, so the gizmo cannot promise a framing that Play
won't deliver. On Play the renderer switches to that camera; on Stop it returns to the
editor's fly camera exactly where it was. With no camera in the scene, Play still starts,
falls back to the editor camera, and logs why the view didn't change.

## Animator

A Unity-style animation state machine for skinned meshes. A **node** is a state holding one
of the model's animation clips; a **link** is a directed transition. There is no blending —
a transition is an instant cut. The component is opt-in: **Properties → Add → Animator**,
greyed out on non-skinned objects (an Animator has no clips to name without a skeleton).

Open the graph with **View → Animator**. In the node panel:

- **Add State from Clip** adds a node from one of the model's clips.
- Drag from a node's **output pin** to another's **input pin** to create a transition.
- Right-click a node → **Set as Entry** to mark the entry state (shown tinted); a state whose
  clip name no longer resolves against the model is flagged red.
- Right-click a link to edit its **conditions**; each node has a **loop** checkbox.

A parameter is one of four types — **`bool`**, **`trigger`**, **`int`** or **`float`** —
declared in the Animator's parameter list and set/queried from code by name. A condition
matches a `bool` or `trigger` parameter's own value, or, independent of any parameter,
**`animation finished`** (the current clip reached its end). `int`/`float` parameters
condition by comparing a threshold with `>`, `<`, `==` or `!=` — the last two (`==`/`!=`)
are offered only for `int`, since equality on a float is rarely what fires. A transition
fires when *all* its conditions hold; a transition with no conditions never fires.

The graph only evaluates transitions in **Play** mode. In **Edit** the entry state's clip
previews in place. Stopping Play resets to the entry state — the scene rebuilds from its JSON,
so no runtime state is carried over.

Drive it from Lua via `GetComponent("Animator")`:

```lua
local anim = self.entity:GetComponent("Animator")
anim:SetBool("running", true)
anim:SetTrigger("jump")
anim:SetInt("combo", anim:GetInt("combo") + 1)
if anim:GetState() == "Jump" then
    -- ...
end
```

`SetInt(name, v)` / `GetInt(name)` and `SetFloat(name, v)` / `GetFloat(name)` read/write
the graph's numeric parameters the same way `SetBool`/`GetBool` do. All four setters
silently ignore an undeclared name or a name of the wrong type; the getters return `0`
for an unknown name — none of them throws over a bad *name*. (They do still throw if the
GameObject lost its Animator component between `GetComponent` and the call.)

The whole graph — nodes, canvas positions, links, conditions, parameters, per-node loop and
the entry state — is saved in the scene file. Clips are referenced **by name**, so
re-exporting the model with a clip renamed unlinks that state (it warns on load rather than
silently pointing at the wrong animation).

## Lua Scripting

Attach one or more `ScriptComponent`s to a GameObject via **Properties → Add → Script**
(or **Add → Script → Nuevo Script...** to scaffold a new `.lua` file from a template).
Editing a loaded script while the engine is running hot-reloads it (~1s polling),
preserving serializable property values.

Double-clicking a `.lua` file in the Content Browser (or the **Edit** button next to a
`ScriptComponent` in Properties) opens it in the **Script Editor** panel — a multi-tab
code editor (ImGuiColorTextEdit, Lua syntax highlighting) docked alongside the other
panels. `Ctrl+S` or the **Save** button writes the file to disk; the existing hot-reload
polling picks up the change like any external edit. Closing a tab with unsaved changes
prompts to save/discard/cancel.

Saving (`Ctrl+S`/**Save**) also runs a syntax-only compile check; a Lua syntax
error is shown as an inline marker on the offending line (hover for the
message) and clears automatically on the next successful save. While typing,
an autocomplete popup suggests Lua keywords and the scripting API (`Entity`,
`Transform`, `Log`, `Input`, colliders, `Scene`, etc.) filtered by prefix —
`Enter`/`Tab` accepts, `Escape` dismisses, `Ctrl+Space` re-opens it manually.

```lua
-- Scripts/Rotator.lua — filename == global table name (the script's class)
Rotator = {
    speed = 45   -- serializable props (number/boolean/string) auto-show in the editor
}

function Rotator:Awake() end
function Rotator:Start() end
function Rotator:Update(dt)
    local t = self.entity:GetTransform()
    t:Rotate(Vec3.new(0, self.speed * dt, 0))
end
function Rotator:FixedUpdate(dt) end
function Rotator:LateUpdate() end
function Rotator:OnDestroy() end
```

API surface: `self.entity` (`GetTransform`, `GetComponent`/`AddComponent`/`RemoveComponent`,
`GetParent`/`GetChildren`), `Transform` (position/rotation/scale, `Translate`/`Rotate`),
`Scene` (`Find`/`CreateGameObject`/`Instantiate`/`Destroy`), `Input` (`IsKeyDown`/`IsKeyPressed`/
`IsKeyReleased`, `Key.*`), `Log.Info/Warn/Error` (+ `print`) routed to the Log Console. Scripts
only run in Play Mode; a broken script never crashes the engine (compile/runtime errors are
logged and the component is quarantined). See `Scripts/Rotator.lua` and `Scripts/Mover.lua`.

## Planned

| System | Candidates |
| --- | --- |
| Cascaded shadow maps | — |
| PBR environment maps / IBL | — |
| Post-processing | Bloom, SSAO, TAA |

## License

TBD
