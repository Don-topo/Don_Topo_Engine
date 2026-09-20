# Curvas de clip (fila 15 / C9) — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** que una pista de un clip de propiedades pueda escribir un parámetro Float del Animator en vez de una propiedad del GameObject, y que ese valor condicione las transiciones del mismo frame.

**Architecture:** `PropertyTrack` gana un destino (`Property` | `Parameter`). Las pistas de parámetro las escribe el propio `AnimatorComponent` dentro de `updateLayer`, tras avanzar los relojes y antes de evaluar las transiciones de esa capa; el host (`SkinnedFrameSync`) las salta. Nada llega a la GPU.

**Tech Stack:** C++17, ImGui + imgui-node-editor (panel), nlohmann::json (escena), arnés de tests propio (`engine/tests/animator_tests.cpp`, macro `CHECK`).

**Spec:** `docs/superpowers/specs/2026-09-20-clip-curves-design.md`

## Global Constraints

- Ficheros en **CRLF**: editar con la herramienta Edit o con Python usando `newline=''`; nunca `sed -i`.
- Build: `configure.bat` / `build.bat` por PowerShell; comprobar el **código de salida** antes de ejecutar nada (un exe viejo engaña).
- Tests desde la **raíz del repo**: `build-ninja/engine/tests/dt_animator_tests.exe`.
- Solo parámetros de tipo `Float`. Int, Bool y Trigger quedan fuera.
- Un `.scene` existente debe seguir cargando igual: sin `"target"`, la pista es de propiedad.
- Cada guarda nueva se sabotea **de una en una** (parchear → compilar → ejecutar → revertir) y se comprueba que cae su test.

---

### Task 1: Destino de la pista y resolución contra los parámetros

**Files:**
- Modify: `engine/include/DonTopo/Core/PropertyTracks.h` (struct `PropertyTrack`)
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (declarar `hasFloatParameter`)
- Modify: `engine/src/Core/AnimatorComponent.cpp` (`bindProperties`, ~línea 637)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Produces: `DonTopo::TrackTarget { Property, Parameter }`; `PropertyTrack::target`, `PropertyTrack::parameterName`; `bool AnimatorComponent::hasFloatParameter(const std::string& n) const`.

- [ ] **Step 1: Escribir el test que falla**

En `animator_tests.cpp`, antes de `test_property_samples_follow_the_graph`:

```cpp
// Una curva es una pista con destino parámetro: se resuelve contra los
// parámetros DECLARADOS, no contra los componentes del objeto.
static void test_curve_track_resolves_against_float_parameter()
{
    AnimatorComponent a;
    a.addParameter("velocidad", AnimatorComponent::ParamType::Float);
    a.addParameter("vivo",      AnimatorComponent::ParamType::Bool);
    PropertyClip c;
    c.name = "curvas"; c.duration = 1.0f;
    PropertyTrack ok;  ok.target = TrackTarget::Parameter;  ok.parameterName = "velocidad";
    ok.keys = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    PropertyTrack tipo; tipo.target = TrackTarget::Parameter; tipo.parameterName = "vivo";
    tipo.keys = { { 0.0f, 0.0f } };
    PropertyTrack no;  no.target = TrackTarget::Parameter;  no.parameterName = "noExiste";
    no.keys = { { 0.0f, 0.0f } };
    PropertyTrack vacia; vacia.target = TrackTarget::Parameter;
    vacia.keys = { { 0.0f, 0.0f } };
    c.tracks = { ok, tipo, no, vacia };
    a.addPropertyClip(c);

    CHECK(a.hasFloatParameter("velocidad"));
    CHECK(!a.hasFloatParameter("vivo"));
    CHECK(!a.hasFloatParameter("noExiste"));

    std::vector<std::string> avisos;
    a.bindProperties(nullptr, &avisos);
    CHECK(a.propertyClips()[0].tracks[0].resolved);
    CHECK(!a.propertyClips()[0].tracks[1].resolved);   // existe, pero es Bool
    CHECK(!a.propertyClips()[0].tracks[2].resolved);
    CHECK(!a.propertyClips()[0].tracks[3].resolved);   // sin nombre
    CHECK(avisos.size() == 3u);
    // El destino por defecto sigue siendo la propiedad: una pista de siempre
    // no cambia de significado.
    CHECK(PropertyTrack{}.target == TrackTarget::Property);
}
```

Registrarlo en `main` junto a los demás `test_property_*` (tras `test_property_clip_tracks_resolved_against_object();`).

- [ ] **Step 2: Compilar y ver que falla**

`build.bat`; esperado: error de compilación, `TrackTarget` no existe.

- [ ] **Step 3: Implementar**

En `PropertyTracks.h`, sobre `struct PropertyTrack`:

```cpp
    // A dónde va el valor de la pista. Property: una propiedad del GameObject
    // (una puerta, una luz, un material). Parameter: un parámetro Float del
    // Animator — una "curva de clip", que sirve para condicionar transiciones
    // o alimentar speedParam desde el propio tiempo de la animación.
    enum class TrackTarget { Property, Parameter };

    struct PropertyTrack
    {
        TrackTarget              target = TrackTarget::Property;
        PropertyId               property = PropertyId::PositionX;   // si target == Property
        std::string              parameterName;                      // si target == Parameter
        std::vector<PropertyKey> keys;
        bool                     resolved = false;
    };
```

(borrar la declaración anterior de `property`/`keys`/`resolved`, que este bloque sustituye).

En `AnimatorComponent.h`, junto a los getters de parámetros (`getFloat`, ~línea 381):

```cpp
            // ¿Hay un parámetro DECLARADO con ese nombre y de tipo Float? Es lo
            // que hace resoluble una curva de clip.
            bool  hasFloatParameter(const std::string& n) const;
```

En `AnimatorComponent.cpp`, el bucle final de `bindProperties`:

```cpp
        for (auto& clip : m_propertyClips)
            for (auto& tr : clip.tracks)
            {
                if (tr.target == TrackTarget::Parameter)
                {
                    // Una curva no depende del objeto: depende de que el
                    // parámetro exista y sea Float.
                    tr.resolved = !tr.parameterName.empty() && hasFloatParameter(tr.parameterName);
                    if (!tr.resolved && warnings)
                        warnings->push_back("Animator: la curva del clip '" + clip.name +
                                            "' escribe '" + tr.parameterName +
                                            "', que no es un parametro Float declarado");
                    continue;
                }
                tr.resolved = !go || propertyAvailable(*go, tr.property);
                if (!tr.resolved && warnings)
                    warnings->push_back("Animator: la pista '" + std::string(propertyName(tr.property)) +
                                        "' del clip '" + clip.name + "' necesita un componente que el objeto no tiene");
            }
```

Y la implementación del helper, junto a `getFloat`:

```cpp
    bool AnimatorComponent::hasFloatParameter(const std::string& n) const
    {
        for (const auto& p : m_parameters)
            if (p.name == n) return p.type == ParamType::Float;
        return false;
    }
```

- [ ] **Step 4: Compilar y ejecutar**

`build.bat` (exit 0) y `build-ninja/engine/tests/dt_animator_tests.exe` desde la raíz: todo PASS.

- [ ] **Step 5: Sabotaje**

Quitar `&& hasFloatParameter(tr.parameterName)` y comprobar que cae **solo** `test_curve_track_resolves_against_float_parameter`. Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Core/PropertyTracks.h engine/include/DonTopo/Core/AnimatorComponent.h engine/src/Core/AnimatorComponent.cpp engine/tests/animator_tests.cpp
git commit -m "feat(animation): una pista de clip puede tener como destino un parametro Float"
```

---

### Task 2: La curva escribe el parámetro antes de las transiciones

**Files:**
- Modify: `engine/include/DonTopo/Core/PropertyTracks.h` (`blendScalarValues`)
- Modify: `engine/src/Core/PropertyTracks.cpp` (`blendPropertyValues` pasa a usarlo)
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (declarar `layerPropertySamples` y `applyCurves`, privados)
- Modify: `engine/src/Core/AnimatorComponent.cpp` (`propertySamples` ~647, `updateLayer` ~1193)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `TrackTarget`, `PropertyTrack::parameterName`, `AnimatorComponent::hasFloatParameter` (Task 1).
- Produces: `float DonTopo::blendScalarValues(const PropertyContribution* c, int n)`; `int AnimatorComponent::layerPropertySamples(int li, PropertySampleRef* out, int max) const` (privado); `void AnimatorComponent::applyCurves(int li)` (privado).

- [ ] **Step 1: Escribir los tests que fallan**

```cpp
// Estado único con una curva 0 -> 10 en 1 s sobre el parámetro "velocidad".
static AnimatorComponent makeCurvaVelocidad()
{
    AnimatorComponent a;
    a.addParameter("velocidad", AnimatorComponent::ParamType::Float);
    PropertyClip c;
    c.name = "acelera"; c.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 0.0f }, { 1.0f, 10.0f } };
    c.tracks.push_back(cur);
    a.addPropertyClip(c);
    AnimatorComponent::State s;
    s.name = "Arranca"; s.propertyClipName = "acelera";
    a.addState(s);
    a.setEntryState(0);
    a.bindProperties(nullptr, nullptr);
    return a;
}

static void test_curve_writes_the_parameter()
{
    AnimatorComponent a = makeCurvaVelocidad();
    a.update(0.25f, true);
    CHECK(nearlyEqual(a.getFloat("velocidad"), 2.5f));
    a.update(0.25f, true);
    CHECK(nearlyEqual(a.getFloat("velocidad"), 5.0f));
    // Una curva sin resolver no escribe: el parámetro se queda como estaba.
    AnimatorComponent b = makeCurvaVelocidad();
    b.propertyClipsMutable()[0].tracks[0].parameterName = "otro";
    b.bindProperties(nullptr, nullptr);
    b.setFloat("velocidad", -1.0f);
    b.update(0.5f, true);
    CHECK(nearlyEqual(b.getFloat("velocidad"), -1.0f));
}

static void test_curve_blends_during_cross_fade()
{
    AnimatorComponent a = makeCurvaVelocidad();
    // Segundo estado con una curva constante a 0 y transición por trigger.
    PropertyClip frena;
    frena.name = "frena"; frena.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 0.0f }, { 1.0f, 0.0f } };
    frena.tracks.push_back(cur);
    a.addPropertyClip(frena);
    AnimatorComponent::State s;
    s.name = "Frena"; s.propertyClipName = "frena";
    a.addState(s);
    a.addParameter("para", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 1.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "para";
    t.conditions.push_back(c);
    a.addTransition(t);
    a.bindProperties(nullptr, nullptr);

    a.update(0.5f, true);                 // Arranca en t=0,5 -> 5
    CHECK(nearlyEqual(a.getFloat("velocidad"), 5.0f));
    a.setTrigger("para");
    a.update(0.0f, true);                 // arranca el fade, peso del destino 0
    a.update(0.5f, true);                 // mitad del fade: 0,5*10 y 0,5*0
    // Arranca va por t=1 (valor 10) y Frena por t=0,5 (valor 0), a mitad de peso.
    CHECK(nearlyEqual(a.getFloat("velocidad"), 5.0f));
    a.update(0.5f, true);                 // fade terminado: manda Frena
    CHECK(nearlyEqual(a.getFloat("velocidad"), 0.0f));
}

static void test_curve_fires_its_transition_in_the_same_frame()
{
    AnimatorComponent a = makeCurvaVelocidad();
    AnimatorComponent::State s;
    s.name = "Corre";
    a.addState(s);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::FloatGreater;
    c.paramName = "velocidad"; c.floatValue = 4.0f;
    t.conditions.push_back(c);
    a.addTransition(t);
    a.bindProperties(nullptr, nullptr);
    // En t=0,5 la curva vale 5: la transición salta en ESTE update, no en el
    // siguiente.
    a.update(0.5f, true);
    CHECK(a.currentState() == 1);
}

static void test_curve_of_a_zero_weight_layer_still_writes()
{
    AnimatorComponent a = makeCurvaVelocidad();
    // Una capa con peso 0 no se ve, pero su máquina corre: su curva es
    // información, no pose.
    CHECK(a.addLayer("Info") == 1);
    a.setLayerWeight(1, 0.0f);
    a.addParameter("info", AnimatorComponent::ParamType::Float);
    PropertyClip c;
    c.name = "infoClip"; c.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "info";
    cur.keys = { { 0.0f, 0.0f }, { 1.0f, 8.0f } };
    c.tracks.push_back(cur);
    a.addPropertyClip(c);
    AnimatorComponent::State s;
    s.name = "Info"; s.propertyClipName = "infoClip";
    a.addState(s, 1);
    a.setEntryState(0, 1);
    a.bindProperties(nullptr, nullptr);
    a.update(0.5f, true);
    CHECK(nearlyEqual(a.getFloat("info"), 4.0f));
}

static void test_curve_last_layer_wins()
{
    AnimatorComponent a = makeCurvaVelocidad();
    CHECK(a.addLayer("Encima") == 1);
    a.setLayerWeight(1, 1.0f);
    PropertyClip c;
    c.name = "encima"; c.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 100.0f }, { 1.0f, 100.0f } };
    c.tracks.push_back(cur);
    a.addPropertyClip(c);
    AnimatorComponent::State s;
    s.name = "Encima"; s.propertyClipName = "encima";
    a.addState(s, 1);
    a.setEntryState(0, 1);
    a.bindProperties(nullptr, nullptr);
    a.update(0.5f, true);
    // Las capas se recorren en orden: escribe la última.
    CHECK(nearlyEqual(a.getFloat("velocidad"), 100.0f));
}
```

Registrar los cinco en `main`, tras `test_curve_track_resolves_against_float_parameter();`.

Antes de implementar, comprobar en `AnimatorComponent.h` los nombres exactos de `addLayer`, `setLayerWeight`, `addState(s, capa)`, `setEntryState(i, capa)`, `currentState()` y `ConditionType::FloatGreater`/`Condition::floatValue`, y ajustar los tests a las firmas reales (la capa es un argumento **obligatorio** en la API por capas).

- [ ] **Step 2: Compilar y ver que fallan**

`build.bat`; esperado: los cinco FAIL (el parámetro no se escribe), o error de compilación si algún nombre no coincide (ajustar y repetir).

- [ ] **Step 3: Implementar**

En `PropertyTracks.h`, junto a `blendPropertyValues`:

```cpp
    // Media ponderada a secas. Es lo que usa una curva: un parámetro no es un
    // ángulo, así que no hay camino corto que respetar.
    float blendScalarValues(const PropertyContribution* c, int n);
```

En `PropertyTracks.cpp`:

```cpp
    float blendScalarValues(const PropertyContribution* c, int n)
    {
        if (n <= 0) return 0.0f;
        float total = 0.0f;
        for (int i = 0; i < n; i++) total += c[i].weight;
        if (total <= 0.0f) return 0.0f;
        float v = 0.0f;
        for (int i = 0; i < n; i++) v += c[i].value * c[i].weight;
        return v / total;
    }
```

y en `blendPropertyValues`, sustituir su rama no-rotación por `if (!propertyIsRotation(id)) return blendScalarValues(c, n);` (el cálculo del total queda solo para la rama de ángulos).

En `AnimatorComponent.h`, en la sección privada, junto a `updateLayer`:

```cpp
            // Muestras de UNA capa (estado actual y, en un fade, el que se
            // apaga). propertySamples es la suma de todas las capas; applyCurves
            // necesita la de una sola, y los dos repartos tienen que ser el
            // mismo, así que hay un único sitio donde vive.
            int  layerPropertySamples(int li, PropertySampleRef* out, int max) const;
            // Escribe los parámetros de las pistas con destino Parameter de esa
            // capa. Se llama desde updateLayer ANTES de evaluar las
            // transiciones: el valor de este frame condiciona este frame.
            void applyCurves(int li);
```

En `AnimatorComponent.cpp`, partir `propertySamples` en el helper por capa:

```cpp
    int AnimatorComponent::layerPropertySamples(int li, PropertySampleRef* out, int max) const
    {
        int n = 0;
        auto add = [&](int stateIdx, float animTime, float peso) {
            if (n >= max || peso <= 0.0f) return;
            const Layer& L = m_layers[(size_t)li];
            if (stateIdx < 0 || stateIdx >= (int)L.states.size()) return;
            const State& st = L.states[(size_t)stateIdx];
            if (st.propertyClipIndex < 0) return;
            out[n++] = { st.propertyClipIndex,
                         st.ticksPerSecond > 0.0f ? animTime / st.ticksPerSecond : 0.0f,
                         peso };
        };
        const Layer& L = m_layers[(size_t)li];
        const float  w = blendWeight(li);
        if (blending(li))
        {
            add(L.prevState, L.prevAnimTime, 1.0f - w);
            add(L.currentState, L.animTime, w);
        }
        else
            add(L.currentState, L.animTime, 1.0f);
        return n;
    }

    int AnimatorComponent::propertySamples(PropertySampleRef* out, int max) const
    {
        int n = 0;
        // Mismo reparto que pose(): cada capa aporta su estado actual y, en un
        // fade, también el que se apaga, con el peso de los dos multiplicado
        // por el de la capa.
        for (int li = 0; li < (int)m_layers.size(); li++)
        {
            const float capa = layerWeight(li);
            if (capa <= 0.0f) continue;
            PropertySampleRef propias[kMaxPoseSamplesPerLayer];
            const int m = layerPropertySamples(li, propias, kMaxPoseSamplesPerLayer);
            for (int k = 0; k < m && n < max; k++)
            {
                propias[k].weight *= capa;      // el peso de la capa escala lo que se APLICA
                out[n++] = propias[k];
            }
        }
        return n;
    }
```

(comprobar el nombre real de la constante de muestras por capa en `PoseBlock.h` / `AnimatorComponent.h` — en `SkinnedFrameSync.h` se usa `kMaxPoseSamplesPerLayer` — y usarlo tal cual.)

`applyCurves`, a continuación:

```cpp
    void AnimatorComponent::applyCurves(int li)
    {
        // El peso de la CAPA no entra: una pose se mezcla, un parámetro se
        // escribe. Si dos capas tienen curva para el mismo parámetro, gana la
        // última, porque updateLayer las recorre en orden.
        PropertySampleRef muestras[kMaxPoseSamplesPerLayer];
        const int n = layerPropertySamples(li, muestras, kMaxPoseSamplesPerLayer);
        if (n == 0) return;
        for (int k = 0; k < n; k++)
        {
            const PropertyClip& clip = m_propertyClips[(size_t)muestras[k].clip];
            for (const auto& tr : clip.tracks)
            {
                if (tr.target != TrackTarget::Parameter || !tr.resolved || tr.keys.empty()) continue;
                // Todas las muestras que escriban ESTE parámetro, mezcladas por
                // el peso del fade.
                PropertyContribution aporta[kMaxPoseSamplesPerLayer];
                int m = 0;
                bool yaHecho = false;
                for (int j = 0; j < n && !yaHecho; j++)
                {
                    const PropertyClip& otro = m_propertyClips[(size_t)muestras[j].clip];
                    for (const auto& t2 : otro.tracks)
                    {
                        if (t2.target != TrackTarget::Parameter || !t2.resolved) continue;
                        if (t2.parameterName != tr.parameterName) continue;
                        if (j < k) { yaHecho = true; break; }   // ya se escribió en una vuelta anterior
                        aporta[m++] = { samplePropertyTrack(t2, muestras[j].time, getFloat(tr.parameterName)),
                                        muestras[j].weight };
                        break;   // una pista por parámetro y clip: la primera manda
                    }
                }
                if (yaHecho || m == 0) continue;
                setFloat(tr.parameterName, blendScalarValues(aporta, m));
            }
        }
    }
```

En `updateLayer`, justo antes de `if (!evaluateTransitions) return;`:

```cpp
        // Curvas ANTES de las transiciones y antes del return de Edit: el valor
        // de este frame condiciona las transiciones de este frame, y en el
        // preview del editor el parámetro se ve moverse en el panel.
        applyCurves(li);

        if (!evaluateTransitions) return;
```

- [ ] **Step 4: Compilar y ejecutar**

`build.bat` (exit 0) y `dt_animator_tests.exe` desde la raíz: todo PASS, **incluidos** los tests de C14 ya existentes (`test_property_samples_follow_the_graph`, `test_property_clips_drive_a_non_skinned_object`), que cubren el reparto que acaba de moverse a `layerPropertySamples`.

- [ ] **Step 5: Sabotajes (uno a uno)**

1. Mover `applyCurves(li);` debajo del `return` de `evaluateTransitions` → cae `test_curve_fires_its_transition_in_the_same_frame`. Revertir.
2. En `propertySamples`, quitar `propias[k].weight *= capa;` → cae un test de C14 (el peso de capa en las propiedades). Revertir.
3. En `applyCurves`, usar `muestras[k]` en lugar de mezclar todas las muestras (escribir solo la primera) → cae `test_curve_blends_during_cross_fade`. Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Core/PropertyTracks.h engine/src/Core/PropertyTracks.cpp engine/include/DonTopo/Core/AnimatorComponent.h engine/src/Core/AnimatorComponent.cpp engine/tests/animator_tests.cpp
git commit -m "feat(animation): las curvas escriben su parametro antes de evaluar las transiciones"
```

---

### Task 3: El host no aplica las curvas como propiedades

**Files:**
- Modify: `engine/include/DonTopo/Renderer/SkinnedFrameSync.h` (bucle de `applyAnimatorFrame`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `TrackTarget` (Task 1), `applyCurves` (Task 2).

- [ ] **Step 1: Escribir el test que falla**

```cpp
// Una curva no es una propiedad: no mueve el objeto ni marca el resultado del
// frame. El parámetro sí se escribe (lo hace el componente en update).
static void test_curve_does_not_touch_the_transform()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    AnimatorComponent a;
    a.addParameter("velocidad", AnimatorComponent::ParamType::Float);
    PropertyClip c;
    c.name = "acelera"; c.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 0.0f }, { 1.0f, 10.0f } };
    c.tracks.push_back(cur);
    a.addPropertyClip(c);
    AnimatorComponent::State s;
    s.name = "Arranca"; s.propertyClipName = "acelera";
    a.addState(s);
    a.setEntryState(0);
    go->setAnimator(std::make_shared<AnimatorComponent>(a));
    go->getAnimator()->bindProperties(go, nullptr);
    const glm::vec3 antes = go->position;

    const AnimatorFrameResult r = applyAnimatorFrame(*go, 0.5f, true);
    CHECK(!r.transform);
    CHECK(!r.material);
    CHECK(nearlyEqual(go->position.x, antes.x));
    CHECK(nearlyEqual(go->position.y, antes.y));
    CHECK(nearlyEqual(go->position.z, antes.z));
    CHECK(nearlyEqual(go->getAnimator()->getFloat("velocidad"), 5.0f));
}
```

(comprobar cómo crean el Animator los tests vecinos de C14 —`test_property_clips_drive_a_non_skinned_object`— y usar ese mismo camino para colgarlo del GameObject.)

Registrarlo en `main` junto a los demás.

- [ ] **Step 2: Compilar y ver que falla**

`build.bat` y ejecutar: FAIL — la pista de parámetro entra en el bucle por `PropertyId` y mueve `PositionX`.

- [ ] **Step 3: Implementar**

En `SkinnedFrameSync.h`, dentro del bucle de pistas de `applyAnimatorFrame`:

```cpp
                for (const auto& tr : clip.tracks)
                {
                    // Las pistas con destino parámetro las escribió ya el
                    // componente en update(): aquí solo van las propiedades del
                    // objeto.
                    if (tr.target != TrackTarget::Property) continue;
                    if (tr.property != id || !tr.resolved || tr.keys.empty()) continue;
```

- [ ] **Step 4: Compilar y ejecutar**: todo PASS.

- [ ] **Step 5: Sabotaje**

Quitar la línea `if (tr.target != TrackTarget::Property) continue;` → cae `test_curve_does_not_touch_the_transform`. Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer/SkinnedFrameSync.h engine/tests/animator_tests.cpp
git commit -m "fix(animation): una pista con destino parametro no se aplica como propiedad del objeto"
```

---

### Task 4: Serialización

**Files:**
- Modify: `engine/src/Core/Scene.cpp` (escritura ~línea 693, lectura ~línea 959)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `TrackTarget`, `PropertyTrack::parameterName` (Task 1).

- [ ] **Step 1: Escribir el test que falla**

Siguiendo el patrón de `test_property_clips_serialization(PhysicsManager& pm, AudioManager& am)` (guarda la escena a un fichero temporal y la vuelve a cargar):

```cpp
static void test_curve_serialization(PhysicsManager& pm, AudioManager& am)
{
    // Igual que test_property_clips_serialization: crear escena, objeto y
    // animator, guardar y recargar por el mismo camino que usa ese test.
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto anim = std::make_shared<AnimatorComponent>();
    anim->addParameter("velocidad", AnimatorComponent::ParamType::Float);
    PropertyClip c;
    c.name = "acelera"; c.duration = 2.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 1.0f }, { 2.0f, 7.0f } };
    PropertyTrack pos; pos.property = PropertyId::PositionY;
    pos.keys = { { 0.0f, 0.0f }, { 2.0f, 3.0f } };
    c.tracks = { cur, pos };
    anim->addPropertyClip(c);
    go->setAnimator(anim);

    const nlohmann::json j = animatorToJson(*anim);
    // La pista de propiedad se guarda como siempre: sin "target".
    CHECK(!j["propertyClips"][0]["tracks"][1].contains("target"));
    CHECK(j["propertyClips"][0]["tracks"][0]["target"] == "parameter");
    CHECK(j["propertyClips"][0]["tracks"][0]["parameter"] == "velocidad");

    AnimatorComponent leido;
    std::vector<std::string> avisos;
    animatorFromJson(j, leido, &avisos);
    CHECK(leido.propertyClips().size() == 1u);
    CHECK(leido.propertyClips()[0].tracks.size() == 2u);
    CHECK(leido.propertyClips()[0].tracks[0].target == TrackTarget::Parameter);
    CHECK(leido.propertyClips()[0].tracks[0].parameterName == "velocidad");
    CHECK(leido.propertyClips()[0].tracks[1].target == TrackTarget::Property);
    CHECK(leido.propertyClips()[0].tracks[1].property == PropertyId::PositionY);

    // Una curva sin nombre se descarta con aviso.
    nlohmann::json roto = j;
    roto["propertyClips"][0]["tracks"][0]["parameter"] = "";
    AnimatorComponent leido2;
    avisos.clear();
    animatorFromJson(roto, leido2, &avisos);
    CHECK(leido2.propertyClips()[0].tracks.size() == 1u);
    CHECK(!avisos.empty());
}
```

`animatorToJson` / `animatorFromJson` viven en `Scene.cpp`; si no son accesibles desde el test, hacer el round-trip guardando y cargando la escena, como hace `test_property_clips_serialization`, y comprobar los mismos campos sobre el animator recargado (el `contains("target")` se comprueba entonces sobre el JSON del fichero). Registrarlo en `main` junto a `test_property_clips_serialization(pm, am);`.

- [ ] **Step 2: Compilar y ver que falla**: FAIL, el JSON no tiene `target`.

- [ ] **Step 3: Implementar**

Escritura, en el bucle de pistas:

```cpp
                for (const auto& tr : c.tracks)
                {
                    auto keys = nlohmann::json::array();
                    for (const auto& k : tr.keys)
                        keys.push_back({ {"t", k.time}, {"v", k.value} });
                    // Una curva se distingue por "target"; una pista de
                    // propiedad se guarda como siempre, para que un .scene
                    // anterior y uno nuevo sean idénticos si no hay curvas.
                    if (tr.target == TrackTarget::Parameter)
                        pistas.push_back({ {"target", "parameter"},
                                           {"parameter", tr.parameterName},
                                           {"keys", std::move(keys)} });
                    else
                        pistas.push_back({ {"property", propertyName(tr.property)}, {"keys", std::move(keys)} });
                }
```

Lectura, en el bucle de pistas, antes de resolver `property`:

```cpp
                        DonTopo::PropertyTrack tr;
                        if (tj.value("target", std::string("property")) == "parameter")
                        {
                            tr.target        = DonTopo::TrackTarget::Parameter;
                            tr.parameterName = tj.value("parameter", std::string());
                            if (tr.parameterName.empty())
                            {
                                if (warnings)
                                    warnings->push_back(ctxClip + ": una curva sin parametro, se descarta");
                                continue;
                            }
                        }
                        else
                        {
                            const std::string prop = tj.value("property", std::string());
                            tr.property = DonTopo::propertyFromName(prop);
                            if (tr.property == DonTopo::PropertyId::Count)
                            {
                                if (warnings)
                                    warnings->push_back(ctxClip + ": propiedad '" + prop +
                                                         "' desconocida, la pista se descarta");
                                continue;
                            }
                        }
```

(el bloque anterior que declaraba `tr` y resolvía `property` se sustituye por este; el resto del bucle —las keys y el `push_back` de la pista— no cambia.)

- [ ] **Step 4: Compilar y ejecutar**: todo PASS, incluidos los tests de escena de C14.

- [ ] **Step 5: Sabotaje**

Escribir siempre `{"property", ...}` (ignorar el destino) → cae `test_curve_serialization`. Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/animator_tests.cpp
git commit -m "feat(animation): las curvas de clip se guardan y se cargan en la escena"
```

---

### Task 5: Panel y documentación

**Files:**
- Modify: `engine/src/Editor/AnimatorPanel.cpp` (bloque de pistas, ~líneas 549-570)
- Modify: `README.md` (sección del Animator)
- Modify: `docs/animation-audit.md` (fila C9 y fila 15)

**Interfaces:**
- Consumes: `TrackTarget`, `PropertyTrack::parameterName`, `AnimatorComponent::hasFloatParameter` (Task 1).

- [ ] **Step 1: Destino y combo de parámetros en el panel**

En `AnimatorPanel.cpp`, dentro del `for` de pistas, sustituir el combo de propiedad por:

```cpp
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::BeginCombo("###destino",
                                          pista.target == TrackTarget::Parameter ? "Parametro" : "Propiedad"))
                    {
                        if (ImGui::Selectable("Propiedad", pista.target == TrackTarget::Property))
                        {
                            pista.target = TrackTarget::Property;
                            anim->bindProperties(go, nullptr);
                        }
                        if (ImGui::Selectable("Parametro", pista.target == TrackTarget::Parameter))
                        {
                            pista.target = TrackTarget::Parameter;
                            anim->bindProperties(go, nullptr);
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150.0f);
                    if (pista.target == TrackTarget::Parameter)
                    {
                        // Solo los Float: una curva no puede escribir otra cosa.
                        // El nombre actual se ofrece aunque ya no exista, para no
                        // perderlo en silencio al abrir una escena cuyo parámetro
                        // se borró.
                        const char* actual = pista.parameterName.empty() ? "(sin parametro)"
                                                                         : pista.parameterName.c_str();
                        if (ImGui::BeginCombo("###param", actual))
                        {
                            for (const auto& p : anim->parameters())
                            {
                                if (p.type != AnimatorComponent::ParamType::Float) continue;
                                if (ImGui::Selectable(p.name.c_str(), p.name == pista.parameterName))
                                {
                                    pista.parameterName = p.name;
                                    anim->bindProperties(go, nullptr);
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    else if (ImGui::BeginCombo("###prop", propertyName(pista.property)))
                    {
                        for (int q = 0; q < (int)PropertyId::Count; q++)
                        {
                            const PropertyId id = (PropertyId)q;
                            if (ImGui::Selectable(propertyName(id), id == pista.property))
                            {
                                pista.property = id;
                                anim->bindProperties(go, nullptr);
                            }
                        }
                        ImGui::EndCombo();
                    }
```

(comprobar el nombre real del getter de parámetros en `AnimatorComponent.h` —`parameters()`— y usarlo tal cual. El coloreado en rojo de una pista sin resolver ya existe y no se toca.)

- [ ] **Step 2: Compilar y abrir el editor**

`build.bat` (exit 0) y abrir el Sandbox **desde la GUI** con el proyecto (el backend sale del último project.json). Comprobar: el combo de destino cambia la pista, el de parámetros solo lista los Float, y una curva sin parámetro sale en rojo.

- [ ] **Step 3: Documentar**

En `README.md`, en la sección del Animator, tras los clips de propiedades:

```markdown
Una pista de un clip de propiedades puede escribir, en vez de una propiedad del
objeto, un **parámetro Float** del Animator: es una *curva de clip*. Sirve para
que el propio tiempo de la animación condicione la máquina de estados (una
transición por `velocidad > 4`) o alimente `speedParam`.

Dos reglas no obvias:

- La curva se evalúa **antes** que las transiciones, así que su valor de este
  frame ya decide las transiciones de este frame. En el editor, el preview
  también la escribe: el valor que hayas puesto a mano en el panel queda pisado
  mientras el preview avanza.
- Si dos capas tienen una curva para el mismo parámetro, **gana la última**; el
  peso de la capa no escala el valor (una pose se mezcla, un parámetro se
  escribe). Una capa con peso 0 escribe su curva igualmente.
```

En `docs/animation-audit.md`: fila C9 a `✅ **EXISTE** (2026-09-20, <rango de commits>, fila 15)` con el fichero donde vive (`Core/PropertyTracks.h`, `AnimatorComponent::applyCurves`), y la fila 15 al día (queda C10 como único pendiente).

- [ ] **Step 4: Commit**

```bash
git add engine/src/Editor/AnimatorPanel.cpp README.md docs/animation-audit.md
git commit -m "feat(editor): destino de la pista en el panel y documentacion de las curvas de clip"
```

- [ ] **Step 5: Verificación manual (usuario)**

En Vulkan **y** en D3D12, con el proyecto abierto desde la GUI: un estado con un clip de propiedades que tenga una curva sobre un parámetro Float usado en la condición de una transición; se ve el parámetro moverse en el panel y el estado cambiar al cruzar el umbral. Solo tras el visto bueno: `git merge --no-ff` a main, push y borrar la rama.
