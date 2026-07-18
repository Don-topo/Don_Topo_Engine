# Animator Int/Float Parameters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Añadir parámetros de tipo `Int` y `Float` al `AnimatorComponent`, con condiciones de comparación estilo Unity, editables en el panel y accesibles desde Lua.

**Architecture:** Los enums `ParamType` y `ConditionType` crecen con los dos tipos nuevos (valores al final, para no romper la inicialización agregada de los tests ni las escenas guardadas). `Condition` gana `compare` y `threshold` (un único `float` sirve para Int y Float). El componente guarda dos mapas nuevos, `m_ints` y `m_floats`, con la misma forma que `m_bools`. Los cuatro consumidores — serialización, panel del grafo, resumen de Properties y bindings de Lua — se extienden en cascada.

**Tech Stack:** C++20, ImGui + imgui-node-editor, nlohmann/json, sol2/Lua, CMake + Ninja + MSVC.

## Global Constraints

- Spec de referencia: `docs/superpowers/specs/2026-07-18-animator-int-float-params-design.md`.
- Los valores nuevos de `ParamType` y `ConditionType` van **al final** del enum, y los campos nuevos de `Condition` **al final** del struct: `animator_tests.cpp` usa inicialización agregada `{ConditionType::Bool, "running", true}` y romperla obliga a reescribir tests que no son parte de este trabajo.
- Los enums se serializan **como string**, nunca como int (criterio ya establecido en `Scene.cpp`).
- No se serializan valores por defecto de parámetro: `reset()` deja todo a 0 / false. La serialización de un parámetro sigue siendo `{name, type}`.
- Escenas guardadas antes de este cambio deben cargar sin migración ni warnings.
- Build: `.\build.bat` desde PowerShell en la raíz del repo (envuelve vcvarsall + Ninja). **No** invocar `cmake` a pelo.
- Tests: `.\build-ninja\engine\tests\dt_animator_tests.exe`, ejecutado **desde la raíz del repo** (carga `assets/modelAnimation.fbx` por ruta relativa). Sin framework: `CHECK(cond)` y `main()` que devuelve 1 si hubo fallos.
- Comentarios de código en castellano, explicando el *porqué*, siguiendo el estilo del archivo que se toca.

## File Structure

| Archivo | Responsabilidad | Tarea |
|---|---|---|
| `engine/include/DonTopo/Core/AnimatorComponent.h` | Enums, `Condition`, API de runtime, helper `paramTypeLabel` | 1, 3 |
| `engine/src/Core/AnimatorComponent.cpp` | Mapas, `evalCompare`, `conditionsMet`, `reset`, `addParameter` | 1, 3 |
| `engine/tests/animator_tests.cpp` | Tests headless de todo lo anterior | 1, 2 |
| `engine/src/Core/Scene.cpp` | JSON de parámetros y condiciones | 2 |
| `engine/src/Editor/AnimatorPanel.cpp` | Lista de parámetros con valores, combo de creación, popup de condiciones | 3 |
| `engine/include/DonTopo/Editor/AnimatorPanel.h` | Comentario del combo de tipo | 3 |
| `engine/src/Editor/PropertiesPanel.cpp` | Resumen de solo lectura | 3 |
| `engine/src/Scripting/ScriptBindings.cpp` | `SetInt`/`GetInt`/`SetFloat`/`GetFloat` | 4 |
| `README.md` | Documentación de la API Lua del Animator | 4 |

---

### Task 1: Core — tipos, valores y evaluación de condiciones

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h:27-35` (enums y `Condition`), `:103-105` (API runtime), `:124-138` (privados)
- Modify: `engine/src/Core/AnimatorComponent.cpp:68-77` (`addParameter`), `:79-109` (`removeParameter`), `:118-140` (accesores), `:155-162` (`reset`), `:188-211` (`conditionsMet`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: nada de tareas anteriores.
- Produces: `AnimatorComponent::ParamType::{Int,Float}`, `AnimatorComponent::ConditionType::{Int,Float}`, `AnimatorComponent::Compare::{Greater,Less,Equals,NotEquals}`, campos `Condition::compare` (tipo `Compare`) y `Condition::threshold` (tipo `float`), y los métodos `void setInt(const std::string&, int)`, `int getInt(const std::string&) const`, `void setFloat(const std::string&, float)`, `float getFloat(const std::string&) const`.

- [ ] **Step 1: Escribir los tests que fallan**

Añadir en `engine/tests/animator_tests.cpp`, justo antes de `int main()`:

```cpp
// Criterio: un parámetro Int dispara una transición según su comparador. Se
// construye un grafo mínimo aparte de makeGraph() pa no tocar los tests ya
// existentes, que dependen de su forma exacta.
static AnimatorComponent makeNumericGraph()
{
    AnimatorComponent a;

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "Idle";
    idle.clipIndex = 0; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f; idle.loop = true;

    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "Run";
    run.clipIndex = 1; run.duration = 20.0f; run.ticksPerSecond = 20.0f; run.loop = true;

    a.addState(idle);
    a.addState(run);
    a.setEntryState(0);

    a.addParameter("combo", AnimatorComponent::ParamType::Int);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);

    a.reset();
    return a;
}

static void test_int_condition_greater_and_equals()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Int;
    c.paramName = "combo";
    c.compare   = AnimatorComponent::Compare::Greater;
    c.threshold = 2.0f;
    t.conditions.push_back(c);
    a.addTransition(t);

    // Valor inicial 0: no dispara
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    // Igual al umbral: Greater es estricto, sigue sin disparar
    a.setInt("combo", 2);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    a.setInt("combo", 3);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);

    // Equals: mismo grafo, otro comparador
    AnimatorComponent b = makeNumericGraph();
    AnimatorComponent::Transition tb;
    tb.fromState = 0; tb.toState = 1;
    AnimatorComponent::Condition cb;
    cb.type      = AnimatorComponent::ConditionType::Int;
    cb.paramName = "combo";
    cb.compare   = AnimatorComponent::Compare::Equals;
    cb.threshold = 3.0f;
    tb.conditions.push_back(cb);
    b.addTransition(tb);

    b.setInt("combo", 4);
    b.update(0.016f, true);
    CHECK(b.currentState() == 0);

    b.setInt("combo", 3);
    b.update(0.016f, true);
    CHECK(b.currentState() == 1);
}

static void test_float_condition_less()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Float;
    c.paramName = "speed";
    c.compare   = AnimatorComponent::Compare::Less;
    c.threshold = -1.0f;
    t.conditions.push_back(c);
    a.addTransition(t);

    // Valor inicial 0.0f: no es menor que -1.0f
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    a.setFloat("speed", -2.5f);
    CHECK(nearlyEqual(a.getFloat("speed"), -2.5f));
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);
}

// Misma guarda que setBool: un nombre no declarado o de otro tipo se ignora en
// vez de crear un parámetro fantasma que ninguna condición miraría.
static void test_numeric_api_type_guards()
{
    AnimatorComponent a = makeNumericGraph();
    a.addParameter("running", AnimatorComponent::ParamType::Bool);

    a.setInt("combo", 7);
    CHECK(a.getInt("combo") == 7);
    a.setFloat("speed", 1.5f);
    CHECK(nearlyEqual(a.getFloat("speed"), 1.5f));

    // No declarado
    a.setInt("noExiste", 5);
    CHECK(a.getInt("noExiste") == 0);
    a.setFloat("tampoco", 5.0f);
    CHECK(nearlyEqual(a.getFloat("tampoco"), 0.0f));

    // Tipo equivocado en ambos sentidos
    a.setInt("speed", 9);            // speed es float
    CHECK(a.getInt("speed") == 0);
    a.setFloat("combo", 9.0f);       // combo es int
    CHECK(nearlyEqual(a.getFloat("combo"), 0.0f));
    a.setInt("running", 1);          // running es bool
    CHECK(a.getInt("running") == 0);

    // reset devuelve los numéricos a cero, igual que los bools
    a.reset();
    CHECK(a.getInt("combo") == 0);
    CHECK(nearlyEqual(a.getFloat("speed"), 0.0f));
}

// removeParameter ya limpiaba bools y triggers; los numéricos entran en el mismo
// camino, incluida la poda de transiciones que se quedan sin condiciones.
static void test_remove_numeric_parameter_cleans_conditions()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Int;
    c.paramName = "combo";
    c.compare   = AnimatorComponent::Compare::Greater;
    c.threshold = 0.0f;
    t.conditions.push_back(c);
    a.addTransition(t);
    CHECK(a.transitions().size() == 1);

    a.removeParameter("combo");
    CHECK(a.parameters().size() == 1);          // solo queda "speed"
    CHECK(a.transitions().empty());             // se quedó sin condiciones
    CHECK(a.getInt("combo") == 0);
}
```

Y registrarlos en `main()`, después de `test_parameter_api_ignores_undeclared();`:

```cpp
    test_int_condition_greater_and_equals();
    test_float_condition_less();
    test_numeric_api_type_guards();
    test_remove_numeric_parameter_cleans_conditions();
```

- [ ] **Step 2: Ejecutar el build para verificar que falla**

Run: `.\build.bat`
Expected: FALLA la compilación de `animator_tests.cpp` con errores del tipo `'Int': is not a member of 'DonTopo::AnimatorComponent::ParamType'` y `'setInt': is not a member of 'DonTopo::AnimatorComponent'`.

- [ ] **Step 3: Extender los tipos en el header**

En `engine/include/DonTopo/Core/AnimatorComponent.h`, sustituir el bloque de enums y `Condition` (líneas 27-35) por:

```cpp
            // Los valores nuevos van AL FINAL: los tests usan inicialización
            // agregada de Condition y la serialización va por string, así que
            // añadir por el medio rompería lo primero sin ganar nada.
            enum class ConditionType { Bool, Trigger, AnimationFinished, Int, Float };
            enum class ParamType     { Bool, Trigger, Int, Float };
            // Comparadores de las condiciones numéricas. La UI solo ofrece
            // Greater/Less para Float (== sobre float casi nunca dispara), pero
            // el evaluador los implementa los cuatro: un JSON editado a mano con
            // Equals sobre un float se evalúa, no se ignora en silencio.
            enum class Compare       { Greater, Less, Equals, NotEquals };

            struct Condition
            {
                ConditionType type     = ConditionType::Bool;
                std::string   paramName;          // vacío si AnimationFinished
                bool          expected = true;    // solo Bool
                // Solo Int/Float. Un único umbral en float sirve a los dos: la
                // UI de Int usa DragInt, así que siempre entra un valor íntegro,
                // y float representa enteros exactos hasta 2^24.
                Compare       compare   = Compare::Greater;
                float         threshold = 0.0f;
            };
```

Sustituir el bloque de runtime (líneas 103-105) por:

```cpp
            void setBool(const std::string& n, bool v);
            bool getBool(const std::string& n) const;
            void setTrigger(const std::string& n);
            // Devuelven 0 si el parámetro no existe; los setters no hacen nada
            // si el nombre no está declarado o es de otro tipo (misma guarda que
            // setBool).
            void  setInt(const std::string& n, int v);
            int   getInt(const std::string& n) const;
            void  setFloat(const std::string& n, float v);
            float getFloat(const std::string& n) const;
```

Y en la sección privada, junto a `m_bools`/`m_triggers` (líneas 137-138):

```cpp
            std::unordered_map<std::string, bool> m_bools;
            std::unordered_map<std::string, bool> m_triggers;
            std::unordered_map<std::string, int>    m_ints;
            std::unordered_map<std::string, float>  m_floats;
```

Añadir también la declaración del helper de comparación en la sección privada, junto a `conditionsMet`:

```cpp
            bool conditionsMet(const Transition& t) const;
            // Estático porque no toca estado: aísla los cuatro comparadores en
            // un sitio y sirve tanto a Int como a Float.
            template <typename T>
            static bool evalCompare(T value, Compare op, T threshold)
            {
                switch (op)
                {
                    case Compare::Greater:   return value >  threshold;
                    case Compare::Less:      return value <  threshold;
                    case Compare::Equals:    return value == threshold;
                    case Compare::NotEquals: return value != threshold;
                }
                return false;
            }
```

- [ ] **Step 4: Implementar los valores en el .cpp**

En `engine/src/Core/AnimatorComponent.cpp`, sustituir el final de `addParameter` (líneas 74-76) por:

```cpp
        const std::string& n = m_parameters.back().name;
        switch (type)
        {
            case ParamType::Bool:    m_bools[n]    = false;  break;
            case ParamType::Trigger: m_triggers[n] = false;  break;
            case ParamType::Int:     m_ints[n]     = 0;      break;
            case ParamType::Float:   m_floats[n]   = 0.0f;   break;
        }
```

En `removeParameter`, junto a los dos `erase` existentes (líneas 90-91):

```cpp
        m_bools.erase(name);
        m_triggers.erase(name);
        m_ints.erase(name);
        m_floats.erase(name);
```

En `reset()`, junto a los dos bucles existentes (líneas 160-161):

```cpp
        for (auto& b : m_bools)    b.second = false;
        for (auto& t : m_triggers) t.second = false;
        for (auto& i : m_ints)     i.second = 0;
        for (auto& f : m_floats)   f.second = 0.0f;
```

Añadir los cuatro accesores nuevos justo después de `isTriggerSet` (tras la línea 140):

```cpp
    void AnimatorComponent::setInt(const std::string& n, int v)
    {
        if (!hasParam(n, ParamType::Int)) return;
        m_ints[n] = v;
    }

    int AnimatorComponent::getInt(const std::string& n) const
    {
        auto it = m_ints.find(n);
        return it != m_ints.end() ? it->second : 0;
    }

    void AnimatorComponent::setFloat(const std::string& n, float v)
    {
        if (!hasParam(n, ParamType::Float)) return;
        m_floats[n] = v;
    }

    float AnimatorComponent::getFloat(const std::string& n) const
    {
        auto it = m_floats.find(n);
        return it != m_floats.end() ? it->second : 0.0f;
    }
```

Y en `conditionsMet`, añadir los dos `case` nuevos dentro del `switch` (tras el `case ConditionType::AnimationFinished`, línea 207):

```cpp
                case ConditionType::Int:
                    if (!evalCompare(getInt(c.paramName), c.compare, (int)c.threshold)) return false;
                    break;
                case ConditionType::Float:
                    if (!evalCompare(getFloat(c.paramName), c.compare, c.threshold)) return false;
                    break;
```

- [ ] **Step 5: Compilar y ejecutar los tests**

Run: `.\build.bat; if ($?) { .\build-ninja\engine\tests\dt_animator_tests.exe }`
Expected: `dt_animator_tests: OK`

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Core/AnimatorComponent.h engine/src/Core/AnimatorComponent.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): add Int and Float animator parameters"
```

---

### Task 2: Serialización JSON

**Files:**
- Modify: `engine/src/Core/Scene.cpp:52-78` (helpers de enum), `:100-108` (`animatorToJson`), `:161-168` (`animatorFromJson`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `AnimatorComponent::ParamType::{Int,Float}`, `ConditionType::{Int,Float}`, `Compare`, `Condition::compare`, `Condition::threshold` (Task 1).
- Produces: strings de JSON `"int"`, `"float"` para `type`, y `"greater"`, `"less"`, `"equals"`, `"notEquals"` para el campo `compare` de una condición.

- [ ] **Step 1: Escribir los tests que fallan**

Añadir en `engine/tests/animator_tests.cpp`, justo antes de `int main()`:

```cpp
// Round-trip completo de un grafo con parámetros y condiciones numéricas: si
// compare o threshold no sobrevivieran, la transición cargada dispararía cuando
// no debe (o nunca).
static void test_numeric_graph_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle"; idle.loop = true;
    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "ClipRun"; run.loop = true;
    a->addState(idle);
    a->addState(run);

    a->addParameter("combo", AnimatorComponent::ParamType::Int);
    a->addParameter("speed", AnimatorComponent::ParamType::Float);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition ci;
    ci.type      = AnimatorComponent::ConditionType::Int;
    ci.paramName = "combo";
    ci.compare   = AnimatorComponent::Compare::NotEquals;
    ci.threshold = 4.0f;
    t.conditions.push_back(ci);
    AnimatorComponent::Condition cf;
    cf.type      = AnimatorComponent::ConditionType::Float;
    cf.paramName = "speed";
    cf.compare   = AnimatorComponent::Compare::Less;
    cf.threshold = 2.5f;
    t.conditions.push_back(cf);
    a->addTransition(t);

    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->hasAnimator());
    if (!found->hasAnimator()) return;

    auto la = found->getAnimator();
    CHECK(la->parameters().size() == 2);
    CHECK(la->parameters()[0].type == AnimatorComponent::ParamType::Int);
    CHECK(la->parameters()[1].type == AnimatorComponent::ParamType::Float);

    CHECK(la->transitions().size() == 1);
    if (la->transitions().empty()) return;
    const auto& lc = la->transitions()[0].conditions;
    CHECK(lc.size() == 2);
    if (lc.size() != 2) return;

    CHECK(lc[0].type    == AnimatorComponent::ConditionType::Int);
    CHECK(lc[0].compare == AnimatorComponent::Compare::NotEquals);
    CHECK(nearlyEqual(lc[0].threshold, 4.0f));
    CHECK(lc[1].type    == AnimatorComponent::ConditionType::Float);
    CHECK(lc[1].compare == AnimatorComponent::Compare::Less);
    CHECK(nearlyEqual(lc[1].threshold, 2.5f));
}

// Retrocompatibilidad: una condición guardada antes de este cambio no lleva
// "compare" ni "threshold" y debe cargar con los defaults del struct, sin
// warnings ni excepciones de nlohmann.
static void test_condition_without_compare_fields_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle";
    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "ClipRun";
    a->addState(idle);
    a->addState(run);
    a->addParameter("running", AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    t.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", true });
    a->addTransition(t);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    // Una condición Bool no debe emitir los campos numéricos: son ruido en el
    // .scene y confundirían a quien lo lea a mano. El árbol es
    // root["root"]["children"], no un array plano (ver Scene::toJson).
    bool checkedEmission = false;
    for (const auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        const auto& cond = node["animator"]["transitions"][0]["conditions"][0];
        CHECK(!cond.contains("compare"));
        CHECK(!cond.contains("threshold"));
        checkedEmission = true;
    }
    CHECK(checkedEmission);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    const auto& lc = found->getAnimator()->transitions()[0].conditions[0];
    CHECK(lc.type    == AnimatorComponent::ConditionType::Bool);
    CHECK(lc.expected);
    CHECK(lc.compare == AnimatorComponent::Compare::Greater);   // default
    CHECK(nearlyEqual(lc.threshold, 0.0f));                     // default
}
```

Registrarlos en `main()`, después de `test_scene_without_animator_block_loads(pm, am);`:

```cpp
    test_numeric_graph_survives_scene_round_trip(pm, am);
    test_condition_without_compare_fields_loads(pm, am);
```

- [ ] **Step 2: Ejecutar el build/test para verificar que falla**

Run: `.\build.bat; if ($?) { .\build-ninja\engine\tests\dt_animator_tests.exe }`
Expected: compila, pero el ejecutable imprime `FAIL:` en las líneas de `compare`/`threshold` y termina con `dt_animator_tests: N FAILURES` (los helpers `paramTypeToStr`/`condTypeToStr` aún devuelven `"bool"` para Int/Float, y `compare`/`threshold` ni se escriben ni se leen).

- [ ] **Step 3: Extender los helpers de enum**

En `engine/src/Core/Scene.cpp`, sustituir `paramTypeToStr` / `paramTypeFromStr` / `condTypeToStr` / `condTypeFromStr` (líneas 52-78) por:

```cpp
    const char* paramTypeToStr(AnimatorComponent::ParamType t)
    {
        switch (t)
        {
            case AnimatorComponent::ParamType::Trigger: return "trigger";
            case AnimatorComponent::ParamType::Int:     return "int";
            case AnimatorComponent::ParamType::Float:   return "float";
            default:                                    return "bool";
        }
    }

    AnimatorComponent::ParamType paramTypeFromStr(const std::string& s)
    {
        if (s == "trigger") return AnimatorComponent::ParamType::Trigger;
        if (s == "int")     return AnimatorComponent::ParamType::Int;
        if (s == "float")   return AnimatorComponent::ParamType::Float;
        return AnimatorComponent::ParamType::Bool;
    }

    const char* condTypeToStr(AnimatorComponent::ConditionType t)
    {
        switch (t)
        {
            case AnimatorComponent::ConditionType::Trigger:           return "trigger";
            case AnimatorComponent::ConditionType::AnimationFinished: return "animationFinished";
            case AnimatorComponent::ConditionType::Int:               return "int";
            case AnimatorComponent::ConditionType::Float:             return "float";
            default:                                                  return "bool";
        }
    }

    AnimatorComponent::ConditionType condTypeFromStr(const std::string& s)
    {
        if (s == "trigger")           return AnimatorComponent::ConditionType::Trigger;
        if (s == "animationFinished") return AnimatorComponent::ConditionType::AnimationFinished;
        if (s == "int")               return AnimatorComponent::ConditionType::Int;
        if (s == "float")             return AnimatorComponent::ConditionType::Float;
        return AnimatorComponent::ConditionType::Bool;
    }

    const char* compareToStr(AnimatorComponent::Compare c)
    {
        switch (c)
        {
            case AnimatorComponent::Compare::Less:      return "less";
            case AnimatorComponent::Compare::Equals:    return "equals";
            case AnimatorComponent::Compare::NotEquals: return "notEquals";
            default:                                    return "greater";
        }
    }

    AnimatorComponent::Compare compareFromStr(const std::string& s)
    {
        if (s == "less")      return AnimatorComponent::Compare::Less;
        if (s == "equals")    return AnimatorComponent::Compare::Equals;
        if (s == "notEquals") return AnimatorComponent::Compare::NotEquals;
        return AnimatorComponent::Compare::Greater;
    }
```

- [ ] **Step 4: Escribir y leer los campos nuevos**

En `animatorToJson`, sustituir el cuerpo del bucle de condiciones (líneas 101-108) por:

```cpp
            for (const auto& c : t.conditions)
            {
                nlohmann::json cj = { {"type", condTypeToStr(c.type)} };
                if (c.type != AnimatorComponent::ConditionType::AnimationFinished)
                    cj["param"] = c.paramName;
                if (c.type == AnimatorComponent::ConditionType::Bool)
                    cj["expected"] = c.expected;
                // Solo las numéricas: en una Bool serían ruido en el .scene.
                if (c.type == AnimatorComponent::ConditionType::Int ||
                    c.type == AnimatorComponent::ConditionType::Float)
                {
                    cj["compare"]   = compareToStr(c.compare);
                    cj["threshold"] = c.threshold;
                }
                conds.push_back(cj);
            }
```

En `animatorFromJson`, sustituir la construcción de la condición (líneas 163-167) por:

```cpp
                        AnimatorComponent::Condition cond;
                        cond.type      = condTypeFromStr(c.value("type", std::string("bool")));
                        cond.paramName = c.value("param", std::string());
                        cond.expected  = c.value("expected", true);
                        // Ausentes en escenas anteriores a los parámetros
                        // numéricos: caen en los defaults del struct.
                        cond.compare   = compareFromStr(c.value("compare", std::string("greater")));
                        cond.threshold = c.value("threshold", 0.0f);
                        tr.conditions.push_back(cond);
```

- [ ] **Step 5: Compilar y ejecutar los tests**

Run: `.\build.bat; if ($?) { .\build-ninja\engine\tests\dt_animator_tests.exe }`
Expected: `dt_animator_tests: OK`

- [ ] **Step 6: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/animator_tests.cpp
git commit -m "feat(anim): serialize numeric parameters and comparisons"
```

---

### Task 3: UI — lista de parámetros con valores y condiciones numéricas

**Files:**
- Modify: `engine/include/DonTopo/Core/AnimatorComponent.h` (declarar `paramTypeLabel`)
- Modify: `engine/src/Core/AnimatorComponent.cpp` (definir `paramTypeLabel`)
- Modify: `engine/src/Editor/AnimatorPanel.cpp:53-61` (`condLabel`), `:97-139` (`drawParameterList`), `:377-418` (popup de condiciones)
- Modify: `engine/include/DonTopo/Editor/AnimatorPanel.h:62` (comentario de `m_newParamType`)
- Modify: `engine/src/Editor/PropertiesPanel.cpp:1167-1169` (resumen)

**Interfaces:**
- Consumes: todo lo de Task 1 (`ParamType`, `Compare`, `setInt`/`getInt`/`setFloat`/`getFloat`).
- Produces: `const char* DonTopo::paramTypeLabel(AnimatorComponent::ParamType)` — devuelve `"bool"`, `"trigger"`, `"int"` o `"float"`, sin paréntesis; quien los quiera los pone.

Esta tarea no lleva tests automáticos: son widgets de ImGui, y el proyecto no tiene arnés de UI. La verificación es manual, en el paso final.

- [ ] **Step 1: Añadir el helper de etiqueta compartido**

En `engine/include/DonTopo/Core/AnimatorComponent.h`, después del cierre de la clase (antes del `}` del namespace):

```cpp
    // Etiqueta legible de un tipo de parámetro, compartida por AnimatorPanel y
    // PropertiesPanel. Vive aquí y no en el editor porque con cuatro tipos el
    // ternario "trigger : bool" que ambos duplicaban deja de funcionar, y dos
    // copias de un switch se desincronizan al añadir el quinto tipo.
    //
    // NO reutiliza (ni la reutiliza) paramTypeToStr de Scene.cpp: aquello es el
    // formato del .scene y no puede cambiar al retocar un texto de la UI.
    const char* paramTypeLabel(AnimatorComponent::ParamType t);
```

En `engine/src/Core/AnimatorComponent.cpp`, al final del namespace:

```cpp
    const char* paramTypeLabel(AnimatorComponent::ParamType t)
    {
        switch (t)
        {
            case AnimatorComponent::ParamType::Trigger: return "trigger";
            case AnimatorComponent::ParamType::Int:     return "int";
            case AnimatorComponent::ParamType::Float:   return "float";
            default:                                    return "bool";
        }
    }
```

- [ ] **Step 2: Reescribir la lista de parámetros del panel**

En `engine/src/Editor/AnimatorPanel.cpp`, sustituir el cuerpo de `drawParameterList` (líneas 99-138) por:

```cpp
    auto anim = go->getAnimator();

    // 260 y no 200: con el widget de valor de int/float al lado del nombre, a
    // 200 el DragFloat se comía el botón de borrado.
    ImGui::BeginChild("params", ImVec2(260, 0), true);
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();

    std::string toRemove;
    for (const auto& p : anim->parameters())
    {
        ImGui::PushID(p.name.c_str());
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", paramTypeLabel(p.type));
        ImGui::SameLine();

        // Valor editable in situ: en Play permite provocar una transición a mano
        // sin escribir Lua, que es como se depura un grafo.
        ImGui::SetNextItemWidth(70);
        switch (p.type)
        {
            case AnimatorComponent::ParamType::Bool:
            {
                bool v = anim->getBool(p.name);
                if (ImGui::Checkbox("##val", &v)) anim->setBool(p.name, v);
                break;
            }
            case AnimatorComponent::ParamType::Trigger:
                // Un trigger no tiene valor que mostrar: se arma y lo consume la
                // primera transición que lo mire (ver consumeTriggers).
                if (ImGui::SmallButton("Set")) anim->setTrigger(p.name);
                break;
            case AnimatorComponent::ParamType::Int:
            {
                int v = anim->getInt(p.name);
                if (ImGui::DragInt("##val", &v)) anim->setInt(p.name, v);
                break;
            }
            case AnimatorComponent::ParamType::Float:
            {
                float v = anim->getFloat(p.name);
                if (ImGui::DragFloat("##val", &v, 0.01f)) anim->setFloat(p.name, v);
                break;
            }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("X")) toRemove = p.name;
        ImGui::PopID();
    }
    // Diferido: borrar dentro del for-range invalidaría el iterador.
    if (!toRemove.empty())
    {
        anim->removeParameter(toRemove);
        ctx.pushLog("Animator: parámetro '" + toRemove + "' eliminado");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##newparam", m_newParamName, sizeof(m_newParamName));
    // El orden coincide con el del enum ParamType, así que el índice del combo
    // castea directo: si el enum crece, esta lista crece con él.
    const char* types[] = { "bool", "trigger", "int", "float" };
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##newparamtype", &m_newParamType, types, IM_ARRAYSIZE(types));
    if (ImGui::Button("Add Parameter") && m_newParamName[0] != '\0')
    {
        anim->addParameter(m_newParamName, (AnimatorComponent::ParamType)m_newParamType);
        ctx.pushLog(std::string("Animator: parámetro '") + m_newParamName + "' añadido");
        m_newParamName[0] = '\0';
    }

    ImGui::EndChild();
```

En `engine/include/DonTopo/Editor/AnimatorPanel.h`, actualizar el comentario de la línea 62:

```cpp
    int  m_newParamType     = 0;   // índice en ParamType: 0 bool, 1 trigger, 2 int, 3 float
```

- [ ] **Step 3: Extender `condLabel` y el popup de condiciones**

En `engine/src/Editor/AnimatorPanel.cpp`, sustituir `condLabel` (líneas 53-61) por:

```cpp
    const char* condLabel(AnimatorComponent::ConditionType t)
    {
        switch (t)
        {
            case AnimatorComponent::ConditionType::Trigger:           return "trigger";
            case AnimatorComponent::ConditionType::AnimationFinished: return "animation finished";
            case AnimatorComponent::ConditionType::Int:               return "int";
            case AnimatorComponent::ConditionType::Float:             return "float";
            default:                                                  return "bool";
        }
    }

    // Etiquetas de Compare en el orden del enum. Float solo expone las dos
    // primeras (Greater, Less): un == sobre float casi nunca dispara y sería una
    // trampa ofrecerlo en el combo.
    const char* kCompareLabels[] = { ">", "<", "==", "!=" };
```

Sustituir el cuerpo del bucle de condiciones existentes (líneas 379-394) por:

```cpp
        ImGui::PushID((int)c);
        auto& cond = tr.conditions[c];
        ImGui::Text("%s", condLabel(cond.type));
        if (cond.type != AnimatorComponent::ConditionType::AnimationFinished)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(cond.paramName.c_str());
        }
        if (cond.type == AnimatorComponent::ConditionType::Bool)
        {
            ImGui::SameLine();
            ImGui::Checkbox("expected", &cond.expected);
        }
        if (cond.type == AnimatorComponent::ConditionType::Int ||
            cond.type == AnimatorComponent::ConditionType::Float)
        {
            const bool isFloat = cond.type == AnimatorComponent::ConditionType::Float;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50);
            int op = (int)cond.compare;
            // Float recorta el combo a Greater/Less; Int ofrece los cuatro.
            if (ImGui::Combo("##cmp", &op, kCompareLabels, isFloat ? 2 : 4))
                cond.compare = (AnimatorComponent::Compare)op;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (isFloat)
            {
                ImGui::DragFloat("##thr", &cond.threshold, 0.01f);
            }
            else
            {
                // El umbral vive en float pa no duplicar el campo; la UI de Int
                // pasa por un int temporal, así que nunca entra un valor con
                // parte fraccionaria que el evaluador truncaría a espaldas del
                // usuario.
                int thr = (int)cond.threshold;
                if (ImGui::DragInt("##thr", &thr)) cond.threshold = (float)thr;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) toRemove = (int)c;
        ImGui::PopID();
```

Sustituir el cuerpo del `Selectable("Add: ...")` de parámetros (líneas 409-415) por:

```cpp
            AnimatorComponent::Condition cond;
            // El tipo del parámetro decide el de la condición 1:1.
            switch (p.type)
            {
                case AnimatorComponent::ParamType::Trigger:
                    cond.type = AnimatorComponent::ConditionType::Trigger; break;
                case AnimatorComponent::ParamType::Int:
                    cond.type = AnimatorComponent::ConditionType::Int;     break;
                case AnimatorComponent::ParamType::Float:
                    cond.type = AnimatorComponent::ConditionType::Float;   break;
                default:
                    cond.type = AnimatorComponent::ConditionType::Bool;    break;
            }
            cond.paramName = p.name;
            cond.expected  = true;
            tr.conditions.push_back(cond);
```

- [ ] **Step 4: Actualizar el resumen de Properties**

En `engine/src/Editor/PropertiesPanel.cpp`, sustituir las líneas 1167-1169 por:

```cpp
        for (const auto& p : anim->parameters())
            ImGui::BulletText("%s (%s)", p.name.c_str(), paramTypeLabel(p.type));
```

- [ ] **Step 5: Compilar y verificar que los tests siguen verdes**

Run: `.\build.bat; if ($?) { .\build-ninja\engine\tests\dt_animator_tests.exe }`
Expected: compila sin errores y `dt_animator_tests: OK`

- [ ] **Step 6: Verificación manual en el editor**

Run: `.\build-ninja\sandbox\DonTopoSandbox.exe` (si el nombre del ejecutable difiere, buscarlo con `Get-ChildItem build-ninja -Recurse -Filter *.exe`)

Comprobar, con un GameObject que tenga Animator y el panel Animator abierto:
1. El combo "Add Parameter" ofrece las cuatro opciones y crea el tipo elegido.
2. Cada fila muestra su widget: checkbox, botón `Set`, `DragInt`, `DragFloat`, y el botón `X` es visible en todas.
3. En el popup de condiciones de una transición, `Add: <param int>` crea una fila con combo de 4 comparadores y `DragInt`; la de un float, combo de 2 y `DragFloat`.
4. Guardar la escena, recargarla, y confirmar que comparador y umbral se conservan.
5. En Play, mover el `DragInt` por encima del umbral dispara la transición.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Core/AnimatorComponent.h engine/src/Core/AnimatorComponent.cpp engine/src/Editor/AnimatorPanel.cpp engine/include/DonTopo/Editor/AnimatorPanel.h engine/src/Editor/PropertiesPanel.cpp
git commit -m "feat(anim): edit numeric parameters and conditions in the panel"
```

---

### Task 4: Bindings de Lua y documentación

**Files:**
- Modify: `engine/src/Scripting/ScriptBindings.cpp:366-371` (usertype `Animator`)
- Modify: `README.md` (sección del Animator)

**Interfaces:**
- Consumes: `setInt`/`getInt`/`setFloat`/`getFloat` (Task 1).
- Produces: métodos Lua `Animator:SetInt(name, v)`, `Animator:GetInt(name)`, `Animator:SetFloat(name, v)`, `Animator:GetFloat(name)`.

- [ ] **Step 1: Añadir los cuatro métodos al usertype**

En `engine/src/Scripting/ScriptBindings.cpp`, sustituir el `new_usertype<LuaAnimator>` (líneas 366-371) por:

```cpp
            lua.new_usertype<LuaAnimator>("Animator",
                sol::no_constructor,
                "SetBool",    [animOf](const LuaAnimator& c, const std::string& n, bool v) { animOf(c)->setBool(n, v); },
                "GetBool",    [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getBool(n); },
                "SetTrigger", [animOf](const LuaAnimator& c, const std::string& n) { animOf(c)->setTrigger(n); },
                // Numéricos: mismo contrato que los bools — un nombre no
                // declarado (o de otro tipo) se ignora en el setter y devuelve 0
                // en el getter, nunca lanza.
                "SetInt",     [animOf](const LuaAnimator& c, const std::string& n, int v) { animOf(c)->setInt(n, v); },
                "GetInt",     [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getInt(n); },
                "SetFloat",   [animOf](const LuaAnimator& c, const std::string& n, float v) { animOf(c)->setFloat(n, v); },
                "GetFloat",   [animOf](const LuaAnimator& c, const std::string& n) { return animOf(c)->getFloat(n); },
                "GetState",   [animOf](const LuaAnimator& c) { return animOf(c)->currentStateName(); });
```

- [ ] **Step 2: Documentar en el README**

Localizar la sección del Animator en `README.md` (buscar con `Grep` el patrón `SetTrigger`) y añadir, junto a los métodos ya documentados, las entradas nuevas siguiendo el formato de la tabla o lista que ya use ese bloque:

```
- `SetInt(name, value)` / `GetInt(name)` — parámetros enteros del grafo.
- `SetFloat(name, value)` / `GetFloat(name)` — parámetros float del grafo.
```

Añadir también, en la descripción de los tipos de parámetro de esa sección, que existen cuatro: `bool`, `trigger`, `int` y `float`, y que las condiciones numéricas comparan con `>`, `<`, `==` o `!=` (las dos últimas solo se ofrecen para `int`).

- [ ] **Step 3: Compilar y verificar los tests**

Run: `.\build.bat; if ($?) { .\build-ninja\engine\tests\dt_animator_tests.exe }`
Expected: compila y `dt_animator_tests: OK`

- [ ] **Step 4: Verificación manual desde Lua**

Con un GameObject que tenga Animator y un script con un parámetro int llamado `combo`:

```lua
function OnUpdate(dt)
    local anim = self:GetComponent("Animator")
    anim:SetInt("combo", 3)
    print(anim:GetInt("combo"))
end
```

Expected: la consola imprime `3` en Play, y una transición con condición `combo > 2` dispara.

- [ ] **Step 5: Commit**

```bash
git add engine/src/Scripting/ScriptBindings.cpp README.md
git commit -m "feat(anim): expose numeric animator parameters to Lua"
```

---

## Cobertura de la spec

| Sección de la spec | Tarea |
|---|---|
| 1. Core (enums, `Condition`, mapas, API, `evalCompare`, `reset`, `removeParameter`) | 1 |
| 2. Serialización (`paramType`, `condType`, `compare`, back-compat) | 2 |
| 3. UI (lista con valores, combo de 4 tipos, popup de condiciones, `paramTypeLabel`) | 3 |
| 4. Lua (`SetInt`/`GetInt`/`SetFloat`/`GetFloat`, README) | 4 |
| 5. Resumen de Properties | 3 |
| Tests 1-6 | 1 (1-4), 2 (5-6) |
