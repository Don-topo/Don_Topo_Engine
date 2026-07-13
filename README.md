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
- Physics (PhysX): Box/Sphere/Capsule/Plane colliders (toggleable dynamic/kinematic via gravity), raycasting
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

GLFW, GLM, Assimp, stb_image, ImGui, ImGuiFileDialog, ImGuizmo, PhysX, nlohmann/json, Lua and sol2 are downloaded and built automatically by CMake.

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
├── cmake/          # Custom Find modules (PhysX, Lua)
├── engine/         # Core engine (static library: DonTopoEngine)
│   ├── include/    # Public headers (DonTopo/)
│   └── src/        # Implementation
├── sandbox/        # Test playground executable (Sandbox)
└── shaders/        # GLSL sources + compiled SPIR-V
```

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
