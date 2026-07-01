# Skeletal Animation — GPU Compute Skinning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implementar animación esquelética GPU-driven con tres passes de compute (interpolación de keyframes, traversal jerárquico, skinning por vértice) y un pipeline gráfico separado para meshes animados.

**Architecture:** CPU carga datos desde Assimp y los sube a SSBOs estáticos una sola vez. Cada frame, tres compute shaders en GPU calculan bone matrices y aplican skinning; el resultado se bindea como vertex buffer al graphics pass existente sin modificar los shaders de rendering.

**Tech Stack:** C++20, Vulkan 1.2, GLM, Assimp, GLSL 450

## Global Constraints

- Máximo 128 huesos por mesh (límite del array local en bone_hierarchy.comp)
- Máximo 4 influencias por vértice (`aiProcess_LimitBoneWeights`)
- `MAX_FRAMES = 2` (definido en Renderer.h)
- `MAX_SKINNED_OBJECTS = 8` (pool de descriptors de compute)
- Shaders compilados con `glslc` a `.spv` antes de ejecutar
- Huesos deben estar en orden topológico (parent antes que hijo): garantizado por DFS sobre `aiNode`
- Skinned meshes NO proyectan sombras en v1 (fuera de alcance)

---

## File Structure

| Acción | Archivo | Responsabilidad |
|---|---|---|
| Crear | `engine/include/DonTopo/SkinnedMesh.h` | Tipos CPU (`SkinnedVertex`, `Skeleton`, `AnimationClip`) + tipos GPU (`GpuPosKey`, `GpuRotKey`, `GpuBoneInfo`) |
| Modificar | `engine/include/DonTopo/ModelLoader.h` | Declaración de `loadSkinned()` |
| Modificar | `engine/src/ModelLoader.cpp` | Implementación de `loadSkinned()` |
| Crear | `shaders/bone_eval.comp` | Interpolación de keyframes, local_size_x=64 |
| Crear | `shaders/bone_hierarchy.comp` | Traversal jerárquico, local_size_x=1 |
| Crear | `shaders/skinning.comp` | Skinning por vértice, local_size_x=64 |
| Modificar | `engine/include/DonTopo/Renderer.h` | `SkinnedRenderObject`, `ComputePush`, pipelines de compute, métodos públicos |
| Modificar | `engine/src/Renderer.cpp` | Toda la lógica de compute: pipelines, SSBOs, dispatch, draw |
| Modificar | `sandbox/src/main.cpp` | Carga modelAnimation.fbx, llama updateAnimation en el loop |

---

## Task 1: CPU Data Types — SkinnedMesh.h

**Files:**
- Create: `engine/include/DonTopo/SkinnedMesh.h`

**Interfaces:**
- Produces: `DonTopo::SkinnedVertex`, `DonTopo::GpuPosKey`, `DonTopo::GpuRotKey`, `DonTopo::GpuBoneInfo`, `DonTopo::BoneKeyframe`, `DonTopo::BoneKeyframeQ`, `DonTopo::BoneChannel`, `DonTopo::AnimationClip`, `DonTopo::Skeleton`, `DonTopo::SkinnedMesh`

- [ ] **Step 1: Crear SkinnedMesh.h**

```cpp
// engine/include/DonTopo/SkinnedMesh.h
#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "DonTopo/Mesh.h"

namespace DonTopo
{
    // Vértice con datos de skinning — input SSBO del compute pass
    // Todos los campos son vec4 para alineación std430
    struct SkinnedVertex {
        glm::vec4  pos;          // xyz + pad
        glm::vec4  normal;       // xyz + pad
        glm::vec4  tangent;      // xyz + pad
        glm::vec4  uv_pad;       // xy + pad
        glm::vec4  color;        // rgb + pad
        glm::ivec4 boneIndices;  // hasta 4 influencias (índices en sorted order)
        glm::vec4  boneWeights;  // suma = 1.0
    };

    // Keyframes CPU (almacenamiento intermedio antes de subir a GPU)
    struct BoneKeyframe  { float time; glm::vec3 value; };
    struct BoneKeyframeQ { float time; glm::quat value; };

    struct BoneChannel {
        int boneIndex; // sorted index (topológico)
        std::vector<BoneKeyframe>  posKeys, scaleKeys;
        std::vector<BoneKeyframeQ> rotKeys;
    };

    struct AnimationClip {
        std::string              name;
        float                    duration;        // en ticks
        float                    ticksPerSecond;
        std::vector<BoneChannel> channels;
    };

    struct Skeleton {
        std::vector<std::string>            names;
        std::vector<int>                    parentIndex;   // -1 = raíz; índice en sorted order
        std::vector<glm::mat4>              inverseBindPose;
        std::unordered_map<std::string,int> boneMap;       // nombre → sorted index
    };

    struct SkinnedMesh : Mesh {
        std::vector<SkinnedVertex> skinnedVertices;
        Skeleton                   skeleton;
        AnimationClip              clip;
    };

    // ── Tipos GPU-aligned (std430) ──────────────────────────────────────────

    // Keyframe de posición/escala: vec4(time,0,0,0) + vec4(x,y,z,0)
    struct GpuPosKey {
        glm::vec4 timePad; // .x = time
        glm::vec4 value;   // .xyz = pos o scale
    };

    // Keyframe de rotación: vec4(time,0,0,0) + vec4(qx,qy,qz,qw)
    struct GpuRotKey {
        glm::vec4 timePad; // .x = time
        glm::vec4 quat;    // xyzw (memory layout de glm::quat)
    };

    // Información por hueso: offsets en keyframe arrays + inversa bind pose
    // Tamaño: 8 int32 (32 bytes) + mat4 (64 bytes) = 96 bytes
    // inverseBindPose está en offset 32 → alineado a 16 bytes
    struct GpuBoneInfo {
        int32_t   posOffset,   posCount;
        int32_t   rotOffset,   rotCount;
        int32_t   scaleOffset, scaleCount;
        int32_t   parentIndex; // -1 = raíz
        int32_t   pad;
        glm::mat4 inverseBindPose;
    };
}
```

- [ ] **Step 2: Compilar — verificar que el header no da errores**

```
cmake --build build --config Debug
```

Esperado: sin errores de compilación (el header no es incluido aún por nadie).

- [ ] **Step 3: Commit**

```bash
git add engine/include/DonTopo/SkinnedMesh.h
git commit -m "feat(anim): add SkinnedMesh CPU/GPU data types"
```

---

## Task 2: ModelLoader::loadSkinned()

**Files:**
- Modify: `engine/include/DonTopo/ModelLoader.h`
- Modify: `engine/src/ModelLoader.cpp`

**Interfaces:**
- Consumes: `DonTopo::SkinnedMesh` (Task 1)
- Produces: `ModelLoader::loadSkinned(path)` → `SkinnedMesh`

- [ ] **Step 1: Añadir declaración en ModelLoader.h**

```cpp
// engine/include/DonTopo/ModelLoader.h
#pragma once
#include <string>
#include "DonTopo/Mesh.h"
#include "DonTopo/SkinnedMesh.h"

namespace DonTopo
{
    class ModelLoader
    {
        public:
            static Mesh        load(const std::string& path);
            static SkinnedMesh loadSkinned(const std::string& path);
    };
}
```

- [ ] **Step 2: Añadir includes y helper en ModelLoader.cpp**

Al inicio de `engine/src/ModelLoader.cpp`, después de los includes existentes:

```cpp
#include "DonTopo/SkinnedMesh.h"
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <functional>
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
```

Y añadir este helper estático antes de `loadSkinned` (dentro del namespace DonTopo):

```cpp
static glm::mat4 aiToGlm(const aiMatrix4x4& m) {
    // Assimp row-major → GLM column-major: transponer
    return glm::transpose(glm::mat4(
        m.a1, m.a2, m.a3, m.a4,
        m.b1, m.b2, m.b3, m.b4,
        m.c1, m.c2, m.c3, m.c4,
        m.d1, m.d2, m.d3, m.d4
    ));
}
```

- [ ] **Step 3: Implementar loadSkinned() — parte 1: esqueleto y vértices**

```cpp
// engine/src/ModelLoader.cpp — añadir al final del namespace DonTopo

SkinnedMesh ModelLoader::loadSkinned(const std::string& path)
{
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_FlipUVs     |
        aiProcess_GenNormals  |
        aiProcess_CalcTangentSpace |
        aiProcess_LimitBoneWeights);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        throw std::runtime_error("Assimp: " + std::string(importer.GetErrorString()));

    aiMesh* ai = scene->mMeshes[0];

    // ── 1. Recopilar datos originales de huesos ──────────────────────────
    std::unordered_map<std::string,int> origBoneMap;
    std::vector<glm::mat4>              origInvBind;
    for (uint32_t b = 0; b < ai->mNumBones; b++) {
        std::string name = ai->mBones[b]->mName.C_Str();
        origBoneMap[name] = (int)b;
        origInvBind.push_back(aiToGlm(ai->mBones[b]->mOffsetMatrix));
    }
    int boneCount = (int)origBoneMap.size();

    // ── 2. Ordenar huesos topológicamente (DFS sobre jerarquía aiNode) ──
    std::vector<int> sortedTopo;      // sortedTopo[si] = originalIdx
    std::vector<int> sortedParent;    // sortedParent[si] = si del padre (-1 raíz)
    std::vector<int> origToSorted(boneCount, -1);

    std::function<void(aiNode*,int)> dfs = [&](aiNode* node, int parentSI) {
        std::string name = node->mName.C_Str();
        int mySI = parentSI;
        auto it = origBoneMap.find(name);
        if (it != origBoneMap.end()) {
            mySI = (int)sortedTopo.size();
            origToSorted[it->second] = mySI;
            sortedTopo.push_back(it->second);
            sortedParent.push_back(parentSI);
        }
        for (uint32_t i = 0; i < node->mNumChildren; i++)
            dfs(node->mChildren[i], mySI);
    };
    dfs(scene->mRootNode, -1);

    // ── 3. Construir Skeleton en sorted order ────────────────────────────
    SkinnedMesh mesh;
    Skeleton& skel = mesh.skeleton;
    skel.names.resize(boneCount);
    skel.parentIndex.resize(boneCount);
    skel.inverseBindPose.resize(boneCount);
    for (int si = 0; si < boneCount; si++) {
        int oi = sortedTopo[si];
        skel.names[si]           = ai->mBones[oi]->mName.C_Str();
        skel.parentIndex[si]     = sortedParent[si];
        skel.inverseBindPose[si] = origInvBind[oi];
        skel.boneMap[skel.names[si]] = si;
    }

    // ── 4. Acumular influencias por vértice ──────────────────────────────
    struct Influence { int boneIdx; float weight; };
    std::vector<std::vector<Influence>> influences(ai->mNumVertices);
    for (uint32_t b = 0; b < ai->mNumBones; b++) {
        int si = origToSorted[b];
        for (uint32_t w = 0; w < ai->mBones[b]->mNumWeights; w++) {
            uint32_t vid = ai->mBones[b]->mWeights[w].mVertexId;
            float    wt  = ai->mBones[b]->mWeights[w].mWeight;
            influences[vid].push_back({si, wt});
        }
    }

    // ── 5. Construir SkinnedVertex[] ─────────────────────────────────────
    mesh.skinnedVertices.resize(ai->mNumVertices);
    for (uint32_t i = 0; i < ai->mNumVertices; i++) {
        SkinnedVertex sv{};
        sv.pos    = glm::vec4(ai->mVertices[i].x, ai->mVertices[i].y, ai->mVertices[i].z, 0.0f);
        sv.normal = ai->mNormals
            ? glm::vec4(ai->mNormals[i].x, ai->mNormals[i].y, ai->mNormals[i].z, 0.0f)
            : glm::vec4(0,1,0,0);
        sv.tangent = ai->mTangents
            ? glm::vec4(ai->mTangents[i].x, ai->mTangents[i].y, ai->mTangents[i].z, 0.0f)
            : glm::vec4(1,0,0,0);
        sv.uv_pad = ai->mTextureCoords[0]
            ? glm::vec4(ai->mTextureCoords[0][i].x, ai->mTextureCoords[0][i].y, 0.0f, 0.0f)
            : glm::vec4(0);
        sv.color = glm::vec4(1.0f);

        auto& inf = influences[i];
        std::sort(inf.begin(), inf.end(), [](auto& a, auto& b){ return a.weight > b.weight; });
        if (inf.size() > 4) inf.resize(4);
        float total = 0;
        for (auto& bi : inf) total += bi.weight;
        if (total > 0) for (auto& bi : inf) bi.weight /= total;
        for (int j = 0; j < (int)inf.size(); j++) {
            sv.boneIndices[j] = inf[j].boneIdx;
            sv.boneWeights[j] = inf[j].weight;
        }
        mesh.skinnedVertices[i] = sv;
    }

    // ── 6. Índices ───────────────────────────────────────────────────────
    mesh.indices.reserve(ai->mNumFaces * 3);
    for (uint32_t f = 0; f < ai->mNumFaces; f++)
        for (uint32_t j = 0; j < ai->mFaces[f].mNumIndices; j++)
            mesh.indices.push_back(ai->mFaces[f].mIndices[j]);

    // ── 7. Texturas (igual que load()) ───────────────────────────────────
    if (scene->mNumMaterials > 0) {
        namespace fs = std::filesystem;
        aiMaterial* mat   = scene->mMaterials[ai->mMaterialIndex];
        fs::path    dir   = fs::path(path).parent_path();

        auto loadTex = [&](aiTextureType type,
                           std::vector<uint8_t>& outEmb,
                           std::string& outPath)
        {
            aiString texPath;
            if (mat->GetTexture(type, 0, &texPath) != AI_SUCCESS) return;
            const char* raw = texPath.C_Str();
            const aiTexture* emb = scene->GetEmbeddedTexture(raw);
            if (emb) {
                const uint8_t* data = reinterpret_cast<const uint8_t*>(emb->pcData);
                size_t sz = emb->mWidth;
                if (emb->mHeight == 0) sz = emb->mWidth;
                outEmb.assign(data, data + sz);
            } else {
                outPath = (dir / raw).string();
            }
        };

        loadTex(aiTextureType_DIFFUSE,  mesh.embeddedTexture,   mesh.texturePath);
        loadTex(aiTextureType_NORMALS,  mesh.embeddedNormalMap, mesh.normalMapPath);
        if (mesh.normalMapPath.empty() && mesh.embeddedNormalMap.empty())
            loadTex(aiTextureType_HEIGHT, mesh.embeddedNormalMap, mesh.normalMapPath);
    }

    // ── 8. AnimationClip ─────────────────────────────────────────────────
    if (scene->mNumAnimations > 0) {
        aiAnimation* anim       = scene->mAnimations[0];
        mesh.clip.name          = anim->mName.C_Str();
        mesh.clip.duration      = (float)anim->mDuration;
        mesh.clip.ticksPerSecond = anim->mTicksPerSecond > 0
                                  ? (float)anim->mTicksPerSecond : 24.0f;

        for (uint32_t c = 0; c < anim->mNumChannels; c++) {
            aiNodeAnim* ch       = anim->mChannels[c];
            std::string boneName = ch->mNodeName.C_Str();
            auto it = skel.boneMap.find(boneName);
            if (it == skel.boneMap.end()) continue;

            BoneChannel bc;
            bc.boneIndex = it->second;

            for (uint32_t k = 0; k < ch->mNumPositionKeys; k++) {
                auto& pk = ch->mPositionKeys[k];
                bc.posKeys.push_back({(float)pk.mTime, {pk.mValue.x, pk.mValue.y, pk.mValue.z}});
            }
            for (uint32_t k = 0; k < ch->mNumRotationKeys; k++) {
                auto& rk = ch->mRotationKeys[k];
                bc.rotKeys.push_back({(float)rk.mTime,
                    glm::quat(rk.mValue.w, rk.mValue.x, rk.mValue.y, rk.mValue.z)});
            }
            for (uint32_t k = 0; k < ch->mNumScalingKeys; k++) {
                auto& sk = ch->mScalingKeys[k];
                bc.scaleKeys.push_back({(float)sk.mTime, {sk.mValue.x, sk.mValue.y, sk.mValue.z}});
            }
            mesh.clip.channels.push_back(bc);
        }
    }

    return mesh;
}
```

- [ ] **Step 4: Compilar**

```
cmake --build build --config Debug
```

Esperado: sin errores.

- [ ] **Step 5: Smoke test en main.cpp (temporal)**

En `sandbox/src/main.cpp`, después de `renderer.init(...)`, añadir temporalmente:

```cpp
auto skinnedMesh = DonTopo::ModelLoader::loadSkinned("assets/modelAnimation.fbx");
std::cout << "Bones: "    << skinnedMesh.skeleton.names.size()   << "\n";
std::cout << "Vertices: " << skinnedMesh.skinnedVertices.size()  << "\n";
std::cout << "Duration: " << skinnedMesh.clip.duration           << " ticks\n";
std::cout << "Channels: " << skinnedMesh.clip.channels.size()    << "\n";
```

Ejecutar y verificar que los valores son > 0.

- [ ] **Step 6: Eliminar el código temporal del Step 5**

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/ModelLoader.h engine/src/ModelLoader.cpp
git commit -m "feat(anim): ModelLoader::loadSkinned() with topological bone sort"
```

---

## Task 3: Compute Shaders

**Files:**
- Create: `shaders/bone_eval.comp`
- Create: `shaders/bone_hierarchy.comp`
- Create: `shaders/skinning.comp`

**Interfaces:**
- Binding layout compartida: 0=posKeys, 1=rotKeys, 2=scaleKeys, 3=boneInfo, 4=localTransform, 5=finalBone, 6=inputVerts, 7=outputVerts
- Push constant: `struct { float animTime; uint boneCount; uint vertexCount; uint pad; }`

- [ ] **Step 1: Crear bone_eval.comp**

```glsl
// shaders/bone_eval.comp
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct PosKey   { vec4 timePad; vec4 value; };
struct RotKey   { vec4 timePad; vec4 quat;  };
struct ScaleKey { vec4 timePad; vec4 value; };

struct BoneInfo {
    int posOffset,   posCount;
    int rotOffset,   rotCount;
    int scaleOffset, scaleCount;
    int parentIndex, pad;
    mat4 inverseBindPose;
};

layout(std430, binding = 0) readonly  buffer PosKeys        { PosKey   posKeys[];        };
layout(std430, binding = 1) readonly  buffer RotKeys        { RotKey   rotKeys[];        };
layout(std430, binding = 2) readonly  buffer ScaleKeys      { ScaleKey scaleKeys[];      };
layout(std430, binding = 3) readonly  buffer BoneInfos      { BoneInfo boneInfos[];      };
layout(std430, binding = 4) writeonly buffer LocalTransforms{ mat4     localTransforms[];};

layout(push_constant) uniform Push {
    float animTime;
    uint  boneCount;
    uint  vertexCount;
    uint  pad;
} push;

vec3 lerpPos(int offset, int count, float t) {
    if (count == 0) return vec3(0.0);
    if (count == 1) return posKeys[offset].value.xyz;
    int i = 0;
    for (int k = 0; k < count - 1; k++) {
        i = k;
        if (t < posKeys[offset + k + 1].timePad.x) break;
    }
    float t0 = posKeys[offset + i].timePad.x;
    float t1 = posKeys[offset + i + 1].timePad.x;
    float f  = (t1 > t0) ? clamp((t - t0) / (t1 - t0), 0.0, 1.0) : 0.0;
    return mix(posKeys[offset+i].value.xyz, posKeys[offset+i+1].value.xyz, f);
}

vec4 slerpQuat(vec4 a, vec4 b, float t) {
    float d = dot(a, b);
    if (d < 0.0) { b = -b; d = -d; }
    if (d > 0.9995) return normalize(mix(a, b, t));
    float theta = acos(clamp(d, -1.0, 1.0));
    float s = sin(theta);
    return (sin((1.0 - t) * theta) * a + sin(t * theta) * b) / s;
}

vec4 lerpRot(int offset, int count, float t) {
    if (count == 0) return vec4(0.0, 0.0, 0.0, 1.0);
    if (count == 1) return rotKeys[offset].quat;
    int i = 0;
    for (int k = 0; k < count - 1; k++) {
        i = k;
        if (t < rotKeys[offset + k + 1].timePad.x) break;
    }
    float t0 = rotKeys[offset + i].timePad.x;
    float t1 = rotKeys[offset + i + 1].timePad.x;
    float f  = (t1 > t0) ? clamp((t - t0) / (t1 - t0), 0.0, 1.0) : 0.0;
    return slerpQuat(rotKeys[offset+i].quat, rotKeys[offset+i+1].quat, f);
}

vec3 lerpScale(int offset, int count, float t) {
    if (count == 0) return vec3(1.0);
    if (count == 1) return scaleKeys[offset].value.xyz;
    int i = 0;
    for (int k = 0; k < count - 1; k++) {
        i = k;
        if (t < scaleKeys[offset + k + 1].timePad.x) break;
    }
    float t0 = scaleKeys[offset + i].timePad.x;
    float t1 = scaleKeys[offset + i + 1].timePad.x;
    float f  = (t1 > t0) ? clamp((t - t0) / (t1 - t0), 0.0, 1.0) : 0.0;
    return mix(scaleKeys[offset+i].value.xyz, scaleKeys[offset+i+1].value.xyz, f);
}

mat4 buildTRS(vec3 pos, vec4 q, vec3 s) {
    float qx=q.x, qy=q.y, qz=q.z, qw=q.w;
    vec3 c0 = vec3(1.0-2.0*(qy*qy+qz*qz),  2.0*(qx*qy+qz*qw),       2.0*(qx*qz-qy*qw)     ) * s.x;
    vec3 c1 = vec3(2.0*(qx*qy-qz*qw),       1.0-2.0*(qx*qx+qz*qz),  2.0*(qy*qz+qx*qw)     ) * s.y;
    vec3 c2 = vec3(2.0*(qx*qz+qy*qw),       2.0*(qy*qz-qx*qw),       1.0-2.0*(qx*qx+qy*qy)) * s.z;
    return mat4(vec4(c0, 0.0), vec4(c1, 0.0), vec4(c2, 0.0), vec4(pos, 1.0));
}

void main() {
    uint bi = gl_GlobalInvocationID.x;
    if (bi >= push.boneCount) return;
    BoneInfo info = boneInfos[bi];
    float t = push.animTime;
    vec3 pos   = lerpPos  (info.posOffset,   info.posCount,   t);
    vec4 rot   = lerpRot  (info.rotOffset,   info.rotCount,   t);
    vec3 scale = lerpScale(info.scaleOffset, info.scaleCount, t);
    localTransforms[bi] = buildTRS(pos, rot, scale);
}
```

- [ ] **Step 2: Crear bone_hierarchy.comp**

```glsl
// shaders/bone_hierarchy.comp
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

struct BoneInfo {
    int posOffset,   posCount;
    int rotOffset,   rotCount;
    int scaleOffset, scaleCount;
    int parentIndex, pad;
    mat4 inverseBindPose;
};

layout(std430, binding = 3) readonly  buffer BoneInfos      { BoneInfo boneInfos[];      };
layout(std430, binding = 4) readonly  buffer LocalTransforms{ mat4     localTransforms[];};
layout(std430, binding = 5) writeonly buffer FinalBones     { mat4     finalBones[];     };

layout(push_constant) uniform Push {
    float animTime;
    uint  boneCount;
    uint  vertexCount;
    uint  pad;
} push;

mat4 worldTransforms[128]; // máximo 128 huesos

void main() {
    for (uint i = 0; i < push.boneCount; i++) {
        int parent = boneInfos[i].parentIndex;
        mat4 world = (parent < 0)
            ? localTransforms[i]
            : worldTransforms[parent] * localTransforms[i];
        worldTransforms[i] = world;
        finalBones[i]      = world * boneInfos[i].inverseBindPose;
    }
}
```

- [ ] **Step 3: Crear skinning.comp**

```glsl
// shaders/skinning.comp
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct SkinnedVertex {
    vec4  pos;
    vec4  normal;
    vec4  tangent;
    vec4  uv_pad;
    vec4  color;
    ivec4 boneIndices;
    vec4  boneWeights;
};

// OutputVertex: 5 × vec4 = stride 80 bytes
// Mismo layout que el vertex buffer que bindea el graphics pass
struct OutputVertex {
    vec4 pos;     // offset  0
    vec4 color;   // offset 16
    vec4 uv;      // offset 32 (xy usados)
    vec4 normal;  // offset 48
    vec4 tangent; // offset 64
};

layout(std430, binding = 5) readonly  buffer FinalBones  { mat4          finalBones[];  };
layout(std430, binding = 6) readonly  buffer InputVerts  { SkinnedVertex inputVerts[];  };
layout(std430, binding = 7) writeonly buffer OutputVerts { OutputVertex  outputVerts[]; };

layout(push_constant) uniform Push {
    float animTime;
    uint  boneCount;
    uint  vertexCount;
    uint  pad;
} push;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= push.vertexCount) return;

    SkinnedVertex v = inputVerts[idx];

    mat4 skin = v.boneWeights.x * finalBones[v.boneIndices.x]
              + v.boneWeights.y * finalBones[v.boneIndices.y]
              + v.boneWeights.z * finalBones[v.boneIndices.z]
              + v.boneWeights.w * finalBones[v.boneIndices.w];

    OutputVertex o;
    o.pos     = skin * vec4(v.pos.xyz, 1.0);
    o.color   = v.color;
    o.uv      = v.uv_pad;
    o.normal  = vec4(normalize((skin * vec4(v.normal.xyz,  0.0)).xyz), 0.0);
    o.tangent = vec4(normalize((skin * vec4(v.tangent.xyz, 0.0)).xyz), 0.0);
    outputVerts[idx] = o;
}
```

- [ ] **Step 4: Compilar los tres shaders**

```bash
glslc shaders/bone_eval.comp      -o shaders/bone_eval.comp.spv
glslc shaders/bone_hierarchy.comp -o shaders/bone_hierarchy.comp.spv
glslc shaders/skinning.comp       -o shaders/skinning.comp.spv
```

Esperado: tres archivos `.spv` creados sin errores.

- [ ] **Step 5: Commit**

```bash
git add shaders/bone_eval.comp shaders/bone_eval.comp.spv
git add shaders/bone_hierarchy.comp shaders/bone_hierarchy.comp.spv
git add shaders/skinning.comp shaders/skinning.comp.spv
git commit -m "feat(anim): add GPU compute shaders for bone eval, hierarchy and skinning"
```

---

## Task 4: Renderer — Compute Infrastructure (header + pipeline creation)

**Files:**
- Modify: `engine/include/DonTopo/Renderer.h`
- Modify: `engine/src/Renderer.cpp`

**Interfaces:**
- Consumes: `DonTopo::SkinnedMesh` (Task 1)
- Produces: `m_boneEvalPipeline`, `m_boneHierarchyPipeline`, `m_skinningPipeline`, `m_computePipelineLayout`, `m_computeDescLayout`, `m_computeDescPool`

- [ ] **Step 1: Actualizar Renderer.h — añadir structs y miembros**

Añadir en Renderer.h después de los includes existentes:

```cpp
#include "DonTopo/SkinnedMesh.h"
```

Dentro de la clase `Renderer`, antes de `private:`, añadir métodos públicos:

```cpp
int  addSkinnedMesh(const SkinnedMesh& mesh);
void updateAnimation(int index, float deltaTime);
void setSkinnedTransform(int index, const glm::mat4& t);
```

Dentro de `private:`, añadir justo antes de `std::vector<RenderObject> m_objects;`:

```cpp
static constexpr int MAX_SKINNED_OBJECTS = 8;

struct ComputePush {
    float    animTime;
    uint32_t boneCount;
    uint32_t vertexCount;
    uint32_t pad;
};

struct SkinnedRenderObject {
    // SSBOs estáticos (subidos una vez)
    VkBuffer       posKeyBuffer    = VK_NULL_HANDLE;
    VkDeviceMemory posKeyMemory    = VK_NULL_HANDLE;
    VkBuffer       rotKeyBuffer    = VK_NULL_HANDLE;
    VkDeviceMemory rotKeyMemory    = VK_NULL_HANDLE;
    VkBuffer       scaleKeyBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory scaleKeyMemory  = VK_NULL_HANDLE;
    VkBuffer       boneInfoBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory boneInfoMemory  = VK_NULL_HANDLE;
    VkBuffer       inputVertBuffer = VK_NULL_HANDLE;
    VkDeviceMemory inputVertMemory = VK_NULL_HANDLE;
    // SSBOs dinámicos (escritos por compute cada frame)
    VkBuffer       localTransBuffer = VK_NULL_HANDLE;
    VkDeviceMemory localTransMemory = VK_NULL_HANDLE;
    VkBuffer       finalBoneBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory finalBoneMemory  = VK_NULL_HANDLE;
    // Output vertex buffer (STORAGE | VERTEX)
    VkBuffer       outputVertBuffer = VK_NULL_HANDLE;
    VkDeviceMemory outputVertMemory = VK_NULL_HANDLE;
    // Index buffer
    VkBuffer       indexBuffer      = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory      = VK_NULL_HANDLE;
    uint32_t       indexCount       = 0;
    uint32_t       vertexCount      = 0;
    uint32_t       boneCount        = 0;
    // Texturas (igual que RenderObject)
    VkImage        textureImage     = VK_NULL_HANDLE;
    VkDeviceMemory textureMem       = VK_NULL_HANDLE;
    VkImageView    textureView      = VK_NULL_HANDLE;
    VkSampler      sampler          = VK_NULL_HANDLE;
    VkImage        normalImage      = VK_NULL_HANDLE;
    VkDeviceMemory normalMem        = VK_NULL_HANDLE;
    VkImageView    normalView       = VK_NULL_HANDLE;
    VkSampler      normalSampler    = VK_NULL_HANDLE;
    // Descriptor sets
    VkDescriptorSet computeDescSet               = VK_NULL_HANDLE;
    VkDescriptorSet graphicsDescSets[MAX_FRAMES] = {};
    // Estado de animación
    float           animTime        = 0.0f;
    float           duration        = 0.0f;   // en ticks
    float           ticksPerSecond  = 24.0f;
    glm::mat4       transform{1.0f};
};

// Métodos privados de compute
void createComputePipelines();
void uploadBuffer(const void* data, VkDeviceSize size, VkBuffer dst);
void destroySkinnedRenderObject(SkinnedRenderObject& obj);

// Recursos de compute
VkDescriptorSetLayout    m_computeDescLayout     = VK_NULL_HANDLE;
VkDescriptorPool         m_computeDescPool       = VK_NULL_HANDLE;
VkPipelineLayout         m_computePipelineLayout = VK_NULL_HANDLE;
VkPipeline               m_boneEvalPipeline      = VK_NULL_HANDLE;
VkPipeline               m_boneHierarchyPipeline = VK_NULL_HANDLE;
VkPipeline               m_skinningPipeline      = VK_NULL_HANDLE;
VkPipeline               m_skinnedGfxPipeline    = VK_NULL_HANDLE;
std::vector<SkinnedRenderObject> m_skinnedObjects;
```

- [ ] **Step 2: Implementar createComputePipelines() en Renderer.cpp**

```cpp
void Renderer::createComputePipelines()
{
    // ── Descriptor Set Layout (8 storage buffers) ────────────────────────
    VkDescriptorSetLayoutBinding bindings[8]{};
    for (int i = 0; i < 8; i++) {
        bindings[i].binding         = (uint32_t)i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 8;
    layoutInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_computeDescLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute descriptor set layout!");

    // ── Descriptor Pool ──────────────────────────────────────────────────
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = (uint32_t)(MAX_SKINNED_OBJECTS * 8);
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = MAX_SKINNED_OBJECTS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_computeDescPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute descriptor pool!");

    // ── Pipeline Layout ───────────────────────────────────────────────────
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(ComputePush);

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount         = 1;
    plInfo.pSetLayouts            = &m_computeDescLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(m_device, &plInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create compute pipeline layout!");

    // ── Helper: crea un compute pipeline desde un .spv ───────────────────
    auto makeComputePipeline = [&](const std::string& spvPath, VkPipeline& out) {
        auto code = loadShaderFile(spvPath);
        VkShaderModule shader = createShaderModule(code);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName  = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage  = stage;
        ci.layout = m_computePipelineLayout;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr, &out) != VK_SUCCESS)
            throw std::runtime_error("failed to create compute pipeline: " + spvPath);
        vkDestroyShaderModule(m_device, shader, nullptr);
    };

    makeComputePipeline("shaders/bone_eval.comp.spv",      m_boneEvalPipeline);
    makeComputePipeline("shaders/bone_hierarchy.comp.spv", m_boneHierarchyPipeline);
    makeComputePipeline("shaders/skinning.comp.spv",       m_skinningPipeline);
}
```

- [ ] **Step 3: Implementar uploadBuffer() helper**

```cpp
void Renderer::uploadBuffer(const void* data, VkDeviceSize size, VkBuffer dst)
{
    VkBuffer stagingBuf; VkDeviceMemory stagingMem;
    createBuffer(size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuf, stagingMem);
    void* mapped;
    vkMapMemory(m_device, stagingMem, 0, size, 0, &mapped);
    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(m_device, stagingMem);
    copyBuffer(stagingBuf, dst, size);
    vkDestroyBuffer(m_device, stagingBuf, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);
}
```

- [ ] **Step 4: Llamar createComputePipelines() desde init()**

En `Renderer::init()`, después de `createShadowResources()`:

```cpp
createComputePipelines();
```

- [ ] **Step 5: Añadir cleanup de compute en shutdown()**

En `Renderer::shutdown()`, antes de `vkDestroyDevice`:

```cpp
for (auto& obj : m_skinnedObjects)
    destroySkinnedRenderObject(obj);
vkDestroyPipeline(m_device, m_skinnedGfxPipeline,    nullptr);
vkDestroyPipeline(m_device, m_skinningPipeline,       nullptr);
vkDestroyPipeline(m_device, m_boneHierarchyPipeline,  nullptr);
vkDestroyPipeline(m_device, m_boneEvalPipeline,       nullptr);
vkDestroyPipelineLayout(m_device, m_computePipelineLayout, nullptr);
vkDestroyDescriptorPool(m_device, m_computeDescPool,   nullptr);
vkDestroyDescriptorSetLayout(m_device, m_computeDescLayout, nullptr);
```

- [ ] **Step 6: Implementar destroySkinnedRenderObject()**

```cpp
void Renderer::destroySkinnedRenderObject(SkinnedRenderObject& obj)
{
    vkDestroySampler  (m_device, obj.normalSampler,    nullptr);
    vkDestroyImageView(m_device, obj.normalView,       nullptr);
    vkDestroyImage    (m_device, obj.normalImage,      nullptr);
    vkFreeMemory      (m_device, obj.normalMem,        nullptr);
    vkDestroySampler  (m_device, obj.sampler,          nullptr);
    vkDestroyImageView(m_device, obj.textureView,      nullptr);
    vkDestroyImage    (m_device, obj.textureImage,     nullptr);
    vkFreeMemory      (m_device, obj.textureMem,       nullptr);
    vkDestroyBuffer   (m_device, obj.outputVertBuffer, nullptr);
    vkFreeMemory      (m_device, obj.outputVertMemory, nullptr);
    vkDestroyBuffer   (m_device, obj.finalBoneBuffer,  nullptr);
    vkFreeMemory      (m_device, obj.finalBoneMemory,  nullptr);
    vkDestroyBuffer   (m_device, obj.localTransBuffer, nullptr);
    vkFreeMemory      (m_device, obj.localTransMemory, nullptr);
    vkDestroyBuffer   (m_device, obj.inputVertBuffer,  nullptr);
    vkFreeMemory      (m_device, obj.inputVertMemory,  nullptr);
    vkDestroyBuffer   (m_device, obj.boneInfoBuffer,   nullptr);
    vkFreeMemory      (m_device, obj.boneInfoMemory,   nullptr);
    vkDestroyBuffer   (m_device, obj.scaleKeyBuffer,   nullptr);
    vkFreeMemory      (m_device, obj.scaleKeyMemory,   nullptr);
    vkDestroyBuffer   (m_device, obj.rotKeyBuffer,     nullptr);
    vkFreeMemory      (m_device, obj.rotKeyMemory,     nullptr);
    vkDestroyBuffer   (m_device, obj.posKeyBuffer,     nullptr);
    vkFreeMemory      (m_device, obj.posKeyMemory,     nullptr);
    vkDestroyBuffer   (m_device, obj.indexBuffer,      nullptr);
    vkFreeMemory      (m_device, obj.indexMemory,      nullptr);
}
```

- [ ] **Step 7: Compilar y ejecutar — debe arrancar sin crash**

```
cmake --build build --config Debug
```

Ejecutar: la app debe iniciar igual que antes (sin modelos animados todavía).

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer.h engine/src/Renderer.cpp
git commit -m "feat(anim): renderer compute infrastructure (pipelines, layout, pool)"
```

---

## Task 5: addSkinnedMesh() — Upload GPU Resources

**Files:**
- Modify: `engine/src/Renderer.cpp`

**Interfaces:**
- Consumes: `SkinnedMesh` (Task 1), `GpuPosKey`, `GpuRotKey`, `GpuBoneInfo` (Task 1), `uploadBuffer()` (Task 4)
- Produces: `Renderer::addSkinnedMesh(mesh)` → `int` (index en m_skinnedObjects)

- [ ] **Step 1: Implementar addSkinnedMesh()**

```cpp
int Renderer::addSkinnedMesh(const SkinnedMesh& mesh)
{
    m_skinnedObjects.emplace_back();
    SkinnedRenderObject& obj = m_skinnedObjects.back();

    int boneCount = (int)mesh.skeleton.names.size();
    obj.boneCount   = (uint32_t)boneCount;
    obj.vertexCount = (uint32_t)mesh.skinnedVertices.size();
    obj.indexCount  = (uint32_t)mesh.indices.size();
    obj.duration    = mesh.clip.duration;
    obj.ticksPerSecond = mesh.clip.ticksPerSecond;

    // ── 1. Aplanar keyframes a arrays GPU ────────────────────────────────
    std::unordered_map<int, const BoneChannel*> channelMap;
    for (auto& ch : mesh.clip.channels)
        channelMap[ch.boneIndex] = &ch;

    std::vector<GpuPosKey>   gpuPos;
    std::vector<GpuRotKey>   gpuRot;
    std::vector<GpuPosKey>   gpuScale;
    std::vector<GpuBoneInfo> gpuBoneInfo(boneCount);

    for (int i = 0; i < boneCount; i++) {
        GpuBoneInfo& info    = gpuBoneInfo[i];
        info.parentIndex     = mesh.skeleton.parentIndex[i];
        info.pad             = 0;
        info.inverseBindPose = mesh.skeleton.inverseBindPose[i];

        auto it = channelMap.find(i);
        if (it != channelMap.end()) {
            const BoneChannel* ch = it->second;

            info.posOffset = (int32_t)gpuPos.size();
            info.posCount  = (int32_t)ch->posKeys.size();
            for (auto& k : ch->posKeys)
                gpuPos.push_back({{k.time,0,0,0}, {k.value.x,k.value.y,k.value.z,0}});

            info.rotOffset = (int32_t)gpuRot.size();
            info.rotCount  = (int32_t)ch->rotKeys.size();
            for (auto& k : ch->rotKeys)
                gpuRot.push_back({{k.time,0,0,0},
                    {k.value.x, k.value.y, k.value.z, k.value.w}});

            info.scaleOffset = (int32_t)gpuScale.size();
            info.scaleCount  = (int32_t)ch->scaleKeys.size();
            for (auto& k : ch->scaleKeys)
                gpuScale.push_back({{k.time,0,0,0}, {k.value.x,k.value.y,k.value.z,0}});
        } else {
            info.posOffset=0; info.posCount=0;
            info.rotOffset=0; info.rotCount=0;
            info.scaleOffset=0; info.scaleCount=0;
        }
    }

    // Garantizar al menos 1 elemento para buffers vacíos (evitar size=0)
    if (gpuPos.empty())   gpuPos.push_back({});
    if (gpuRot.empty())   gpuRot.push_back({});
    if (gpuScale.empty()) gpuScale.push_back({});

    // ── 2. Crear y subir SSBOs estáticos ─────────────────────────────────
    auto makeSSBO = [&](const void* data, VkDeviceSize size,
                        VkBuffer& buf, VkDeviceMemory& mem)
    {
        createBuffer(size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buf, mem);
        uploadBuffer(data, size, buf);
    };

    makeSSBO(gpuPos.data(),      gpuPos.size()   *sizeof(GpuPosKey),
             obj.posKeyBuffer,   obj.posKeyMemory);
    makeSSBO(gpuRot.data(),      gpuRot.size()   *sizeof(GpuRotKey),
             obj.rotKeyBuffer,   obj.rotKeyMemory);
    makeSSBO(gpuScale.data(),    gpuScale.size() *sizeof(GpuPosKey),
             obj.scaleKeyBuffer, obj.scaleKeyMemory);
    makeSSBO(gpuBoneInfo.data(), gpuBoneInfo.size()*sizeof(GpuBoneInfo),
             obj.boneInfoBuffer, obj.boneInfoMemory);
    makeSSBO(mesh.skinnedVertices.data(),
             mesh.skinnedVertices.size()*sizeof(SkinnedVertex),
             obj.inputVertBuffer, obj.inputVertMemory);

    // ── 3. Crear SSBOs dinámicos (localTransform, finalBone) ─────────────
    VkDeviceSize matSize = (VkDeviceSize)boneCount * sizeof(glm::mat4);
    createBuffer(matSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        obj.localTransBuffer, obj.localTransMemory);
    createBuffer(matSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        obj.finalBoneBuffer, obj.finalBoneMemory);

    // ── 4. Output vertex buffer (STORAGE | VERTEX) ───────────────────────
    VkDeviceSize outSize = (VkDeviceSize)mesh.skinnedVertices.size() * sizeof(glm::vec4) * 5;
    createBuffer(outSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        obj.outputVertBuffer, obj.outputVertMemory);

    // ── 5. Index buffer ──────────────────────────────────────────────────
    createIndexBuffer(mesh.indices, obj.indexBuffer, obj.indexMemory);

    // ── 6. Texturas (reutiliza helpers existentes) ───────────────────────
    createTextureImage(mesh.texturePath, mesh.embeddedTexture, obj.textureImage, obj.textureMem);
    createTextureImageView(obj.textureImage, obj.textureView);
    createTextureSampler(obj.sampler);
    createNormalMapImage(mesh.normalMapPath, mesh.embeddedNormalMap, obj.normalImage, obj.normalMem);
    createTextureImageView(obj.normalImage, obj.normalView);
    createTextureSampler(obj.normalSampler);

    // ── 7. Compute descriptor set ────────────────────────────────────────
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_computeDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_computeDescLayout;
    vkAllocateDescriptorSets(m_device, &allocInfo, &obj.computeDescSet);

    auto writeSSBO = [&](uint32_t binding, VkBuffer buf, VkDeviceSize range) {
        VkDescriptorBufferInfo info{buf, 0, range};
        VkWriteDescriptorSet   w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = obj.computeDescSet;
        w.dstBinding      = binding;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo     = &info;
        vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
    };

    writeSSBO(0, obj.posKeyBuffer,    gpuPos.size()   *sizeof(GpuPosKey));
    writeSSBO(1, obj.rotKeyBuffer,    gpuRot.size()   *sizeof(GpuRotKey));
    writeSSBO(2, obj.scaleKeyBuffer,  gpuScale.size() *sizeof(GpuPosKey));
    writeSSBO(3, obj.boneInfoBuffer,  gpuBoneInfo.size()*sizeof(GpuBoneInfo));
    writeSSBO(4, obj.localTransBuffer,matSize);
    writeSSBO(5, obj.finalBoneBuffer, matSize);
    writeSSBO(6, obj.inputVertBuffer, mesh.skinnedVertices.size()*sizeof(SkinnedVertex));
    writeSSBO(7, obj.outputVertBuffer,outSize);

    // ── 8. Graphics descriptor sets (UBO + textures + shadowMap) ─────────
    VkDescriptorSetLayout layouts[MAX_FRAMES] = {m_descriptorSetLayout, m_descriptorSetLayout};
    VkDescriptorSetAllocateInfo gAllocInfo{};
    gAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    gAllocInfo.descriptorPool     = m_descriptorPool;
    gAllocInfo.descriptorSetCount = MAX_FRAMES;
    gAllocInfo.pSetLayouts        = layouts;
    vkAllocateDescriptorSets(m_device, &gAllocInfo, obj.graphicsDescSets);

    for (int i = 0; i < MAX_FRAMES; i++) {
        VkDescriptorBufferInfo uboInfo{m_uniformBuffers[i], 0, sizeof(UniformBufferObject)};
        VkDescriptorImageInfo  diffInfo{obj.sampler,      obj.textureView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo  normInfo{obj.normalSampler,obj.normalView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo  shadInfo{m_shadowSampler,  m_shadowView,    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};

        VkWriteDescriptorSet writes[4]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = obj.graphicsDescSets[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &uboInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = obj.graphicsDescSets[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &diffInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = obj.graphicsDescSets[i];
        writes[2].dstBinding      = 2;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo      = &normInfo;

        writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet          = obj.graphicsDescSets[i];
        writes[3].dstBinding      = 3;
        writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].descriptorCount = 1;
        writes[3].pImageInfo      = &shadInfo;

        vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    }

    // Expandir descriptor pool para acomodar sets adicionales de skinned meshes:
    // IMPORTANTE: createDescriptorPool() reserva n = meshes.size() sets.
    // addSkinnedMesh() añade MAX_FRAMES sets extra al m_descriptorPool.
    // Si el pool se agota, Vulkan lanzará VK_ERROR_OUT_OF_POOL_MEMORY.
    // Solución temporal: en createDescriptorPool(), aumentar el maxSets y
    // poolSizes.descriptorCount para dejar margen (ver Task 4 shutdown note).

    return (int)m_skinnedObjects.size() - 1;
}
```

> **Nota sobre el descriptor pool:** `m_descriptorPool` (gráfico) se crea en `createDescriptorPool()` con `maxSets = n * MAX_FRAMES` donde n = número de meshes estáticos. Para acomodar los skinned objects hay que aumentar ese maxSets. Editar `createDescriptorPool()`:
> ```cpp
> // Antes:
> poolInfo.maxSets = (uint32_t)(n * MAX_FRAMES);
> poolSizes[1].descriptorCount = n * 3 * MAX_FRAMES;
> // Después (añadir margen para MAX_SKINNED_OBJECTS):
> poolInfo.maxSets = (uint32_t)((n + MAX_SKINNED_OBJECTS) * MAX_FRAMES);
> poolSizes[0].descriptorCount = (n + MAX_SKINNED_OBJECTS) * MAX_FRAMES;
> poolSizes[1].descriptorCount = (n + MAX_SKINNED_OBJECTS) * 3 * MAX_FRAMES;
> ```

- [ ] **Step 2: Actualizar createDescriptorPool() como se describe en la nota**

- [ ] **Step 3: Implementar updateAnimation() y setSkinnedTransform()**

```cpp
void Renderer::updateAnimation(int index, float deltaTime)
{
    if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
    SkinnedRenderObject& obj = m_skinnedObjects[index];
    obj.animTime += deltaTime * obj.ticksPerSecond;
    if (obj.duration > 0.0f)
        obj.animTime = fmod(obj.animTime, obj.duration);
}

void Renderer::setSkinnedTransform(int index, const glm::mat4& t)
{
    if (index >= 0 && index < (int)m_skinnedObjects.size())
        m_skinnedObjects[index].transform = t;
}
```

- [ ] **Step 4: Compilar**

```
cmake --build build --config Debug
```

- [ ] **Step 5: Commit**

```bash
git add engine/src/Renderer.cpp engine/include/DonTopo/Renderer.h
git commit -m "feat(anim): addSkinnedMesh() — upload SSBOs and descriptor sets"
```

---

## Task 6: recordComputePass() + Barriers

**Files:**
- Modify: `engine/src/Renderer.cpp`

**Interfaces:**
- Consumes: `m_skinnedObjects`, `m_boneEvalPipeline`, `m_boneHierarchyPipeline`, `m_skinningPipeline`
- Produces: `recordComputePass(VkCommandBuffer)` — llamado antes de recordShadowPass

- [ ] **Step 1: Añadir declaración en Renderer.h**

En `private:`, añadir:
```cpp
void recordComputePass(VkCommandBuffer cmd);
```

- [ ] **Step 2: Implementar recordComputePass()**

```cpp
void Renderer::recordComputePass(VkCommandBuffer cmd)
{
    if (m_skinnedObjects.empty()) return;

    for (auto& obj : m_skinnedObjects) {
        ComputePush push{};
        push.animTime    = obj.animTime;
        push.boneCount   = obj.boneCount;
        push.vertexCount = obj.vertexCount;
        push.pad         = 0;

        // === Pass 1: bone_eval — interpolación de keyframes ===
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boneEvalPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_computePipelineLayout, 0, 1, &obj.computeDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, m_computePipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
        vkCmdDispatch(cmd, (obj.boneCount + 63) / 64, 1, 1);

        // Barrier: localTransform write → read
        VkMemoryBarrier b1{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b1.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &b1, 0, nullptr, 0, nullptr);

        // === Pass 2: bone_hierarchy — traversal jerárquico ===
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boneHierarchyPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_computePipelineLayout, 0, 1, &obj.computeDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, m_computePipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
        vkCmdDispatch(cmd, 1, 1, 1);

        // Barrier: finalBone write → read
        VkMemoryBarrier b2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &b2, 0, nullptr, 0, nullptr);

        // === Pass 3: skinning — skinning por vértice ===
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinningPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            m_computePipelineLayout, 0, 1, &obj.computeDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, m_computePipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
        vkCmdDispatch(cmd, (obj.vertexCount + 63) / 64, 1, 1);
    }

    // Barrier final: outputVertexBuffer write (compute) → vertex read (graphics)
    VkMemoryBarrier bFinal{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    bFinal.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bFinal.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 1, &bFinal, 0, nullptr, 0, nullptr);
}
```

- [ ] **Step 3: Llamar recordComputePass() desde recordCommandBuffer()**

En `recordCommandBuffer()`, antes de `recordShadowPass(...)`:

```cpp
recordComputePass(m_commandBuffers[m_currentFrame]);
```

- [ ] **Step 4: Compilar y ejecutar — sin crash, sin validation errors**

```
cmake --build build --config Debug
```

- [ ] **Step 5: Commit**

```bash
git add engine/src/Renderer.cpp engine/include/DonTopo/Renderer.h
git commit -m "feat(anim): recordComputePass() with barriers between bone eval/hierarchy/skinning"
```

---

## Task 7: Skinned Graphics Pipeline + Draw Call

**Files:**
- Modify: `engine/src/Renderer.cpp`

**Interfaces:**
- Consumes: `m_skinnedObjects[i].outputVertBuffer` (stride=80, 5×vec4)
- Produces: `m_skinnedGfxPipeline`, draw loop para skinned objects en recordCommandBuffer

- [ ] **Step 1: Crear m_skinnedGfxPipeline en createComputePipelines()**

Al final de `createComputePipelines()`, después de crear los 3 compute pipelines:

```cpp
// ── Skinned Graphics Pipeline ─────────────────────────────────────────
{
    auto vertCode = loadShaderFile("shaders/triangle.vert.spv");
    auto fragCode = loadShaderFile("shaders/triangle.frag.spv");
    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName  = "main";

    // Vertex input: stride=80 (5 × vec4), atributos con offsets vec4
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = (uint32_t)(sizeof(glm::vec4) * 5); // 80 bytes
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[5]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};   // pos   (xyz de vec4 en offset 0)
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16};  // color (xyz de vec4 en offset 16)
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    32};  // uv    (xy  de vec4 en offset 32)
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 48};  // normal(xyz de vec4 en offset 48)
    attrs[4] = {4, 0, VK_FORMAT_R32G32B32_SFLOAT, 64};  // tangent

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = 5;
    vertexInput.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &colorBlendAttachment;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_pipelineLayout; // mismo layout que pipeline estático
    pipelineInfo.renderPass          = m_renderPass;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                  nullptr, &m_skinnedGfxPipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create skinned graphics pipeline!");

    vkDestroyShaderModule(m_device, vertModule, nullptr);
    vkDestroyShaderModule(m_device, fragModule, nullptr);
}
```

- [ ] **Step 2: Añadir draw loop para skinned objects en recordCommandBuffer()**

En `recordCommandBuffer()`, después del draw loop de static meshes (después del `vkCmdEndRenderPass` NO, sino dentro del render pass, después del último `vkCmdDrawIndexed` de static meshes):

```cpp
// Draw skinned meshes con pipeline de stride-80
if (!m_skinnedObjects.empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedGfxPipeline);
    for (auto& obj : m_skinnedObjects) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipelineLayout, 0, 1, &obj.graphicsDescSets[m_currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &obj.transform);
        VkBuffer     vb[]      = {obj.outputVertBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vb, offsets);
        vkCmdBindIndexBuffer(cmd, obj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, obj.indexCount, 1, 0, 0, 0);
    }
}
```

- [ ] **Step 3: Compilar**

```
cmake --build build --config Debug
```

- [ ] **Step 4: Commit**

```bash
git add engine/src/Renderer.cpp
git commit -m "feat(anim): skinned graphics pipeline (stride=80) and draw call"
```

---

## Task 8: Sandbox Integration + Animation Loop

**Files:**
- Modify: `sandbox/src/main.cpp`

**Interfaces:**
- Consumes: `ModelLoader::loadSkinned()`, `renderer.addSkinnedMesh()`, `renderer.updateAnimation()`, `renderer.setSkinnedTransform()`

- [ ] **Step 1: Actualizar sandbox/src/main.cpp**

```cpp
// Después de renderer.init(window, meshes):

// Modelo animado
auto skinnedMesh = DonTopo::ModelLoader::loadSkinned("assets/modelAnimation.fbx");
int animIdx = renderer.addSkinnedMesh(skinnedMesh);

// Posicionar el modelo animado (ajustar según la escena)
renderer.setSkinnedTransform(animIdx,
    glm::translate(glm::mat4(1.0f), glm::vec3(-200.0f, 0.0f, 0.0f)));
```

En el loop principal, antes de `renderer.drawFrame(window)`:

```cpp
renderer.updateAnimation(animIdx, dt);
```

- [ ] **Step 2: Compilar**

```
cmake --build build --config Debug
```

- [ ] **Step 3: Ejecutar y verificar**

- El modelo animado aparece en la escena con movimiento
- Los modelos estáticos existentes siguen renderizando correctamente
- La animación hace loop automáticamente
- Sin validation layers errors (verificar con Vulkan validation layers activas en Debug)

Si el modelo aparece pero en T-pose (sin movimiento): verificar `clip.duration > 0` y `ticksPerSecond > 0`.

Si el modelo aparece deformado: verificar que `aiToGlm()` transpone correctamente la `mOffsetMatrix` de Assimp.

- [ ] **Step 4: Commit final**

```bash
git add sandbox/src/main.cpp
git commit -m "feat(anim): sandbox integration — GPU skeletal animation with compute skinning"
git push
```

---

## Limitaciones conocidas (v1)

- Skinned meshes no proyectan sombras
- Máximo 128 huesos por mesh (límite array local en `bone_hierarchy.comp`)
- Máximo `MAX_SKINNED_OBJECTS = 8` instancias animadas simultáneas
- Una sola animación por mesh (no hay blending entre clips)
- GPU hierarchy traversal es secuencial (un thread) — paralelizable en v2 con multiple dispatches por nivel
