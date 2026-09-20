# Clips de propiedades (fila 15, C14) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un estado del Animator puede reproducir un clip de propiedades autorado en la escena, que escribe transform, luz y material en el GameObject, también en objetos sin esqueleto.

**Architecture:** Una tabla de propiedades (`PropertyTracks.h/.cpp`) declara cómo se lee y se escribe cada propiedad del GameObject; el Animator guarda los clips y dice qué suena con qué peso; `applyAnimatorFrame` (nuevo, dentro de `SkinnedFrameSync.h`) acumula y escribe. Los dos hosts ya llaman a `applySkinnedFrame` para TODOS los objetos, así que el camino sin esqueleto entra sin tocarlos.

**Tech Stack:** C++17, glm, ImGui, nlohmann/json.

**Spec:** `docs/superpowers/specs/2026-09-20-property-clips-design.md`

## Global Constraints

- `kMaxPropertyClips = 16`; cada pista anima UN escalar; keys en segundos; rotaciones en grados (euler XYZ).
- Escena sin clips de propiedades: se guarda idéntica a hoy, byte a byte (`"propertyClips"` y `"propertyClip"` solo si se usan).
- Una propiedad que ninguna muestra anima NO se escribe (una pista de solo Y no pisa X y Z).
- Las máscaras de capa son de huesos y no se aplican a las propiedades.
- El material NO se toca por `editMesh()` (copiaría la malla cada frame): va por `GameObject::materialOverrides` y, cuando el Animator lo anima, el helper empuja `renderer.setObjectMaterialFactors`.
- Build: `configure.bat` (hay ficheros nuevos en CMake) y `build.bat` desde PowerShell; tests desde la raíz del repo. CRLF salvo ficheros ya en LF.
- Commit antes de cada sabotaje; sabotajes de uno en uno.
- Trailer: `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.

---

### Task 1: Tabla de propiedades y muestreo (`PropertyTracks`)

**Files:**
- Create: `engine/include/DonTopo/Core/PropertyTracks.h`, `engine/src/Core/PropertyTracks.cpp`
- Modify: `engine/CMakeLists.txt` (añadir `src/Core/PropertyTracks.cpp` tras `Blend2D.cpp`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces — Produces:**

```cpp
enum class PropertyId {
    PositionX, PositionY, PositionZ,
    RotationX, RotationY, RotationZ,     // grados, euler XYZ
    ScaleX, ScaleY, ScaleZ,
    LightColorR, LightColorG, LightColorB, LightIntensity, LightRange,
    MaterialMetallic, MaterialRoughness,
    Count
};
struct PropertyKey   { float time = 0.0f; float value = 0.0f; };
struct PropertyTrack { PropertyId property = PropertyId::PositionX; std::vector<PropertyKey> keys; bool resolved = false; };
struct PropertyClip  { std::string name; float duration = 1.0f; std::vector<PropertyTrack> tracks; };

const char* propertyName(PropertyId id);            // "position.x", ...
PropertyId  propertyFromName(const char* n);        // Count si no existe
bool        propertyIsRotation(PropertyId id);
// Lineal entre keys; fuera del rango, la key del extremo; sin keys, `actual`.
float samplePropertyTrack(const PropertyTrack& t, float tiempo, float actual);
// Mezcla de varias muestras de la MISMA propiedad, ya ponderadas. Las
// rotaciones van por el camino corto respecto a la primera.
struct PropertyContribution { float value; float weight; };
float blendPropertyValues(PropertyId id, const PropertyContribution* c, int n);
```

- [ ] **Step 1: tests**

```cpp
static void test_property_track_sampling()
{
    PropertyTrack t;
    t.property = PropertyId::PositionY;
    CHECK(nearlyEqual(samplePropertyTrack(t, 0.5f, 7.0f), 7.0f));       // sin keys: el actual
    t.keys = { { 1.0f, 10.0f } };
    CHECK(nearlyEqual(samplePropertyTrack(t, 0.0f, 0.0f), 10.0f));      // una sola key
    CHECK(nearlyEqual(samplePropertyTrack(t, 5.0f, 0.0f), 10.0f));
    t.keys = { { 1.0f, 10.0f }, { 3.0f, 30.0f } };
    CHECK(nearlyEqual(samplePropertyTrack(t, 0.0f, 0.0f), 10.0f));      // antes de la primera
    CHECK(nearlyEqual(samplePropertyTrack(t, 1.0f, 0.0f), 10.0f));      // justo en una key
    CHECK(nearlyEqual(samplePropertyTrack(t, 2.0f, 0.0f), 20.0f));      // lineal
    CHECK(nearlyEqual(samplePropertyTrack(t, 3.0f, 0.0f), 30.0f));
    CHECK(nearlyEqual(samplePropertyTrack(t, 9.0f, 0.0f), 30.0f));      // después de la última
    // Keys desordenadas: el muestreo NO puede depender del orden del fichero.
    t.keys = { { 3.0f, 30.0f }, { 1.0f, 10.0f } };
    CHECK(nearlyEqual(samplePropertyTrack(t, 2.0f, 0.0f), 20.0f));
}

static void test_property_blend_short_path()
{
    const PropertyContribution mitad[2] = { { 0.0f, 0.5f }, { 10.0f, 0.5f } };
    CHECK(nearlyEqual(blendPropertyValues(PropertyId::PositionX, mitad, 2), 5.0f));
    // Rotación: 350 y 10 son 20 grados de diferencia, no 340.
    const PropertyContribution giro[2] = { { 350.0f, 0.5f }, { 10.0f, 0.5f } };
    const float r = blendPropertyValues(PropertyId::RotationY, giro, 2);
    CHECK(std::fabs(std::remainder(r - 0.0f, 360.0f)) < 1e-3f);
    // Pesos que no suman 1 (una capa a media potencia): se renormalizan.
    const PropertyContribution parcial[2] = { { 0.0f, 0.25f }, { 8.0f, 0.25f } };
    CHECK(nearlyEqual(blendPropertyValues(PropertyId::PositionX, parcial, 2), 4.0f));
    CHECK(nearlyEqual(blendPropertyValues(PropertyId::PositionX, mitad, 0), 0.0f));   // sin nada
}

static void test_property_names_round_trip()
{
    for (int i = 0; i < (int)PropertyId::Count; i++)
    {
        const PropertyId id = (PropertyId)i;
        CHECK(propertyFromName(propertyName(id)) == id);
    }
    CHECK(propertyFromName("noExiste") == PropertyId::Count);
    CHECK(propertyIsRotation(PropertyId::RotationZ) && !propertyIsRotation(PropertyId::ScaleX));
}
```

- [ ] **Step 2:** build → falla (no existe el header).
- [ ] **Step 3: implementar.** `PropertyTracks.h` con los structs y las firmas; `.cpp` con la tabla de nombres (`"position.x"`, `"position.y"`, `"position.z"`, `"rotation.x"`, `"rotation.y"`, `"rotation.z"`, `"scale.x"`, `"scale.y"`, `"scale.z"`, `"light.color.r"`, `"light.color.g"`, `"light.color.b"`, `"light.intensity"`, `"light.range"`, `"material.metallic"`, `"material.roughness"`), `propertyIsRotation` (las tres de `Rotation*`), y:

```cpp
    float samplePropertyTrack(const PropertyTrack& t, float tiempo, float actual)
    {
        if (t.keys.empty()) return actual;
        // El fichero (o el panel) puede traerlas en cualquier orden: se busca
        // el tramo por comparación, no por índice.
        const PropertyKey* lo = nullptr;
        const PropertyKey* hi = nullptr;
        for (const auto& k : t.keys)
        {
            if (k.time <= tiempo && (!lo || k.time > lo->time)) lo = &k;
            if (k.time >= tiempo && (!hi || k.time < hi->time)) hi = &k;
        }
        if (!lo) return hi->value;      // todas por delante: la primera
        if (!hi) return lo->value;      // todas por detrás: la última
        const float span = hi->time - lo->time;
        if (span <= 0.0f) return hi->value;
        return glm::mix(lo->value, hi->value, (tiempo - lo->time) / span);
    }

    float blendPropertyValues(PropertyId id, const PropertyContribution* c, int n)
    {
        float total = 0.0f;
        for (int i = 0; i < n; i++) total += c[i].weight;
        if (n <= 0 || total <= 0.0f) return 0.0f;
        if (!propertyIsRotation(id))
        {
            float v = 0.0f;
            for (int i = 0; i < n; i++) v += c[i].value * c[i].weight;
            return v / total;
        }
        // Ángulos: se mezcla la DIFERENCIA con la primera, normalizada a
        // [-180, 180]. Sin esto, 350 y 10 darían 180 en vez de 0.
        const float base = c[0].value;
        float delta = 0.0f;
        for (int i = 0; i < n; i++)
            delta += std::remainder(c[i].value - base, 360.0f) * c[i].weight;
        return base + delta / total;
    }
```

- [ ] **Step 4:** `configure.bat` + `build.bat`; suite entera en verde.
- [ ] **Step 5:** commit `feat(animator): tabla de propiedades y muestreo de pistas`.
- [ ] **Step 6: sabotajes:** S1 `samplePropertyTrack` devuelve siempre `keys[0].value` → cae `sampling`; S2 el tramo se busca por índice (`keys[0]`/`keys[1]`) → cae `sampling` (las desordenadas); S3 `blendPropertyValues` sin el camino corto (mezcla lineal de ángulos) → cae `short_path`; S4 sin renormalizar por `total` → cae `short_path` (el caso parcial).

---

### Task 2: Acceso a las propiedades del GameObject

**Files:**
- Modify: `engine/include/DonTopo/Core/PropertyTracks.h`, `engine/src/Core/PropertyTracks.cpp`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces — Produces:**

```cpp
class GameObject;
// Lectura/escritura de una propiedad en un GameObject. `available` dice si el
// objeto tiene el componente que hace falta (luz, malla).
bool  propertyAvailable(const GameObject& go, PropertyId id);
float propertyGet(const GameObject& go, PropertyId id);
// Escribe SOLO las propiedades marcadas en `escritas` (una por PropertyId).
// El transform se descompone y se recompone UNA vez, no por propiedad.
void  propertyApply(GameObject& go, const bool* escritas, const float* valores);
```

- [ ] **Step 1: tests**

```cpp
static void test_property_get_set_transform()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionY), 2.0f));
    CHECK(propertyAvailable(*go, PropertyId::PositionY));

    bool  escritas[(int)PropertyId::Count] = {};
    float valores [(int)PropertyId::Count] = {};
    escritas[(int)PropertyId::PositionY] = true; valores[(int)PropertyId::PositionY] = 9.0f;
    escritas[(int)PropertyId::RotationZ] = true; valores[(int)PropertyId::RotationZ] = 90.0f;
    escritas[(int)PropertyId::ScaleX]    = true; valores[(int)PropertyId::ScaleX]    = 2.0f;
    propertyApply(*go, escritas, valores);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionY), 9.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionX), 1.0f));   // lo no escrito NO cambia
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionZ), 3.0f));
    CHECK(std::fabs(std::remainder(propertyGet(*go, PropertyId::RotationZ) - 90.0f, 360.0f)) < 1e-3f);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::ScaleX), 2.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::ScaleY), 1.0f));
    // El eje X del transform ya rotado 90 grados en Z apunta a +Y.
    CHECK(glm::length(glm::normalize(glm::vec3(go->localTransform[0])) - glm::vec3(0, 1, 0)) < 1e-4f);
}

static void test_property_light_and_material()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Farola");
    CHECK(!propertyAvailable(*go, PropertyId::LightIntensity));   // sin LightComponent
    go->setLight(std::make_shared<LightComponent>());
    go->getLight()->setIntensity(2.0f);
    go->getLight()->setColor(glm::vec3(1, 0, 0));
    CHECK(propertyAvailable(*go, PropertyId::LightIntensity));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightIntensity), 2.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightColorG), 0.0f));

    bool  escritas[(int)PropertyId::Count] = {};
    float valores [(int)PropertyId::Count] = {};
    escritas[(int)PropertyId::LightIntensity] = true; valores[(int)PropertyId::LightIntensity] = 5.0f;
    escritas[(int)PropertyId::LightColorG]    = true; valores[(int)PropertyId::LightColorG]    = 1.0f;
    escritas[(int)PropertyId::MaterialMetallic] = true; valores[(int)PropertyId::MaterialMetallic] = 0.75f;
    propertyApply(*go, escritas, valores);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightIntensity), 5.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightColorG), 1.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightColorR), 1.0f));   // lo no escrito sigue
    // El material va por el override del OBJETO, no por la malla: animar no
    // puede copiar la malla cada frame (editMesh copia si está compartida).
    CHECK(!go->materialOverrides.empty());
    if (!go->materialOverrides.empty())
        CHECK(nearlyEqual(go->materialOverrides[0].metallic, 0.75f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::MaterialMetallic), 0.75f));
}
```

- [ ] **Step 2:** build → falla.
- [ ] **Step 3: implementar.** En el `.cpp` (que ya puede incluir `GameObject.h` y `LightComponent.h`):
  - `propertyAvailable`: transform siempre; luz si `go.getLight()`; material siempre (el override vive en el GameObject, exista o no malla).
  - `propertyGet`: transform por `glm::decompose` del `localTransform` (euler con `glm::eulerAngles(quat)` pasado a grados); luz por sus getters; material del override si tiene valor (`>= 0`), si no del material de la malla (`go.getMesh()->material`), y 0 sin malla.
  - `propertyApply`: si hay alguna de transform, descompone una vez, sustituye las escritas y recompone (`translate * mat4_cast(quat(radians(euler))) * scale`); luz por setters; material escribiendo en `materialOverrides[0]` (creándolo si no hay, con `index = 0`).
  - Un `static_assert` de que la tabla de nombres tiene `(int)PropertyId::Count` entradas.
- [ ] **Step 4:** build; suite en verde.
- [ ] **Step 5:** commit `feat(animator): lectura y escritura de las propiedades animables`.
- [ ] **Step 6: sabotajes:** S5 `propertyApply` escribe todas las propiedades, no solo las marcadas → cae `transform` (PositionX cambia); S6 el transform se recompone sin la rotación → cae `transform`; S7 el material va por `editMesh()` en vez del override → cae `light_and_material`; S8 `propertyAvailable` de la luz siempre true → cae `light_and_material`.

---

### Task 3: Clips de propiedades en el Animator

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h`, `engine/src/Core/AnimatorComponent.cpp`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces — Consumes:** `PropertyClip`, `PropertyId` (Task 1). **Produces:**

```cpp
static constexpr int kMaxPropertyClips = 16;
const std::vector<PropertyClip>& propertyClips() const;
std::vector<PropertyClip>&       propertyClipsMutable();
int  addPropertyClip(PropertyClip c);      // índice, -1 si ya hay kMaxPropertyClips
void removePropertyClip(int i);
// State gana: std::string propertyClipName; int propertyClipIndex = -1;
// Resuelve propertyClipName de cada estado y el `resolved` de cada pista
// contra el objeto (qué componentes tiene). Sin GameObject, solo lo primero.
void bindProperties(const GameObject* go, std::vector<std::string>* warnings);
// Lo que suena este frame, con el tiempo YA en segundos y el peso de la capa.
struct PropertySampleRef { int clip; float time; float weight; };
int  propertySamples(PropertySampleRef* out, int max) const;
```

- [ ] **Step 1: tests**

```cpp
// Puerta: Cerrada (clip "cerrar") -> Abierta (clip "abrir") por trigger.
static AnimatorComponent makePuerta()
{
    AnimatorComponent a;
    PropertyClip cerrar;
    cerrar.name = "cerrar"; cerrar.duration = 1.0f;
    PropertyTrack tc; tc.property = PropertyId::PositionY;
    tc.keys = { { 0.0f, 0.0f }, { 1.0f, 0.0f } };
    cerrar.tracks.push_back(tc);
    PropertyClip abrir;
    abrir.name = "abrir"; abrir.duration = 2.0f;
    PropertyTrack ta; ta.property = PropertyId::PositionY;
    ta.keys = { { 0.0f, 0.0f }, { 2.0f, 4.0f } };
    abrir.tracks.push_back(ta);
    a.addPropertyClip(cerrar);
    a.addPropertyClip(abrir);
    AnimatorComponent::State s;
    s.name = "Cerrada"; s.propertyClipName = "cerrar";
    a.addState(s);
    s.name = "Abierta"; s.propertyClipName = "abrir";
    a.addState(s);
    a.setEntryState(0);
    a.addParameter("abre", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.5f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "abre";
    t.conditions.push_back(c);
    a.addTransition(t);
    return a;
}

static void test_property_clip_binding_and_duration()
{
    AnimatorComponent a = makePuerta();
    std::vector<std::string> avisos;
    a.bindProperties(nullptr, &avisos);
    CHECK(avisos.empty());
    CHECK(a.states()[1].propertyClipIndex == 1);
    // Sin clip de malla, la duración del estado sale del clip de propiedades.
    CHECK(a.states()[1].ticksPerSecond > 0.0f);
    CHECK(nearlyEqual(a.states()[1].duration / a.states()[1].ticksPerSecond, 2.0f));
    // Un nombre que no existe avisa y deja el estado sin clip.
    a.statesMutable()[0].propertyClipName = "noExiste";
    avisos.clear();
    a.bindProperties(nullptr, &avisos);
    CHECK(a.states()[0].propertyClipIndex == -1);
    CHECK(avisos.size() == 1u);
    CHECK(a.addPropertyClip(PropertyClip{}) >= 0);
    while (a.propertyClips().size() < (size_t)AnimatorComponent::kMaxPropertyClips)
        CHECK(a.addPropertyClip(PropertyClip{}) >= 0);
    CHECK(a.addPropertyClip(PropertyClip{}) == -1);
}

static void test_property_clip_tracks_resolved_against_object()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Farola");
    AnimatorComponent a;
    PropertyClip parpadeo;
    parpadeo.name = "parpadeo"; parpadeo.duration = 1.0f;
    PropertyTrack luz; luz.property = PropertyId::LightIntensity;
    luz.keys = { { 0.0f, 0.0f }, { 1.0f, 5.0f } };
    PropertyTrack pos; pos.property = PropertyId::PositionY;
    pos.keys = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    parpadeo.tracks = { luz, pos };
    a.addPropertyClip(parpadeo);
    std::vector<std::string> avisos;
    a.bindProperties(go, &avisos);
    CHECK(!a.propertyClips()[0].tracks[0].resolved);   // no hay LightComponent
    CHECK(a.propertyClips()[0].tracks[1].resolved);
    CHECK(avisos.size() == 1u);
    go->setLight(std::make_shared<LightComponent>());
    avisos.clear();
    a.bindProperties(go, &avisos);
    CHECK(a.propertyClips()[0].tracks[0].resolved);
    CHECK(avisos.empty());
}

static void test_property_samples_follow_the_graph()
{
    AnimatorComponent a = makePuerta();
    a.bindProperties(nullptr, nullptr);
    a.reset();
    a.update(0.1f, true);
    AnimatorComponent::PropertySampleRef m[8];
    int n = a.propertySamples(m, 8);
    CHECK(n == 1);
    CHECK(m[0].clip == 0 && nearlyEqual(m[0].weight, 1.0f));
    CHECK(nearlyEqual(m[0].time, 0.1f));          // tiempo EN SEGUNDOS
    // En el cross-fade suenan los dos, con pesos que suman 1. Hacen falta DOS
    // updates: en el que dispara la transición el fade va por 0, así que el
    // estado nuevo entra con peso 0 y todavía no aporta muestra.
    a.setTrigger("abre");
    a.update(0.016f, true);
    a.update(0.25f, true);
    n = a.propertySamples(m, 8);
    CHECK(n == 2);
    CHECK(nearlyEqual(m[0].weight + m[1].weight, 1.0f));
    CHECK(m[0].weight > 0.0f && m[1].weight > 0.0f);
    CHECK(m[0].clip != m[1].clip);
    // Un estado sin clip de propiedades no aporta muestra.
    a.statesMutable()[1].propertyClipName.clear();
    a.bindProperties(nullptr, nullptr);
    a.reset();
    a.update(0.016f, true);
    a.setTrigger("abre");
    a.update(0.016f, true);
    a.update(0.25f, true);      // a mitad del fade: el estado nuevo YA pesa
    n = a.propertySamples(m, 8);
    CHECK(n == 1 && m[0].clip == 0);
}
```

- [ ] **Step 2:** build → falla.
- [ ] **Step 3: implementar.** `m_propertyClips` en el componente con su gestión (tope `kMaxPropertyClips`); `State::propertyClipName` / `propertyClipIndex`; `bindProperties`:

```cpp
    void AnimatorComponent::bindProperties(const GameObject* go, std::vector<std::string>* warnings)
    {
        for (auto& L : m_layers)
            for (auto& st : L.states)
            {
                st.propertyClipIndex = -1;
                if (st.propertyClipName.empty()) continue;
                for (int i = 0; i < (int)m_propertyClips.size(); i++)
                    if (m_propertyClips[(size_t)i].name == st.propertyClipName) { st.propertyClipIndex = i; break; }
                if (st.propertyClipIndex < 0 && warnings)
                    warnings->push_back("Animator: el estado '" + st.name + "' referencia el clip de propiedades '" +
                                        st.propertyClipName + "', que no existe");
                // Sin clip de malla resuelto, el reloj del estado sale del clip
                // de propiedades: el Animator cuenta en ticks y así no hace
                // falta un caso especial en advanceClock.
                if (st.propertyClipIndex >= 0 && st.clipIndex < 0)
                {
                    st.ticksPerSecond = 30.0f;
                    st.duration       = m_propertyClips[(size_t)st.propertyClipIndex].duration * st.ticksPerSecond;
                }
            }
        for (auto& clip : m_propertyClips)
            for (auto& tr : clip.tracks)
            {
                tr.resolved = !go || propertyAvailable(*go, tr.property);
                if (!tr.resolved && warnings)
                    warnings->push_back("Animator: la pista '" + std::string(propertyName(tr.property)) +
                                        "' del clip '" + clip.name + "' necesita un componente que el objeto no tiene");
            }
    }
```

  y `propertySamples`, que recorre las mismas muestras que `pose()` (capa por capa, con el peso del fade y de la capa) pero quedándose con el `propertyClipIndex` del estado y convirtiendo su reloj a segundos (`animTime / ticksPerSecond`, con `ticksPerSecond > 0`; si no, 0).
- [ ] **Step 4:** build; suite en verde.
- [ ] **Step 5:** commit `feat(animator): clips de propiedades en el grafo`.
- [ ] **Step 6: sabotajes:** S9 `bindProperties` no fija la duración del estado → cae `binding_and_duration`; S10 `propertySamples` devuelve el tiempo en ticks → cae `follow_the_graph`; S11 un estado sin clip de propiedades igualmente emite muestra → cae `follow_the_graph`; S12 `tr.resolved` siempre true → cae `tracks_resolved_against_object`.

---

### Task 4: Aplicación por frame (el Animator sin esqueleto)

**Files:**
- Modify: `engine/include/DonTopo/Renderer/SkinnedFrameSync.h`, `engine/src/Editor/PropertiesPanel.cpp:9207-9212` (el gate del Add), `engine/src/Editor/Command.cpp:550,640` y `engine/src/Core/Scene.cpp` (llamar a `bindProperties` donde hoy se llama a `bindClips`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces — Produces:**

```cpp
// Avanza el grafo y escribe las propiedades. Vale para cualquier GameObject con
// Animator, tenga o no malla con esqueleto. Devuelve true si escribió alguna
// propiedad de material (el llamante tiene que empujarla al backend).
bool applyAnimatorFrame(GameObject& go, float dt, bool evaluateTransitions);   // inline, sin renderer
```

- [ ] **Step 1: tests**

```cpp
static void test_property_clips_drive_a_non_skinned_object()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    auto a = std::make_shared<AnimatorComponent>(makePuerta());
    go->setAnimator(a);
    a->bindProperties(go, nullptr);
    a->reset();
    CHECK(go->skinnedRenderIndex < 0);           // no es skinned: antes no se animaba

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.5f, /*evaluateTransitions=*/true);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionY), 0.0f));   // "cerrar" es plano
    a->setTrigger("abre");
    applySkinnedFrame(*go, r, 0.016f, true);     // entra en "Abierta"
    applySkinnedFrame(*go, r, 1.0f, true);       // 1 s de un clip de 2 s: mitad del recorrido
    const float y = propertyGet(*go, PropertyId::PositionY);
    CHECK(y > 0.5f && y < 4.0f);
    // En Edit el grafo no transiciona, pero el tiempo del estado corre.
    Scene scene2("Test");
    GameObject* go2 = scene2.addGameObject("Puerta");
    auto b = std::make_shared<AnimatorComponent>(makePuerta());
    go2->setAnimator(b);
    b->bindProperties(go2, nullptr);
    b->reset();
    b->setTrigger("abre");
    applySkinnedFrame(*go2, r, 0.5f, /*evaluateTransitions=*/false);
    CHECK(b->currentStateName() == "Cerrada");
}

static void test_property_clips_material_goes_to_the_backend()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    go->staticRenderIndex = 3;
    auto a = std::make_shared<AnimatorComponent>();
    PropertyClip brillo;
    brillo.name = "brillo"; brillo.duration = 1.0f;
    PropertyTrack tr; tr.property = PropertyId::MaterialMetallic;
    tr.keys = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    brillo.tracks.push_back(tr);
    a->addPropertyClip(brillo);
    AnimatorComponent::State s;
    s.name = "Brilla"; s.propertyClipName = "brillo";
    a->addState(s);
    a->setEntryState(0);
    go->setAnimator(a);
    a->bindProperties(go, nullptr);
    a->reset();

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.5f, true);
    CHECK(r.factoresRecibidos);
    CHECK(r.factoresIndice == 3u);
    CHECK(r.factorMetallic > 0.1f);
    CHECK(nearlyEqual(r.factorMetallic, propertyGet(*go, PropertyId::MaterialMetallic)));
}
```

  (`SkinnedRendererDoble` gana `bool factoresRecibidos = false; size_t factoresIndice = 0; float factorMetallic = -1.0f, factorRoughness = -1.0f;` y
  `void setObjectMaterialFactors(size_t i, float m, float r) { orden.push_back("factores"); factoresRecibidos = true; factoresIndice = i; factorMetallic = m; factorRoughness = r; }`.)

- [ ] **Step 2:** build → falla.
- [ ] **Step 3: implementar** en `SkinnedFrameSync.h`:

```cpp
    // Lo del Animator que NO depende de la malla con esqueleto: avanzar el
    // grafo, el root motion y las propiedades. Antes esto vivía dentro de
    // applySkinnedFrame, que salía en cuanto el objeto no tenía índice skinned,
    // así que un objeto sin esqueleto no animaba nada.
    inline bool applyAnimatorFrame(GameObject& go, float dt, bool evaluateTransitions)
    {
        const auto& anim = go.getAnimator();
        if (!anim) return false;
        anim->update(dt, evaluateTransitions);
        if (!anim->rootMotionSamples().empty())
            if (const SkinnedMesh* sk = go.getSkinnedMesh())
                applyRootMotion(go, rootMotionDelta(*sk, *anim), dt);

        AnimatorComponent::PropertySampleRef muestras[kMaxLayersPose * kMaxPoseSamplesPerLayer];
        const int n = anim->propertySamples(muestras, (int)std::size(muestras));
        if (n == 0) return false;

        bool  escritas[(int)PropertyId::Count] = {};
        float valores [(int)PropertyId::Count] = {};
        for (int p = 0; p < (int)PropertyId::Count; p++)
        {
            const PropertyId id = (PropertyId)p;
            PropertyContribution aporta[kMaxLayersPose * kMaxPoseSamplesPerLayer];
            int m = 0;
            for (int k = 0; k < n; k++)
            {
                const PropertyClip& clip = anim->propertyClips()[(size_t)muestras[k].clip];
                for (const auto& tr : clip.tracks)
                {
                    if (tr.property != id || !tr.resolved || tr.keys.empty()) continue;
                    aporta[m++] = { samplePropertyTrack(tr, muestras[k].time, propertyGet(go, id)),
                                    muestras[k].weight };
                    break;   // una pista por propiedad y clip: la primera manda
                }
            }
            // Una propiedad que NADIE anima no se toca: así una pista de solo Y
            // no pisa la X y la Z del objeto.
            if (m == 0) continue;
            escritas[p] = true;
            valores[p]  = blendPropertyValues(id, aporta, m);
        }
        propertyApply(go, escritas, valores);
        return escritas[(int)PropertyId::MaterialMetallic] || escritas[(int)PropertyId::MaterialRoughness];
    }
```

  y en `applySkinnedFrame`, al principio: `const bool material = applyAnimatorFrame(go, dt, evaluateTransitions);` seguido de, si `material && go.staticRenderIndex >= 0`, `renderer.setObjectMaterialFactors((size_t)go.staticRenderIndex, propertyGet(go, PropertyId::MaterialMetallic), propertyGet(go, PropertyId::MaterialRoughness));`. Después, `if (go.skinnedRenderIndex < 0) return;` y el resto de hoy **sin** el `anim->update` ni el root motion (que ya los hizo `applyAnimatorFrame`).
  Además: el gate de `Properties → Add → Animator` pasa a `const bool canAnimate = true;` (se quita `isSkinned()`, y el tooltip explica que sin malla con esqueleto solo valen los clips de propiedades), y donde hoy se llama a `rebindClips` (Command.cpp y Scene.cpp) se llama también a `bindProperties(go, &warnings)`.
- [ ] **Step 4:** build; suite en verde.
- [ ] **Step 5:** commit `feat(animator): el grafo corre y escribe propiedades tambien sin esqueleto`.
- [ ] **Step 6: sabotajes:** S13 `applyAnimatorFrame` escribe las propiedades que nadie anima (quitar el `continue`) → cae `non_skinned_object` (X y Z se van a 0); S14 `applySkinnedFrame` vuelve a salir antes si no hay índice skinned → cae `non_skinned_object`; S15 no se empujan los factores al backend → cae `material_goes_to_the_backend`.

---

### Task 5: Serialización y undo

**Files:** `engine/src/Core/Scene.cpp` (`animatorToJson`, `animatorFromJson`), `AnimatorComponent.h/.cpp` (`Graph`, `applyGraph`), `engine/tests/animator_tests.cpp`

- [ ] **Step 1: tests**

```cpp
static void test_property_clips_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    const uint64_t id = go->id;
    go->setAnimator(std::make_shared<AnimatorComponent>(makePuerta()));
    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& anim = *found->getAnimator();
    CHECK(anim.propertyClips().size() == 2u);
    if (anim.propertyClips().size() != 2u) return;
    CHECK(anim.propertyClips()[1].name == "abrir");
    CHECK(nearlyEqual(anim.propertyClips()[1].duration, 2.0f));
    CHECK(anim.propertyClips()[1].tracks.size() == 1u);
    CHECK(anim.propertyClips()[1].tracks[0].property == PropertyId::PositionY);
    CHECK(anim.propertyClips()[1].tracks[0].keys.size() == 2u);
    CHECK(nearlyEqual(anim.propertyClips()[1].tracks[0].keys[1].value, 4.0f));
    CHECK(anim.states()[1].propertyClipName == "abrir");
    // Un Animator sin clips de propiedades no escribe ninguna clave nueva.
    AnimatorComponent vacio;
    const std::string texto = animatorToJson(vacio).dump();
    CHECK(texto.find("propertyClip") == std::string::npos);
}

static void test_property_clips_bad_file_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    go->setAnimator(std::make_shared<AnimatorComponent>(makePuerta()));
    nlohmann::json j = scene.toJson();
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["propertyClips"][0]["tracks"][0]["property"] = "noExiste";
        node["animator"]["propertyClips"][1]["duration"] = 0.0f;
        while (node["animator"]["propertyClips"].size() < 20)
            node["animator"]["propertyClips"].push_back(node["animator"]["propertyClips"][0]);
    }
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool prop = false, dur = false, tope = false;
    for (const auto& w : loaded.lastWarnings())
    {
        if (w.find("noExiste") != std::string::npos) prop = true;
        if (w.find("duración") != std::string::npos) dur = true;
        if (w.find("clips de propiedades") != std::string::npos) tope = true;
    }
    CHECK(prop && dur && tope);
    GameObject* found = loaded.getRoot().children[0].get();
    const auto& anim = *found->getAnimator();
    CHECK(anim.propertyClips().size() == (size_t)AnimatorComponent::kMaxPropertyClips);
    CHECK(anim.propertyClips()[0].tracks.empty());              // la pista mala se descarta
    CHECK(anim.propertyClips()[1].duration > 0.0f);             // la duración se acota
}

static void test_property_clips_apply_graph_restores()
{
    AnimatorComponent a = makePuerta();
    const AnimatorComponent::Graph snap = a.graph();
    a.propertyClipsMutable()[1].duration = 9.0f;
    a.removePropertyClip(0);
    a.applyGraph(snap);
    CHECK(a.propertyClips().size() == 2u);
    if (a.propertyClips().size() == 2u) CHECK(nearlyEqual(a.propertyClips()[1].duration, 2.0f));
}
```

- [ ] **Step 2:** build → falla.
- [ ] **Step 3: implementar.** En `animatorToJson`, tras el bloque de `"ik"`: si `!a.propertyClips().empty()`, escribir `"propertyClips"` con `{name, duration, tracks:[{property, keys:[{t, v}]}]}`; y en cada estado, `sj["propertyClip"] = s.propertyClipName;` **solo si no está vacío**. En `animatorFromJson`: leer `"propertyClips"` (tope `kMaxPropertyClips` con aviso; `duration` con `readFloat` y, si `<= 0`, se acota a `0.001f` y avisa `"...: duración no positiva, se acota"`; `property` por `propertyFromName`, y si es `Count` avisa y descarta la pista; keys con `readFloat`), y en cada estado `st.propertyClipName = s.value("propertyClip", std::string())`. `Graph` gana `std::vector<PropertyClip> propertyClips;`, `graph()` la copia y `applyGraph` la restituye (`m_propertyClips = g.propertyClips;`).
- [ ] **Step 4:** build; suite en verde.
- [ ] **Step 5:** commit `feat(animator): serializacion y undo de los clips de propiedades`.
- [ ] **Step 6: sabotajes:** S16 escribir `"propertyClips"` siempre → cae `serialization`; S17 no leer las keys → cae `serialization`; S18 sin tope al cargar → cae `bad_file_warns`; S19 `applyGraph` no restituye los clips → cae `apply_graph_restores`.

---

### Task 6: Editor, documentación y verificación

**Files:** `engine/src/Editor/AnimatorPanel.cpp`, `engine/include/DonTopo/Editor/AnimatorPanel.h`, `README.md`, `docs/animation-audit.md`

- [ ] **Step 1:** sección **Property Clips** en la columna del panel (`drawPropertyClips(ctx, go)`, llamada tras `drawIkList`), desplegable con `PushID("propclips")`: lista de clips con nombre (`InputText`), duración (`DragFloat`, mínimo 0.001), **+** (desactivado en `kMaxPropertyClips`) y **−**; dentro del clip, sus pistas con un `Combo` de propiedad (los `propertyName` de la tabla, en rojo si `!resolved`), **+ pista** y **−**; y dentro de la pista, sus keys en filas de `DragFloat` (tiempo y valor) con **+ key** y **x**. Tras cambiar nombre, propiedad o añadir/quitar algo: `anim->bindProperties(go, nullptr)`.
- [ ] **Step 2:** en el nodo del estado (`drawGraph`), un `Combo` **Property clip** con "(ninguno)" y los clips del componente, que escribe `propertyClipName` y llama a `bindProperties`.
- [ ] **Step 3:** build; suite en verde; commit `feat(editor): clips de propiedades en el panel del Animator`.
- [ ] **Step 4:** `README.md` (sección Animator: qué es un clip de propiedades, las 16 propiedades, que las keys van en segundos y la interpolación es lineal, que el Animator ya no exige malla con esqueleto, y que las máscaras de capa no aplican a las propiedades); `docs/animation-audit.md` (C14 EXISTE con el rango de commits; fila 15 anotada: quedan C9 y C10). Commit `docs(animation): clips de propiedades (fila 15, C14)`.
- [ ] **Step 5:** runtime Debug con `rr10.scene` en los dos backends (20 s, cierre con `CloseMainWindow`): Vulkan sin validación, D3D12 solo `id=1328`. Es la comprobación de no-regresión: esa escena no tiene clips de propiedades.
- [ ] **Step 6:** verificación manual del usuario en Vulkan y D3D12: una puerta (cubo sin esqueleto) con dos estados y un trigger abre y cierra; una luz parpadea; un personaje conserva su animación de malla y además le cambia el material; una escena vieja se ve igual.
