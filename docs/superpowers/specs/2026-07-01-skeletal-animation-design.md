# Skeletal Animation — Design Spec
**Date:** 2026-07-01  
**Engine:** Don_Topo_Engine (C++20 / Vulkan)

---

## 1. Objetivo

Añadir animación esquelética GPU-driven al motor. Un modelo FBX con esqueleto y una animación se renderiza con skinning completamente en GPU mediante compute shaders. El sistema está diseñado para escalar a múltiples instancias animadas.

---

## 2. Arquitectura — Flujo de datos

```
[Load time — CPU, una sola vez]
  ModelLoader::loadSkinned("modelAnimation.fbx")
    → SkinnedMesh { skinnedVertices[], skeleton, clip }
    → Huesos ordenados en topological order (parent siempre antes que hijo)

[Init — GPU, una sola vez por instancia]
  Renderer::addSkinnedMesh()
    → Crea SSBOs: keyframes, boneInfo, localTransform, finalBone, inputVertex
    → Crea outputVertexBuffer (STORAGE_BUFFER | VERTEX_BUFFER)
    → Crea descriptor sets para compute y para graphics

[Per frame]
  renderer.updateAnimation(idx, dt)   ← avanza animTime

  recordComputePass():
    1. bone_eval.comp       (parallel/bone)   → localTransformSSBO
       barrier compute→compute
    2. bone_hierarchy.comp  (1 thread)        → finalBoneSSBO
       barrier compute→compute
    3. skinning.comp        (parallel/vertex) → outputVertexBuffer
       barrier compute→vertex

  recordShadowPass()    ← lee outputVertexBuffer (ya skinneado)
  vkCmdBeginRenderPass() ← graphics pass, outputVertexBuffer como VB
```

---

## 3. Tipos de datos

### CPU

```cpp
// engine/include/DonTopo/SkinnedMesh.h

struct SkinnedVertex {
    glm::vec4  pos;           // xyz + pad
    glm::vec4  normal;        // xyz + pad
    glm::vec4  tangent;       // xyz + pad
    glm::vec4  uv_pad;        // xy + pad
    glm::vec4  color;         // rgb + pad
    glm::ivec4 boneIndices;   // hasta 4 influencias
    glm::vec4  boneWeights;   // suma = 1.0
};

struct BoneKeyframe  { float time; glm::vec3 value; };
struct BoneKeyframeQ { float time; glm::quat value; };

struct BoneChannel {
    int boneIndex;
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
    std::vector<int>                    parentIndex;      // -1 = raíz
    std::vector<glm::mat4>              inverseBindPose;
    std::unordered_map<std::string,int> boneMap;
};

struct SkinnedMesh : Mesh {
    std::vector<SkinnedVertex> skinnedVertices;
    Skeleton                   skeleton;
    AnimationClip              clip;
};
```

### GPU (SSBOs)

| Buffer | Uso | Actualización |
|---|---|---|
| `keyframePosSSBO` | vec4(time,x,y,z)[] por hueso | una vez |
| `keyframeRotSSBO` | vec4(time,x,y,z,w)[] por hueso — quaternion | una vez |
| `keyframeScaleSSBO` | vec4(time,x,y,z)[] por hueso | una vez |
| `boneInfoSSBO` | parentIndex + inverseBindPose + key offsets/counts | una vez |
| `localTransformSSBO` | mat4[] — escrito por bone_eval | cada frame |
| `finalBoneSSBO` | mat4[] — escrito por bone_hierarchy | cada frame |
| `inputVertexSSBO` | SkinnedVertex[] originales | una vez |
| `outputVertexBuffer` | OutputVertex[] stride=80 | escrito por skinning |

**OutputVertex (std430, stride=80):**
```glsl
struct OutputVertex {
    vec4 pos;     // offset  0
    vec4 color;   // offset 16
    vec4 uv;      // offset 32 (xy usados)
    vec4 normal;  // offset 48
    vec4 tangent; // offset 64
};
```

---

## 4. Compute Shaders

### `bone_eval.comp` — interpolación de keyframes

- `local_size_x = 64`, un invocation por hueso
- Push constant: `float time`, `uint boneCount`
- Lee keyframes del hueso, encuentra el intervalo `[k, k+1]` donde `time` cae
- Interpola: `lerp` para pos/scale, `slerp` para rotación (quaternion)
- Construye `mat4` TRS local → escribe en `localTransformSSBO[boneIdx]`

### `bone_hierarchy.comp` — traversal jerárquico

- `local_size_x = 1`, procesa todos los huesos secuencialmente
- Push constant: `uint boneCount`
- Huesos en topological order garantizado por `ModelLoader`
- `world[i] = (parent < 0) ? local[i] : world[parent] * local[i]`
- `final[i] = world[i] * inverseBindPose[i]`
- Escribe en `finalBoneSSBO`

### `skinning.comp` — skinning por vértice

- `local_size_x = 64`, un invocation por vértice
- Push constant: `uint vertexCount`
- `skin = Σ boneWeight[j] * finalBone[boneIdx[j]]` (4 influencias)
- Transforma pos (w=1), normal (w=0), tangent (w=0)
- Escribe `OutputVertex` en `outputVertexBuffer`

---

## 5. Integración en Renderer

### Nuevas estructuras

```cpp
struct SkinnedRenderObject {
    // SSBOs (estáticos)
    VkBuffer       keyframePosBuffer, keyframeRotBuffer, keyframeScaleBuffer;
    VkDeviceMemory keyframePosMemory, keyframeRotMemory, keyframeScaleMemory;
    VkBuffer       boneInfoBuffer;
    VkDeviceMemory boneInfoMemory;
    VkBuffer       inputVertexBuffer;
    VkDeviceMemory inputVertexMemory;

    // SSBOs (por frame)
    VkBuffer       localTransformBuffer;
    VkDeviceMemory localTransformMemory;
    VkBuffer       finalBoneBuffer;
    VkDeviceMemory finalBoneMemory;

    // Output — usado como vertex buffer en graphics pass
    VkBuffer       outputVertexBuffer;
    VkDeviceMemory outputVertexMemory;

    // Index buffer (igual que RenderObject)
    VkBuffer       indexBuffer;
    VkDeviceMemory indexMemory;
    uint32_t       indexCount;
    uint32_t       vertexCount;
    uint32_t       boneCount;

    // Descriptor sets
    VkDescriptorSet computeDescriptorSets[MAX_FRAMES_IN_FLIGHT];
    VkDescriptorSet graphicsDescriptorSets[MAX_FRAMES_IN_FLIGHT];

    // Estado de animación
    float animTime = 0.0f;
    float duration = 0.0f;
    float ticksPerSecond = 24.0f;
    glm::mat4 transform{1.0f};
};
```

### Nuevos miembros en Renderer

```cpp
// Pipelines
VkPipeline       m_boneEvalPipeline       = VK_NULL_HANDLE;
VkPipeline       m_boneHierarchyPipeline  = VK_NULL_HANDLE;
VkPipeline       m_skinningPipeline       = VK_NULL_HANDLE;
VkPipelineLayout m_computePipelineLayout  = VK_NULL_HANDLE;
VkDescriptorSetLayout m_computeDescLayout = VK_NULL_HANDLE;

std::vector<SkinnedRenderObject> m_skinnedObjects;
```

### Nuevos métodos públicos

```cpp
int  addSkinnedMesh(const SkinnedMesh& mesh);
void updateAnimation(int index, float deltaTime);
void setSkinnedTransform(int index, const glm::mat4& t);
```

### Orden de grabación por frame

```
recordComputePass(cmd)     // 3 dispatches + barriers
recordShadowPass(cmd)      // lee outputVertexBuffer skinneado
vkCmdBeginRenderPass(...)  // graphics, idem
```

### Pipeline gráfica de skinned meshes

- Vertex input binding: stride = 80 (OutputVertex)
- Atributos: pos@0, color@16, uv@32, normal@48, tangent@64
- Mismo `triangle.vert` / `triangle.frag` (los locations coinciden)
- Mismo descriptor set layout de graphics (UBO + textures + shadowMap)

---

## 6. ModelLoader — loadSkinned()

```cpp
static SkinnedMesh loadSkinned(const std::string& path);
```

Pasos internos:
1. `aiImporter.ReadFile()` con flags habituales + `aiProcess_LimitBoneWeights`
2. Por cada `aiMesh::mBones[i]`: registrar en `boneMap`, guardar `mOffsetMatrix`
3. Por cada vértice: acumular hasta 4 pares (boneIndex, weight), normalizar pesos
4. Cargar `aiScene::mAnimations[0]` → `AnimationClip`
5. Ordenar huesos en topological order: BFS desde raíz del `aiNode` skeleton
6. Reindexar referencias de bone si el orden cambia

---

## 7. Uso en sandbox

```cpp
// Load
auto skinnedMesh = DonTopo::ModelLoader::loadSkinned("assets/modelAnimation.fbx");
int animIdx = renderer.addSkinnedMesh(skinnedMesh);

// Scene node
auto* animated = root.addChild("animated", animIdx); // meshIndex para transforms

// Loop
renderer.updateAnimation(animIdx, dt);
root.traverse([&](SceneNode* node) {
    if (node->meshIndex >= 0)
        renderer.setSkinnedTransform(node->meshIndex, node->worldTransform);
});
```

---

## 8. Archivos afectados

| Acción | Archivo |
|---|---|
| Nuevo | `engine/include/DonTopo/SkinnedMesh.h` |
| Nuevo | `shaders/bone_eval.comp` |
| Nuevo | `shaders/bone_hierarchy.comp` |
| Nuevo | `shaders/skinning.comp` |
| Modificado | `engine/include/DonTopo/ModelLoader.h` |
| Modificado | `engine/src/ModelLoader.cpp` |
| Modificado | `engine/include/DonTopo/Renderer.h` |
| Modificado | `engine/src/Renderer.cpp` |
| Modificado | `sandbox/src/main.cpp` |

---

## 9. Fuera de alcance (v1)

- Blending entre clips
- IK (Inverse Kinematics)
- Morphing / blend shapes
- Instanced draw para múltiples copias del mismo mesh animado
- GPU-parallel hierarchy traversal (múltiples dispatches por nivel)
