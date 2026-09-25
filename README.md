# Don Topo Engine

A game engine written in C++20, with two interchangeable render backends: **Vulkan** and
**DirectX 12**. It ships an ImGui editor, a PhysX-backed scene, Lua gameplay scripting and a
game exporter that packages a standalone runtime linking no editor code at all.

## Contents

- [Features](#features)
- [Tech Stack](#tech-stack)
- [Getting Started](#getting-started) — [Prerequisites](#prerequisites) · [Build (Windows)](#build-windows) · [Build (Linux)](#build-linux)
- [Project Structure](#project-structure)
- [Rendering](#rendering) — [DirectX 12 Backend](#directx-12-backend) · [HDR & Bloom](#hdr--bloom) · [Screen-Space Effects](#screen-space-effects) · [Motion Blur](#motion-blur) · [Anti-aliasing](#anti-aliasing) · [Lights](#lights) · [Reflection Probes](#reflection-probes) · [Skybox & Environment](#skybox--environment) · [Mesh Visibility](#mesh-visibility) · [Batching, Culling & Asset Streaming](#batching-culling--asset-streaming)
- [Editor](#editor) — [Selection Outline](#selection-outline) · [Viewport Picking](#viewport-picking) · [Viewport Transform Gizmo](#viewport-transform-gizmo) · [Scene, Play Mode & Undo](#scene-play-mode--undo)
- [Gameplay](#gameplay) — [Camera](#camera) · [Physics](#physics) · [Audio](#audio) · [Game UI](#game-ui)
- [Animation](#animation) — [Animator](#animator)
- [Tooling](#tooling) — [Performance Panel](#performance-panel) · [Rendering Panel](#rendering-panel) · [Sprite Editor](#sprite-editor) · [Input Actions](#input-actions) · [Export Game](#export-game)
- [Scripting](#scripting) — [Lua Scripting](#lua-scripting)
- [Planned](#planned)
- [License](#license)

## Features

One line each; the detail lives in the section each row points at.

| Rendering | What it is |
| --- | --- |
| [Two render backends](#directx-12-backend) | Vulkan and DirectX 12, picked per project, from the same GLSL sources and with the same feature set |
| [PBR and image-based lighting](#hdr--bloom) | Cook-Torrance GGX lit in linear float; the skybox cubemap is convolved into an irradiance map and a roughness-prefiltered one, with a live `Ambient (IBL)` weight |
| [HDR bloom](#hdr--bloom) | Soft-knee threshold, compute mip chain, additive composition; ACES + gamma applied once, in the composition pass |
| [SSAO](#ssao) | Depth-only pre-pass plus a 16-sample compute occlusion and its blur, on the ambient term only |
| [Screen-space reflections](#ssr) | View-space ray march with binary refinement, enabled and weighted **per GameObject** |
| [Volumetric fog](#volumetric-fog) | Height-exponential fog with Henyey-Greenstein in-scattering and the key light's cascaded shadows |
| [Motion blur](#motion-blur) | Camera motion blur from the reprojected depth pre-pass; objects contribute no velocity of their own |
| [Lights](#lights) | `Point` / `Spot` / `Directional` / `Area` on any GameObject, several of each, 64 of them reach the shader |
| [Shadows](#shadows-beyond-the-key-light) | 4 cascades for the key light and 6 shared layers for secondary spots and points, PCF 3×3 |
| [Reflection probes](#reflection-probes) | Placeable captures that replace the global IBL inside their radius, baked on demand and never per frame |
| [Anti-aliasing](#anti-aliasing) | `None` / `FXAA` / `SSAA` / `MSAA` / `TAA`, mutually exclusive and switchable at runtime |
| [Forward+ light culling](#lights) | `Off` / `Tiled` / `Clustered` compute pre-pass that bins lights into a screen grid |
| [GPU skeletal animation](#animator) | Compute skinning (bone eval → hierarchy → skinning); a mesh with no Animator loops its first clip |
| [Skybox and environment](#skybox--environment) | Cubemap picked per project and swapped live; changing it re-convolves the ambient lighting |
| Normal maps, wireframe | Tangent-space normal mapping, and a wireframe render mode |
| [Mesh visibility](#mesh-visibility) | A `Visible` checkbox that removes the mesh from every pass while physics and scripting keep running |
| [Batching, culling, streaming](#batching-culling--asset-streaming) | Instanced draws of shared meshes, frustum culling in every pass, async asset loading |

| Editor | What it is |
| --- | --- |
| Dockable ImGui editor | Offscreen viewport, hierarchy with drag-drop reorder, Content Browser, basic shapes and a Log Console |
| [Transform gizmo](#viewport-transform-gizmo) | ImGuizmo move / rotate / scale with `W` / `E` / `R`; one drag leaves one undoable command |
| [Selection outline](#selection-outline) | An orange inverted-hull contour around the selected object |
| [Click-to-select](#viewport-picking) | CPU ray picking in the viewport; clicking empty space clears the selection |
| [Play Mode and undo](#scene-play-mode--undo) | Snapshot restore, physics gated to Play, and undo/redo that also works while playing |
| [Scene serialization](#scene-play-mode--undo) | JSON save/load of the whole GameObject tree |
| [Rendering panel](#rendering-panel) | The 43 render settings, each undoable and persisted to `project.json`, with its own GPU time |
| [Performance panel](#performance-panel) | Framerate graphs, GPU time per pass, draw counters and RAM/CPU/VRAM — free while closed |
| [Sprite editor](#sprite-editor) · [Input actions](#input-actions) | Slice a texture into named sprites; bind named actions to keys, mouse and gamepad |
| [Export Game](#export-game) | Packages a standalone runtime that links no editor code at all |

| Gameplay | What it is |
| --- | --- |
| [Camera component](#camera) | Any GameObject can be the scene camera; frustum gizmo in edit mode, renders from it on Play |
| [Animator](#animator) | State graph with layers, 1D/2D blends, sub-state machines, IK, property clips and clip curves |
| [Physics](#physics) | PhysX colliders and `Rigidbody`, triggers, raycasts and collision layers |
| [Audio](#audio) | FMOD 3D spatial clips and one Audio Listener per scene |
| [Game UI](#game-ui) | Fourteen data-only UI components, on screen-space and world-space canvases |
| [Lua scripting](#lua-scripting) | `ScriptComponent` with a Unity-style lifecycle, hot reload and auto-generated property UI |
| Model loading | FBX, OBJ, glTF / GLB (no Draco), embedded textures and textures in subfolders |

## Tech Stack

| Component | Library | Source |
| --- | --- | --- |
| Graphics | Vulkan | System SDK |
| Graphics (2nd backend) | Direct3D 12 | Windows SDK |
| DX12 memory allocator | D3D12MemoryAllocator 3.0.1 | Auto-fetched |
| Window / Input | GLFW 3.4 | Auto-fetched |
| Math | GLM 1.0.1 | Auto-fetched |
| 3D Model loading | Assimp 5.3.1 | Auto-fetched |
| Image loading | stb_image | Auto-fetched |
| Editor UI | Dear ImGui (`docking` branch) | Auto-fetched |
| File dialog | ImGuiFileDialog | Auto-fetched |
| Transform gizmo | ImGuizmo | Auto-fetched |
| Node graph UI | imgui-node-editor (thedmd) | Auto-fetched |
| Script code editor | ImGuiColorTextEdit | Auto-fetched |
| Font rasterisation (game UI) | FreeType 2.13.3 | Auto-fetched |
| SDF glyph atlas (game UI) | msdfgen 1.12 | Auto-fetched |
| Physics | NVIDIA PhysX 5.8.0 | Auto-fetched |
| Audio | FMOD Studio (optional) | Manual install |
| Scene serialization | nlohmann/json 3.11.3 | Auto-fetched |
| Scripting | Lua 5.4.7 + sol2 3.3.0 | Auto-fetched |
| Build | CMake 3.25+ | — |
| Language | C++20 | — |

## Getting Started

### Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| CMake | 3.25+ | Required |
| Vulkan SDK | 1.3+ | Required — includes `glslc`, and also `spirv-cross` and `dxc` for the DX12 backend |
| MSVC | 2022+ | Required on Windows — the Windows SDK it installs supplies `d3d12`/`dxgi` |
| FMOD Studio API | Latest | Optional — audio disabled if not found |

GLFW, GLM, Assimp, stb_image, ImGui, ImGuiFileDialog, ImGuizmo, ImGuiColorTextEdit,
imgui-node-editor, PhysX, nlohmann/json, Lua, sol2, FreeType, msdfgen and D3D12MemoryAllocator
are downloaded and built automatically by CMake.

The DirectX 12 backend is built by default on Windows (`DTE_ENABLE_D3D12=ON`) and forced off
everywhere else. It needs no extra download — `spirv-cross` and `dxc` ship inside the Vulkan SDK
you already have — but configure with `-DDTE_ENABLE_D3D12=OFF` to skip it and build Vulkan only.

### Build (Windows)

```batch
# Debug (build-ninja\)
configure.bat
build.bat
build-ninja\sandbox\Sandbox.exe

# Release (build-ninja-release\)
configure-release.bat
build-release.bat
build-ninja-release\sandbox\Sandbox.exe
```

Each configuration has its own build directory and its own configure step: `build-release.bat`
fails until `configure-release.bat` has generated `build-ninja-release\`.

Or via VS Code: `Ctrl+Shift+B` → **Build Release**.

**Ship games from a Release editor.** A Debug build links the MSVC debug CRT (`ucrtbased.dll`,
`MSVCP140D.dll` and friends), which Microsoft does not allow redistributing and which only exists
on machines with Visual Studio installed. A package exported from a Debug editor runs on the
machine that produced it and dies with a missing-DLL error anywhere else — File > Export Game
warns when this applies. Debug packages are still fine for testing locally.

**Splash screen.** The exported game shows the engine logo while it loads, covering the black
window during Vulkan/asset init. The exporter copies `assets/MainEngineLogo.png` into the package
as `splash.png`; if the PNG is missing the game just starts without a splash (never blocked). The
logo fades in, holds during load, and fades to the scene. The logo is fixed for now (not yet
configurable per project). Only the exported runtime shows it — the editor does not.

**No console window, logs go to a file.** The exported game links against the Windows subsystem, so
double-clicking it opens the game window and nothing else. Everything the engine and Lua `print()`
would have written to the terminal goes to `game.log`, created next to the executable on every run
(overwritten each time). Errors that stop the game — a scene that fails to load, an unhandled
exception — also pop up a message box, so the window never just disappears without explanation.
The editor (`Sandbox.exe`) keeps its console: run it from a terminal and its output stays there,
while Lua `print()` goes to the editor's Log Console panel.

Shaders are compiled from `shaders/*.{vert,frag,comp}` to SPIR-V automatically during build and
copied to both the executable directory and `shaders/`. With the DX12 backend enabled each SPIR-V
module is then translated to HLSL and lowered to DXIL in the same step (see
[DirectX 12 Backend](#directx-12-backend)). The source
list is globbed, so a brand-new shader needs a re-run of `configure.bat` before `build.bat` will
see it.

**Tests.** The suite is headless — no Vulkan device, no window — and builds as one executable per
area under `build-ninja\engine\tests\`. There is no test framework and no CTest registration: each
file is a `main` with asserts that returns non-zero on failure, so running them is just running them.

```batch
build.bat
for %f in (build-ninja\engine\tests\dt_*_tests.exe) do @%f
```

### Build (Linux)

Recent Ubuntu/Debian (GCC 12+, CMake 3.25+):

```bash
sudo apt install build-essential cmake ninja-build glslc libvulkan-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols pkg-config
# FMOD: download "FMOD Engine" for Linux from fmod.com and unpack it into third_party/fmod
./configure.sh && ./build.sh                               # Debug   (build-linux/)
./configure.sh linux-release && ./build.sh linux-release   # Release (build-linux-release/)
```

Tests are launched **from the repository root** (`build-linux/engine/tests/dt_*`) — several of
them resolve `assets/` relative to the working directory. There is no DirectX 12 backend on
Linux; the editor runs on Vulkan. The `linux` GitHub Actions job builds Release on Ubuntu 24.04
and runs the whole suite on every push, without FMOD — everything has to compile and pass with
audio disabled too.

**Audio.** FMOD outputs through PulseAudio or ALSA (`libpulse0`, `libasound2t64`). A desktop
Ubuntu already ships them; the minimal WSL image does not, and without them FMOD starts with no
output and nothing is heard. Under WSLg everything works — window and audio — with software
Vulkan (llvmpipe).

## Project Structure

```text
Don_Topo_Engine/
├── assets/         # Runtime assets (models, textures, audio, skybox)
├── Scripts/        # Lua gameplay scripts (Scripts/<Name>.lua defines global table <Name>)
├── cmake/          # Custom Find modules (PhysX, Lua, FMOD)
├── docs/           # Design specs and implementation plans (superpowers/)
├── engine/         # Two static libraries: DonTopoCore and DonTopoEditor
│   ├── include/    # Public headers, mirroring the module layout (DonTopo/<Module>/)
│   ├── src/        # Implementation, split into eight modules:
│   │   ├── Core/       # Engine loop, Window, Input, Scene, GameObject, Camera
│   │   ├── Renderer/   # Vulkan device, meshes, materials, model loading, skybox, gizmos
│   │   │   └── D3D12/  # DirectX 12 backend (same feature set, own device/PSOs)
│   │   ├── Physics/    # PhysX integration, Rigidbody, Colliders/
│   │   ├── Audio/      # FMOD wrapper, AudioClipComponent, AudioListenerComponent
│   │   ├── Scripting/  # Lua/sol2 bindings, ScriptManager, syntax check
│   │   ├── UI/         # Game UI: canvas tree, sprite batch, atlas, MSDF fonts
│   │   ├── Editor/     # ImGui panels, undo/redo, game exporter  -> DonTopoEditor
│   │   └── Files/      # Filesystem helpers
│   └── tests/      # Headless unit tests (plain main + asserts), one executable per area
├── runtime/        # Standalone game runtime (DonTopoRuntime) — links Core only
├── sandbox/        # Editor executable / test playground (Sandbox)
└── shaders/        # GLSL sources + compiled SPIR-V
```

Everything outside `src/Editor/` builds into **`DonTopoCore`**; the panels, the undo stack, the
exporter and the ImGui backends build into **`DonTopoEditor`**, which depends on Core and never
the other way round. The renderer only ever sees the editor through a `UiLayer` interface, so
`DonTopoRuntime` links Core alone and pulls in no ImGui symbols at all.

## Rendering

### DirectX 12 Backend

A second, complete render backend. It is chosen per project in **View → Rendering → Render
backend**, stored as a name in `project.json`, and applied on the next start — a device, its swapchain and every
pipeline are built once at init, so nothing can swap them mid-run. **File → Export Game** has its
own selector: what the packaged game starts with is written to `game.cfg`, and it need not match
the editor you exported from. If a build was configured without DX12, or the machine cannot
provide it, startup falls back to Vulkan and says why — in the Log Console for the editor, in
`game.log` for the game.

**One shader source, two APIs.** The `shaders/*.{vert,frag,comp}` GLSL files remain the only
source. `glslc` produces the SPIR-V that Vulkan consumes; the DX12 branch hangs off *that same
SPIR-V*, which `spirv-cross` translates to HLSL and `dxc` lowers to DXIL. Nothing is written
twice, and the HLSL is by construction a translation of exactly the module Vulkan runs. Shader
model 6.0 is the floor: below 5.1 there are no register spaces, and `pbr.frag` — which declares
eight descriptor sets — would collide. One consequence worth knowing when reading the D3D12 input
layouts: spirv-cross names vertex inputs by *location*, so every semantic comes out as
`TEXCOORD`n, `POSITION` included.

Feature parity with the Vulkan path: PBR and image-based lighting, cascaded shadows, SSAO, SSR,
volumetric fog, motion blur, bloom, the five anti-aliasing modes, Forward+ light culling,
frustum culling, reflection probes, the 2D game UI, instanced draws of repeated meshes,
per-pass GPU timings in the Performance panel (D3D12 timestamp queries, same read-from-`N-2`
rule), the scene `CameraComponent` on Play, and the exported runtime.

The Forward+ **statistics** — average lights per cell and the number of cells that overflowed
their list — are read back from the GPU here too, rather than being wired to zero: the two
numbers are what tells you whether the culling is doing anything, so a backend that reports
zero is indistinguishable from one where every cell is empty.

Four places where the implementation differs rather than the result:

- **Probe assignment is per GameObject**, not per shared mesh. Objects that share a mesh share
  its buffers and textures but keep their own descriptor block, so two copies of the same crate
  in two rooms reflect their own room. The instanced draw survives because objects only group
  when their blocks say the same thing — the probe is part of the grouping key.
- **A probe bake submits one command list per face.** The six faces share one scene constant
  buffer, and D3D12 reads that memory when it *executes*, not when the command was recorded:
  recorded back to back, all six faces would render with the sixth one's camera.
- **Instanced draws offset the instance buffer view** instead of using
  `StartInstanceLocation`. `gl_InstanceIndex` includes the base instance by specification;
  `SV_InstanceID`, which spirv-cross translates it to, is not guaranteed to. Pointing the view at
  the group's first matrix costs the same and does not depend on the driver.
- **The selection outline needs the depth pre-pass under MSAA.** It is drawn over the LDR target,
  after the tonemap, so its orange arrives flat; that target has one sample and the scene's depth
  has N, so with MSAA the single-sample depth from the pre-pass is used instead — which is why
  that pre-pass also runs when something is selected.

### HDR & Bloom

The frame is rendered in three stages. The **scene pass** draws geometry and skybox into an
`R16G16B16A16_SFLOAT` target and writes linear radiance with no clamping — material shaders do
no tonemapping at all. A **compute chain** then extracts the bright parts and blurs them: the
first downsample applies a threshold with a soft knee (a quadratic ramp instead of a hard cut,
so a surface crossing the threshold fades in rather than popping as the camera moves), each
level halves the resolution with a 13-tap filter, and the way back up adds each mip into the
one above it with a 3×3 tent. Finally the **composition pass** samples the HDR scene, adds the
bloom, and applies ACES + gamma — the single point in the engine where HDR becomes LDR — into
the LDR image the editor viewport samples and the standalone runtime blits to the swapchain.

The chain starts at half viewport resolution and runs 5 mips; it is recreated with the
swapchain, so resizing never leaks it. Every level lives in `VK_IMAGE_LAYOUT_GENERAL` for the
whole chain, which is valid both for `imageStore` and for sampling, so the passes are separated
by plain memory barriers instead of layout transitions.

`threshold`, `knee` and `intensity` are push constants of the bloom pipelines, not UBO fields,
so they take effect on the next frame without recreating anything — the UBO block is declared in
five shaders and adding a member there would silently shift everything behind it under std140.
The **Rendering** panel exposes the three as sliders plus the measured GPU cost (~0.2 ms at 1280×720,
including composition and tonemap). With `intensity = 0` the image is identical to the one
before the feature existed, which is the check that the tonemap was moved without drift.

The selection outline and the gizmos are drawn **in the composition pass**, after the tonemap,
so they keep their exact flat colours and never bloom. The skybox stays in the scene pass and is
tonemapped with the rest — that is what lets a bright sky feed the bloom.

### Screen-Space Effects

Several effects share one **depth-only pre-pass** that draws the whole scene into a sampled
`D32_SFLOAT` image before the scene pass, with the same frustum culling and the same instanced
batching as the real draw — occluding against geometry that is not actually drawn would be a
visible artifact. It is recorded when SSAO, SSR, TAA, Forward+ tiled **or** the volumetric fog
needs it, and skipped entirely when none does. SSAO and SSR reconstruct view-space position and a
geometric normal from that depth alone
(the normal comes from the smaller of the two depth slopes on each axis, so a pixel on a
silhouette does not blend two surfaces), which is why neither needs a G-buffer, an extra
attachment on the scene pass, or a new UBO member.

#### SSAO

A compute shader traces 16 samples in a cosine-weighted hemisphere around each pixel, packed
towards the origin where contact occlusion actually lives, with a per-pixel rotation so the
kernel does not band; a second pass blurs the result. The AO multiplies the **ambient term only**
— applying it to direct light would dim shadows the cascade maps already compute.

The **Rendering** panel carries a toggle plus `radius`, `bias`, `intensity` and `power`, all push
constants, so they take effect the next frame. Turned off, neither the pre-pass nor the two
dispatches are recorded: the AO map is cleared to 1.0 **once** (on creation and on switch-off)
and `pbr.frag` multiplies by unity, so the image is identical to the one before the feature and
the GPU cost is zero, not "computed and multiplied by zero".

#### SSR

Reflections run **after the scene pass** — they need colour that is already lit — and write into
the HDR target **before** the bloom chain, so a reflection blooms and goes through ACES exactly
like the surface it mirrors. That takes two dispatches: `ssr.comp` marches the ray and writes an
isolated reflection image, `ssr_resolve.comp` adds it into `m_hdrImage` texel by texel. They are
split on purpose — the march samples *arbitrary* pixels of the scene colour, so writing into that
same image would be a race; the resolve is strictly 1:1 and cannot be. The HDR image gained
`STORAGE_BIT` and is put back into `SHADER_READ_ONLY_OPTIMAL` before bloom and composition read it.

The ray marches in **view space**, reprojecting each step with the frame's four projection
coefficients, and a 4-step binary search refines the last segment — with linear steps alone the
hit lands up to a full step past the real contact and the reflection looks detached from the
object. A hit fades out towards the screen border, with ray length, and as the ray turns back
towards the camera (what it would reflect is behind the viewer, which is precisely what the
screen does not contain). A ray that finds nothing contributes **nothing**: `pbr.frag` already
adds the prefiltered cubemap for every pixel, so any fallback here would count the environment
twice.

Which surface reflects, and how much, is **per GameObject** (`Properties → Screen Space
Reflections`: an `Enable SSR` checkbox and a `Reflectivity` slider, both serialised with the
scene and honoured by the exported runtime). The value reaches the post-pass through the **alpha
channel of the HDR target**, which `pbr.frag` writes from a spare push-constant slot — before
this it was always 1.0 and nothing read it. Reflectivity acts as `F0` in a Schlick term, so 1.0
is a mirror at any angle while low values only show up at grazing incidence, which is how a
polished floor or water behaves. Because it is a push constant per shared entry — like `metallic`
and `roughness` already were — it also enters the **instancing key**: two objects sharing a mesh
but not a reflectivity are split into two draws, and nothing else about batching changes.

The **Rendering** panel has the global switch plus `distance`, `thickness`, `steps`, `edge fade` and
`intensity`, and reports the measured GPU cost (~0.3 ms at 1280×720 with 32 steps, pre-pass
included). With the switch off — or on with no object marked — not a single dispatch is recorded
and the HDR image is left exactly as the scene pass produced it.

Normals come from depth, not from an attachment, so the normal map's detail does not reach the
reflection: polished metal with a normal map mirrors as if it were flat. And being screen-space,
anything off-screen or hidden behind another object simply is not reflected.

#### Volumetric Fog

A single compute dispatch (`fog.comp`) recorded **after** the scene pass and the SSR — it needs
colour that is already lit and already has its reflections in — and **before** the bloom chain, so
the in-scattering blooms and goes through ACES like everything else. Unlike SSR it rewrites
`m_hdrImage` **in place**: every invocation touches only its own pixel, so there is no race and no
intermediate image is needed. The alpha channel is copied untouched — it carries the object's SSR
strength, not opacity.

Each pixel reconstructs its world position from the pre-pass depth (sky included: at `depth = 1`
that is the far plane, which is the right answer for height fog — the horizon fills in too) and
marches `steps` samples between the camera and that point, each pixel offset by an interleaved
gradient noise so few steps do not band into concentric rings. Density at a sample is
`density · exp(-(y - baseHeight) · heightFalloff)`, transmittance follows Beer-Lambert per segment,
and the energy each segment absorbs is exactly what can scatter towards the camera, weighted by a
**Henyey-Greenstein** phase term (`anisotropy > 0` = forward scattering, the halo you see looking
into the light) and by the **key light's cascaded shadow**, sampled with a single tap per step —
the accumulation plus the dither dissolve the noise that one tap leaves, and `pbr.frag`'s 3×3 PCF
would cost N times more here. That is what makes light shafts appear where geometry occludes the
key light.

The fog's parameters travel in a **128-byte push constant** of its own, never in the UBO — that
block is declared by six shaders and one new member would silently shift everything behind it
under `std140`. The key light's colour is folded into the scattering tint on the CPU because the
push constant is already at the exact 128 bytes Vulkan guarantees. The shader binds the UBO
declared only up to `cascadeSplits` (the members after it are laid out later, so omitting them
moves no offset) to get the view matrix and the four cascade matrices.

The **Rendering** panel carries the global switch plus `density`, `height falloff`, `base height`,
`anisotropy`, `steps` and the scattering colour, and reports the measured GPU cost. **Off by
default**: with the switch off not a single dispatch, barrier or timestamp is recorded, `Fog GPU`
reads `0.000 ms`, and the HDR image is left exactly as the scene pass and the SSR produced it.

### Motion Blur

**Camera** motion blur, in a compute pass. The per-pixel velocity is stored nowhere: it comes
from reprojecting the depth pre-pass with the same `prevViewProj × inverse(currViewProj)` the
TAA already uses. It runs after the fog and before the bloom chain, so a streak blooms with the
highlight that produced it. `intensity`, `max radius` and `samples` are live sliders, it is off
by default, and both backends have it.

Objects that move on their own contribute no velocity of their own: a spinning fan under a
still camera does not smear. That needs a per-object velocity buffer, which is on the
[Planned](#planned) list.

### Anti-aliasing

Five mutually exclusive modes, switched at runtime from the Rendering panel — `None`, `FXAA`,
`SSAA`, `MSAA` and `TAA` — each with its own resources, rebuilt between frames and never
mid-frame.

| Mode | What it costs | Its settings |
| --- | --- | --- |
| `FXAA` | A pass of its own | `subpixel`, `edge threshold`, `edge min` |
| `SSAA` | A pass of its own, on a larger render target | factor `1×`–`4×`, applied on release (it rebuilds targets) |
| `MSAA` | Spread through the render | sample count, from `1×` up to what the device supports for colour *and* depth |
| `TAA` | A pass of its own | `feedback`, `jitter` |
| `None` | — | — |

Because two of the five spread their cost instead of concentrating it in one pass, the section
prints the whole render's GPU time next to the AA pass's own: comparing that total against
`None` is the only way to read the real overhead of MSAA or supersampling.

### Lights

Any GameObject can be a light — **Properties → Add → Light**. The component holds only what
the light *is* (type, colour, intensity and the parameters of its shape); **where it is and
where it points come from the GameObject's transform**: position is the transform's
translation, direction is its **local −Z**, exactly like the camera. Moving or rotating the
object moves the light, and there is no second source of truth to keep in sync.

| Type | What it uses | Falloff |
| --- | --- | --- |
| `Point` | `Range` | smooth window, exactly 0 at `Range` |
| `Spot` | `Range`, `Inner`/`Outer Angle` | point falloff × `smoothstep` between the two cone angles |
| `Directional` | nothing else | none — same intensity everywhere, position ignored |
| `Area` | `Area Width`, `Area Height` | approximated as a point of radius `Width / 2` |

Properties shows **only the fields the selected type uses** — no area size on a spot light.
The hidden values stay in the component and in the `.scene`, so switching type and back loses
nothing. `Inner` and `Outer Angle` drag each other so the cone can never invert, and the clamp
lives in the component rather than in the UI, so a hand-edited scene cannot install a
degenerate light either. Everything is serialised under a `light` block that older scenes
simply don't have (they load unchanged).

**Several lights of each type per scene.** `Scene::collectLights()` walks the tree in pre-order
every frame and hands the renderer the first `MAX_LIGHTS` (**64**) it finds; the rest are
dropped — that is a limit of the UBO block, not an error in the scene. It is no longer dropped
*silently*: when the scene holds more than 64, the Rendering panel says so under **Forward+**,
in amber, with how many there are and how many actually light — because that is precisely the
feature that promises to scale the light count and that this ceiling caps. Raising the ceiling
means recompiling the shaders that declare the block, so it is not a UI setting. The block grew from two
`vec4` per light to four (`direction` carries the type in its `w`, `params` carries range, the
two cosines and the area width), which is why the shaders that declare it were all touched: in
std140 a struct that changes size shifts everything behind it.

**Forward+ light culling** is a compute pre-pass that bins the lights into a screen grid so
`pbr.frag` only iterates the ones that reach each pixel. It has three modes — `Off`, `Tiled`
and `Clustered` — and `Off` records no command at all and lights the scene exactly as before
the feature existed. Under it the same light data reaches the culling compute shaders, with one special case: a
`Directional` light has neither position nor range, so it is marked visible in *every* tile and
cluster instead of being tested against the volume. The radius used for binning is the same
reach the fragment shader uses (`Range`, or `Width / 2` for an area light) — if they differed, a
light would pop off as it crossed a tile edge.

#### Shadows beyond the key light

The shadow map is a **12-layer** array. The first six belong to the **key** light, which is
the only one that can use more than one: 4 if it is `Directional` (one per cascade), 6 if it
is `Point` or a spot open enough to need a cubemap, 1 if it is an ordinary spot. They never
coexist — there is one key light and it has one type.

The remaining **six** layers are shared by the secondary lights: **one layer per narrow
spot**, **six per point light** (or per spot whose shadow FOV would exceed 90°, where a
single face stops being the right technique — a texel's footprint goes with `tan(FOV/2)`, so
at 150° it is 3.7× worse than at 90°). A light either fits **whole or not at all**: reserving
three faces out of six would leave the shader sampling another light's layers, and no
validation layer catches that — the layer exists and has content, it just is not the right
one. A **secondary `Directional`** never casts: it would need its own four cascades to not
look worse than no shadow, and then it costs the same as the key. It still lights the scene.

Slots are handed out **in scene order**, not by brightness or distance to the camera, on
purpose: a camera-dependent criterion would make a light win and lose its shadow as you move,
and that flickers. The distribution lives in shared code rather than in each backend, because
the slot decides which layer is written *and* which matrix the shader samples — a different
split per backend would be the same scene with different lights shadowed.

The ceiling is **memory**: these layers share the texture array with the key's, and an array
has a single resolution, so at 2048 each one is 16 MB. Giving them a smaller array of their
own would allow many more, but it means a new binding and sampler in the six shaders that
declare the block, plus rebuilding the mesh, material and fog descriptor sets.

Filtering is **PCF 3×3** taken from the map's *real* size (`textureSize`), not a hardcoded
2048 — with a fixed value, raising the resolution neither widened nor narrowed the filter, and
at 1024 the nine taps fell inside half a texel and the PCF vanished. The key light and the
secondary spots share it, or the same geometry would get different edges depending on which
light shadowed it.

Three settings are live in **View → Rendering → Shadows**: the map **resolution**
(`1024`/`2048`/`4096`/`8192` — the only one of the three that moves resources), the **shadow
distance** the four cascades divide up (lowering it packs the same texels into less world and
sharpens nearby shadows, at the cost of nothing beyond), and the **cascade split** blend
between a logarithmic (1) and a uniform (0) distribution. The number of cascades is not among
them: it has to match the UBO array that five shaders declare and the layers of the texture
array.

In the editor an **orange gizmo** shows each light: a wire sphere of its range for `Point`,
that sphere plus the four edge generatrices of the cone for `Spot`, a long ray for
`Directional`, and a width × height grid with its normal for `Area`. It is drawn in edit mode
*and* in Play, and it lives in the editor's viewport panel — which is why the exported game,
that links no editor code, can never show it.

### Reflection Probes

A **Reflection Probe** is a component on any GameObject (**Properties → Add → Reflection
Probe**). It captures the environment from that GameObject's position and replaces the global
IBL — both the irradiance map and the roughness-prefiltered map — for every object that falls
inside its radius of influence. `Radius` and `Intensity` are serialised with the scene; the
cubemap is not, it is rebaked.

**The bake is an event, never a pass.** It does not record a single command into the frame's
command buffer: it is its own set of submits, triggered by the **Bake** button, by **Bake All
Reflection Probes** in the Rendering panel, or automatically when a probe has no valid capture yet (on
creation and on scene load, so `DonTopoRuntime` renders the same image as the editor without
anyone pressing anything). Auto-bake waits for the settings to stop moving, so dragging a
slider costs one bake on release rather than one per frame. With probes already baked the
per-frame GPU cost is identical to having none — measured at 0.77 ms with zero probes and
0.78–0.87 ms with four, inside the run-to-run noise of the same binary.

Each face is rendered by **reusing the existing scene pass**: same pipeline, same descriptor
sets, same shadow maps and skybox, into a square `renderArea` of the offscreen framebuffer,
then blitted into the cubemap layer. SSAO, SSR, bloom, AA, composition and UI are skipped —
the capture is linear HDR — and Forward+ is forced to `Off` for the duration, since its light
grid was culled against the frame's camera and not against these six faces. The convolution
then runs the **same two compute shaders** as the global IBL. Cost: **~0.86 ms of GPU and
1.05 MB per probe** (irradiance 32² + prefiltered 128²×5 mips, six faces, `rgba16f`), plus one
128² capture cubemap shared by all probes and created only on the first bake.

The probe's cubemap reaches `pbr.frag` by **rewriting bindings 5 and 6 of set 0** for the
affected objects — the same single-binding-write pattern the SSAO map already used. Nothing
else moves: no new descriptor set layout, no new UBO member (that block is declared in five
shaders and std140 would silently shift everything behind it) and no room needed in `PushData`,
which is 80 bytes exactly. `Intensity` is baked *into* the cubemap through a push constant of
the two convolution shaders, which is why changing it triggers a rebake while changing the
radius does not — the radius only decides who is affected.

The capture is always taken with the **global IBL** bound, never with the probes' own cubemaps.
Otherwise the scene would be photographed lit by the very probe being baked, and each bake would
re-multiply light that already carried the intensity — the effect amplifying (or fading) bake
after bake. Capturing against the global IBL makes the bake idempotent and independent of the
order the probes are processed.

Assignment is resolved by **nearest probe whose radius contains the object**, recomputed on the
CPU and pushed to the GPU only when it actually changes; an object outside every radius, and a
scene with no probes at all, keep the global IBL views they were given at allocation, so the
image is identical to the one before the feature. Deleting a probe or loading another scene
returns the affected objects to the global IBL *before* freeing the cubemaps, so no descriptor
set is ever left pointing at a dead view.

One deliberate limitation **of the Vulkan path**: the descriptor set is per **shared mesh**, not
per GameObject. Two instances of the same mesh under different probes share a probe — the first
one in traversal order wins. Splitting them would mean duplicating the sets and losing the
instanced draw. The DirectX 12 backend does not have this limitation: there the descriptor block
is per object and only the resources behind it are shared, so the probe is per GameObject and
still groups into one draw (see [DirectX 12 Backend](#directx-12-backend)).

### Skybox & Environment

The sky is a cubemap drawn as a fullscreen quad through the inverse view-projection, **picked
per project and swapped live**. The **Environment** window takes a folder holding the six faces
(`px`/`nx`/`py`/`ny`/`pz`/`nz`) — typed in, browsed for, or dragged in from the Content Browser
— and the choice is stored in `project.json`. Changing it also re-convolves the ambient
lighting, because the irradiance and prefiltered maps come from that same cubemap.

The window is its own, not a section of the Rendering panel: an ImGui popup closes when the
mouse is released outside it, so a folder cannot be dropped onto one. The Rendering panel's
**Skybox** section only holds the entry that opens it.

### Mesh Visibility

The Mesh component in the Properties panel carries a `Visible` checkbox, on by default. Unchecked,
the mesh is not handed to the GPU in **any** pass: it disappears from the scene pass, stops casting
shadows into the cascades, stops occluding in the SSAO pre-pass, and — if it is skinned — its
skinning compute is not dispatched and `updateAnimation` freezes its clock, so the pose resumes
where it left off instead of jumping forward when it is shown again. The selection outline is
skipped too: with no skinning dispatch there is no pose to trace.

Everything that is not drawing keeps running. Physics and colliders, click-to-select in the
viewport, transform gizmos, audio and scripting all behave as if the mesh were on screen — an
invisible trigger volume with a mesh attached still fires, and a script still finds its entity.

The flag is a plain `bool` on the GameObject, next to the SSR fields, synced to the renderer once
per frame alongside the transform. That is the same route the per-object SSR strength takes, so
Play Mode, undo/redo and scene loading need no path of their own. It is serialized inside the
`mesh` object of the scene JSON and defaults to `true`, so scenes saved before the feature load
exactly as they looked. Toggling it pushes an undo entry like any other property.

One thing the checkbox does *not* do is free GPU memory: the vertex buffers, textures and
descriptor sets stay resident so re-showing the mesh costs nothing. Removing the component with
the `x` button is still the way to release them.

### Batching, Culling & Asset Streaming

**Frustum culling** runs in the main, the shadow and the skinned passes. Skinned meshes are
bounded by a pose-independent sphere, so no character can vanish mid-animation because its pose
left the bounds its vertices had at load time.

**Draw batching**: objects sharing a mesh and a material collapse into one instanced draw, and
their GPU resources — buffers, textures — are deduplicated by a content key, so identical
meshes are uploaded once. Both backends. Whatever travels as a push constant per entry
(`metallic`, `roughness`, the per-object SSR reflectivity, and the reflection probe in the
D3D12 backend) is part of the grouping key: two objects that differ there are two draws.

**Async asset loading**: a worker thread pool (`JobSystem`) decodes images off-thread and the
GPU uploads are batched, with deferred visibility and deferred destruction — dropping a model
in or loading a scene never stalls on `vkDeviceWaitIdle`.

## Editor

| Shortcut | What it does |
| --- | --- |
| `W` / `E` / `R` | Transform gizmo: move / rotate / scale |
| Right mouse held | Flies the editor camera: `W`/`A`/`S`/`D` to move, `Q`/`E` down and up. Released, the letters go back to the gizmo |
| Gamepad | Flies the camera at all times — it competes with no shortcut |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo, in edit mode and during Play |
| `Ctrl+F` / `Ctrl+G` | Script Editor: find and replace, go to line |

### Selection Outline

Selecting a GameObject that carries a mesh — static or skinned — traces it with an orange
contour in the viewport; deselecting clears it the same frame. Objects without a mesh (empties,
cameras, pure collider nodes) get the usual axis gizmo but no outline, since there is no
geometry to trace. A mesh with `Visible` unchecked gets none either: it is drawn in no pass, and
for a skinned one the pose the outline would trace was never computed.

The technique is an **inverted hull**: the mesh is redrawn extruded along its normals with
front faces culled, in the composition pass, against the depth buffer the scene pass left
behind. It lives there and not in the scene pass so that the tonemap never touches it — its
flat orange has to stay the same orange. Only the rim that falls
outside the original silhouette survives — the rest of the hull lands behind the surface and
the depth test discards it. Stencil was not an option here: the depth attachment is
`D32_SFLOAT`, so a stencil buffer would have meant changing the format, the render pass, the
framebuffer and the image.

Sharing the scene's depth buffer, the outline obeys everything around it. It is hidden when
another object occludes the selection, it is skipped entirely when the selection is outside the
camera frustum (the same culling test the object itself went through, not a bypass), it follows
the skinned pose of the current frame rather than the rest pose, and in **wireframe mode** the
hull switches to line rasterisation — a filled hull would cover the object in flat colour there,
since only the edges write depth.

Outline thickness is proportional to the object's size in world space, so a crate and a
character show a comparable border on screen. Orange is deliberate: collider gizmos are yellow,
the camera frustum is cyan and the wireframe mode is green, so the selection never reads as one
of those.

The editor drives this through a single setter, `Renderer::setOutlineTarget`, called once per
frame with the selection's render indices (or `-1`). It defaults to "nothing selected", which is
what the exported runtime always sees — no outline is ever drawn there, and no editor code
reaches the runtime path.

### Viewport Picking

Left-clicking inside the viewport selects whatever mesh is under the cursor; clicking empty
space clears the selection. It is the same selection state the Scene panel writes
(`EditorContext::selected`), so the outline, the axis gizmo and the Properties panel all follow
in the same frame — the viewport does not keep a selection of its own.

Picking is a **CPU ray cast**, not a GPU id buffer: no extra render pass, no readback, no frame
of latency. The mouse position is taken relative to the **image rect** of the panel, not to the
ImGui window, and unprojected with the very camera the frame was rendered with — the editor fly
camera in edit mode, the scene `CameraComponent` on Play, Y-flip and Vulkan `z = [0, 1]`
included. The ray starts at the camera position (inverse of the view matrix) and aims at the far
plane, so the hit distance is a real world-space distance and the nearest object wins.

Each object is tested in two steps. First its bounding sphere, as a cheap reject; then the slab
test against its local AABB pushed through the transform — an oriented box in world space. The
second step is not optional: a floor plane is huge and flat, its bounding sphere swallows the
camera, and sphere-only picking would hand every click to the floor. Bounds come from the mesh
vertices, and for a `SkinnedMesh` from `skinnedVertices` (bind pose, since the animated pose only
exists on the GPU) — otherwise animated characters would never be pickable, their `Mesh::vertices`
being empty by design.

The click only picks when it is really a click on the scene: the image is hovered, no ImGui
widget is active, no ImGuizmo handle is hovered or being dragged, the camera axis gizmo did not
take the click, and no load modal is up. Dragging the transform gizmo therefore never changes the
selection.

### Viewport Transform Gizmo

Selecting a GameObject puts an ImGuizmo handle on it: drag it and the object moves, rotates or
scales live in the viewport, with the Properties panel and the collider keeping up. One mode at a
time — three handle sets at once would be unclickable — picked from the three toolbar buttons
(the active one takes the `ImGuiCol_ButtonActive` colour, the same idiom as `Wireframe` beside
it) or with **`W` / `E` / `R`**.

Those are Unity's keys, and they were not free: the editor fly camera uses `W`/`A`/`S`/`D` plus
`Q`/`E`. They were freed the way Unity frees them — `Camera::update` now takes a
`keyboardEnabled` flag, and both backends pass "is the right mouse button down". **Hold right
mouse to fly, release it and `W`/`E`/`R` switch gizmo mode.** The gamepad is deliberately outside
that split: it competes with no shortcut, so it still flies the camera at all times.

The handle **space** is not the same in all three, which is a decision and not an oversight.
Translate works in `WORLD`, so dragging "X" moves along world X however the object is oriented.
Rotate works in `LOCAL`, so the rings sit on the object's own axes — in `WORLD` they are drawn
world-aligned and, the moment the object is tilted, they match nothing on screen. Scale is
`LOCAL` because ImGuizmo forces it: `ComputeContext(..., (operation & SCALE) ? LOCAL : mode)`
discards whatever mode you pass. It gets `LOCAL` explicitly so the call does not lie. Scale
cannot be dragged through zero either — ImGuizmo clamps each axis at `0.001`, the same floor as
the Properties `DragFloat`, so no singular matrix ever reaches PhysX.

Three things make it agree with what is on screen.

**The projection is the frame's, minus the Vulkan Y-flip.** The renderer negates `proj[1][1]`
because Vulkan's NDC has Y pointing down (and D3D12 reaches the same image from the other side,
with a *negative-height* viewport). ImGuizmo does neither: it projects on the CPU and maps to
pixels with `y = 1 - y`, the OpenGL convention, Y up. Handing it the flipped matrix mirrors the
gizmo vertically — glued to the object only at the exact centre of the screen, and dragging Y
backwards. The canvas gizmos next door *do* use the flipped matrix, because their own pixel
mapping has no `1 - y` and the two negations cancel; here there is only one.

**What gets edited is `localTransform`, not the world matrix.** ImGuizmo manipulates a world
matrix; the scene serializes, the inspector edits and undo stores the *local* one. The
conversion is `inverse(parentWorld) * newWorld` — the inverse on the **left**, since
`world = parentWorld * local`. Writing the world matrix straight into the local one works for
root objects and teleports every child of a moved parent, which is why the tests use a parent
that is both translated *and* rotated: with an identity parent all four wrong variants agree
with the right one.

**One drag is one undo command, whatever the mode.** All three modes edit the same `glm::mat4`,
so rotate and scale needed no new command type — but they do report a different channel, and the
Log Console line is written to be indistinguishable from the one Properties emits for the same
edit (`Rotation de 'Cubo' cambiado a (0.00, 35.00, 0.00)`, in degrees, not radians). The mode is
latched when the drag *starts*: the shortcuts stay live while dragging, so pressing `E` halfway
through a move would otherwise mislabel the log. The `before` snapshot is taken on the frame ImGuizmo starts
being used and the `PropertyCommand<glm::mat4>` is pushed on the frame it stops — not once per
frame, and not at all if the object ended where it started. The object is resolved by **id**
through `Scene::findById`, never by a captured `GameObject*`: a delete and a scene load both fit
between grabbing the handle and pressing `Ctrl+Z`, and a rebuilt GameObject keeps its id but not
its address. Moving the transform does not move the PhysX actor, so the collider is
`teleport()`-ed in the same step — otherwise the object renders in its new place and collides in
the old one, silently.

Two things this also fixed. `ImGuizmo::BeginFrame()` had never been called anywhere in the
repo, so the `IsOver() || IsUsing()` guard that keeps a click on the gizmo from changing the
selection was returning `false` unconditionally — a gate that closed nothing. And the Properties
panel only re-read its edit cache when the *selection* changed, so anything that moved an object
without changing the selection (this gizmo, a Lua script, an undo) left the Position fields
frozen — and the next touch of any `DragFloat` recomposed the matrix from that stale cache and
wiped the move. It now also re-reads when `localTransform` differs from what it last decomposed
and no drag is in progress.

### Scene, Play Mode & Undo

The scene is a tree of GameObjects with hierarchical transforms, edited in the **Scene** panel:
create, delete, rename and drag-drop to reorder, plus a basic-shapes menu (Cube, Sphere, Plane,
Capsule) and right-click shortcuts such as **Create Camera**. The **Content Browser** browses the project as a folder
tree with a breadcrumb, filters by name and by asset type, creates folders, renames, moves (by
dragging onto a folder, with scene references rewritten) and deletes assets — one or several at
once with Ctrl/Shift selection — shows real thumbnails for textures, 3D models and materials
(rendered on the CPU in a worker and cached in `.dt-cache/thumbs/`; animation-only FBX files get an
`ANI` icon), follows changes made on disk
outside the editor, and imports files dropped from the OS file explorer or picked in a Browse
dialog by copying them into `assets/Imported/`. Right-clicking a texture opens **Import Settings...**:
a colour space (Auto = decided by the material slot, sRGB or Linear) and mipmaps on/off, saved next to
the image as `<name>.import.json` (only when it differs from the defaults, and it follows the asset when
you move, rename or delete it, and into the exported game); applying it rebuilds the materials that use
that texture. For audio clips (`.wav/.mp3/.ogg/.flac`) it offers a gain in dB (-30 to +12) added to
every voice of that file and a Force mono switch for 2D clips; both are saved in the same
`<name>.import.json`, and a gain change reaches a clip that is already playing. For models (`.fbx`,
`.obj`, `.gltf` and `.glb`) it offers a uniform scale, the normals mode (from the file, smooth or flat),
recalculating tangents, flipping UVs and importing the embedded animations, again in the same sidecar;
applying it reloads every object in the scene that uses that model, keeping its transform, material
overrides and Animator (it is not undoable). Each FBX uses its own settings, so a character and its
animation files (a Mixamo download, for example) need the same scale. Materials can be
saved as a reusable `.mat` asset (right-click empty space → **Create → Material**), assigned to any
material slot by dragging it from the grid, and edited by double-clicking it; editing a shared
`.mat` updates every object that references it, while each object's own texture/factor overrides
still win. It is also the drag source for models, textures and skybox folders. The
**Log Console** keeps the history of edit actions and the values they wrote. All of it is
serialised to JSON — the whole tree, with meshes, colliders, audio, scripts, UI components and
the Animator graph.

**Play Mode** toggles between editing and running: entering it snapshots the scene and starts
the physics step, which never runs in edit mode, and Stop restores that snapshot, so nothing a
script did survives. Editor actions are undoable with `Ctrl+Z` **including while playing** —
entering Play clears the history, undo then walks back what you did *during* that Play session,
and Stop restores from the snapshot and clears the history again.

## Gameplay

### Camera

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

### Physics

PhysX. A GameObject can carry one of four colliders — **Box**, **Sphere**, **Capsule** or
**Plane** — each with its shape, its own material (static and dynamic friction, bounciness) and
an `Is Trigger` flag, and a **Rigidbody** with mass, gravity, drag, a kinematic switch, 6-axis
constraints and forces/impulses. Raycasts, sphere casts, overlaps and collision layers complete
it; the layer matrix is in **View → Collision Layers**. Everything is editable in Properties,
drawn as yellow gizmos in the viewport, and scriptable from Lua. The simulation only steps in
Play.

### Audio

FMOD, and optional: without it the build succeeds and the engine runs, silently.
`AudioClipComponent` carries the clip — loop, a 3D/2D toggle, per-channel volume and pitch, and
the 3D min/max attenuation distances with a gizmo in the viewport — and loading one never
blocks the frame. The global mixer, one-shot clips at a point and reverb zones are reachable
from Lua.

**Audio Listener**: one per scene, and its GameObject's transform is where the scene is heard
from — position, `-Z` forward, `+Y` up. With none in the scene the camera is used instead: the
clips still play, they are simply heard from the camera, and the log says so once per Play.

### Game UI

Fourteen UI components, **data-only** components of the scene: one per-frame sync rebuilds or
updates the live canvas tree from them, so what you see in Play and in the exported game comes
from the scene and not from a hand-wired tree. All of them are editable in Properties and
**fully scriptable from Lua** — every field, plus `OnClick`/`OnDoubleClick` callbacks and the
button state.

| Component | Notable settings |
| --- | --- |
| `Canvas` | Scale modes, reference resolution, safe area, render mode (see below) |
| `Panel` | — |
| `Image` | Simple / sliced / tiled / filled |
| `Text` | Font, size, outline, shadow, alignment, wrap and overflow |
| `Button` | 5 states, colour-tint / sprite-swap / fade transitions, optional text label |
| `Slider`, `Checkbox`, `Toggle`, `Scrollbar` | — |
| `ProgressBar` | Value range, fill direction, background and fill sprites |
| `InputField` | Caret, content types |
| `Dropdown`, `ScrollView` | — |
| `Layout` | Horizontal / vertical / grid auto-layout with padding, spacing, cell size, cross-axis alignment, content-size fitters and per-child `ignoreLayout`; on a GameObject with no other UI component it builds its own non-drawing container that groups, places and clips |

**World-space canvases and multi-canvas.** A `Canvas` can render as a quad **inside the scene**
instead of on the screen (`renderMode`, `worldScale`, `billboard` none/yaw-only/full,
`depthTest`) — health bars over enemies, diegetic screens. World canvases are drawn in the
scene pass, sorted back to front, so geometry occludes them; screen canvases stay on top as
before. A scene can hold **any number of canvases**, each with its own tree: pointer input goes
to the topmost one under the cursor, and a canvas that owns a press keeps it until release.
Both backends.

Three known limits of a world canvas: it cannot be clicked (select it from the Hierarchy),
`clipChildren` does not clip on one, and fog, motion blur and TAA read the depth of whatever is
*behind* it.

## Animation

### Animator

A Unity-style animation state machine for skinned meshes. A **node** is a state holding one
of the model's animation clips; a **link** is a directed transition. A transition cross-fades
over a duration in seconds (0 = instant cut, the default, and what every old scene carries);
a state can also be a **1D blend** of any number of its clips: each clip has a threshold on a
float parameter, and the two clips around the parameter's value mix linearly (below the first
threshold or above the last, only that clip plays; with equal thresholds the first one wins).
Scenes saved with the old two-clip blend load with the same pose. Ticking **2D** on a blend state adds a
second float parameter: each clip becomes a point (X threshold, Y threshold), the points are
triangulated (Delaunay) and the three clips of the triangle holding (X, Y) mix by barycentric
weight; outside the points it takes the nearest edge, and with all points on a line it blends
along it. The node draws the points, the triangles and the current value. Cross-fades are continuous: fading
out of a blend state fades the whole blend, not just its first clip, and a transition that fires
while a cross-fade is still running starts from the pose on screen instead of jumping. Each state can also
carry named **animation events** at normalized times of its cycle, delivered to Lua as
`OnAnimationEvent`. The root of each state has three modes: **normal** (the pose moves it),
**locked** (the clip plays in place) and **root motion**: the horizontal travel of the root
moves the GameObject — as a velocity when it has a dynamic Rigidbody, so it collides and falls —
while the vertical bob stays in the pose; rotation isn't applied.

#### Property clips

The Animator also plays **property clips**, authored in the editor and stored with the scene:
a clip is a duration in seconds plus tracks, and a track is one scalar property of the object
with its keyframes (linear between keys; outside the range, the end key holds). The sixteen
animatable properties are local position, rotation (degrees) and scale, the light's colour,
intensity and range, and the material's metallic and roughness. Each state can reference one,
so a door, a platform, a camera or a blinking light gets the whole graph — transitions,
conditions, exit time, cross-fade, layers and the Lua API — and a character can run its mesh
clip and a property clip at once. Properties no track animates are left alone, and during a
cross-fade the values blend (rotations take the short way round). Layer masks are about bones,
so they do not apply to properties. The component is opt-in: **Properties → Add → Animator**,
now available on any object — without a skinned mesh it simply has no mesh clips to name.

#### Sub-state machines

A state can also be a **sub-state machine**: a box that holds other states. It never plays —
entering it enters its **entry** state, following the chain down to a leaf — and a transition
drawn *from* the box fires from any state inside it, at any depth. A box whose entry chain is
broken doesn't fire at all: entering half-way into a state that isn't there is worse than not
moving. Transitions are evaluated Any State first, then the current state's own, then its
box's, then the box above it, so a general exit never beats a specific one. Deleting a box
deletes what it holds, in one undo step. Double-click a box to go in, use the breadcrumb above
the canvas to come back out, and the `padre` button on a node to move it into a box. It
organises a graph; it is not Unity's hierarchical state machine, as there is no active
compound state.

#### Links that go both ways

When two states transition **both ways**, the return link hangs off a second pair of pins —
its own row on the node, with a wider curve — so the two don't overlap: the way back has to go
around both nodes and would otherwise run straight over the way there. Which one moves is
decided by the order the transitions were created, not by where the nodes sit, so dragging a
node never reshuffles the curves.

#### Clip curves

A track can also write a **Float parameter** of the Animator instead of a property of the
object: that is a **clip curve**. It lets the animation's own time drive the state machine —
a transition on `speed > 4`, a value feeding a state's speed parameter, a window that opens
and closes during an attack. In the **Property Clips** section, `+ curva` adds one and the
combo picks which Float parameter it writes; a curve naming a parameter that isn't a declared
Float shows in red and does nothing. Two rules are worth knowing: a curve is evaluated
**before** the transitions, so its value of this frame already decides this frame's
transitions (the editor preview writes it too, so it overwrites whatever you typed into the
panel while the preview runs); and if two layers have a curve for the same parameter the
**last one wins** — the layer's weight does not scale the value (a pose is blended, a
parameter is written), and a layer at weight 0 still writes its curve.

Every track draws itself above its keyframe list, and that canvas is **editable**: drag a key
to move it in time and value, double-click empty space to add one, right-click a key to
remove it. The list below stays for typing exact numbers. One drag is one undo step, and the
vertical range freezes while you drag — otherwise the plot would rescale under the cursor and
the key would slip away. The canvas shows the sampled shape, a dot per key, the
range's ends, and — while the preview runs — a playhead at the clip's current time. A curve
also draws a line for each **threshold** of the Float conditions that read its parameter, so
whether it crosses `speed > 4`, and when, is one glance rather than arithmetic.

#### Layers

The Animator has **layers**, like Unity's. Layer 0 is the base graph; every extra layer is a
full state machine of its own (states, transitions, entry, Any State) that reads the same
parameters, so one trigger can move several layers in the same frame. Each extra layer has a
**weight** (0..1), a **mode** and a **mask**: **Override** replaces the pose of the masked
bones, blended by the weight; **Additive** adds each clip's difference from its own first
frame on top of what is below (a breathing or recoil clip over any locomotion). The mask is a
set of bones, picked on the skeleton tree — a click takes the bone and its whole branch,
Ctrl+click only that bone; an empty mask means the whole body. Only the base layer moves the
GameObject with root motion; animation events fire from every layer whose weight is above 0.
In the panel the layer list sits above the graph: **+** / **−**, the arrows reorder, and a
double click renames; the graph shown is the selected layer's.

#### IK constraints

An Animator can also carry up to four **IK constraints**, solved on the GPU after the pose is
evaluated, so they bend whatever the graph is playing. **Look at** turns one bone (a head, a
chest) until its chosen local axis points at the target, capped by a maximum angle; **two
bone** solves a three-bone chain (shoulder-elbow-hand, hip-knee-foot) so its tip reaches the
target, with a *pole* object deciding which way the elbow or knee points. The bone is named
like a clip is, the target and the pole are GameObjects, and a weight of 0..1 fades the
constraint in and out — from the **IK** section of the Animator panel, or from Lua with
`SetIkWeight`, `SetIkTarget` and `SetIkPole`. A target further away than the chain leaves it
stretched, not broken, and a chain that cannot be resolved (the bone is missing, or it has no
parent and grandparent) is reported and skipped. The solver assumes the character's scale is
uniform.

#### The node panel

Open the graph with **View → Animator**. In the node panel:

- **Add State from Clip** adds a node from one of the model's clips.
- Drag from a node's **output pin** to another's **input pin** to create a transition.
- Right-click a node → **Set as Entry** to mark the entry state (shown tinted); a state whose
  clip name no longer resolves against the model is flagged red.
- Right-click a link to edit its **conditions**, its cross-fade and its exit time; each node
  has a **loop** checkbox.
- Each node has a **speed** multiplier (`1` = normal, `0` = frozen; it never plays
  backwards) and an optional **x parameter**: a float parameter that multiplies that speed,
  so a script can drive it with `SetFloat`.
- The purple **Any State** node is always there while the graph has states: drag from its
  output pin to create an Any State transition. It cannot be deleted; its links can.

A transition can also wait for time. **Has Exit Time** makes it fire when the source state
reaches **exit time**, in normalized time: `0.9` is 90% of the clip, and values above `1`
count loops (`2.5` waits two and a half loops). On a looping state an exit time below `1` is
checked once per loop. With no conditions it fires on time alone; with conditions it needs
both.

The **Any State** node holds transitions that apply from whichever state is current. They are
evaluated before the current state's own transitions. By default an Any State transition
never re-enters the state that is already playing; turn on **Can Transition To Self** on that
transition to allow it (for example, a hit reaction that restarts on every trigger).

Every edit to the graph (states, transitions, conditions, parameters, blend, entry state, layers) is
undoable with Ctrl+Z, one step per gesture — dragging a value is a single step. Moving nodes
on the canvas is not recorded.

#### Parameters and conditions

Parameters are declared in the Animator's parameter list and set or queried from code **by
name**. A transition fires when *all* its conditions hold; a transition with no conditions
never fires.

| Condition on | How it reads |
| --- | --- |
| `bool` | the parameter's own value |
| `trigger` | the parameter's own value; `ResetTrigger` disarms a pending one |
| `int` | a threshold compared with `>`, `<`, `==` or `!=` |
| `float` | the same comparisons — but `==` is exact binary equality: a value you assign with `SetFloat` matches, one you arrive at by accumulating usually will not |
| nothing | `animation finished`, independent of any parameter: the current clip reached its end |

The graph only evaluates transitions in **Play** mode. In **Edit** the entry state's clip
previews in place. Stopping Play resets to the entry state — the scene rebuilds from its JSON,
so no runtime state is carried over.

#### From Lua

It is driven from Lua via `GetComponent("Animator")`: the graph's `bool`, `trigger`, `int`
and `float` parameters are read and written **by name**, and the active state, the blend
weight and the state being faded out can be queried. Scripts can also drive the graph
directly: `Play(state)` and `CrossFade(state, seconds)` jump to a state by name (returning
`false` for an unknown one; both take an optional layer index, and `SetLayerWeight(layer, w)`
fades a layer in and out), `ResetTrigger(name)` disarms a pending trigger, `SetSpeed(v)`/`GetSpeed()` scale the whole
Animator (runtime only, not saved), and
`GetNormalizedTime()` reports how far into the current state it is (it keeps growing on a
loop, so `2.5` is two and a half loops). Parameter names are never fatal — an
undeclared name is ignored by the setters and returns the neutral value from the getters.
See [`Scripts/README.md`](Scripts/README.md) for the method list.

#### Saving, sanitising, and skinning without an Animator

The whole graph — nodes, canvas positions, links, conditions, parameters, per-node loop and
the entry state — is saved in the scene file. Clips are referenced **by name**, so
re-exporting the model with a clip renamed unlinks that state (it warns on load rather than
silently pointing at the wrong animation).

A loaded graph is not trusted as it comes. A single sanitising pass runs where the graph is
known to have just changed — on load, and when an editor gesture closes its undo step — and
drops what cannot hold: references out of range, a transition whose endpoints no longer exist,
a sub-machine entry that exists but is not a child of its own box (entering it would leave the
box). A healthy graph comes out of it byte for byte identical, which is why calling it too
often is free.

**A skinned mesh needs no Animator.** Without the component it simply loops its first clip,
and the backend keeps the clock: one shared `advanceMeshClock` paces it in Assimp ticks, wraps
it at the clip's duration and freezes it while the mesh is hidden — the same in Vulkan and in
D3D12, which is the point of it living in one place. Vertices the FBX left with no bone weight
at all keep their bind position instead of collapsing to the origin — the skinning compute falls
back to the identity when the four weights add up to zero — and the loader counts them on the
mesh (`verticesWithoutWeights`), so a badly exported asset is a number rather than a mystery.

## Tooling

### Performance Panel

Open it with **View → Performance**. It is an editor-only panel — nothing in it links into
`DonTopoCore` or the exported runtime — and it monitors, live, in collapsible sections:

- **Summary** (always visible): framerate, CPU frame time, and the verdict on what paces the
  frame — *CPU-bound* or *GPU-bound*, from the average CPU time against the GPU total. Under
  Vsync the comparison is meaningless (everything lands on the refresh interval) and the panel
  says so instead of printing a verdict.
- **CPU (history)**: a 120-sample history (`PlotLines` for the frame time, `PlotHistogram` for
  the FPS) plus min / average / max in ms and the **1% low** in FPS — the 99th-percentile
  frame, which is the number that exposes a micro-stutter the average swallows.
- **GPU per pass**: shadows, scene, AO, Forward+ culling, SSR, fog, motion blur, bloom and
  anti-aliasing, in ms and as a share of the total render time (everything but the UI pass).
  The share column is a proportional bar, and the most expensive pass of the frame is marked
  in the panel's warning orange. Rows are listed in pipeline order — that order is information
  in itself — and the `ms` column is sortable if you would rather rank them. The same numbers
  also appear per effect in the Rendering panel, next to the sliders that move them.
- **Draw counters**: draw calls, instances and culled objects of the scene pass (instanced
  statics + skinned), plus GPU object slots against their capacity.
- **Scene**: objects in the hierarchy and how many carry a mesh or a light; scene lights
  against `MAX_LIGHTS` (past that they neither light nor cast — it warns); Forward+ average
  lights per cell and overflowed cells when the culling is on; reflection probes, their VRAM
  per probe *as reported by the active backend* and the last bake time.
- **Active configuration**: internal render resolution and megapixels, output size and the
  SSAA factor when they differ, AA mode (with the MSAA sample count), shadow map side and
  distance, requested present mode, and a warning while wireframe is on. It is read-only
  context — you change all of it in the Rendering panel — but without it a pass timing cannot
  be compared against the one you took yesterday.
- **Process**: RAM working set and peak, CPU usage of the process, and VRAM in use against
  the budget the system grants it.

Everything in the Scene and Active configuration sections comes from getters the renderer and
the scene already exposed (`probeCount`, `sceneLightTotal`, `forwardPlusOverflowCells`,
`renderWidth`/`uiWidth`, …); the panel adds no measurement of its own for them, and the derived
statistics — min/avg/max, 1% low, the CPU-versus-GPU delta — are computed from the same 120
samples the graph already plots. The scene walk that counts objects and lights runs only while
the panel is open. Warning orange (`1.0, 0.6, 0.2`) means the same thing everywhere in the
panel: the instance-SSBO overflow, slots or VRAM at 90% of capacity, lights past `MAX_LIGHTS`,
overflowed Forward+ cells, wireframe mode, and the hottest pass.

The GPU times come from timestamp queries written into the command buffer that is
already being recorded — no extra pass, pipeline or render target. Results are read from the
frame `N-2`, the slot whose fence this frame already waited on, and **without**
`VK_QUERY_RESULT_WAIT_BIT`: nothing ever blocks a frame in flight, and there is no
`vkDeviceWaitIdle` anywhere near it. The first two frames after opening the panel therefore
show `--`, and so does any pass that is switched off. The DirectX 12 backend feeds the same
panel through its own query heap and readback buffer, with the same non-blocking rule; there
every slot is written at the start of the frame, so a pass that did not run reads zero instead
of keeping a stale tick from three frames ago.

A pass with nothing measured prints `--`, not `0.000 ms`: zero is a legitimate reading for a
pass that ran and cost almost nothing, so it cannot double as "no data". The formatting rule
is shared by both panels.

Closing the panel costs exactly nothing. `PerformancePanel::draw` calls
`Renderer::setPerfCaptureEnabled(false)`, and with the capture off the renderer records no
query reset, no timestamp and touches no counter — the frame is byte-for-byte the one it
recorded before the feature existed. Pending query slots are invalidated on the way out, so
reopening never reads a pool that was left unreset.

**One clock drives the whole panel.** Every number and bar in it — framerate, per-pass GPU times,
draw counters, scene and probe counts, RAM/CPU/VRAM — is frozen and refreshed once a second, on
the same tick. A figure that changes 60 times a second cannot be read, and a bar that dances
hides the very comparison it exists for. The graphs advance on that tick too — one point per
second, so 120 points are two minutes of trend instead of two seconds of blur. Each point is the
**worst** frame of its second on the ms curve (a stutter lasting three frames vanishes from an
average of sixty, and that stutter is what you are looking for) and the average FPS on the
histogram, where a worst case would read as a rate it never ran at.

Two histories are kept, and the difference matters: the plotted one is per second, but min /
average / max and the **1% low** come from a separate per-frame ring — the 1% low *is* the worst
individual frame, so it cannot be computed from anything coarser. Nothing about the
*measurement* slows down either: the GPU capture still runs every frame, and what ticks at 1 Hz
is the readout. That same tick is the window of the process CPU percentage, computed as a
difference between two samples, so it averages over a full second.

RAM, CPU and VRAM are the expensive reads, and they ride the same tick rather than being polled
once per frame. VRAM comes from DXGI (`IDXGIAdapter3::QueryVideoMemoryInfo`, adapter 0),
which reports the usage of *this process*: Vulkan cannot report it without
`VK_EXT_memory_budget`, and enabling that extension would mean touching device creation in
Core for a number only the editor displays.

### Rendering Panel

Open it with **View → Rendering**. It gathers the 43 render settings that used to live
inside the `View` menu: **Ambient (IBL)**, **Reflection probes**, **Skybox**,
**Presentation (vsync)**, **Shadows**, **Bloom**, **SSAO**, **SSR**, **Fog**, **Motion
blur**, **Anti-aliasing**, **Forward+** and the **render backend** selector, one
`CollapsingHeader` each — only the first open by default, since thirteen expanded sections
do not fit a narrow column. ImGui remembers in `imgui.ini` which ones you left open, and
the panel's own visibility is stored per project in `project.json`.

The reason it stopped being a menu is that an ImGui menu closes the moment you release
the mouse, so tuning bloom or fog *while watching the viewport* meant reopening it on
every nudge. Docked, it stays put and the effect is visible while the slider is being
dragged. Like the Performance panel, it is editor-only: none of it links into
`DonTopoCore` or the exported runtime.

Every control does four things — draw the widget, apply the value, push an **undo**
command and persist to `project.json` — through a shared `RenderSettingControls`
wrapper, so `Ctrl+Z` walks back a render setting exactly like it walks back a transform,
and undoing one leaves the project file as it was. The drag widgets register the value
from the **start** of the drag, not the frame the mouse is released, so undoing a long
drag does not land on the second-to-last pixel.

Two settings do not apply while you drag them, because each change would rebuild render
targets: the **SSAA factor** is held until the slider is released, and the **shadow map
resolution** moves the image, its views and its framebuffers, so it goes through the
backend rather than the settings object.

**Presentation** offers `Vsync`, `Mailbox` and `Immediate`. Modes the device does not
provide are shown **disabled with their reason**, not hidden — if the core supports N
options the UI offers N. The mode that is *granted* need not be the one requested (a
device with no Mailbox falls back to Vsync), so the undo entry re-reads what actually
took effect instead of assuming. `Immediate` is the only mode in which a frame's real
cost can be measured: with Vsync everything reads 16 ms.

Each effect's section also prints its own **GPU time**, from the same timestamp queries
the Performance panel reads, so the cost of a setting is visible right next to the
slider that changes it. A pass that measured nothing reads `--`, never `0.000 ms`.

### Sprite Editor

Open it with **View → Sprite Editor**, or with the **Editar sprites...** button that every UI
component with an atlas offers. It shows the image and lets you cut named rectangles out of it:
drag to draw one, drag a corner to resize it, rename it in the list. A rect is kept inside the
image and never degenerate — one that ran past the edge would sample UVs outside `[0, 1]` and
the sampler would repeat or stretch the border without saying anything.

The result is a sidecar next to the image, `<atlas>.sprites.json`. UI components reference a
sprite **by name** inside that atlas, so renaming one in the editor is what unlinks it, not
moving the file. Reopening the same image does not discard what is being edited.

### Input Actions

Open it with **View → Input Actions**. It maps **named actions** to keys, mouse buttons,
gamepad buttons and stick/trigger directions — bindings are captured by pressing the key or
button rather than by picking from a list. It is a panel of its own and not a Properties block
because the map is global to the project: there is no selection to hang it from. It is stored
in `input_actions.json` next to the editor's `imgui.ini`, and a missing or unreadable file
leaves the panel empty instead of stopping the editor from starting.

Actions are read from Lua by name, and the panel publishes them into the **Script Editor's
autocomplete**, so an action added here is offered there without retyping it. A binding with no
equivalent in the runtime's own input map (a mouse wheel, an exotic key) is still drawn in the
panel, but it never reaches the game.

### Export Game

**File → Export Game** packages a standalone runtime: the scene, its assets, the Lua scripts,
the compiled shaders, the splash screen and the DLLs it needs (FMOD and the MSVC CRT). The
runtime links `DonTopoCore` alone — no editor code, no ImGui symbols — and the exporter has its
own **render backend** selector, written to `game.cfg`, which need not match the editor you
exported from. Export from a **Release** editor: see [Build (Windows)](#build-windows) for what
a Debug package drags along and why it only runs on the machine that built it.

## Scripting

### Lua Scripting

Gameplay is scripted in **Lua 5.4** (sol2). Attach one or more `ScriptComponent`s to a
GameObject via **Properties → Add → Script**; the scripts themselves are `.lua` files
under `Scripts/`, one global table per file, with a Unity-style lifecycle
(`Awake`/`Start`/`Update`/`FixedUpdate`/`LateUpdate`/`OnDestroy` plus the trigger,
collision and animation-event callbacks) and serializable properties that show up in
Properties on their own. `OnAnimationEvent(name)` fires on every script of a GameObject
whose Animator crosses one of its current state's events: Play only, once per cycle
(twice if a long frame skips two), and during a cross-fade only the incoming state fires.

They are edited in the built-in **Script Editor** panel — multi-tab, Lua highlighting,
a live syntax check with a status bar, find/replace (`Ctrl+F`), go-to-line (`Ctrl+G`),
reload-on-external-change and an autocomplete popup that shows each entry's signature
and matches by member name, so `t:GetPos` on a local variable suggests
`Transform:GetPosition` — or in any external editor: either way the running engine
hot-reloads the file and keeps the property values.

From a script you reach the entity and its transform (including world position, the
object's axes and `LookAt`), the scene graph (including reparenting, which keeps the
world pose by default), the frame clock (`Time`), input
(keyboard, mouse, named actions and raw gamepad), physics (the four colliders,
`Rigidbody`, raycasts, sphere casts, overlaps and collision layers), audio (clips, the
global mixer, reverb zones), the animator, lights and the game camera, the fourteen UI
components and runtime scene switching with `DonTopo.loadScene`.

**The complete API reference is in [`Scripts/README.md`](Scripts/README.md)** — every
table and every method, with the caveats that are otherwise found out the hard way.

## Planned

| System | Candidates |
| --- | --- |
| Post-processing | Depth of field, per-object motion vectors (camera motion blur is in; objects contribute no velocity of their own) |
| Platforms | macOS — it would need a Metal backend, and neither the build presets nor the platform layer target it today. Windows (Vulkan + DirectX 12) and Linux (Vulkan, `configure.sh`/`build.sh`, `linux` CI job building Release and running the test suite) are supported; Linux is verified on Ubuntu 24.04 and under WSLg, other distributions are not |

## License

TBD
