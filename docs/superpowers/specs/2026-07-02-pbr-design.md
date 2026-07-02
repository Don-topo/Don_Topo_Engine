# PBR Rendering Pipeline Implementation Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Phong shading with Cook-Torrance GGX PBR on both static and skinned pipelines, with optional metallic/roughness texture and ACES tone mapping.

**Architecture:** Analytical PBR only (no IBL). Metallic/roughness provided via optional glTF ORM texture (binding 4) multiplied by per-object scalar push constants. Tone mapping and gamma correction done inline in the fragment shader — no HDR framebuffer or post-process pass required.

**Tech Stack:** Vulkan 1.2, GLSL 450, GLM, Assimp, stb_image.

---

## Global Constraints

- C++20, MSVC, Ninja build via CMakePresets
- Vulkan min push constant guarantee: 128 bytes — stay within it
- Both pipelines (static `m_pipeline`, skinned `m_skinnedGfxPipeline`) share `m_descriptorSetLayout` and `m_pipelineLayout` — changes apply to both
- No new render passes, no new framebuffers
- YAGNI: no IBL, no bloom, no HDR framebuffer this iteration

---

## Architecture

### Files Changed

| Action | File | What changes |
|--------|------|-------------|
| New    | `shaders/pbr.frag` | Cook-Torrance GGX BRDF + ACES tone mapping. Replaces `triangle.frag` in both pipelines. |
| Keep   | `shaders/triangle.vert` | No change — vertex stage is identical. |
| Modify | `engine/include/DonTopo/Mesh.h` | Add `metallicRoughnessPath`, `embeddedMetallicRoughness`, `float metallic = 0.0f`, `float roughness = 0.5f` |
| Modify | `engine/include/DonTopo/Renderer.h` | Extend `PushData` to 80 bytes; add metallic/roughness image+view+sampler + `float metallic` + `float roughness` scalars to `RenderObject` and `SkinnedRenderObject` |
| Modify | `engine/src/Renderer.cpp` | Descriptor layout adds binding 4; `buildRenderObject` loads ORM texture or creates 1×1 fallback; `recordCommandBuffer` pushes metallic/roughness; both pipelines use `pbr.frag` |
| Modify | `engine/src/ModelLoader.cpp` | Read `aiTextureType_METALNESS` / `ROUGHNESS` or `UNKNOWN` (glTF ORM pack) |

---

## Material System

### Push Constant Layout (80 bytes)

```glsl
layout(push_constant) uniform PushData {
    mat4  transform;  // bytes  0–63
    float metallic;   // byte  64
    float roughness;  // byte  68
    vec2  _pad;       // bytes 72–79
} push;
```

C++ mirror in `Renderer.h`:
```cpp
struct PushData {
    glm::mat4 transform{1.0f};
    float     metallic  = 1.0f;
    float     roughness = 1.0f;
    glm::vec2 _pad{};
};
```

Scalar defaults are `1.0` so they act as a neutral multiplier when a texture is present. When no texture is loaded, set `metallic = mesh.metallic` and `roughness = mesh.roughness` (default 0.0 and 0.5 in Mesh).

### Binding 4 — ORM Texture

Format: `VK_FORMAT_R8G8B8A8_UNORM`  
Channels: R = Ambient Occlusion, G = Roughness, B = Metallic (glTF standard)

- If `mesh.metallicRoughnessPath` is non-empty OR `mesh.embeddedMetallicRoughness` is non-empty → load texture normally (same path as albedo/normal map loading).
- Otherwise → create a shared 1×1 fallback image with pixel `{255, 255, 255, 255}` (AO=1, roughness=1, metallic=1, scalars in push constant then control the actual values). Create one shared fallback image at init time; all objects without an ORM texture share it.

### Sampling in Shader

```glsl
layout(set=0, binding=4) uniform sampler2D metallicRoughnessTex;

vec3  orm      = texture(metallicRoughnessTex, fragUV).rgb;
float ao       = orm.r;
float roughness = orm.g * push.roughness;
float metallic  = orm.b * push.metallic;
```

---

## BRDF: Cook-Torrance GGX

### F0 and kD

```glsl
vec3 F0 = mix(vec3(0.04), albedo, metallic);
```

Metals use albedo as specular color; dielectrics use 0.04.

### Per-light Loop

```glsl
for (int i = 0; i < ubo.numLights; i++) {
    vec3  L         = normalize(ubo.lights[i].position.xyz - fragWorldPos);
    vec3  H         = normalize(V + L);
    float dist      = length(ubo.lights[i].position.xyz - fragWorldPos);
    float atten     = 1.0 / (dist * dist);
    vec3  radiance  = ubo.lights[i].color.rgb * ubo.lights[i].color.a * atten;

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // D — GGX distribution
    float a   = roughness * roughness;
    float a2  = a * a;
    float d   = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    float D   = a2 / (PI * d * d);

    // G — Smith-Schlick-GGX
    float r  = roughness + 1.0;
    float k  = (r * r) / 8.0;
    float G  = (NdotV / (NdotV*(1.0-k)+k)) * (NdotL / (NdotL*(1.0-k)+k));

    // F — Schlick
    vec3 F = F0 + (1.0 - F0) * pow(clamp(1.0 - HdotV, 0.0, 1.0), 5.0);

    vec3 kD = (1.0 - F) * (1.0 - metallic);

    float s = (i == 0) ? shadow : 1.0;  // key light only casts shadow

    Lo += s * (kD * albedo / PI + D*G*F / (4.0*NdotV*NdotL + 0.0001)) * radiance * NdotL;
}
```

### Ambient + AO

```glsl
vec3 ambient = vec3(0.03) * albedo * ao;
vec3 color   = ambient + Lo;
```

---

## Tone Mapping + Gamma

ACES fitted curve (inline, no post pass):

```glsl
vec3 aces(vec3 x) {
    return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14), 0.0, 1.0);
}

outColor = vec4(pow(aces(color), vec3(1.0/2.2)), 1.0);
```

---

## Descriptor Set Layout Change

Current bindings (set=0):

| Binding | Type | Name |
|---------|------|------|
| 0 | UNIFORM_BUFFER | UBO (view/proj/lights) |
| 1 | COMBINED_IMAGE_SAMPLER | albedo |
| 2 | COMBINED_IMAGE_SAMPLER | normalMap |
| 3 | COMBINED_IMAGE_SAMPLER (shadow) | shadowMap |
| **4** | **COMBINED_IMAGE_SAMPLER** | **metallicRoughnessTex (new)** |

`VkDescriptorSetLayoutBinding` for binding 4:
```cpp
{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}
```

Descriptor pool must allocate one additional `COMBINED_IMAGE_SAMPLER` per descriptor set.

---

## ModelLoader Changes

In `ModelLoader::load()` and `ModelLoader::loadSkinned()`, after loading albedo and normal map, attempt to load ORM:

```cpp
aiString ormPath;
// glTF packs ORM in aiTextureType_UNKNOWN (index 0) or separate channels
if (mat->GetTexture(AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE, &ormPath) == AI_SUCCESS
    || mat->GetTexture(aiTextureType_UNKNOWN, 0, &ormPath) == AI_SUCCESS)
{
    // embedded or file path — same logic as albedo
}
mesh.metallic  = 0.0f;   // default non-metallic
mesh.roughness = 0.5f;   // default mid-roughness
```

If no ORM texture found → leave `metallicRoughnessPath` empty and `embeddedMetallicRoughness` empty; Renderer uses the shared 1×1 fallback.

---

## Sandbox Usage

No changes needed in `sandbox/src/main.cpp` for default behavior. Optionally:
```cpp
mesh.metallic  = 0.0f;   // dieléctrico
mesh.roughness = 0.3f;   // relativamente pulido
```
before passing to `renderer.init(window, meshes)` or `renderer.addSkinnedMesh(mesh)`.
