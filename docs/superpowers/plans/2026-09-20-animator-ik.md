# IK del Animator (fila 15, C13) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restricciones de IK (look-at y dos huesos) en el Animator, resueltas en la GPU entre la jerarquía y el skinning.

**Architecture:** El Animator guarda las restricciones y resuelve sus huesos por nombre; `applySkinnedFrame` pasa objetivo y pole al espacio del modelo y los manda al backend; un bloque por personaje (`IkBlock.h`, copia por frame en vuelo) lo lee `bone_ik.comp`, que corrige las transformaciones LOCALES entre una jerarquía "solo mundo" y la jerarquía normal. La matemática se escribe dos veces a propósito: el shader y su réplica en CPU en los tests, como con `bone_eval`/`evalLayeredTrs`.

**Tech Stack:** C++17, glm, GLSL → SPIR-V / HLSL por spirv-cross, ImGui + imgui-node-editor, sol2.

**Spec:** `docs/superpowers/specs/2026-09-20-animator-ik-design.md`

## Global Constraints

- `kMaxIkConstraints = kMaxIkPose = 4`; una restricción sin hueso resuelto, sin objetivo o con peso 0 no se manda.
- Escena sin IK: se guarda idéntica a hoy, byte a byte.
- Push de los `.comp` = `boneCount, vertexCount, rootMotionMode, poseBlockOffset, ikBlockOffset, flags` (24 bytes), espejado en `SkinningPass::Push` y `ComputePush`. `flags` bit 0 = "jerarquía solo mundo".
- Bloque de IK (uints): `[0] count`, `[1..3] relleno`; por restricción en `4 + 16k`: `type, bone, parent, grandParent, weight, targetXYZ, poleXYZ, hasPole, aimXYZ, maxAngle`.
- Binding 11 = bloque de IK (10 es el de pose). `bone_ik` escribe `localXforms` (binding 4) y lee `finalBones` (binding 5).
- Se asume escala uniforme del personaje (limitación documentada).
- Build: `configure.bat` (hay shader nuevo y el CMake los recoge por GLOB) y `build.bat` desde PowerShell; tests desde la raíz del repo. CRLF salvo ficheros ya en LF.
- Commit antes de cada sabotaje; sabotajes de uno en uno.
- Binding Lua nuevo ⇒ `ScriptBindings.cpp` + `LuaApiReference.cpp` + README `## Lua Scripting` + `Scripts/README.md`.
- Trailer: `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.

---

### Task 1: Datos y resolución de la cadena

**Files:** `engine/include/DonTopo/Core/AnimatorComponent.h`, `engine/src/Core/AnimatorComponent.cpp`, `engine/tests/animator_tests.cpp`

**Interfaces — Produces:**

```cpp
enum class IkType { LookAt, TwoBone };
static constexpr int kMaxIkConstraints = 4;
struct IkConstraint
{
    std::string name;
    IkType      type     = IkType::LookAt;
    std::string boneName;
    uint64_t    targetId = 0;
    uint64_t    poleId   = 0;
    float       weight   = 1.0f;
    glm::vec3   aimAxis  = { 0.0f, 0.0f, 1.0f };
    float       maxAngle = 80.0f;
    int boneIndex = -1, parentIndex = -1, grandParentIndex = -1;   // bindClips
};
const std::vector<IkConstraint>& ikConstraints() const;
std::vector<IkConstraint>&       ikConstraintsMutable();
int   addIkConstraint(IkConstraint c);      // índice, -1 si ya hay kMaxIkConstraints
void  removeIkConstraint(int i);
void  setIkWeight(const std::string& nombre, float w);   // [0,1]; nombre inexistente: nada
float ikWeight(const std::string& nombre) const;         // 0 si no existe
void  setIkTarget(const std::string& nombre, uint64_t id);
void  setIkPole(const std::string& nombre, uint64_t id);
```

- [ ] **Step 1: tests**

```cpp
// Esqueleto de brazo: hips -> spine -> shoulder -> elbow -> hand, y head.
static SkinnedMesh makeIkSkeleton()
{
    SkinnedMesh m;
    m.skeleton.names       = { "hips", "spine", "shoulder", "elbow", "hand", "head" };
    m.skeleton.parentIndex = { -1, 0, 1, 2, 3, 1 };
    m.skeleton.inverseBindPose.assign(6, glm::mat4(1.0f));
    for (int i = 0; i < 6; i++) m.skeleton.boneMap[m.skeleton.names[(size_t)i]] = i;
    AnimationClip clip; clip.name = "Idle"; clip.duration = 10.0f; clip.ticksPerSecond = 10.0f;
    m.animationClips.push_back(clip);
    return m;
}

static AnimatorComponent makeIkAnimator()
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Idle"; s.clipName = "Idle"; s.duration = 10.0f; s.ticksPerSecond = 10.0f;
    a.addState(s);
    a.setEntryState(0);
    AnimatorComponent::IkConstraint mirar;
    mirar.name = "mirar"; mirar.type = AnimatorComponent::IkType::LookAt;
    mirar.boneName = "head"; mirar.targetId = 7;
    AnimatorComponent::IkConstraint mano;
    mano.name = "mano"; mano.type = AnimatorComponent::IkType::TwoBone;
    mano.boneName = "hand"; mano.targetId = 8; mano.poleId = 9;
    a.addIkConstraint(mirar);
    a.addIkConstraint(mano);
    return a;
}

static void test_ik_constraint_management()
{
    AnimatorComponent a = makeIkAnimator();
    CHECK(a.ikConstraints().size() == 2u);
    for (int i = 2; i < AnimatorComponent::kMaxIkConstraints; i++)
    {
        AnimatorComponent::IkConstraint c;
        c.name = "x" + std::to_string(i);
        CHECK(a.addIkConstraint(c) == i);
    }
    AnimatorComponent::IkConstraint sobra;
    sobra.name = "sobra";
    CHECK(a.addIkConstraint(sobra) == -1);
    a.setIkWeight("mano", 1.7f);   CHECK(nearlyEqual(a.ikWeight("mano"), 1.0f));
    a.setIkWeight("mano", -1.0f);  CHECK(nearlyEqual(a.ikWeight("mano"), 0.0f));
    a.setIkWeight("mano", 0.25f);  CHECK(nearlyEqual(a.ikWeight("mano"), 0.25f));
    a.setIkWeight("noExiste", 1.0f);
    CHECK(nearlyEqual(a.ikWeight("noExiste"), 0.0f));
    a.setIkTarget("mirar", 42);    CHECK(a.ikConstraints()[0].targetId == 42u);
    a.setIkPole("mano", 43);       CHECK(a.ikConstraints()[1].poleId == 43u);
    a.setIkTarget("noExiste", 99);   // no revienta ni toca nada
    CHECK(a.ikConstraints()[0].targetId == 42u);
    a.removeIkConstraint(0);
    CHECK(a.ikConstraints().size() == (size_t)AnimatorComponent::kMaxIkConstraints - 1);
    CHECK(a.ikConstraints()[0].name == "mano");
}

static void test_ik_chain_resolution()
{
    const SkinnedMesh m = makeIkSkeleton();
    AnimatorComponent a = makeIkAnimator();
    std::vector<std::string> avisos;
    a.bindClips(m, &avisos);
    const auto& mirar = a.ikConstraints()[0];
    CHECK(mirar.boneIndex == 5 && mirar.parentIndex == 1);
    const auto& mano = a.ikConstraints()[1];
    CHECK(mano.boneIndex == 4 && mano.parentIndex == 3 && mano.grandParentIndex == 2);
    CHECK(avisos.empty());

    // Hueso que no existe: aviso y restricción inactiva.
    a.ikConstraintsMutable()[0].boneName = "noExiste";
    // Cadena demasiado corta para TwoBone: hips no tiene ni padre ni abuelo.
    a.ikConstraintsMutable()[1].boneName = "hips";
    avisos.clear();
    a.rebindClips(m, &avisos);
    CHECK(a.ikConstraints()[0].boneIndex == -1);
    CHECK(a.ikConstraints()[1].boneIndex == -1);
    CHECK(avisos.size() == 2u);
    bool nombre = false, cadena = false;
    for (const auto& w : avisos)
    {
        if (w.find("noExiste") != std::string::npos) nombre = true;
        if (w.find("cadena")   != std::string::npos) cadena = true;
    }
    CHECK(nombre && cadena);
}
```

- [ ] **Step 2:** build → falla (no existen los tipos).
- [ ] **Step 3: implementar.** `IkType`, `kMaxIkConstraints` e `IkConstraint` en el header junto a `Layer`; `std::vector<IkConstraint> m_ik;` en los miembros. Gestión:

```cpp
    int AnimatorComponent::addIkConstraint(IkConstraint c)
    {
        if ((int)m_ik.size() >= kMaxIkConstraints) return -1;
        m_ik.push_back(std::move(c));
        return (int)m_ik.size() - 1;
    }

    void AnimatorComponent::removeIkConstraint(int i)
    {
        if (i < 0 || i >= (int)m_ik.size()) return;
        m_ik.erase(m_ik.begin() + i);
    }

    // Los setters por nombre no hacen nada si no existe, como los parámetros.
    AnimatorComponent::IkConstraint* AnimatorComponent::ikPorNombre(const std::string& n)
    {
        for (auto& c : m_ik) if (c.name == n) return &c;
        return nullptr;
    }
```

  `setIkWeight` acota con `std::isfinite(w) ? std::clamp(w, 0.0f, 1.0f) : 0.0f`; `ikWeight` devuelve 0 si no existe. En `rebindClips`, tras resolver clips y máscaras:

```cpp
        // Huesos de las restricciones de IK, por nombre como los clips. En
        // TwoBone el hueso es el EXTREMO y la cadena son sus dos padres: sin
        // ellos la restricción no se puede resolver y se apaga.
        for (auto& c : m_ik)
        {
            c.boneIndex = c.parentIndex = c.grandParentIndex = -1;
            auto it = std::find(mesh.skeleton.names.begin(), mesh.skeleton.names.end(), c.boneName);
            if (it == mesh.skeleton.names.end())
            {
                if (warnings && !c.boneName.empty())
                    warnings->push_back("Animator: la IK '" + c.name + "' usa el hueso '" + c.boneName +
                                        "', que el modelo no tiene");
                continue;
            }
            const int bi = (int)(it - mesh.skeleton.names.begin());
            const auto& padres = mesh.skeleton.parentIndex;
            const int p  = bi < (int)padres.size() ? padres[(size_t)bi] : -1;
            const int gp = (p >= 0 && p < (int)padres.size()) ? padres[(size_t)p] : -1;
            if (c.type == IkType::TwoBone && (p < 0 || gp < 0))
            {
                if (warnings)
                    warnings->push_back("Animator: la IK '" + c.name + "' necesita una cadena de tres "
                                        "huesos y '" + c.boneName + "' no tiene padre y abuelo");
                continue;
            }
            c.boneIndex = bi; c.parentIndex = p; c.grandParentIndex = gp;
        }
```

- [ ] **Step 4:** build; suite entera en verde.
- [ ] **Step 5:** commit `feat(animator): restricciones de IK y resolucion de su cadena`.
- [ ] **Step 6: sabotajes:** S1 `addIkConstraint` sin tope → cae `management`; S2 `setIkWeight` sin acotar → cae `management`; S3 TwoBone sin exigir abuelo → cae `chain_resolution`; S4 `rebindClips` sin resolver la IK → cae `chain_resolution`.

---

### Task 2: Serialización y undo

**Files:** `engine/src/Core/Scene.cpp` (`animatorToJson`, `animatorGraphKey`, `animatorFromJson`), `AnimatorComponent.h/.cpp` (`Graph`, `applyGraph`), `engine/tests/animator_tests.cpp`

**Interfaces — Consumes:** `IkConstraint` (Task 1). **Produces:** `Graph` gana `std::vector<IkConstraint> ik;` y `applyGraph` la restituye (las restricciones son diseño, no ejecución).

- [ ] **Step 1: tests**

```cpp
static void test_ik_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeIkAnimator());
    a->ikConstraintsMutable()[0].aimAxis  = { 0.0f, 1.0f, 0.0f };
    a->ikConstraintsMutable()[0].maxAngle = 55.0f;
    a->setIkWeight("mano", 0.5f);
    go->setAnimator(a);
    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& ik = found->getAnimator()->ikConstraints();
    CHECK(ik.size() == 2u);
    if (ik.size() != 2u) return;
    CHECK(ik[0].name == "mirar" && ik[0].type == AnimatorComponent::IkType::LookAt);
    CHECK(ik[0].boneName == "head" && ik[0].targetId == 7u);
    CHECK(nearlyEqual(ik[0].maxAngle, 55.0f) && nearlyEqual(ik[0].aimAxis.y, 1.0f));
    CHECK(ik[1].type == AnimatorComponent::IkType::TwoBone);
    CHECK(ik[1].poleId == 9u && nearlyEqual(ik[1].weight, 0.5f));
    // Un animator sin IK no escribe la clave.
    AnimatorComponent sinIk;
    CHECK(!animatorToJson(sinIk).contains("ik"));
    CHECK(animatorToJson(*a).contains("ik"));
}

static void test_ik_bad_file_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->setAnimator(std::make_shared<AnimatorComponent>(makeIkAnimator()));
    nlohmann::json j = scene.toJson();
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["ik"][0]["type"] = "loQueSea";
        while (node["animator"]["ik"].size() < 6) node["animator"]["ik"].push_back(node["animator"]["ik"][1]);
    }
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool tipo = false, tope = false;
    for (const auto& w : loaded.lastWarnings())
    {
        if (w.find("loQueSea") != std::string::npos) tipo = true;
        if (w.find("restricciones") != std::string::npos) tope = true;
    }
    CHECK(tipo && tope);
    GameObject* found = loaded.getRoot().children[0].get();
    CHECK(found->getAnimator()->ikConstraints().size() == (size_t)AnimatorComponent::kMaxIkConstraints);
    CHECK(found->getAnimator()->ikConstraints()[0].type == AnimatorComponent::IkType::LookAt);
}

static void test_ik_graph_key_and_apply_graph()
{
    AnimatorComponent a = makeIkAnimator();
    const auto k0 = animatorGraphKey(a);
    a.setIkWeight("mano", 0.25f);
    CHECK(animatorGraphKey(a) != k0);
    const AnimatorComponent::Graph snap = a.graph();
    a.removeIkConstraint(0);
    a.setIkWeight("mano", 1.0f);
    a.applyGraph(snap);
    CHECK(a.ikConstraints().size() == 2u);
    CHECK(nearlyEqual(a.ikWeight("mano"), 0.25f));
}
```

- [ ] **Step 2:** build → falla.
- [ ] **Step 3: implementar.** En `animatorToJson`, tras el bloque de `layers`:

```cpp
        if (!a.ikConstraints().empty())
        {
            auto ik = nlohmann::json::array();
            for (const auto& c : a.ikConstraints())
                ik.push_back({ {"name", c.name},
                               {"type", c.type == AnimatorComponent::IkType::TwoBone ? "twoBone" : "lookAt"},
                               {"bone", c.boneName},
                               {"target", c.targetId},
                               {"pole", c.poleId},
                               {"weight", c.weight},
                               {"aimAxis", nlohmann::json::array({ c.aimAxis.x, c.aimAxis.y, c.aimAxis.z })},
                               {"maxAngle", c.maxAngle} });
            out["ik"] = std::move(ik);
        }
```

  En `animatorFromJson`, tras las capas: por cada elemento (hasta `kMaxIkConstraints`; si hay más, aviso `"Animator: el fichero trae N restricciones de IK; se cargan las 4"`), leer los campos con `readFloat`/`value`, el tipo con aviso si no es `lookAt` ni `twoBone`, y `addIkConstraint`. `animatorGraphKey` no borra nada de `"ik"` (peso, objetivo y ángulo SÍ son edición). `Graph::ik` y `applyGraph`: `m_ik = g.ik;` (no hay estado de ejecución que conservar), y `graph()` lo copia.
- [ ] **Step 4:** build; suite en verde.
- [ ] **Step 5:** commit `feat(animator): serializacion y undo de las restricciones de IK`.
- [ ] **Step 6: sabotajes:** S5 escribir `"ik"` siempre → cae `serialization`; S6 no leer `aimAxis` → cae `serialization`; S7 sin tope al cargar → cae `bad_file_warns`; S8 `applyGraph` sin restituir la IK → cae `graph_key_and_apply_graph`.

---

### Task 3: Los dos resolvedores (réplica en CPU) y el bloque

**Files:**
- Create: `engine/include/DonTopo/Renderer/IkBlock.h`, `engine/include/DonTopo/Core/AnimationIk.h`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces — Produces:**

```cpp
// AnimationIk.h
static constexpr int kMaxIkPose = 4;
struct IkSolve
{
    uint32_t  type = 0;               // 0 LookAt, 1 TwoBone
    int       bone = -1, parent = -1, grandParent = -1;
    float     weight = 0.0f;
    glm::vec3 target{ 0.0f };         // espacio del modelo
    glm::vec3 pole{ 0.0f };
    uint32_t  hasPole = 0;
    glm::vec3 aimAxis{ 0.0f, 0.0f, 1.0f };
    float     maxAngle = 80.0f;       // grados
};
struct AnimationIk { IkSolve solves[kMaxIkPose] = {}; int count = 0; };

// IkBlock.h
constexpr uint32_t kIkBlockSolves = 4;                 // offset de la primera
constexpr uint32_t kIkSolveUints  = 16;
inline uint32_t ikBlockUints() { return kIkBlockSolves + kIkSolveUints * kMaxIkPose; }
void writeIkBlock(const AnimationIk& ik, uint32_t* dst);
```

- [ ] **Step 1: tests** (la réplica de CPU de `bone_ik.comp` y el layout)

```cpp
// ── IK: réplica en CPU de bone_ik.comp ─────────────────────────────────────
// Rotación de un transform de mundo (se asume escala uniforme, como la spec).
static glm::quat rotDe(const glm::mat4& m)
{
    glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
    return glm::normalize(glm::quat_cast(r));
}

// Giro mínimo de a a b (unitarios). Con vectores opuestos, cualquier eje
// perpendicular vale: se elige uno estable.
static glm::quat giroEntre(const glm::vec3& a, const glm::vec3& b)
{
    const float d = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    if (d > 0.9999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.9999f)
    {
        glm::vec3 eje = glm::cross(a, glm::vec3(1, 0, 0));
        if (glm::length(eje) < 1e-4f) eje = glm::cross(a, glm::vec3(0, 1, 0));
        return glm::angleAxis(glm::pi<float>(), glm::normalize(eje));
    }
    return glm::angleAxis(std::acos(d), glm::normalize(glm::cross(a, b)));
}

// Acota el giro a maxAngle grados y lo escala por weight.
static glm::quat acotaYPesa(const glm::quat& q, float maxAngleDeg, float weight)
{
    const glm::quat u = glm::normalize(q);
    float ang = 2.0f * std::acos(glm::clamp(u.w, -1.0f, 1.0f));
    if (ang < 1e-6f) return glm::quat(1, 0, 0, 0);
    const glm::vec3 eje = glm::vec3(u.x, u.y, u.z) / std::sin(ang * 0.5f);
    ang = std::min(ang, glm::radians(glm::clamp(maxAngleDeg, 0.0f, 180.0f)));
    return glm::angleAxis(ang * glm::clamp(weight, 0.0f, 1.0f), glm::normalize(eje));
}

// Locales corregidos por la IK. `mundo` son los transforms de mundo de todos
// los huesos (lo que deja la jerarquía "solo mundo"), `padres` el esqueleto y
// `locales` la entrada/salida. Réplica de bone_ik.comp.
static void resolverIk(const AnimationIk& ik, const std::vector<glm::mat4>& mundo,
                       const std::vector<int>& padres, std::vector<glm::mat4>& locales)
{
    auto mundoRot = [&](int i) { return i >= 0 ? rotDe(mundo[(size_t)i]) : glm::quat(1, 0, 0, 0); };
    auto pos      = [&](int i) { return glm::vec3(mundo[(size_t)i][3]); };
    auto escribeRot = [&](int i, const glm::quat& nuevaMundo) {
        const glm::quat padre = mundoRot(padres[(size_t)i]);
        const glm::quat local = glm::normalize(glm::inverse(padre) * nuevaMundo);
        // Solo cambia la rotación: posición y escala del local se conservan.
        const glm::vec3 t(locales[(size_t)i][3]);
        const glm::vec3 s(glm::length(glm::vec3(locales[(size_t)i][0])),
                          glm::length(glm::vec3(locales[(size_t)i][1])),
                          glm::length(glm::vec3(locales[(size_t)i][2])));
        glm::mat4 m = glm::mat4_cast(local);
        m[0] *= s.x; m[1] *= s.y; m[2] *= s.z;
        m[3] = glm::vec4(t, 1.0f);
        locales[(size_t)i] = m;
    };

    for (int k = 0; k < ik.count; k++)
    {
        const IkSolve& s = ik.solves[k];
        if (s.bone < 0 || s.weight <= 0.0f) continue;
        if (s.type == 0u)
        {
            const glm::vec3 p = pos(s.bone);
            const glm::vec3 d = s.target - p;
            if (glm::length(d) < 1e-5f || glm::length(s.aimAxis) < 1e-5f) continue;
            const glm::quat R    = mundoRot(s.bone);
            const glm::vec3 actual = glm::normalize(R * glm::normalize(s.aimAxis));
            const glm::quat giro = acotaYPesa(giroEntre(actual, glm::normalize(d)), s.maxAngle, s.weight);
            escribeRot(s.bone, glm::normalize(giro * R));
            continue;
        }
        // TwoBone: A abuelo, B padre, C extremo.
        const int A = s.grandParent, B = s.parent, C = s.bone;
        if (A < 0 || B < 0) continue;
        const glm::vec3 pA = pos(A), pB = pos(B), pC = pos(C);
        const float lAB = glm::length(pB - pA), lBC = glm::length(pC - pB);
        if (lAB < 1e-5f || lBC < 1e-5f) continue;
        const glm::vec3 haciaT = s.target - pA;
        if (glm::length(haciaT) < 1e-5f) continue;
        const float lAT = glm::clamp(glm::length(haciaT), 1e-4f, lAB + lBC - 1e-4f);

        // 1) Codo: ángulo actual y deseado por la ley de cosenos.
        const glm::vec3 BA = glm::normalize(pA - pB), BC = glm::normalize(pC - pB);
        const float ang0 = std::acos(glm::clamp(glm::dot(BA, BC), -1.0f, 1.0f));
        const float ang1 = std::acos(glm::clamp((lAB * lAB + lBC * lBC - lAT * lAT) / (2.0f * lAB * lBC),
                                                -1.0f, 1.0f));
        glm::vec3 eje = glm::cross(pC - pA, pB - pA);
        if (glm::length(eje) < 1e-5f) eje = glm::cross(pC - pA, glm::vec3(0, 0, 1));
        if (glm::length(eje) < 1e-5f) eje = glm::vec3(0, 1, 0);
        eje = glm::normalize(eje);
        const glm::quat flex = glm::angleAxis((ang1 - ang0) * glm::clamp(s.weight, 0.0f, 1.0f), eje);

        // 2) Hombro: lleva el extremo (ya flexionado) al objetivo.
        const glm::vec3 pCflex = pB + flex * (pC - pB);
        glm::quat giro = giroEntre(glm::normalize(pCflex - pA), glm::normalize(haciaT));
        giro = glm::slerp(glm::quat(1, 0, 0, 0), giro, glm::clamp(s.weight, 0.0f, 1.0f));

        // 3) Pole: gira alrededor del eje A->objetivo hasta meter el codo en su plano.
        if (s.hasPole != 0u)
        {
            const glm::vec3 ejeT = glm::normalize(haciaT);
            const glm::vec3 pBnuevo = pA + giro * (flex * (pB - pA));
            const glm::vec3 actual = pBnuevo - pA - ejeT * glm::dot(pBnuevo - pA, ejeT);
            const glm::vec3 deseado = s.pole - pA - ejeT * glm::dot(s.pole - pA, ejeT);
            if (glm::length(actual) > 1e-5f && glm::length(deseado) > 1e-5f)
            {
                const glm::quat q = giroEntre(glm::normalize(actual), glm::normalize(deseado));
                giro = glm::normalize(glm::slerp(glm::quat(1, 0, 0, 0), q,
                                                 glm::clamp(s.weight, 0.0f, 1.0f)) * giro);
            }
        }

        const glm::quat RA = mundoRot(A), RB = mundoRot(B);
        const glm::quat RAnuevo = glm::normalize(giro * RA);
        const glm::quat RBnuevo = glm::normalize(giro * flex * RB);
        escribeRot(A, RAnuevo);
        // B se escribe DESPUÉS de A: su local se mide contra el mundo nuevo de A.
        const glm::quat local = glm::normalize(glm::inverse(RAnuevo) * RBnuevo);
        const glm::vec3 t(locales[(size_t)B][3]);
        glm::mat4 mB = glm::mat4_cast(local);
        mB[3] = glm::vec4(t, 1.0f);
        locales[(size_t)B] = mB;
    }
}

// Compone la jerarquía en CPU (lo que hace bone_hierarchy en su pasada 1).
static std::vector<glm::mat4> componer(const std::vector<glm::mat4>& locales, const std::vector<int>& padres)
{
    std::vector<glm::mat4> mundo(locales.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < locales.size(); i++)
        mundo[i] = padres[i] < 0 ? locales[i] : mundo[(size_t)padres[i]] * locales[i];
    return mundo;
}

// Cadena en +Y: A en el origen, B a 2, C a 4. El padre de A es la raíz.
// B se separa un poco en Z para que el plano del codo esté DEFINIDO: con la
// cadena perfectamente recta, cross(C-A, B-A) es nulo y entra el eje de
// reserva, que no es lo que quiere medir el test del pole.
static void cadenaDePrueba(std::vector<glm::mat4>& locales, std::vector<int>& padres)
{
    padres  = { -1, 0, 1, 2 };                       // raíz, A, B, C
    locales = { glm::mat4(1.0f),
                glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0)),
                glm::translate(glm::mat4(1.0f), glm::vec3(0, 2, 0.05f)),
                glm::translate(glm::mat4(1.0f), glm::vec3(0, 2, -0.05f)) };
}

static void test_ik_lookat()
{
    std::vector<int> padres = { -1, 0 };
    std::vector<glm::mat4> locales = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0, 1, 0)) };
    auto mundo = componer(locales, padres);
    AnimationIk ik;
    ik.count = 1;
    ik.solves[0] = { 0u, 1, 0, -1, 1.0f, glm::vec3(5, 1, 0), glm::vec3(0), 0u, glm::vec3(0, 0, 1), 180.0f };
    auto conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    // El eje +Z del hueso acaba apuntando al objetivo.
    glm::vec3 eje = glm::vec3(componer(conIk, padres)[1] * glm::vec4(0, 0, 1, 0));
    CHECK(glm::length(glm::normalize(eje) - glm::vec3(1, 0, 0)) < 1e-4f);

    // Con el límite a 30 grados, gira exactamente 30.
    ik.solves[0].maxAngle = 30.0f;
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    eje = glm::normalize(glm::vec3(componer(conIk, padres)[1] * glm::vec4(0, 0, 1, 0)));
    CHECK(std::fabs(glm::degrees(std::acos(glm::clamp(glm::dot(eje, glm::vec3(0, 0, 1)), -1.0f, 1.0f))) - 30.0f) < 1e-3f);

    // Peso 0 y objetivo encima del hueso: no cambia nada.
    ik.solves[0].maxAngle = 180.0f;
    ik.solves[0].weight   = 0.0f;
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    CHECK(conIk[1] == locales[1]);
    ik.solves[0].weight = 1.0f;
    ik.solves[0].target = glm::vec3(0, 1, 0);
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    CHECK(conIk[1] == locales[1]);
}

static void test_ik_twobone_reaches_target()
{
    std::vector<int> padres; std::vector<glm::mat4> locales;
    cadenaDePrueba(locales, padres);
    const auto mundo = componer(locales, padres);
    AnimationIk ik;
    ik.count = 1;
    ik.solves[0] = { 1u, 3, 2, 1, 1.0f, glm::vec3(2, 2, 0), glm::vec3(0), 0u, glm::vec3(0, 0, 1), 80.0f };
    auto conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    const glm::vec3 extremo = glm::vec3(componer(conIk, padres)[3][3]);
    CHECK(glm::length(extremo - glm::vec3(2, 2, 0)) < 1e-3f);

    // Fuera de alcance: la cadena se estira hacia el objetivo, no se rompe.
    ik.solves[0].target = glm::vec3(0, 40, 0);
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    const auto mundo2 = componer(conIk, padres);
    const glm::vec3 c = glm::vec3(mundo2[3][3]), a = glm::vec3(mundo2[1][3]);
    CHECK(std::fabs(glm::length(c - a) - 4.0f) < 1e-2f);   // la cadena mide 4 salvo el codillo en Z
    CHECK(glm::length(glm::normalize(c - a) - glm::vec3(0, 1, 0)) < 1e-3f);
    CHECK(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));

    // Peso 0: la cadena se queda como estaba.
    ik.solves[0].target = glm::vec3(2, 2, 0);
    ik.solves[0].weight = 0.0f;
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    CHECK(conIk[1] == locales[1] && conIk[2] == locales[2]);

    // Objetivo sobre el propio A: no toca nada y no da NaN.
    ik.solves[0].weight = 1.0f;
    ik.solves[0].target = glm::vec3(0, 0, 0);
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    for (const auto& m : componer(conIk, padres))
        CHECK(std::isfinite(m[3].x) && std::isfinite(m[3].y) && std::isfinite(m[3].z));
}

static void test_ik_twobone_pole_decides_the_plane()
{
    std::vector<int> padres; std::vector<glm::mat4> locales;
    cadenaDePrueba(locales, padres);
    const auto mundo = componer(locales, padres);
    AnimationIk ik;
    ik.count = 1;
    ik.solves[0] = { 1u, 3, 2, 1, 1.0f, glm::vec3(2, 2, 0), glm::vec3(5, 2, 0), 1u, glm::vec3(0, 0, 1), 80.0f };
    auto conZ = locales;
    resolverIk(ik, mundo, padres, conZ);
    const glm::vec3 codoZ = glm::vec3(componer(conZ, padres)[2][3]);
    ik.solves[0].pole = glm::vec3(-5, 2, 0);
    auto conMenosZ = locales;
    resolverIk(ik, mundo, padres, conMenosZ);
    const glm::vec3 codoMenosZ = glm::vec3(componer(conMenosZ, padres)[2][3]);
    // Los dos codos caen a lados opuestos, y el extremo llega igual.
    CHECK(codoZ.x > 0.1f && codoMenosZ.x < -0.1f);
    CHECK(glm::length(glm::vec3(componer(conZ, padres)[3][3]) - glm::vec3(2, 2, 0)) < 1e-3f);
    CHECK(glm::length(glm::vec3(componer(conMenosZ, padres)[3][3]) - glm::vec3(2, 2, 0)) < 1e-3f);
}

static void test_ik_block_layout()
{
    AnimationIk ik;
    ik.count = 2;
    ik.solves[0] = { 0u, 5, 1, -1, 0.5f, glm::vec3(1, 2, 3), glm::vec3(0), 0u, glm::vec3(0, 1, 0), 55.0f };
    ik.solves[1] = { 1u, 4, 3, 2, 1.0f, glm::vec3(7, 8, 9), glm::vec3(4, 5, 6), 1u, glm::vec3(0, 0, 1), 80.0f };
    std::vector<uint32_t> b(ikBlockUints(), 0xDEADBEEFu);
    writeIkBlock(ik, b.data());
    auto f = [&](uint32_t i) { float v; std::memcpy(&v, &b[i], 4); return v; };
    CHECK(b[0] == 2u);
    CHECK(b[4] == 0u && (int)b[5] == 5 && (int)b[6] == 1 && (int)b[7] == -1);
    CHECK(nearlyEqual(f(8), 0.5f) && nearlyEqual(f(9), 1.0f) && nearlyEqual(f(11), 3.0f));
    CHECK(b[15] == 0u && nearlyEqual(f(17), 1.0f) && nearlyEqual(f(19), 55.0f));
    CHECK(b[20] == 1u && (int)b[23] == 2 && b[31] == 1u);
    CHECK(nearlyEqual(f(28), 4.0f) && nearlyEqual(f(35), 80.0f));
    // La restricción 3 queda a cero, no con basura.
    CHECK(b[36] == 0u && b[52] == 0u);
}
```

- [ ] **Step 2:** build → falla.
- [ ] **Step 3: implementar** `AnimationIk.h` y `IkBlock.h`:

```cpp
    inline void writeIkBlock(const AnimationIk& ik, uint32_t* dst)
    {
        const int n = std::clamp(ik.count, 0, kMaxIkPose);
        dst[0] = (uint32_t)n;
        dst[1] = dst[2] = dst[3] = 0u;
        for (int k = 0; k < kMaxIkPose; k++)
        {
            uint32_t* s = dst + kIkBlockSolves + kIkSolveUints * (uint32_t)k;
            if (k >= n) { for (uint32_t i = 0; i < kIkSolveUints; i++) s[i] = 0u; continue; }
            const IkSolve& v = ik.solves[k];
            s[0]  = v.type;
            s[1]  = (uint32_t)v.bone;
            s[2]  = (uint32_t)v.parent;
            s[3]  = (uint32_t)v.grandParent;
            s[4]  = ikBlockBits(v.weight);
            s[5]  = ikBlockBits(v.target.x); s[6] = ikBlockBits(v.target.y); s[7] = ikBlockBits(v.target.z);
            s[8]  = ikBlockBits(v.pole.x);   s[9] = ikBlockBits(v.pole.y);   s[10] = ikBlockBits(v.pole.z);
            s[11] = v.hasPole;
            s[12] = ikBlockBits(v.aimAxis.x); s[13] = ikBlockBits(v.aimAxis.y); s[14] = ikBlockBits(v.aimAxis.z);
            s[15] = ikBlockBits(v.maxAngle);
        }
    }
```

  (`ikBlockBits` igual que `poseBlockBits`: `memcpy` de float a uint32_t. Los índices negativos viajan como uint y el shader los lee con `int(...)`.)
- [ ] **Step 4:** build; suite en verde.
- [ ] **Step 5:** commit `feat(animator): resolvedores de IK (referencia en CPU) y bloque para la GPU`.
- [ ] **Step 6: sabotajes:** S9 `acotaYPesa` ignora `maxAngle` → cae `lookat`; S10 `flex` sin el peso → cae `twobone_reaches_target` (el caso de peso 0); S11 el paso 3 del pole no se aplica → cae `pole_decides_the_plane`; S12 `writeIkBlock` escribe `hasPole` en `s[12]` → cae `block_layout`.

---

### Task 4: GPU (shaders y los dos backends)

**Files:**
- Create: `shaders/bone_ik.comp`
- Modify: `shaders/bone_eval.comp`, `shaders/bone_hierarchy.comp`, `shaders/skinning.comp` (bloque push), `engine/include/DonTopo/Renderer/Passes/SkinningPass.h/.cpp`, `RenderObjects.h`, `Renderer.cpp`, `EditorRenderer.h`, `Renderer.h`, `D3D12Renderer.h/.cpp`

**Interfaces — Produces:** `virtual void setAnimationIk(int index, const AnimationIk& ik) = 0;` en `EditorRenderer` y los dos backends.

- [ ] **Step 1:** push de 24 bytes en los tres `.comp` y en `bone_ik.comp`:

```glsl
layout(push_constant) uniform PC {
    uint boneCount;
    uint vertexCount;
    uint rootMotionMode;
    uint poseBlockOffset;
    uint ikBlockOffset;
    uint flags;            // bit 0: la jerarquía escribe solo mundo (para bone_ik)
} push;
```

  `SkinningPass::Push` y `ComputePush` igual, con `static_assert(... == 24)`.
- [ ] **Step 2:** `bone_hierarchy.comp`: la pasada 2 va dentro de `if ((push.flags & 1u) == 0u) { ... }`.
- [ ] **Step 3:** `bone_ik.comp` nuevo: `layout(local_size_x = 4)`, bindings 3 (BoneInfos, solo lectura), 4 (LocalXforms, escribible), 5 (FinalBones, solo lectura) y 11 (IkBlock, solo lectura). Un hilo por restricción (`gl_LocalInvocationID.x < count`), con la misma matemática que `resolverIk` del Task 3: `rotDe`, `giroEntre`, `acotaYPesa`, LookAt, y TwoBone con sus tres pasos. El padre de A sale de `boneInfos.data[A].parentIndex`. Cada hilo escribe solo los locales de SU cadena; el layout del bloque, el de las Global Constraints.
- [ ] **Step 4:** Vulkan: descriptores 11 → 12 (binding 11 = bloque de IK) y pool `12 * kSetsPerPool`; `SkinnedRenderObject` gana `ikBlockBuffer/Memory/Mapped` (host visible, `MAX_FRAMES * ikBlockUints() * 4`, mapeado) y `AnimationIk ik;`; `Renderer::setAnimationIk` guarda la struct; `SkinningPass` crea el pipeline `m_boneIk` desde `shaders/bone_ik.comp.spv`, escribe el bloque de cada personaje en su copia del frame, y las fases pasan a:

```cpp
    // Los personajes con IK activa necesitan los transforms de MUNDO entre la
    // jerarquía y el skinning, así que su jerarquía corre dos veces: la
    // primera sin la pasada 2 (flag bit 0), bone_ik corrige los locales y la
    // segunda deja las matrices de skinning.
    std::vector<size_t> conIk;
    for (size_t i : activos)
        if (ctx.skinnedObjects[i].ik.count > 0) conIk.push_back(i);

    fase(m_boneEval, activos, 0u, [](const SkinnedRenderObject& o) { return (o.boneCount + 63) / 64; });
    barreraEntreFases();
    if (!conIk.empty())
    {
        fase(m_boneHierarchy, conIk, 1u, [](const SkinnedRenderObject&) { return 1u; });
        barreraEntreFases();
        fase(m_boneIk, conIk, 1u, [](const SkinnedRenderObject&) { return 1u; });
        barreraEntreFases();
    }
    fase(m_boneHierarchy, activos, 0u, [](const SkinnedRenderObject&) { return 1u; });
    barreraEntreFases();
    fase(m_skinning, activos, 0u, [](const SkinnedRenderObject& o) { return (o.vertexCount + 63) / 64; });
```

  (`fase` gana la lista y el valor de `flags`.)
- [ ] **Step 5:** D3D12: `ComputePush` de 24 bytes; root signature de `bone_ik` `{{UAV,4},{SRV,5},{SRV,3},{SRV,11}}` en ese orden de parámetros raíz (1..4) tras las root constants; pipeline desde `shaders/bone_ik.comp.dxil`; buffer UPLOAD del bloque de IK por personaje (`kFrameCount * ikBlockUints() * 4`, mapeado, liberado con los demás); `setAnimationIk`; y el mismo orden de fases que en Vulkan, con la barrera UAV entre ellas.
- [ ] **Step 6:** `configure.bat` (shader nuevo) + `build.bat`; comprobar en `build-ninja/hlsl/bone_ik.comp.hlsl` que están `RWByteAddressBuffer localXforms : register(u4)`, `ByteAddressBuffer finalBones : register(t5)` e `ikBlock : register(t11)`, y que el cbuffer de los cuatro shaders tiene los 6 escalares; suite en verde.
- [ ] **Step 7:** commit `feat(skinning): bone_ik resuelve look-at y dos huesos entre la jerarquia y el skinning`.
- [ ] **Step 8:** runtime Debug `rr10.scene` en los dos backends (20 s, cierre con `CloseMainWindow`): Vulkan sin validación, D3D12 solo `id=1328`. Sin IK en la escena, el camino nuevo no se toca: es la comprobación de que nada se rompió.

---

### Task 5: Host y editor

**Files:** `engine/include/DonTopo/Renderer/SkinnedFrameSync.h`, `engine/src/Editor/AnimatorPanel.cpp`, `engine/include/DonTopo/Editor/AnimatorPanel.h`, `engine/tests/animator_tests.cpp`

**Interfaces — Consumes:** `setAnimationIk` (Task 4), `IkConstraint` (Task 1).

- [ ] **Step 1: test del host** (con el doble que ya existe, que gana `setAnimationIk`)

```cpp
static void test_apply_skinned_frame_passes_ik_in_model_space()
{
    Scene scene("Test");
    auto a = std::make_shared<AnimatorComponent>(makeIkAnimator());
    GameObject* go = makeSkinnedGameObject(scene, a);
    go->setMesh(std::make_shared<SkinnedMesh>(makeIkSkeleton()));
    a->bindClips(*go->getSkinnedMesh(), nullptr);
    GameObject* objetivo = scene.addGameObject("Objetivo");
    objetivo->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
    a->setIkTarget("mirar", objetivo->id);
    a->ikConstraintsMutable()[1].weight = 0.0f;      // la otra no viaja
    // El personaje desplazado: el objetivo llega en ESPACIO DEL MODELO.
    go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(4, 0, 0));
    scene.getRoot().updateWorldTransforms();

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);
    CHECK(r.ik.count == 1);
    if (r.ik.count != 1) return;
    CHECK(r.ik.solves[0].type == 0u && r.ik.solves[0].bone == 5);
    CHECK(glm::length(r.ik.solves[0].target - glm::vec3(6, 0, 0)) < 1e-4f);

    // Sin objetivo resoluble no viaja nada, pero la llamada se hace igual.
    a->setIkTarget("mirar", 999999);
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);
    CHECK(r.ikRecibida && r.ik.count == 0);
}
```

- [ ] **Step 2:** build → falla.
- [ ] **Step 3: implementar** en `applySkinnedFrame`, tras mandar la pose:

```cpp
        // IK: el objetivo y el pole son GameObjects de la escena y el shader los
        // quiere en espacio del modelo. Se resuelven aquí, que es donde hay
        // GameObject; el Animator no conoce la escena.
        AnimationIk ik;
        if (const auto& anim = go.getAnimator())
        {
            const GameObject* raiz = &go;
            while (raiz->parent) raiz = raiz->parent;
            auto porId = [](const GameObject* nodo, uint64_t id) -> const GameObject* {
                if (id == 0) return nullptr;
                std::vector<const GameObject*> pila = { nodo };
                while (!pila.empty())
                {
                    const GameObject* x = pila.back();
                    pila.pop_back();
                    if (x->id == id) return x;
                    for (const auto& h : x->children) pila.push_back(h.get());
                }
                return nullptr;
            };
            const glm::mat4 aModelo = glm::inverse(go.worldTransform);
            for (const auto& c : anim->ikConstraints())
            {
                if (ik.count >= kMaxIkPose) break;
                if (c.boneIndex < 0 || c.weight <= 0.0f) continue;
                const GameObject* objetivo = porId(raiz, c.targetId);
                if (!objetivo) continue;
                IkSolve s;
                s.type        = c.type == AnimatorComponent::IkType::TwoBone ? 1u : 0u;
                s.bone        = c.boneIndex;
                s.parent      = c.parentIndex;
                s.grandParent = c.grandParentIndex;
                s.weight      = c.weight;
                s.target      = glm::vec3(aModelo * glm::vec4(glm::vec3(objetivo->worldTransform[3]), 1.0f));
                s.aimAxis     = c.aimAxis;
                s.maxAngle    = c.maxAngle;
                if (const GameObject* pole = porId(raiz, c.poleId))
                {
                    s.pole    = glm::vec3(aModelo * glm::vec4(glm::vec3(pole->worldTransform[3]), 1.0f));
                    s.hasPole = 1u;
                }
                ik.solves[ik.count++] = s;
            }
        }
        renderer.setAnimationIk(go.skinnedRenderIndex, ik);
```

- [ ] **Step 4:** editor: sección **IK** en `drawLayerBar`... no: función propia `drawIkList(ctx, go)` llamada justo después de `drawLayerBar` en `draw()`, con `PushID("ik")`: lista de restricciones (nombre con `InputText`), botones **+** (desactivado en `kMaxIkConstraints`) y **−**; por restricción, `Combo` de tipo ("Look at", "Two bone"), `Combo` de hueso con `mesh->skeleton.names` (texto rojo si `boneIndex < 0`), `Combo` de objetivo y de pole con los GameObjects de `ctx.scene->getRoot()` recorridos en preorden y "(ninguno)" como primera opción, `SliderFloat` de peso, y solo en Look at `DragFloat3` del eje y `SliderFloat` de ángulo (0..180). Tras cambiar el hueso o el tipo, `anim->rebindClips(*mesh, nullptr)`.
- [ ] **Step 5:** build; suite en verde; commit `feat(editor): restricciones de IK en el panel del Animator`.

---

### Task 6: Lua, documentación y verificación

**Files:** `engine/src/Scripting/ScriptBindings.cpp`, `engine/src/Scripting/LuaApiReference.cpp`, `engine/tests/scripting_tests.cpp`, `README.md`, `Scripts/README.md`, `docs/animation-audit.md`

- [ ] **Step 1: test Lua** junto a `test_animator_lua_layers`: un script que hace `an:SetIkWeight("mano", 0.5)`, lee `an:GetIkWeight("mano")` = 0.5 y `an:GetIkCount()` = 2, `an:SetIkTarget("mirar", e)` y `an:SetIkPole("mano", e)` (comprobando en C++ que los ids son los de `e`), y que `an:GetIkWeight("noExiste")` = 0 y `an:SetIkWeight("noExiste", 1)` no rompe.
- [ ] **Step 2:** bindings `SetIkWeight`, `GetIkWeight`, `SetIkTarget`, `SetIkPole` (los dos últimos toman una entidad y usan su `id`), `GetIkCount`; `ensureFinite` en el peso. `LuaApiReference.cpp`: los cinco en la lista de símbolos y en la tabla de firmas. Test de la referencia: añadir los cinco nombres a `test_animator_lua_new_methods_are_in_the_reference`.
- [ ] **Step 3:** build; suite en verde; commit `feat(lua): restricciones de IK desde Lua`.
- [ ] **Step 4:** `README.md` (sección Animator: qué es cada resolvedor, el pole, el peso, el ángulo máximo, que el objetivo es un GameObject y la limitación de escala uniforme; y la línea de Lua), `Scripts/README.md` (cinco filas nuevas), `docs/animation-audit.md` (C13 EXISTE con el rango de commits; fila 15 anotada: IK hecha, quedan C9, C10 y C14). Commit `docs(animation): IK del Animator (fila 15, C13)`.
- [ ] **Step 5:** verificación manual del usuario en Vulkan y D3D12: la cabeza sigue a un objeto al moverlo y el peso la desvanece; el ángulo máximo la frena; una mano llega a su objetivo y el codo apunta donde dice el pole; un personaje sin IK se ve igual que antes.
