# Don Topo Engine

A Vulkan-based game engine written in C++20.

## Features

- PBR rendering (Cook-Torrance GGX) on an HDR pipeline: the scene is lit in linear float and only tonemapped (ACES + gamma) once, in the composition pass
- **Image-based lighting**: the skybox cubemap is convolved on the GPU into an irradiance map (diffuse) and a roughness-prefiltered map (specular, Karis analytic BRDF), with a live `Ambient (IBL)` weight
- **HDR bloom**: threshold with soft knee, mip chain of compute downsample/upsample passes, additive composition — `threshold`, `knee` and `intensity` are live editor sliders (see below)
- **SSAO**: depth-only pre-pass + compute occlusion (16-sample hemisphere kernel, normals reconstructed from depth) and a blur, applied to the ambient term only; toggle plus `radius`/`bias`/`intensity`/`power` sliders (see below)
- **Screen space reflections**: view-space ray march with binary refinement, added into the HDR target *before* bloom so reflections bloom and tonemap like everything else; enabled and weighted **per GameObject**, with a global switch and 5 sliders (see below)
- **Volumetric fog**: height-exponential fog ray-marched in a compute pass over the HDR target, with Henyey-Greenstein in-scattering from the key light and its cascaded shadows; global switch, off by default, and 6 sliders (see below)
- **Light component**: any GameObject can be a light — `Point` / `Spot` / `Directional` / `Area`, several of each per scene (16 reach the shader), position and direction taken from its transform, with a direction gizmo in the editor (see below)
- **Reflection probes**: placeable environment probes that capture the scene from their position into a cubemap and replace the global IBL for the objects inside their radius; baked on demand, never per frame (see below)
- **Anti-aliasing**: `None` / `FXAA` / `SSAA` / `MSAA` / `TAA`, mutually exclusive and switchable at runtime from the View menu, each with its own resources rebuilt between frames
- **Forward+ light culling**: `Off` / `Tiled` / `Clustered`, a compute pre-pass that bins lights into a screen grid so `pbr.frag` only iterates the ones that reach each pixel; `Off` records no commands and lights exactly as before
- GPU skeletal animation (compute shader skinning: bone eval → hierarchy → skinning)
- Cascaded shadow maps (4 cascades, PCF 3×3)
- Normal maps + tangent space
- Cubemap skybox (fullscreen quad, inverse view-projection)
- Wireframe render mode
- 3D spatial audio (FMOD): `AudioClipComponent` (loop, 3D/2D toggle, per-channel volume and pitch, 3D min/max attenuation distances with viewport gizmo), non-blocking clip loading
- **Audio Listener component**: one per scene — its GameObject transform is where the scene is heard from (position, `-Z` forward, `+Y` up), falling back to the camera when absent; a scene with no listener plays no clips and says so once in the log
- Dockable ImGui editor with offscreen viewport
- Scene graph (hierarchical transforms), GameObject hierarchy panel (create/delete/rename, drag-drop reorder)
- Basic shapes menu (Cube/Sphere/Plane/Capsule), Content Browser (asset browsing, rename/delete)
- ImGuizmo transform gizmo (translate/rotate/scale, camera-oriented axis gizmo), debug-draw gizmos, collider gizmos
- **Mesh visibility toggle**: a `Visible` checkbox on the Mesh component; unchecked, the mesh is submitted to no pass at all — no scene draw, no shadow, no AO, and no skinning dispatch — while physics, colliders, picking and scripting are untouched (see below)
- **Selection outline**: the selected GameObject is traced with an orange contour in the viewport (see below)
- **Click-to-select in the viewport**: left-clicking a mesh in the viewport selects it, clicking empty space clears the selection (CPU ray picking, see below)
- **Camera component**: any GameObject can be the scene camera (perspective/orthographic, fov, near/far); frustum gizmo in edit mode, renders from it on Play
- **Animator component**: Unity-style animation state graph (node = clip, link = transition; `bool`/`trigger`/`animation finished` conditions), edited in a node panel; instant-cut transitions (no blending), driven from Lua
- Physics (PhysX): Box/Sphere/Capsule/Plane colliders (shape only) + `Rigidbody` (mass, gravity, drag, kinematic, 6-axis constraints, forces/impulses), raycasting
- Scene serialization (JSON save/load, full GameObject tree incl. mesh/colliders/audio/scripts)
- Play Mode (edit/play toggle, snapshot restore, physics gated to Play), undo/redo of editor actions
- Log Console panel (edit-action history, live value editing)
- **Performance panel**: live framerate/frame-time graphs, GPU time per pass from Vulkan timestamp queries (read from frame `N-2`, never blocking), draw/instance/culled counters, and process RAM/CPU/VRAM — and it costs literally nothing while closed (see below)
- **Frustum culling** in the main, shadow and skinned passes; skinned meshes bounded by a pose-independent sphere so no character can vanish mid-animation
- **Draw batching**: objects sharing a mesh+material collapse into one instanced draw, and their GPU resources (buffers, textures, descriptor set) are deduplicated behind a refcounted cache
- **Async asset loading**: worker thread pool (`JobSystem`), off-thread image decode, batched GPU uploads with deferred visibility and deferred destruction — no `vkDeviceWaitIdle` stalls on drop or scene load
- **Export Game**: packages a standalone runtime (scene, assets, scripts, shaders, splash screen, FMOD and MSVC CRT DLLs) that links no editor code at all
- **Lua scripting**: `ScriptComponent` (multiple per GameObject), Unity-style lifecycle (Awake/Start/Update/FixedUpdate/LateUpdate/OnDestroy), Entity/Transform/Scene/Input/Audio API, runtime scene switching (`DonTopo.loadScene`), hot reload, auto-generated property UI
- FBX / OBJ model loading (embedded textures supported)

## Tech Stack

| Component | Library | Source |
| --- | --- | --- |
| Graphics | Vulkan | System SDK |
| Window / Input | GLFW 3.4 | Auto-fetched |
| Math | GLM 1.0.1 | Auto-fetched |
| 3D Model loading | Assimp 5.3.1 | Auto-fetched |
| Image loading | stb_image | Auto-fetched |
| Editor UI | Dear ImGui (`docking` branch) | Auto-fetched |
| File dialog | ImGuiFileDialog | Auto-fetched |
| Transform gizmo | ImGuizmo | Auto-fetched |
| Node graph UI | imgui-node-editor (thedmd) | Auto-fetched |
| Script code editor | ImGuiColorTextEdit | Auto-fetched |
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

GLFW, GLM, Assimp, stb_image, ImGui, ImGuiFileDialog, ImGuizmo, ImGuiColorTextEdit,
imgui-node-editor, PhysX, nlohmann/json, Lua and sol2 are downloaded and built automatically by
CMake.

## Build (Windows)

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
copied to both the executable directory and `shaders/`. The source list is globbed, so a brand-new
shader needs a re-run of `configure.bat` before `build.bat` will see it.

**Tests.** The suite is headless — no Vulkan device, no window — and builds as one executable per
area under `build-ninja\engine\tests\`. There is no test framework and no CTest registration: each
file is a `main` with asserts that returns non-zero on failure, so running them is just running them.

```batch
build.bat
for %f in (build-ninja\engine\tests\dt_*_tests.exe) do @%f
```

## Project Structure

```text
Don_Topo_Engine/
├── assets/         # Runtime assets (models, textures, audio, skybox)
├── Scripts/        # Lua gameplay scripts (Scripts/<Name>.lua defines global table <Name>)
├── cmake/          # Custom Find modules (PhysX, Lua, FMOD)
├── docs/           # Design specs and implementation plans (superpowers/)
├── engine/         # Two static libraries: DonTopoCore and DonTopoEditor
│   ├── include/    # Public headers, mirroring the module layout (DonTopo/<Module>/)
│   ├── src/        # Implementation, split into seven modules:
│   │   ├── Core/       # Engine loop, Window, Input, Scene, GameObject, Camera
│   │   ├── Renderer/   # Vulkan device, meshes, materials, model loading, skybox, gizmos
│   │   ├── Physics/    # PhysX integration, Rigidbody, Colliders/
│   │   ├── Audio/      # FMOD wrapper, AudioClipComponent, AudioListenerComponent
│   │   ├── Scripting/  # Lua/sol2 bindings, ScriptManager, syntax check
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

## HDR & Bloom

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
The **View** menu exposes the three as sliders plus the measured GPU cost (~0.2 ms at 1280×720,
including composition and tonemap). With `intensity = 0` the image is identical to the one
before the feature existed, which is the check that the tonemap was moved without drift.

The selection outline and the gizmos are drawn **in the composition pass**, after the tonemap,
so they keep their exact flat colours and never bloom. The skybox stays in the scene pass and is
tonemapped with the rest — that is what lets a bright sky feed the bloom.

## Screen-Space Effects

Several effects share one **depth-only pre-pass** that draws the whole scene into a sampled
`D32_SFLOAT` image before the scene pass, with the same frustum culling and the same instanced
batching as the real draw — occluding against geometry that is not actually drawn would be a
visible artifact. It is recorded when SSAO, SSR, TAA, Forward+ tiled **or** the volumetric fog
needs it, and skipped entirely when none does. SSAO and SSR reconstruct view-space position and a
geometric normal from that depth alone
(the normal comes from the smaller of the two depth slopes on each axis, so a pixel on a
silhouette does not blend two surfaces), which is why neither needs a G-buffer, an extra
attachment on the scene pass, or a new UBO member.

### SSAO

A compute shader traces 16 samples in a cosine-weighted hemisphere around each pixel, packed
towards the origin where contact occlusion actually lives, with a per-pixel rotation so the
kernel does not band; a second pass blurs the result. The AO multiplies the **ambient term only**
— applying it to direct light would dim shadows the cascade maps already compute.

The **View** menu carries a toggle plus `radius`, `bias`, `intensity` and `power`, all push
constants, so they take effect the next frame. Turned off, neither the pre-pass nor the two
dispatches are recorded: the AO map is cleared to 1.0 **once** (on creation and on switch-off)
and `pbr.frag` multiplies by unity, so the image is identical to the one before the feature and
the GPU cost is zero, not "computed and multiplied by zero".

### SSR

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

The **View** menu has the global switch plus `distance`, `thickness`, `steps`, `edge fade` and
`intensity`, and reports the measured GPU cost (~0.3 ms at 1280×720 with 32 steps, pre-pass
included). With the switch off — or on with no object marked — not a single dispatch is recorded
and the HDR image is left exactly as the scene pass produced it.

Normals come from depth, not from an attachment, so the normal map's detail does not reach the
reflection: polished metal with a normal map mirrors as if it were flat. And being screen-space,
anything off-screen or hidden behind another object simply is not reflected.

### Volumetric Fog

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

The **View** menu carries the global switch plus `density`, `height falloff`, `base height`,
`anisotropy`, `steps` and the scattering colour, and reports the measured GPU cost. **Off by
default**: with the switch off not a single dispatch, barrier or timestamp is recorded, `Fog GPU`
reads `0.000 ms`, and the HDR image is left exactly as the scene pass and the SSR produced it.

## Lights

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
every frame and hands the renderer the first `MAX_LIGHTS` (16) it finds; the rest are dropped
silently — that is a limit of the UBO block, not an error in the scene. The block grew from two
`vec4` per light to four (`direction` carries the type in its `w`, `params` carries range, the
two cosines and the area width), which is why the shaders that declare it were all touched: in
std140 a struct that changes size shifts everything behind it.

Under **Forward+** the same data reaches the culling compute shaders, with one special case: a
`Directional` light has neither position nor range, so it is marked visible in *every* tile and
cluster instead of being tested against the volume. The radius used for binning is the same
reach the fragment shader uses (`Range`, or `Width / 2` for an area light) — if they differed, a
light would pop off as it crossed a tile edge.

In the editor an **orange gizmo** shows each light: a wire sphere of its range for `Point`,
that sphere plus the four edge generatrices of the cone for `Spot`, a long ray for
`Directional`, and a width × height grid with its normal for `Area`. It is drawn in edit mode
*and* in Play, and it lives in the editor's viewport panel — which is why the exported game,
that links no editor code, can never show it.

## Reflection Probes

A **Reflection Probe** is a component on any GameObject (**Properties → Add → Reflection
Probe**). It captures the environment from that GameObject's position and replaces the global
IBL — both the irradiance map and the roughness-prefiltered map — for every object that falls
inside its radius of influence. `Radius` and `Intensity` are serialised with the scene; the
cubemap is not, it is rebaked.

**The bake is an event, never a pass.** It does not record a single command into the frame's
command buffer: it is its own set of submits, triggered by the **Bake** button, by **View →
Bake All Reflection Probes**, or automatically when a probe has no valid capture yet (on
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

One deliberate limitation: the descriptor set is per **shared mesh**, not per GameObject. Two
instances of the same mesh under different probes share a probe — the first one in traversal
order wins. Splitting them would mean duplicating the sets and losing the instanced draw.

## Mesh Visibility

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

## Selection Outline

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

## Viewport Picking

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
condition by comparing a threshold with `>`, `<`, `==` or `!=`. On a `float`, `==` means
exact binary equality: a value you assign with `SetFloat` matches, one you arrive at by
accumulating usually will not. A transition fires when *all* its conditions hold; a
transition with no conditions never fires.

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

## Performance Panel

Open it with **View → Performance**. It is an editor-only panel — nothing in it links into
`DonTopoCore` or the exported runtime — and it monitors four things live:

- **CPU**: framerate and frame time in ms, with a 120-sample history (`PlotLines` for the
  frame time, `PlotHistogram` for the FPS).
- **GPU per pass**: shadows, scene, AO, Forward+ culling, SSR, fog, bloom and anti-aliasing,
  in ms and as a share of the total render time (everything but the UI pass).
- **Draw counters**: draw calls, instances and culled objects of the scene pass (instanced
  statics + skinned).
- **Process**: RAM working set and peak, CPU usage of the process, and VRAM in use against
  the budget the system grants it.

The GPU times come from Vulkan timestamp queries written into the command buffer that is
already being recorded — no extra pass, pipeline or render target. Results are read from the
frame `N-2`, the slot whose fence this frame already waited on, and **without**
`VK_QUERY_RESULT_WAIT_BIT`: nothing ever blocks a frame in flight, and there is no
`vkDeviceWaitIdle` anywhere near it. The first two frames after opening the panel therefore
show `--`, and so does any pass that is switched off.

Closing the panel costs exactly nothing. `PerformancePanel::draw` calls
`Renderer::setPerfCaptureEnabled(false)`, and with the capture off the renderer records no
query reset, no timestamp and touches no counter — the frame is byte-for-byte the one it
recorded before the feature existed. Pending query slots are invalidated on the way out, so
reopening never reads a pool that was left unreset.

RAM, CPU and VRAM are the expensive reads, so they are cached and refreshed at ~2 Hz rather
than once per frame. VRAM comes from DXGI (`IDXGIAdapter3::QueryVideoMemoryInfo`, adapter 0),
which reports the usage of *this process*: Vulkan cannot report it without
`VK_EXT_memory_budget`, and enabling that extension would mean touching device creation in
Core for a number only the editor displays.

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

### Scene switching from Lua

`DonTopo.loadScene(path)` swaps the running scene for the one stored at `path`
(a Save Scene file: `version: 1` + `root`), through the same load path the editor's
**File → Load Scene** uses. It works in Play Mode and in the exported runtime.

```lua
function Menu:Update(dt)
    if Input.IsKeyPressed(Key.R) then
        if not DonTopo.loadScene("Scenes/Empty.json") then
            Log.Error("Error loading scene")
        end
    end
end
```

The call does **not** load anything itself: it validates the path and queues the
request, and the scene's owner performs the load on the next frame, outside the
script tick — loading mid-`Update` would destroy the very GameObject running that
script. So the returned `bool` reports the *validation* (file readable, JSON
parseable, v1 scene structure), not the load, whose outcome is logged one frame
later. After the call the old scene is gone, your script included — treat it as the
last useful line. Multiple requests in one frame: the last one wins. Outside Play
Mode the request is ignored with a Log Console warning.

API surface: `self.entity` (`GetTransform`, `GetComponent`/`AddComponent`/`RemoveComponent`,
`GetParent`/`GetChildren`), `Transform` (position/rotation/scale, `Translate`/`Rotate`),
`Scene` (`Find`/`CreateGameObject`/`Instantiate`/`Destroy`), `DonTopo.loadScene`,
`Input` (`IsKeyDown`/`IsKeyPressed`/
`IsKeyReleased`, `Key.*`), `Log.Info/Warn/Error` (+ `print`) routed to the Log Console. Scripts
only run in Play Mode; a broken script never crashes the engine (compile/runtime errors are
logged and the component is quarantined). See `Scripts/Rotator.lua` and `Scripts/Mover.lua`.

## Planned

| System | Candidates |
| --- | --- |
| Post-processing | Motion blur, depth of field |
| Multi-backend RHI | DX12 / Vulkan / Metal, for Windows + Linux + macOS |

## License

TBD
