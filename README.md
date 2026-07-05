# Don Topo Engine

A Vulkan-based game engine written in C++20.

## Features

- PBR rendering (Cook-Torrance GGX, ACES tonemapping)
- GPU skeletal animation (compute shader skinning: bone eval → hierarchy → skinning)
- Shadow mapping (PCF 3×3)
- Normal maps + tangent space
- Cubemap skybox (fullscreen quad, inverse view-projection)
- 3D spatial audio (FMOD)
- Dockable ImGui editor with offscreen viewport
- Scene graph (hierarchical transforms), GameObject hierarchy panel (create/delete/rename, drag-drop reorder)
- ImGuizmo transform gizmo (translate/rotate/scale, camera-oriented axis gizmo)
- Physics (PhysX): static `BoxCollider`, dynamic `RigidBody`, raycasting
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
| Build | CMake 3.25+ | — |
| Language | C++20 | — |

## Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| CMake | 3.25+ | Required |
| Vulkan SDK | 1.3+ | Required — includes `glslc` shader compiler |
| MSVC | 2022+ | Required on Windows |
| FMOD Studio API | Latest | Optional — audio disabled if not found |

GLFW, GLM, Assimp, stb_image, ImGui and ImGuiFileDialog are downloaded and built automatically by CMake.

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
├── cmake/          # Custom Find modules
├── engine/         # Core engine (static library: DonTopoEngine)
│   ├── include/    # Public headers (DonTopo/)
│   └── src/        # Implementation
├── sandbox/        # Test playground executable (Sandbox)
└── shaders/        # GLSL sources + compiled SPIR-V
```

## Planned

| System | Candidates |
| --- | --- |
| Cascaded shadow maps | — |
| PBR environment maps / IBL | — |
| Post-processing | Bloom, SSAO, TAA |

## License

TBD
