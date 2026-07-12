# Lua Scripting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Módulo de scripting Lua completo: ScriptManager + ScriptComponent + binding layer (Entity/Transform/Scene/Input/Audio/Log) + ciclo de vida Unity-style + hot reload + props serializables con UI auto-generada.

**Architecture:** Una sola VM Lua (`sol::state`) propiedad de `ScriptManager`. Los `.lua` de `Scripts/` definen tablas-clase globales (nombre = filename); las instancias son tablas nuevas con `__index` → clase. `ScriptComponent` (varios por GameObject) guarda instancia + overrides de props. Los callbacks solo corren en Play Mode, orquestados por `ScriptManager::update` desde el frame loop.

**Tech Stack:** Lua 5.4 (FetchContent, lib estática propia), sol2 v3.3.0 (header-only, INTERFACE propia estilo ImGuiFileDialog), nlohmann/json (ya presente), ImGui (ya presente).

**Spec:** `docs/superpowers/specs/2026-07-12-lua-scripting-design.md`

## Global Constraints

- Build SIEMPRE vía `./configure.bat` (solo si cambia CMake) y `./build.bat` en PowerShell — nunca cmake/ninja crudo en Bash.
- C++20, comentarios en español, estilo del código circundante (4 espacios, llaves estilo repo).
- Commits estilo repo: `feat(script): ...` / `docs(...)` en español, cuerpo solo si el porqué no es obvio.
- `sol/sol.hpp` NUNCA se incluye desde `GameObject.h` (se incluye en media base de código) — solo desde `ScriptComponent.h`, `ScriptManager.h`, `ScriptBindings.*` y los `.cpp` que lo necesiten.
- Un script roto (compilación o runtime) jamás tira el motor: siempre `protected_function` / `safe_script_file` + log.
- No hay framework de tests: la verificación de cada task es build limpio; la verificación funcional final es Task 11 + GUI manual.

---

### Task 1: Dependencias CMake (Lua 5.4 + sol2)

**Files:**
- Create: `cmake/Lua.cmake`
- Modify: `CMakeLists.txt` (raíz, tras el bloque nlohmann_json, ~línea 151)
- Modify: `engine/CMakeLists.txt` (target_link_libraries)

**Interfaces:**
- Produces: targets `lua_lib` (STATIC, Lua 5.4 core) y `sol2_lib` (INTERFACE, incluye lua_lib). `#include <sol/sol.hpp>` y `#include <lua.h>` disponibles para DonTopoEngine.

- [ ] **Step 1: Crear `cmake/Lua.cmake`**

```cmake
# cmake/Lua.cmake
# Lua 5.4 desde el mirror oficial de GitHub vía FetchContent.
# El repo no trae CMakeLists (solo Makefile), así que se define aquí una
# lib estática propia con los .c del core — mismo patrón que PhysX.cmake.
# Se excluyen lua.c/luac.c (mains del intérprete standalone) y onelua.c
# (amalgamación, duplicaría símbolos).

# El proyecto raíz es LANGUAGES CXX; Lua es C puro.
enable_language(C)

include(FetchContent)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
endif()

set(_LUA_CORE_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c
    ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c
    lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c lparser.c
    lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c
    lutf8lib.c lvm.c lzio.c
)
list(TRANSFORM _LUA_CORE_SOURCES PREPEND "${lua_SOURCE_DIR}/")

add_library(lua_lib STATIC ${_LUA_CORE_SOURCES})
target_include_directories(lua_lib PUBLIC ${lua_SOURCE_DIR})
```

- [ ] **Step 2: Añadir sol2 + include de Lua.cmake al `CMakeLists.txt` raíz**

Insertar tras el bloque nlohmann_json (línea ~150), antes de `include(cmake/PhysX.cmake)`:

```cmake
# Lua 5.4 — VM de scripting (compilado desde fuente, ver cmake/Lua.cmake)
include(cmake/Lua.cmake)

# sol2 — binding C++/Lua (header-only). Populate en vez de MakeAvailable,
# mismo patrón que ImGuiFileDialog: evitamos su CMakeLists (tests/opciones
# que no necesitamos) y definimos una INTERFACE propia.
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(sol2)
if(NOT sol2_POPULATED)
    FetchContent_Populate(sol2)
endif()

add_library(sol2_lib INTERFACE)
target_include_directories(sol2_lib INTERFACE ${sol2_SOURCE_DIR}/include)
target_link_libraries(sol2_lib INTERFACE lua_lib)
# Chequeos de tipo/stack de sol2 activos siempre: los errores de binding
# aparecen como errores Lua legibles en vez de UB.
target_compile_definitions(sol2_lib INTERFACE SOL_ALL_SAFETIES_ON=1)
```

- [ ] **Step 3: Linkear en `engine/CMakeLists.txt`**

En `target_link_libraries(DonTopoEngine PUBLIC ...)` añadir al final de la lista:

```cmake
        sol2_lib
```

- [ ] **Step 4: Configurar y compilar**

Run (PowerShell): `./configure.bat; if ($?) { ./build.bat }`
Expected: configure descarga lua/sol2, build sin errores.

- [ ] **Step 5: Smoke test de include** — añadir temporalmente al final de `engine/src/Engine.cpp`:

```cpp
#include <sol/sol.hpp>
namespace { [[maybe_unused]] void dtLuaSmoke() { sol::state s; s.open_libraries(sol::lib::base); s.script("x = 1 + 1"); } }
```

Run: `./build.bat` → sin errores. **Revertir el cambio de Engine.cpp** tras verificar.

- [ ] **Step 6: Commit**

```bash
git add cmake/Lua.cmake CMakeLists.txt engine/CMakeLists.txt
git commit -m "feat(script): añade Lua 5.4 y sol2 vía FetchContent"
```

---

### Task 2: ScriptComponent + GameObject::m_scripts

**Files:**
- Create: `engine/include/DonTopo/ScriptComponent.h`
- Create: `engine/src/ScriptComponent.cpp`
- Modify: `engine/include/DonTopo/GameObject.h`
- Modify: `engine/src/GameObject.cpp`
- Modify: `engine/CMakeLists.txt` (añadir `src/ScriptComponent.cpp`)

**Interfaces:**
- Produces:
  - `DonTopo::ScriptValue` = `std::variant<double, bool, std::string>`
  - `ScriptComponent{ std::string scriptName; GameObject* owner; sol::table instance; bool started; bool hasError; bool pendingRemove; bool hasAwake/hasStart/hasUpdate/hasFixedUpdate/hasLateUpdate/hasOnDestroy; std::map<std::string, ScriptValue> overrides; }`
  - `GameObject::addScript(std::unique_ptr<ScriptComponent>)`, `removeScript(ScriptComponent*)`, `getScripts()` (mutable y const), `hasScripts()`
- CRÍTICO: `GameObject.h` solo forward-declara `ScriptComponent`. Eso obliga a declarar `~GameObject()` out-of-line, lo que suprime los moves implícitos — y `Scene::fromJson` hace `m_root = std::move(newRoot)`. Hay que re-declarar move ctor/assign y defaultearlos en el .cpp.

- [ ] **Step 1: Crear `ScriptComponent.h`**

```cpp
#pragma once
#include <string>
#include <map>
#include <variant>
#include <sol/sol.hpp>

namespace DonTopo {

class GameObject;

// Valor de una prop serializable de script (los 3 tipos que detecta
// ScriptManager en la tabla clase).
using ScriptValue = std::variant<double, bool, std::string>;

// Un script Lua adjunto a un GameObject. La instancia (tabla Lua) solo
// existe en Play Mode; en Edit Mode el componente es solo nombre + overrides.
class ScriptComponent {
public:
    ScriptComponent(std::string name, GameObject* ownerGo)
        : scriptName(std::move(name)), owner(ownerGo) {}

    ScriptComponent(const ScriptComponent&)            = delete;
    ScriptComponent& operator=(const ScriptComponent&) = delete;

    std::string scriptName;
    GameObject* owner = nullptr;

    // Tabla instancia Lua — inválida (default) fuera de Play Mode.
    sol::table instance;
    bool started = false;
    // Error runtime en un callback: deja de recibir callbacks hasta hot
    // reload o Stop (evita spam de errores y crash loop).
    bool hasError = false;
    // RemoveComponent desde Lua en mitad de Update se difiere al final del
    // frame (misma razón que la cola de destroy de entities).
    bool pendingRemove = false;

    // Cache de qué callbacks define el script — se calcula una vez al
    // instanciar, no cada frame (spec).
    bool hasAwake = false, hasStart = false, hasUpdate = false,
         hasFixedUpdate = false, hasLateUpdate = false, hasOnDestroy = false;

    // Props editadas en el editor que difieren del default del .lua.
    // Solo esto se serializa — los defaults viven en el script.
    std::map<std::string, ScriptValue> overrides;
};

} // namespace DonTopo
```

- [ ] **Step 2: Crear `ScriptComponent.cpp`** (solo ancla de compilación del header):

```cpp
#include "DonTopo/ScriptComponent.h"
```

- [ ] **Step 3: Modificar `GameObject.h`**

Forward declaration + miembros/métodos (NO incluir ScriptComponent.h):

```cpp
// tras los includes existentes, dentro de namespace DonTopo:
class ScriptComponent;
```

En la sección pública, tras el bloque de audio clip:

```cpp
            // Scripts Lua — a diferencia del resto de slots, vector: se
            // permiten varios scripts por GameObject (incluso repetidos).
            void addScript(std::unique_ptr<ScriptComponent> script);
            void removeScript(ScriptComponent* script);
            std::vector<std::unique_ptr<ScriptComponent>>&       getScripts()       { return m_scripts; }
            const std::vector<std::unique_ptr<ScriptComponent>>& getScripts() const { return m_scripts; }
            bool hasScripts() const { return !m_scripts.empty(); }
```

Constructor/destructor/moves (ScriptComponent es tipo incompleto aquí → el
destructor y los moves deben instanciarse en el .cpp; sin los `= default`
del .cpp, `Scene::fromJson` (`m_root = std::move(newRoot)`) dejaría de
compilar porque declarar ~GameObject suprime los moves implícitos):

```cpp
            explicit GameObject(std::string name = "");
            ~GameObject();
            GameObject(GameObject&&) noexcept;
            GameObject& operator=(GameObject&&) noexcept;
```

Miembro privado:

```cpp
            std::vector<std::unique_ptr<ScriptComponent>> m_scripts;
```

- [ ] **Step 4: Modificar `GameObject.cpp`**

```cpp
#include "DonTopo/ScriptComponent.h"
#include <algorithm>

// ...

GameObject::~GameObject() = default;
GameObject::GameObject(GameObject&&) noexcept = default;
GameObject& GameObject::operator=(GameObject&&) noexcept = default;

void GameObject::addScript(std::unique_ptr<ScriptComponent> script)
{
    m_scripts.push_back(std::move(script));
}

void GameObject::removeScript(ScriptComponent* script)
{
    m_scripts.erase(
        std::remove_if(m_scripts.begin(), m_scripts.end(),
            [script](const std::unique_ptr<ScriptComponent>& s) { return s.get() == script; }),
        m_scripts.end());
}
```

- [ ] **Step 5: Añadir `src/ScriptComponent.cpp` a `engine/CMakeLists.txt`** (lista de sources, tras `src/AudioClipComponent.cpp`).

- [ ] **Step 6: Build**

Run: `./configure.bat; if ($?) { ./build.bat }`
Expected: sin errores (incluye que Scene.cpp siga compilando el move-assignment).

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/ScriptComponent.h engine/src/ScriptComponent.cpp engine/include/DonTopo/GameObject.h engine/src/GameObject.cpp engine/CMakeLists.txt
git commit -m "feat(script): añade ScriptComponent y slot m_scripts en GameObject"
```

---

### Task 3: Serialización de scripts + Scene::cloneGameObject

**Files:**
- Modify: `engine/src/Scene.cpp` (nodeToJson, nodeFromJson, y nuevo cloneGameObject)
- Modify: `engine/include/DonTopo/Scene.h`

**Interfaces:**
- Consumes: `ScriptComponent` (Task 2).
- Produces:
  - JSON por GameObject: `"scripts": [{ "name": "Enemy", "overrides": { "speed": 12, "invincible": true } }]` — solo overrides.
  - `GameObject* Scene::cloneGameObject(GameObject* src, GameObject* parent, PhysicsManager& physics, AudioManager& audio)` — deep clone vía roundtrip nodeToJson→nodeFromJson; render indices reseteados a -1 (el caller registra GPU); nullptr si src es root o el clone falla.

- [ ] **Step 1: En `Scene.cpp`, incluir el header y añadir serialización en `nodeToJson`** (tras el bloque `audioClip`):

```cpp
#include "DonTopo/ScriptComponent.h"   // junto a los includes existentes
```

```cpp
        if (node.hasScripts())
        {
            auto arr = nlohmann::json::array();
            for (const auto& s : node.getScripts())
            {
                nlohmann::json ov = nlohmann::json::object();
                for (const auto& [key, val] : s->overrides)
                {
                    std::visit([&](auto&& v) {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, double>)
                        {
                            // Preserva enteros como enteros en el JSON
                            if (v == std::floor(v) && std::abs(v) < 1e15)
                                ov[key] = static_cast<int64_t>(v);
                            else
                                ov[key] = v;
                        }
                        else
                            ov[key] = v;
                    }, val);
                }
                arr.push_back({ {"name", s->scriptName}, {"overrides", std::move(ov)} });
            }
            j["scripts"] = std::move(arr);
        }
```

(`#include <cmath>` si no está.)

- [ ] **Step 2: Deserialización en `nodeFromJson`** (tras el bloque `audioClip`):

```cpp
        if (j.contains("scripts"))
        {
            for (const auto& sj : j["scripts"])
            {
                auto comp = std::make_unique<DonTopo::ScriptComponent>(
                    sj.at("name").get<std::string>(), node);
                if (sj.contains("overrides"))
                {
                    for (const auto& [key, val] : sj["overrides"].items())
                    {
                        if (val.is_boolean())     comp->overrides[key] = val.get<bool>();
                        else if (val.is_string()) comp->overrides[key] = val.get<std::string>();
                        else if (val.is_number()) comp->overrides[key] = val.get<double>();
                        // Otros tipos: ignorados (no son props serializables)
                    }
                }
                // Nota: si el script ya no existe en Scripts/, el componente
                // se conserva igual ("missing script", spec) — la UI lo
                // señala; los overrides no se pierden al re-guardar.
                node->addScript(std::move(comp));
            }
        }
```

- [ ] **Step 3: `Scene::cloneGameObject`** — declaración en `Scene.h` (tras `removeGameObject`):

```cpp
            // Deep clone de src (transform, mesh, colliders, audio, scripts
            // con overrides) como hijo nuevo de parent (o del padre de src si
            // parent es nullptr). Los render indices del subtree quedan a -1:
            // el caller debe registrar los meshes en GPU. nullptr si src es
            // la raíz o la reconstrucción falla.
            GameObject* cloneGameObject(GameObject* src, GameObject* parent,
                                        PhysicsManager& physics, AudioManager& audio);
```

Implementación en `Scene.cpp`:

```cpp
    GameObject* Scene::cloneGameObject(GameObject* src, GameObject* parent,
                                       PhysicsManager& physics, AudioManager& audio)
    {
        if (!src || src == &m_root) return nullptr;

        GameObject* target = parent ? parent : (src->parent ? src->parent : &m_root);
        nlohmann::json j = nodeToJson(*src);

        GameObject* clone = target->addChild(src->name + " (Clone)");
        try
        {
            nodeFromJson(j, clone, target->worldTransform, physics, audio);
        }
        catch (const nlohmann::json::exception&)
        {
            removeGameObject(clone);
            return nullptr;
        }

        clone->traverse([](GameObject* n) {
            n->staticRenderIndex  = -1;
            n->skinnedRenderIndex = -1;
        });
        return clone;
    }
```

- [ ] **Step 4: Limpiar scripts en `Scene::shutdown`** (junto a los demás componentes):

```cpp
            go->getScripts().clear();
```

- [ ] **Step 5: Build** — `./build.bat`, sin errores.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Scene.h engine/src/Scene.cpp
git commit -m "feat(script): serializa scripts (name+overrides) y añade Scene::cloneGameObject"
```

---

### Task 4: ScriptManager core (registry, props, createInstance)

**Files:**
- Create: `engine/include/DonTopo/ScriptManager.h`
- Create: `engine/src/ScriptManager.cpp`
- Modify: `engine/CMakeLists.txt` (añadir `src/ScriptManager.cpp`)

**Interfaces:**
- Consumes: `ScriptComponent`, `ScriptValue` (Task 2).
- Produces (usadas por tasks posteriores):
  - `struct ScriptProp { std::string name; ScriptValue defaultValue; bool isInteger; }`
  - `struct ScriptClass { sol::table classTable; std::filesystem::path path; std::filesystem::file_time_type mtime; std::vector<ScriptProp> props; }`
  - `void ScriptManager::init(const std::string& scriptsDir)` — escanea recursivo y carga todo.
  - `bool loadScript(const std::filesystem::path&)`
  - `bool hasClass(const std::string&) const`, `const std::map<std::string, ScriptClass>& getRegistry() const`
  - `const std::string* getCompileError(const std::string& name) const` (nullptr si no hay error)
  - `sol::table createInstance(const std::string& name, const std::map<std::string, ScriptValue>& overrides)` — tabla inválida si la clase no existe.
  - `void setLogCallback(std::function<void(const std::string&)>)`
  - `sol::state& lua()`
- Lifecycle/bindings/hot-reload llegan en Tasks 5–9; aquí solo carga+factory.

- [ ] **Step 1: `ScriptManager.h`**

```cpp
#pragma once
#include <sol/sol.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "DonTopo/ScriptComponent.h"

namespace DonTopo {

class Scene;
class GameObject;
class PhysicsManager;
class AudioManager;

// Una prop serializable detectada en la tabla clase de un script.
struct ScriptProp {
    std::string name;
    ScriptValue defaultValue;
    // Lua 5.4 distingue 5 (integer) de 5.0 (float); la UI usa DragInt vs
    // DragFloat según esto.
    bool isInteger = false;
};

struct ScriptClass {
    sol::table classTable;
    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
    std::vector<ScriptProp> props;   // orden alfabético, estable pa la UI
};

// Dueño de la única VM Lua del motor. Carga/registra/instancia scripts.
// El motor nunca llama a Lua fuera de esta clase.
class ScriptManager {
public:
    ScriptManager();
    ~ScriptManager();
    ScriptManager(const ScriptManager&)            = delete;
    ScriptManager& operator=(const ScriptManager&) = delete;

    // Abre libs seguras (base/math/string/table), registra bindings y carga
    // todos los .lua de scriptsDir (recursivo). Los punteros son
    // no-propietarios, mismo patrón que EditorUI::setPhysicsManager.
    void init(const std::string& scriptsDir);
    void setScene(Scene* scene)                    { m_scene = scene; }
    void setPhysicsManager(PhysicsManager* p)      { m_physics = p; }
    void setAudioManager(AudioManager* a)          { m_audio = a; }
    void setLogCallback(std::function<void(const std::string&)> cb) { m_log = std::move(cb); }

    bool loadScript(const std::filesystem::path& path);
    bool hasClass(const std::string& name) const { return m_registry.count(name) > 0; }
    const std::map<std::string, ScriptClass>& getRegistry() const { return m_registry; }
    const std::string* getCompileError(const std::string& name) const;

    // Tabla instancia nueva: copia de props (defaults + overrides) +
    // metatable __index -> tabla clase. Tabla inválida si name no existe.
    sol::table createInstance(const std::string& name,
                              const std::map<std::string, ScriptValue>& overrides);

    sol::state& lua() { return m_lua; }
    Scene* scene() const              { return m_scene; }
    PhysicsManager* physics() const   { return m_physics; }
    AudioManager* audioManager() const { return m_audio; }

    void log(const std::string& msg) { if (m_log) m_log(msg); }

private:
    // Extrae las props serializables (number/boolean/string) de classTable.
    std::vector<ScriptProp> detectProps(const sol::table& classTable);

    sol::state m_lua;
    std::filesystem::path m_scriptsDir;
    std::map<std::string, ScriptClass> m_registry;
    // Último error de compilación por nombre de script (persiste aunque el
    // script siga registrado con su versión anterior válida).
    std::map<std::string, std::string> m_compileErrors;
    std::function<void(const std::string&)> m_log;

    Scene*          m_scene   = nullptr;
    PhysicsManager* m_physics = nullptr;
    AudioManager*   m_audio   = nullptr;
};

} // namespace DonTopo
```

- [ ] **Step 2: `ScriptManager.cpp`**

```cpp
#include "DonTopo/ScriptManager.h"
#include <algorithm>

namespace DonTopo
{
    ScriptManager::ScriptManager()  = default;
    ScriptManager::~ScriptManager() = default;

    void ScriptManager::init(const std::string& scriptsDir)
    {
        // Solo libs sin acceso a proceso/filesystem: los scripts de gameplay
        // no necesitan io/os, y así un script no puede tocar disco.
        m_lua.open_libraries(sol::lib::base, sol::lib::math,
                             sol::lib::string, sol::lib::table);

        m_scriptsDir = scriptsDir;
        std::error_code ec;
        if (!std::filesystem::is_directory(m_scriptsDir, ec))
        {
            log("Scripts: carpeta '" + scriptsDir + "' no encontrada — sin scripts");
            return;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(m_scriptsDir, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".lua")
                loadScript(entry.path());
        }
    }

    bool ScriptManager::loadScript(const std::filesystem::path& path)
    {
        const std::string className = path.stem().string();

        auto result = m_lua.safe_script_file(path.string(), sol::script_pass_on_error);
        if (!result.valid())
        {
            sol::error err = result;
            m_compileErrors[className] = err.what();
            log("Script '" + className + "': error de compilación: " + err.what());
            return false;
        }

        sol::object classObj = m_lua[className];
        if (classObj.get_type() != sol::type::table)
        {
            m_compileErrors[className] =
                "el archivo no define una tabla global '" + className + "'";
            log("Script '" + className + "': no define la tabla global '" + className + "'");
            return false;
        }

        ScriptClass cls;
        cls.classTable = classObj.as<sol::table>();
        cls.path       = path;
        std::error_code ec;
        cls.mtime      = std::filesystem::last_write_time(path, ec);
        cls.props      = detectProps(cls.classTable);

        m_registry[className] = std::move(cls);
        m_compileErrors.erase(className);
        log("Script '" + className + "' registrado (" +
            std::to_string(m_registry[className].props.size()) + " props)");
        return true;
    }

    const std::string* ScriptManager::getCompileError(const std::string& name) const
    {
        auto it = m_compileErrors.find(name);
        return it != m_compileErrors.end() ? &it->second : nullptr;
    }

    std::vector<ScriptProp> ScriptManager::detectProps(const sol::table& classTable)
    {
        std::vector<ScriptProp> props;
        for (const auto& [key, value] : classTable)
        {
            if (key.get_type() != sol::type::string) continue;

            ScriptProp p;
            p.name = key.as<std::string>();
            switch (value.get_type())
            {
                case sol::type::number:
                {
                    // lua_isinteger distingue 5 (integer) de 5.0 (float)
                    value.push(m_lua.lua_state());
                    p.isInteger = lua_isinteger(m_lua.lua_state(), -1) != 0;
                    lua_pop(m_lua.lua_state(), 1);
                    p.defaultValue = value.as<double>();
                    break;
                }
                case sol::type::boolean:
                    p.defaultValue = value.as<bool>();
                    break;
                case sol::type::string:
                    p.defaultValue = value.as<std::string>();
                    break;
                default:
                    continue; // funciones/tablas anidadas: no son props
            }
            props.push_back(std::move(p));
        }
        std::sort(props.begin(), props.end(),
                  [](const ScriptProp& a, const ScriptProp& b) { return a.name < b.name; });
        return props;
    }

    sol::table ScriptManager::createInstance(
        const std::string& name, const std::map<std::string, ScriptValue>& overrides)
    {
        auto it = m_registry.find(name);
        if (it == m_registry.end()) return sol::table();

        const ScriptClass& cls = it->second;
        sol::table inst = m_lua.create_table();

        // Copia de props a la instancia: cada instancia tiene las suyas,
        // editar una no toca las demás. Funciones se heredan vía metatable.
        for (const ScriptProp& p : cls.props)
        {
            const ScriptValue* v = &p.defaultValue;
            auto ov = overrides.find(p.name);
            if (ov != overrides.end()) v = &ov->second;

            std::visit([&](auto&& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, double>)
                {
                    if (p.isInteger) inst[p.name] = static_cast<int64_t>(val);
                    else             inst[p.name] = val;
                }
                else inst[p.name] = val;
            }, *v);
        }

        sol::table mt = m_lua.create_table();
        mt["__index"] = cls.classTable;
        inst[sol::metatable_key] = mt;
        return inst;
    }
}
```

- [ ] **Step 3: Añadir `src/ScriptManager.cpp` a `engine/CMakeLists.txt`.**

- [ ] **Step 4: Build** — `./build.bat`, sin errores.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/ScriptManager.h engine/src/ScriptManager.cpp engine/CMakeLists.txt
git commit -m "feat(script): añade ScriptManager (carga, registro, props, factory de instancias)"
```

---

### Task 5: Input + bindings base (Vec3, Log, Key, Input)

**Files:**
- Create: `engine/include/DonTopo/Input.h`
- Create: `engine/src/Input.cpp`
- Create: `engine/include/DonTopo/ScriptBindings.h`
- Create: `engine/src/ScriptBindings.cpp`
- Modify: `engine/CMakeLists.txt` (añadir los 2 .cpp)
- Modify: `engine/src/ScriptManager.cpp` (`init` llama a registerAll)
- Modify: `sandbox/src/main.cpp` (Input::init + Input::update en el loop)

**Interfaces:**
- Consumes: `ScriptManager::lua()`, `log()` (Task 4).
- Produces:
  - `DonTopo::Input` estática: `init(GLFWwindow*)`, `update()`, `isKeyDown(int)`, `isKeyPressed(int)`, `isKeyReleased(int)`, `isMouseButtonDown(int)`.
  - `ScriptBindings::registerAll(ScriptManager& mgr)` — registra TODO el binding (esta task: Vec3, Log, print, Key, Input; Tasks 6–7 amplían esta misma función).
  - Lua: `Vec3.new(x,y,z)`, `v.x/y/z`, `+`, `-`, `* escalar`; `Log.Info/Warn/Error(msg)`; `print` redirigido; `Input.IsKeyDown/IsKeyPressed/IsKeyReleased(Key.X)`, `Input.IsMouseButtonDown(MouseButton.Left/Right/Middle)`.

- [ ] **Step 1: `Input.h`**

```cpp
#pragma once
#include <array>

struct GLFWwindow;

namespace DonTopo {

// Fachada estática sobre el teclado/ratón de GLFW con estado prev/curr por
// frame — permite IsKeyPressed/IsKeyReleased (flancos), que glfwGetKey solo
// no da. Solo la usan los bindings de scripting por ahora (Camera sigue con
// glfwGetKey directo — fuera de alcance migrarla).
class Input {
public:
    static void init(GLFWwindow* window);
    // Llamar una vez por frame, antes de ejecutar scripts.
    static void update();

    static bool isKeyDown(int key);      // mantenida
    static bool isKeyPressed(int key);   // solo el frame del flanco de bajada
    static bool isKeyReleased(int key);  // solo el frame del flanco de subida
    static bool isMouseButtonDown(int button);

private:
    static GLFWwindow* s_window;
    // GLFW_KEY_LAST+1 entradas; índice = keycode GLFW.
    static std::array<bool, 349> s_curr;
    static std::array<bool, 349> s_prev;
};

} // namespace DonTopo
```

- [ ] **Step 2: `Input.cpp`**

```cpp
#include "DonTopo/Input.h"
#include <GLFW/glfw3.h>

namespace DonTopo
{
    GLFWwindow* Input::s_window = nullptr;
    std::array<bool, 349> Input::s_curr{};
    std::array<bool, 349> Input::s_prev{};

    void Input::init(GLFWwindow* window) { s_window = window; }

    void Input::update()
    {
        if (!s_window) return;
        s_prev = s_curr;
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
            s_curr[k] = glfwGetKey(s_window, k) == GLFW_PRESS;
    }

    bool Input::isKeyDown(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && s_curr[key];
    }
    bool Input::isKeyPressed(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && s_curr[key] && !s_prev[key];
    }
    bool Input::isKeyReleased(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && !s_curr[key] && s_prev[key];
    }
    bool Input::isMouseButtonDown(int button)
    {
        return s_window && glfwGetMouseButton(s_window, button) == GLFW_PRESS;
    }
}
```

- [ ] **Step 3: `ScriptBindings.h`**

```cpp
#pragma once

namespace DonTopo {

class ScriptManager;
class GameObject;

// Handle ligero que los bindings pasan a Lua en vez de GameObject* crudo.
// Todos los métodos validan mgr->isAlive(go) antes de tocar el puntero
// (la validación llega con el lifecycle en Task 6/8).
struct LuaEntity {
    GameObject*    go  = nullptr;
    ScriptManager* mgr = nullptr;
};

namespace ScriptBindings {
    // Registra la API completa (Vec3, Log, Input/Key, Entity, Transform,
    // componentes, Scene) en la VM de mgr. Llamado una vez desde
    // ScriptManager::init.
    void registerAll(ScriptManager& mgr);
}

} // namespace DonTopo
```

- [ ] **Step 4: `ScriptBindings.cpp`** (esta task: Vec3, Log, print, Key, Input)

```cpp
#include "DonTopo/ScriptBindings.h"
#include "DonTopo/ScriptManager.h"
#include "DonTopo/Input.h"
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace DonTopo::ScriptBindings
{
    namespace
    {
        void registerVec3(sol::state& lua)
        {
            lua.new_usertype<glm::vec3>("Vec3",
                sol::call_constructor, sol::factories(
                    []() { return glm::vec3(0.0f); },
                    [](float x, float y, float z) { return glm::vec3(x, y, z); }),
                "new", sol::factories(
                    []() { return glm::vec3(0.0f); },
                    [](float x, float y, float z) { return glm::vec3(x, y, z); }),
                "x", &glm::vec3::x,
                "y", &glm::vec3::y,
                "z", &glm::vec3::z,
                sol::meta_function::addition,
                    [](const glm::vec3& a, const glm::vec3& b) { return a + b; },
                sol::meta_function::subtraction,
                    [](const glm::vec3& a, const glm::vec3& b) { return a - b; },
                sol::meta_function::multiplication,
                    [](const glm::vec3& v, float s) { return v * s; },
                sol::meta_function::to_string,
                    [](const glm::vec3& v) {
                        return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) +
                               ", " + std::to_string(v.z) + ")";
                    });
        }

        void registerLog(ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table logTable = lua.create_named_table("Log");
            logTable["Info"]  = [&mgr](const std::string& m) { mgr.log("[Lua] " + m); };
            logTable["Warn"]  = [&mgr](const std::string& m) { mgr.log("[Lua][WARN] " + m); };
            logTable["Error"] = [&mgr](const std::string& m) { mgr.log("[Lua][ERROR] " + m); };
            // print nativo -> mismo destino que Log.Info
            lua["print"] = [&mgr](sol::variadic_args args) {
                std::string out;
                for (auto a : args)
                {
                    if (!out.empty()) out += "\t";
                    out += a.get<sol::object>().as<std::string>();
                }
                mgr.log("[Lua] " + out);
            };
        }

        void registerInput(sol::state& lua)
        {
            sol::table input = lua.create_named_table("Input");
            input["IsKeyDown"]          = [](int k) { return Input::isKeyDown(k); };
            input["IsKeyPressed"]       = [](int k) { return Input::isKeyPressed(k); };
            input["IsKeyReleased"]      = [](int k) { return Input::isKeyReleased(k); };
            input["IsMouseButtonDown"]  = [](int b) { return Input::isMouseButtonDown(b); };

            sol::table key = lua.create_named_table("Key");
            key["Space"]  = GLFW_KEY_SPACE;  key["Enter"] = GLFW_KEY_ENTER;
            key["Escape"] = GLFW_KEY_ESCAPE; key["Tab"]   = GLFW_KEY_TAB;
            key["LeftShift"]  = GLFW_KEY_LEFT_SHIFT;
            key["LeftControl"] = GLFW_KEY_LEFT_CONTROL;
            key["Up"]   = GLFW_KEY_UP;   key["Down"]  = GLFW_KEY_DOWN;
            key["Left"] = GLFW_KEY_LEFT; key["Right"] = GLFW_KEY_RIGHT;
            for (int i = 0; i < 26; ++i)
                key[std::string(1, char('A' + i))] = GLFW_KEY_A + i;
            for (int i = 0; i <= 9; ++i)
                key["Num" + std::to_string(i)] = GLFW_KEY_0 + i;

            sol::table mb = lua.create_named_table("MouseButton");
            mb["Left"]   = GLFW_MOUSE_BUTTON_LEFT;
            mb["Right"]  = GLFW_MOUSE_BUTTON_RIGHT;
            mb["Middle"] = GLFW_MOUSE_BUTTON_MIDDLE;
        }
    }

    void registerAll(ScriptManager& mgr)
    {
        registerVec3(mgr.lua());
        registerLog(mgr);
        registerInput(mgr.lua());
        // Tasks 6-7 añaden aquí: registerEntity, registerTransform,
        // registerComponents, registerScene.
    }
}
```

- [ ] **Step 5: Llamar `registerAll` al final de `ScriptManager::init`** (antes del escaneo de la carpeta — los scripts pueden usar la API a nivel de archivo):

```cpp
#include "DonTopo/ScriptBindings.h"
// ... en init(), justo después de open_libraries:
        ScriptBindings::registerAll(*this);
```

- [ ] **Step 6: Wiring en `sandbox/src/main.cpp`** — tras `window.init(...)`:

```cpp
        DonTopo::Input::init(window.getNativeWindow());
```

Primera línea dentro del `while (!window.shouldClose())` (antes del cálculo de dt está bien, pero después de `pollEvents` del frame anterior — al inicio del cuerpo del while):

```cpp
            DonTopo::Input::update();
```

Include: `#include "DonTopo/Input.h"`.

- [ ] **Step 7: Añadir `src/Input.cpp` y `src/ScriptBindings.cpp` a `engine/CMakeLists.txt`.**

- [ ] **Step 8: Build** — `./build.bat`, sin errores.

- [ ] **Step 9: Commit**

```bash
git add engine/include/DonTopo/Input.h engine/src/Input.cpp engine/include/DonTopo/ScriptBindings.h engine/src/ScriptBindings.cpp engine/src/ScriptManager.cpp engine/CMakeLists.txt sandbox/src/main.cpp
git commit -m "feat(script): añade Input y bindings base (Vec3, Log, Key, Input)"
```

---

### Task 6: Bindings Entity / Transform / Colliders / AudioClip

**Files:**
- Modify: `engine/src/ScriptBindings.cpp`
- Modify: `engine/include/DonTopo/ScriptManager.h` + `engine/src/ScriptManager.cpp` (set de vivos + `rebuildAliveSet`)

**Interfaces:**
- Consumes: `LuaEntity` (Task 5), factories de PhysicsManager/AudioManager (firmas idénticas a las que usa EditorUI::drawAddComponentButton), `ScriptManager` (Task 4).
- Produces:
  - `bool ScriptManager::isAlive(GameObject*) const` + `void rebuildAliveSet()` (traverse de m_scene sobre `std::set<GameObject*> m_alive`; usado cada frame por el lifecycle de Task 8 y bajo demanda).
  - Lua: `entity.name`, `entity:IsValid()`, `entity:GetTransform()`, `entity:GetParent()`, `entity:GetChildren()`, `entity:GetComponent(name)`, `entity:AddComponent(name, [arg])`, `entity:RemoveComponent(name)`.
  - Handles Lua: `Transform` (`GetPosition/SetPosition/GetRotation/SetRotation/GetScale/SetScale/GetWorldPosition/Translate/Rotate`), `BoxCollider/SphereCollider/CapsuleCollider/PlaneCollider` (getters/setters existentes), `AudioClip` (`Play/Stop/SetLoop/GetLoop`).

- [ ] **Step 1: Set de vivos en ScriptManager**

`ScriptManager.h` — añadir:

```cpp
    // true si go sigue en el árbol de la escena. Los bindings lo consultan
    // antes de deref: una entity destruida produce error Lua, nunca crash.
    bool isAlive(GameObject* go) const { return m_alive.count(go) > 0; }
    // Reconstruye el set desde la escena. Llamado al inicio de cada update
    // del lifecycle y tras cualquier alta/baja estructural.
    void rebuildAliveSet();
private:
    std::set<GameObject*> m_alive;
```

`ScriptManager.cpp`:

```cpp
#include "DonTopo/Scene.h"

    void ScriptManager::rebuildAliveSet()
    {
        m_alive.clear();
        if (!m_scene) return;
        m_scene->traverse([this](GameObject* go) { m_alive.insert(go); });
    }
```

- [ ] **Step 2: Helpers y handles en `ScriptBindings.cpp`**

```cpp
#include "DonTopo/Scene.h"
#include "DonTopo/GameObject.h"
#include "DonTopo/PhysicsManager.h"
#include "DonTopo/AudioManager.h"
#include "DonTopo/AudioClipComponent.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <stdexcept>

namespace
{
    using DonTopo::GameObject;
    using DonTopo::LuaEntity;

    // Deref validado: entity muerta -> excepción C++ que sol2 convierte en
    // error Lua (capturado por la protected_function del callback).
    GameObject* deref(const LuaEntity& e)
    {
        if (!e.go || !e.mgr || !e.mgr->isAlive(e.go))
            throw std::runtime_error("Entity destruida o inválida");
        return e.go;
    }

    struct LuaTransform { LuaEntity e; };
    struct LuaBoxCollider { LuaEntity e; };
    struct LuaSphereCollider { LuaEntity e; };
    struct LuaCapsuleCollider { LuaEntity e; };
    struct LuaPlaneCollider { LuaEntity e; };
    struct LuaAudioClip { LuaEntity e; };

    // Descompone localTransform en T/R/S (grados pa Lua).
    void decomposeLocal(GameObject* go, glm::vec3& pos, glm::vec3& eulerDeg, glm::vec3& scale)
    {
        glm::quat rot; glm::vec3 skew; glm::vec4 persp;
        glm::decompose(go->localTransform, scale, rot, pos, skew, persp);
        eulerDeg = glm::degrees(glm::eulerAngles(rot));
    }

    void recomposeLocal(GameObject* go, const glm::vec3& pos, const glm::vec3& eulerDeg, const glm::vec3& scale)
    {
        glm::mat4 r = glm::eulerAngleXYZ(glm::radians(eulerDeg.x),
                                          glm::radians(eulerDeg.y),
                                          glm::radians(eulerDeg.z));
        go->localTransform = glm::translate(glm::mat4(1.0f), pos) * r *
                             glm::scale(glm::mat4(1.0f), scale);
        go->updateWorldTransforms(go->parent ? go->parent->worldTransform : glm::mat4(1.0f));
    }
}
```

- [ ] **Step 3: `registerTransform(sol::state&)`**

```cpp
        void registerTransform(sol::state& lua)
        {
            lua.new_usertype<LuaTransform>("Transform",
                sol::no_constructor,
                "GetPosition", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return p;
                },
                "SetPosition", [](const LuaTransform& t, const glm::vec3& np) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, np, r, s);
                },
                "GetRotation", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return r;
                },
                "SetRotation", [](const LuaTransform& t, const glm::vec3& nr) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p, nr, s);
                },
                "GetScale", [](const LuaTransform& t) {
                    glm::vec3 p, r, s; decomposeLocal(deref(t.e), p, r, s); return s;
                },
                "SetScale", [](const LuaTransform& t, const glm::vec3& ns) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p, r, ns);
                },
                "GetWorldPosition", [](const LuaTransform& t) {
                    GameObject* go = deref(t.e);
                    return glm::vec3(go->worldTransform[3]);
                },
                "Translate", [](const LuaTransform& t, const glm::vec3& d) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p + d, r, s);
                },
                "Rotate", [](const LuaTransform& t, const glm::vec3& dEuler) {
                    GameObject* go = deref(t.e);
                    glm::vec3 p, r, s; decomposeLocal(go, p, r, s);
                    recomposeLocal(go, p, r + dEuler, s);
                });
        }
```

- [ ] **Step 4: `registerComponents(sol::state&)`** — colliders + audio. Patrón por collider (BoxCollider como ejemplo, replicar pa Sphere `GetRadius/SetRadius`, Capsule `GetRadius/SetRadius/GetHalfHeight`, Plane solo `GetCenter/SetCenter`):

```cpp
        void registerComponents(sol::state& lua)
        {
            lua.new_usertype<LuaBoxCollider>("BoxCollider",
                sol::no_constructor,
                "GetUseGravity", [](const LuaBoxCollider& c) {
                    return deref(c.e)->getBoxCollider()->getUseGravity();
                },
                "SetUseGravity", [](const LuaBoxCollider& c, bool g) {
                    deref(c.e)->getBoxCollider()->setUseGravity(g);
                },
                "GetHalfExtents", [](const LuaBoxCollider& c) {
                    return deref(c.e)->getBoxCollider()->getHalfExtents();
                },
                "SetHalfExtents", [](const LuaBoxCollider& c, const glm::vec3& he) {
                    deref(c.e)->getBoxCollider()->setHalfExtents(he);
                },
                "GetCenter", [](const LuaBoxCollider& c) {
                    return deref(c.e)->getBoxCollider()->getCenter();
                },
                "SetCenter", [](const LuaBoxCollider& c, const glm::vec3& ctr) {
                    deref(c.e)->getBoxCollider()->setCenter(ctr);
                },
                "IsDynamic", [](const LuaBoxCollider& c) {
                    return deref(c.e)->getBoxCollider()->isDynamic();
                });

            lua.new_usertype<LuaSphereCollider>("SphereCollider",
                sol::no_constructor,
                "GetUseGravity", [](const LuaSphereCollider& c) {
                    return deref(c.e)->getSphereCollider()->getUseGravity();
                },
                "SetUseGravity", [](const LuaSphereCollider& c, bool g) {
                    deref(c.e)->getSphereCollider()->setUseGravity(g);
                },
                "GetRadius", [](const LuaSphereCollider& c) {
                    return deref(c.e)->getSphereCollider()->getRadius();
                },
                "SetRadius", [](const LuaSphereCollider& c, float r) {
                    deref(c.e)->getSphereCollider()->setRadius(r);
                },
                "GetCenter", [](const LuaSphereCollider& c) {
                    return deref(c.e)->getSphereCollider()->getCenter();
                },
                "SetCenter", [](const LuaSphereCollider& c, const glm::vec3& ctr) {
                    deref(c.e)->getSphereCollider()->setCenter(ctr);
                },
                "IsDynamic", [](const LuaSphereCollider& c) {
                    return deref(c.e)->getSphereCollider()->isDynamic();
                });

            lua.new_usertype<LuaCapsuleCollider>("CapsuleCollider",
                sol::no_constructor,
                "GetUseGravity", [](const LuaCapsuleCollider& c) {
                    return deref(c.e)->getCapsuleCollider()->getUseGravity();
                },
                "SetUseGravity", [](const LuaCapsuleCollider& c, bool g) {
                    deref(c.e)->getCapsuleCollider()->setUseGravity(g);
                },
                "GetRadius", [](const LuaCapsuleCollider& c) {
                    return deref(c.e)->getCapsuleCollider()->getRadius();
                },
                "SetRadius", [](const LuaCapsuleCollider& c, float r) {
                    deref(c.e)->getCapsuleCollider()->setRadius(r);
                },
                "GetHalfHeight", [](const LuaCapsuleCollider& c) {
                    return deref(c.e)->getCapsuleCollider()->getHalfHeight();
                },
                "SetHalfHeight", [](const LuaCapsuleCollider& c, float h) {
                    deref(c.e)->getCapsuleCollider()->setHalfHeight(h);
                },
                "GetCenter", [](const LuaCapsuleCollider& c) {
                    return deref(c.e)->getCapsuleCollider()->getCenter();
                },
                "SetCenter", [](const LuaCapsuleCollider& c, const glm::vec3& ctr) {
                    deref(c.e)->getCapsuleCollider()->setCenter(ctr);
                },
                "IsDynamic", [](const LuaCapsuleCollider& c) {
                    return deref(c.e)->getCapsuleCollider()->isDynamic();
                });

            lua.new_usertype<LuaPlaneCollider>("PlaneCollider",
                sol::no_constructor,
                "GetCenter", [](const LuaPlaneCollider& c) {
                    return deref(c.e)->getPlaneCollider()->getCenter();
                },
                "SetCenter", [](const LuaPlaneCollider& c, const glm::vec3& ctr) {
                    deref(c.e)->getPlaneCollider()->setCenter(ctr);
                });

            lua.new_usertype<LuaAudioClip>("AudioClip",
                sol::no_constructor,
                "Play", [](const LuaAudioClip& c) {
                    GameObject* go = deref(c.e);
                    go->getAudioClip()->play(glm::vec3(go->worldTransform[3]));
                },
                "Stop", [](const LuaAudioClip& c) { deref(c.e)->getAudioClip()->stop(); },
                "SetLoop", [](const LuaAudioClip& c, bool l) { deref(c.e)->getAudioClip()->setLoop(l); },
                "GetLoop", [](const LuaAudioClip& c) { return deref(c.e)->getAudioClip()->getLoop(); });
        }
```

Cada lambda de collider debe comprobar `hasXCollider()` primero y lanzar `std::runtime_error("El GameObject ya no tiene <X>")` si falta (el componente pudo quitarse desde el editor en Play).

- [ ] **Step 5: `registerEntity(ScriptManager&)`**

```cpp
        void registerEntity(DonTopo::ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            lua.new_usertype<LuaEntity>("Entity",
                sol::no_constructor,
                "name", sol::property(
                    [](const LuaEntity& e) { return deref(e)->name; },
                    [](const LuaEntity& e, const std::string& n) { deref(e)->name = n; }),
                "IsValid", [](const LuaEntity& e) {
                    return e.go && e.mgr && e.mgr->isAlive(e.go);
                },
                "GetTransform", [](const LuaEntity& e) { deref(e); return LuaTransform{e}; },
                "GetParent", [](const LuaEntity& e) -> sol::object {
                    GameObject* go = deref(e);
                    if (!go->parent || !go->parent->parent) return sol::nil; // root no se expone
                    return sol::make_object(e.mgr->lua(), LuaEntity{go->parent, e.mgr});
                },
                "GetChildren", [](const LuaEntity& e) {
                    GameObject* go = deref(e);
                    sol::table result = e.mgr->lua().create_table();
                    int i = 1;
                    for (auto& c : go->children)
                        result[i++] = LuaEntity{c.get(), e.mgr};
                    return result;
                },
                "GetComponent", [](const LuaEntity& e, const std::string& name) -> sol::object {
                    GameObject* go = deref(e);
                    sol::state_view lua(e.mgr->lua());
                    if (name == "BoxCollider"     && go->hasBoxCollider())     return sol::make_object(lua, LuaBoxCollider{e});
                    if (name == "SphereCollider"  && go->hasSphereCollider())  return sol::make_object(lua, LuaSphereCollider{e});
                    if (name == "CapsuleCollider" && go->hasCapsuleCollider()) return sol::make_object(lua, LuaCapsuleCollider{e});
                    if (name == "PlaneCollider"   && go->hasPlaneCollider())   return sol::make_object(lua, LuaPlaneCollider{e});
                    if (name == "AudioClip"       && go->hasAudioClip())       return sol::make_object(lua, LuaAudioClip{e});
                    if (name.rfind("Script:", 0) == 0)
                    {
                        const std::string scriptName = name.substr(7);
                        for (auto& s : go->getScripts())
                            if (s->scriptName == scriptName && s->instance.valid())
                                return s->instance;
                    }
                    return sol::nil;
                },
                "AddComponent", [](const LuaEntity& e, const std::string& name,
                                   sol::optional<std::string> arg) -> sol::object {
                    GameObject* go = deref(e);
                    auto* mgr = e.mgr;
                    sol::state_view lua(mgr->lua());
                    // Mismos defaults que EditorUI::drawAddComponentButton;
                    // colliders mutuamente excluyentes, misma regla que la UI.
                    if (name == "BoxCollider" && !go->hasAnyCollider() && mgr->physics())
                    {
                        go->setBoxCollider(mgr->physics()->createBoxColliderComponent(
                            glm::vec3(25.0f), glm::vec3(0.0f), go->worldTransform, false));
                        return sol::make_object(lua, LuaBoxCollider{e});
                    }
                    if (name == "SphereCollider" && !go->hasAnyCollider() && mgr->physics())
                    {
                        go->setSphereCollider(mgr->physics()->createSphereColliderComponent(
                            25.0f, glm::vec3(0.0f), go->worldTransform, false));
                        return sol::make_object(lua, LuaSphereCollider{e});
                    }
                    if (name == "CapsuleCollider" && !go->hasAnyCollider() && mgr->physics())
                    {
                        go->setCapsuleCollider(mgr->physics()->createCapsuleColliderComponent(
                            15.0f, 25.0f, glm::vec3(0.0f), go->worldTransform, false));
                        return sol::make_object(lua, LuaCapsuleCollider{e});
                    }
                    if (name == "PlaneCollider" && !go->hasAnyCollider() && mgr->physics())
                    {
                        go->setPlaneCollider(mgr->physics()->createPlaneColliderComponent(
                            glm::vec3(0.0f), go->worldTransform));
                        return sol::make_object(lua, LuaPlaneCollider{e});
                    }
                    if (name == "AudioClip" && !go->hasAudioClip() && mgr->audioManager() && arg)
                    {
                        auto clip = mgr->audioManager()->createAudioClipComponent(*arg, true, false);
                        if (clip) { go->setAudioClip(std::move(clip)); return sol::make_object(lua, LuaAudioClip{e}); }
                    }
                    if (name.rfind("Script:", 0) == 0)
                    {
                        auto comp = std::make_unique<DonTopo::ScriptComponent>(name.substr(7), go);
                        go->addScript(std::move(comp));
                        // La instanciación + Awake/Start del comp nuevo la
                        // hace el lifecycle en el siguiente update (started
                        // == false lo delata). Task 8.
                        return sol::make_object(lua, true);
                    }
                    return sol::nil;
                },
                "RemoveComponent", [](const LuaEntity& e, const std::string& name) {
                    GameObject* go = deref(e);
                    if (name == "BoxCollider")     go->setBoxCollider(nullptr);
                    else if (name == "SphereCollider")  go->setSphereCollider(nullptr);
                    else if (name == "CapsuleCollider") go->setCapsuleCollider(nullptr);
                    else if (name == "PlaneCollider")   go->setPlaneCollider(nullptr);
                    else if (name == "AudioClip")       go->setAudioClip(nullptr);
                    else if (name.rfind("Script:", 0) == 0)
                    {
                        // Diferido: el lifecycle lo procesa al final del frame
                        // (quitar en mitad de la iteración de Update rompería
                        // el recorrido). Task 8.
                        const std::string scriptName = name.substr(7);
                        for (auto& s : go->getScripts())
                            if (s->scriptName == scriptName) s->pendingRemove = true;
                    }
                });
        }
```

- [ ] **Step 6: Añadir las llamadas en `registerAll`:**

```cpp
        registerTransform(mgr.lua());
        registerComponents(mgr.lua());
        registerEntity(mgr);
```

- [ ] **Step 7: Build** — `./build.bat`, sin errores.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/ScriptManager.h engine/src/ScriptManager.cpp engine/src/ScriptBindings.cpp
git commit -m "feat(script): bindings Entity, Transform, colliders y AudioClip con validación de lifetime"
```

---

### Task 7: Bindings Scene (Find / CreateGameObject / Destroy / Instantiate)

**Files:**
- Modify: `engine/src/ScriptBindings.cpp`
- Modify: `engine/include/DonTopo/ScriptManager.h` + `engine/src/ScriptManager.cpp` (cola de destroy + hooks GPU + `instantiateComponent`)

**Interfaces:**
- Consumes: `Scene::cloneGameObject` (Task 3), `rebuildAliveSet` (Task 6).
- Produces:
  - `void ScriptManager::setOnInstantiated(std::function<void(GameObject*)>)` — el caller (main.cpp) registra meshes GPU del subtree clonado.
  - `void ScriptManager::setOnDestroying(std::function<void(GameObject*)>)` — el caller libera GPU antes del borrado.
  - `void ScriptManager::queueDestroy(GameObject*)` — cola procesada por el lifecycle (Task 8) tras LateUpdate.
  - `void ScriptManager::instantiateComponent(ScriptComponent&)` — crea instancia+entity+flags de callbacks (usada por Play start, Instantiate, hot reload y EditorUI).
  - Lua: `Scene.Find(name)`, `Scene.CreateGameObject(name, [parentEntity])`, `Scene.Destroy(entity)`, `Scene.Instantiate(entity, [parentEntity])`.

- [ ] **Step 1: ScriptManager — cola, hooks e instantiateComponent**

`ScriptManager.h`:

```cpp
    void setOnInstantiated(std::function<void(GameObject*)> cb) { m_onInstantiated = std::move(cb); }
    void setOnDestroying(std::function<void(GameObject*)> cb)   { m_onDestroying = std::move(cb); }
    // Borrado diferido — procesado al final del frame del lifecycle.
    void queueDestroy(GameObject* go) { m_destroyQueue.push_back(go); }
    // Crea la tabla instancia del comp (defaults+overrides), inyecta
    // self.entity y cachea qué callbacks define. NO llama Awake/Start.
    void instantiateComponent(ScriptComponent& comp);
    const std::function<void(GameObject*)>& onInstantiated() const { return m_onInstantiated; }
private:
    std::vector<GameObject*> m_destroyQueue;
    std::function<void(GameObject*)> m_onInstantiated;
    std::function<void(GameObject*)> m_onDestroying;
```

`ScriptManager.cpp`:

```cpp
#include "DonTopo/ScriptBindings.h"   // LuaEntity

    void ScriptManager::instantiateComponent(ScriptComponent& comp)
    {
        comp.instance = createInstance(comp.scriptName, comp.overrides);
        comp.started  = false;
        comp.hasError = false;
        if (!comp.instance.valid()) return;   // clase no registrada (missing)

        comp.instance["entity"] = LuaEntity{ comp.owner, this };

        auto isFn = [&](const char* n) {
            return comp.instance[n].get_type() == sol::type::function;
        };
        comp.hasAwake       = isFn("Awake");
        comp.hasStart       = isFn("Start");
        comp.hasUpdate      = isFn("Update");
        comp.hasFixedUpdate = isFn("FixedUpdate");
        comp.hasLateUpdate  = isFn("LateUpdate");
        comp.hasOnDestroy   = isFn("OnDestroy");
    }
```

(Nota: `instance["Awake"]` resuelve vía `__index` a la tabla clase — correcto, las funciones viven allí.)

- [ ] **Step 2: `registerScene(ScriptManager&)` en ScriptBindings.cpp**

```cpp
        void registerScene(DonTopo::ScriptManager& mgr)
        {
            sol::state& lua = mgr.lua();
            sol::table sceneTable = lua.create_named_table("Scene");

            sceneTable["Find"] = [&mgr](const std::string& name) -> sol::object {
                if (!mgr.scene()) return sol::nil;
                GameObject* found = nullptr;
                mgr.scene()->traverse([&](GameObject* go) {
                    if (!found && go->parent && go->name == name) found = go;
                });
                if (!found) return sol::nil;
                return sol::make_object(mgr.lua(), LuaEntity{found, &mgr});
            };

            sceneTable["CreateGameObject"] = [&mgr](const std::string& name,
                                                    sol::optional<LuaEntity> parent) -> sol::object {
                if (!mgr.scene()) return sol::nil;
                GameObject* p = parent ? deref(*parent) : nullptr;
                GameObject* go = mgr.scene()->addGameObject(name, p);
                mgr.rebuildAliveSet();
                return sol::make_object(mgr.lua(), LuaEntity{go, &mgr});
            };

            sceneTable["Destroy"] = [&mgr](const LuaEntity& e) {
                mgr.queueDestroy(deref(e));
            };

            sceneTable["Instantiate"] = [&mgr](const LuaEntity& src,
                                               sol::optional<LuaEntity> parent) -> sol::object {
                if (!mgr.scene() || !mgr.physics() || !mgr.audioManager()) return sol::nil;
                GameObject* srcGo = deref(src);
                GameObject* p = parent ? deref(*parent) : nullptr;
                GameObject* clone = mgr.scene()->cloneGameObject(
                    srcGo, p, *mgr.physics(), *mgr.audioManager());
                if (!clone) return sol::nil;

                if (mgr.onInstantiated()) mgr.onInstantiated()(clone);
                mgr.rebuildAliveSet();
                // Los scripts del clon se instancian ya; Awake inmediato,
                // Start lo dispara el lifecycle antes de su primer Update
                // (started == false).
                clone->traverse([&mgr](GameObject* n) {
                    for (auto& s : n->getScripts())
                    {
                        mgr.instantiateComponent(*s);
                        if (s->instance.valid() && s->hasAwake)
                        {
                            sol::protected_function f = s->instance["Awake"];
                            auto r = f(s->instance);
                            if (!r.valid())
                            {
                                sol::error err = r;
                                mgr.log("Script '" + s->scriptName + "' Awake: " + std::string(err.what()));
                                s->hasError = true;
                            }
                        }
                    }
                });
                return sol::make_object(mgr.lua(), LuaEntity{clone, &mgr});
            };
        }
```

Y en `registerAll`: `registerScene(mgr);`

- [ ] **Step 3: Build** — `./build.bat`, sin errores.

- [ ] **Step 4: Commit**

```bash
git add engine/include/DonTopo/ScriptManager.h engine/src/ScriptManager.cpp engine/src/ScriptBindings.cpp
git commit -m "feat(script): bindings Scene (Find, CreateGameObject, Destroy diferido, Instantiate)"
```

---

### Task 8: Ciclo de vida + integración Play/Stop + frame loop

**Files:**
- Modify: `engine/include/DonTopo/ScriptManager.h` + `engine/src/ScriptManager.cpp` (onPlayStart/onPlayStop/update/callCallback)
- Modify: `engine/include/DonTopo/EditorUI.h` + `engine/src/EditorUI.cpp` (setScriptManager + hooks en Play/Stop)
- Modify: `engine/include/DonTopo/Renderer.h` + `engine/src/Renderer.cpp` (passthrough `setScriptManager` → EditorUI, mismo patrón que setPhysicsManager)
- Modify: `sandbox/src/main.cpp` (crear ScriptManager, wiring completo, update en loop)

**Interfaces:**
- Consumes: todo lo anterior.
- Produces:
  - `void ScriptManager::onPlayStart()` — instancia todos los comps de la escena, Awake en todos, luego Start en todos (two-pass); resetea acumulador fijo.
  - `void ScriptManager::onPlayStop()` — OnDestroy en todos, invalida instancias, vacía colas.
  - `void ScriptManager::update(float dt)` — orden: rebuildAliveSet → Awake/Start de comps nuevos (started==false) → Update → FixedUpdate (acumulador 1/60, clamp 0.25) → LateUpdate → procesa pendingRemove → procesa cola destroy (OnDestroy subtree + onDestroying + removeGameObject).
  - `void ScriptManager::callOnDestroy(ScriptComponent&)` (público — EditorUI lo usa al quitar componente en Play).
  - `EditorUI::setScriptManager(ScriptManager*)`; Play llama `onPlayStart()` tras el snapshot; Stop llama `onPlayStop()` ANTES de restaurar.
- ORDEN DE DECLARACIÓN en main.cpp: `ScriptManager` se declara ANTES que `Scene` — los ScriptComponent del árbol guardan `sol::table` cuyo destructor toca la VM; la escena debe destruirse antes que el `sol::state`.

- [ ] **Step 1: ScriptManager — lifecycle**

`ScriptManager.h`:

```cpp
    void onPlayStart();
    void onPlayStop();
    void update(float dt);
    // OnDestroy protegido de un solo componente (usado por EditorUI al
    // quitar el componente en Play y por el procesado de colas).
    void callOnDestroy(ScriptComponent& comp);
private:
    // Llama comp.instance[fn](instance[, dt]) protegido; error -> log +
    // hasError (el comp deja de recibir callbacks).
    void callCallback(ScriptComponent& comp, const char* fn, const float* dt);
    // Todos los ScriptComponent vivos de la escena, en orden de traverse.
    std::vector<ScriptComponent*> collectComponents();
    static constexpr float kFixedStep = 1.0f / 60.0f;
    static constexpr float kMaxAccumulator = 0.25f;   // anti spiral-of-death
    float m_fixedAccumulator = 0.0f;
    bool  m_playing = false;
```

`ScriptManager.cpp`:

```cpp
    void ScriptManager::callCallback(ScriptComponent& comp, const char* fn, const float* dt)
    {
        if (comp.hasError || !comp.instance.valid()) return;
        sol::protected_function f = comp.instance[fn];
        auto r = dt ? f(comp.instance, *dt) : f(comp.instance);
        if (!r.valid())
        {
            sol::error err = r;
            log("Script '" + comp.scriptName + "' " + fn + ": " + std::string(err.what()));
            comp.hasError = true;
        }
    }

    void ScriptManager::callOnDestroy(ScriptComponent& comp)
    {
        if (comp.hasOnDestroy) callCallback(comp, "OnDestroy", nullptr);
    }

    std::vector<ScriptComponent*> ScriptManager::collectComponents()
    {
        std::vector<ScriptComponent*> comps;
        if (!m_scene) return comps;
        m_scene->traverse([&](GameObject* go) {
            for (auto& s : go->getScripts()) comps.push_back(s.get());
        });
        return comps;
    }

    void ScriptManager::onPlayStart()
    {
        m_playing = true;
        m_fixedAccumulator = 0.0f;
        m_destroyQueue.clear();
        rebuildAliveSet();

        auto comps = collectComponents();
        for (auto* c : comps) instantiateComponent(*c);
        // Two-pass como Unity: todos los Awake antes del primer Start.
        for (auto* c : comps) if (c->hasAwake) callCallback(*c, "Awake", nullptr);
        for (auto* c : comps)
        {
            if (c->hasStart) callCallback(*c, "Start", nullptr);
            c->started = true;
        }
    }

    void ScriptManager::onPlayStop()
    {
        auto comps = collectComponents();
        for (auto* c : comps) callOnDestroy(*c);
        for (auto* c : comps)
        {
            c->instance = sol::table();
            c->started  = false;
            c->hasError = false;
            c->pendingRemove = false;
        }
        m_destroyQueue.clear();
        m_playing = false;
    }

    void ScriptManager::update(float dt)
    {
        if (!m_playing || !m_scene) return;
        rebuildAliveSet();

        // Snapshot de punteros: los scripts pueden añadir componentes en
        // mitad del frame (se recogen el frame siguiente); los borrados van
        // SIEMPRE por colas diferidas, así que ningún puntero del snapshot
        // muere durante la iteración.
        auto comps = collectComponents();

        // Comps añadidos después de Play (Instantiate, AddComponent, editor):
        // Awake (si no vino ya de Instantiate: instance inválida) + Start.
        for (auto* c : comps)
        {
            if (c->started) continue;
            if (!c->instance.valid())
            {
                instantiateComponent(*c);
                if (c->hasAwake) callCallback(*c, "Awake", nullptr);
            }
            if (c->hasStart) callCallback(*c, "Start", nullptr);
            c->started = true;
        }

        for (auto* c : comps) if (c->hasUpdate) callCallback(*c, "Update", &dt);

        m_fixedAccumulator = std::min(m_fixedAccumulator + dt, kMaxAccumulator);
        while (m_fixedAccumulator >= kFixedStep)
        {
            float step = kFixedStep;
            for (auto* c : comps) if (c->hasFixedUpdate) callCallback(*c, "FixedUpdate", &step);
            m_fixedAccumulator -= kFixedStep;
        }

        for (auto* c : comps) if (c->hasLateUpdate) callCallback(*c, "LateUpdate", nullptr);

        // RemoveComponent("Script:X") diferidos
        m_scene->traverse([&](GameObject* go) {
            auto& scripts = go->getScripts();
            for (auto& s : scripts)
                if (s->pendingRemove) callOnDestroy(*s);
            scripts.erase(
                std::remove_if(scripts.begin(), scripts.end(),
                    [](const std::unique_ptr<ScriptComponent>& s) { return s->pendingRemove; }),
                scripts.end());
        });

        // Cola de destroy de entities (Scene.Destroy) — tras LateUpdate
        for (GameObject* go : m_destroyQueue)
        {
            if (!isAlive(go)) continue;   // destruido dos veces o hijo de otro destruido
            go->traverse([this](GameObject* n) {
                for (auto& s : n->getScripts()) callOnDestroy(*s);
            });
            if (m_onDestroying) m_onDestroying(go);
            m_scene->removeGameObject(go);
            rebuildAliveSet();
        }
        m_destroyQueue.clear();
    }
```

- [ ] **Step 2: EditorUI**

`EditorUI.h`: forward `class ScriptManager;`, miembro `ScriptManager* m_scriptManager = nullptr;`, setter:

```cpp
    // Puntero no-propietario, mismo patrón que m_physics. Necesario para
    // disparar el ciclo de vida al pulsar Play/Stop y para la sección
    // Scripts del panel Properties (Task 10).
    void setScriptManager(ScriptManager* sm) { m_scriptManager = sm; }
```

`EditorUI.cpp` — en el handler de **Play** (línea ~240), tras `m_isPlaying = true;`:

```cpp
            if (m_scriptManager) m_scriptManager->onPlayStart();
```

En el handler de **Stop** (línea ~230), ANTES de `reloadSceneFromJson`:

```cpp
            if (m_scriptManager) m_scriptManager->onPlayStop();
```

Include `"DonTopo/ScriptManager.h"` en EditorUI.cpp.

- [ ] **Step 3: Renderer passthrough** — replicar el patrón exacto de `setPhysicsManager` en `Renderer.h` (forward-declarar `class ScriptManager;` junto a los forwards existentes):

```cpp
            // Passthrough al EditorUI embebido, mismo patrón que
            // setPhysicsManager/setAudioManager.
            void setScriptManager(ScriptManager* sm) { m_editorUI.setScriptManager(sm); }
```

(Si `setPhysicsManager` en Renderer.h delega de otra forma —p. ej. está definido en Renderer.cpp—, copiar esa forma exacta; el miembro EditorUI puede llamarse distinto, usar el nombre real.)

- [ ] **Step 4: main.cpp**

Declaración (ANTES de `DonTopo::Scene scene;` — ver nota de orden de destrucción en Interfaces):

```cpp
        DonTopo::ScriptManager scriptManager;
```

Wiring, tras `renderer.setAudioManager(&audio);`:

```cpp
        scriptManager.setScene(&scene);
        scriptManager.setPhysicsManager(&physics);
        scriptManager.setAudioManager(&audio);
        scriptManager.setLogCallback([](const std::string& msg) { std::cout << msg << std::endl; });
        scriptManager.setOnInstantiated([&renderer](DonTopo::GameObject* go) {
            go->traverse([&renderer](DonTopo::GameObject* n) {
                if (!n->hasMesh()) return;
                if (n->isSkinned()) n->skinnedRenderIndex = renderer.addSkinnedMesh(*n->getSkinnedMesh());
                else                n->staticRenderIndex  = renderer.addStaticMesh(*n->getMesh());
            });
        });
        scriptManager.setOnDestroying([&renderer](DonTopo::GameObject* go) {
            go->traverse([&renderer](DonTopo::GameObject* n) {
                if (n->hasMesh()) renderer.removeMeshComponent(n);
            });
        });
        scriptManager.init("Scripts");
        renderer.setScriptManager(&scriptManager);
```

(El log callback pasa por stdout de momento; Task 10 lo redirige al Log Console del editor vía Renderer/EditorUI si hay un hook razonable — ver Task 10 Step 4.)

En el loop, dentro de `if (renderer.isPlaying())`, tras `scene.update(dt, physics);`:

```cpp
                scriptManager.update(dt);
```

Include `"DonTopo/ScriptManager.h"`.

- [ ] **Step 5: Build** — `./configure.bat; if ($?) { ./build.bat }`, sin errores.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/ScriptManager.h engine/src/ScriptManager.cpp engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp engine/include/DonTopo/Renderer.h engine/src/Renderer.cpp sandbox/src/main.cpp
git commit -m "feat(script): ciclo de vida Unity-style enganchado a Play Mode y frame loop"
```

---

### Task 9: Hot reload

**Files:**
- Modify: `engine/include/DonTopo/ScriptManager.h` + `engine/src/ScriptManager.cpp`
- Modify: `sandbox/src/main.cpp` (llamada en el loop)

**Interfaces:**
- Consumes: `loadScript`, `instantiateComponent`, `callCallback` (tasks previas).
- Produces: `void ScriptManager::pollChanges()` — llamable cada frame; internamente solo actúa 1 de cada 60 llamadas.

- [ ] **Step 1: Implementación**

`ScriptManager.h`:

```cpp
    // Hot reload: detecta cambios de mtime y .lua nuevos en Scripts/.
    // Llamable cada frame — solo escanea 1 de cada 60 llamadas (~1s a 60fps).
    void pollChanges();
private:
    // Núcleo de instantiateComponent parametrizado por valores (el hot
    // reload instancia con los valores actuales, no con comp.overrides).
    void instantiateComponentWith(ScriptComponent& comp,
                                  const std::map<std::string, ScriptValue>& values);
    int m_pollCounter = 0;
```

`ScriptManager.cpp`:

```cpp
    void ScriptManager::pollChanges()
    {
        if (++m_pollCounter < 60) return;
        m_pollCounter = 0;

        std::error_code ec;

        // 1) Cambios en scripts registrados
        std::vector<std::string> changed;
        for (auto& [name, cls] : m_registry)
        {
            auto mtime = std::filesystem::last_write_time(cls.path, ec);
            if (!ec && mtime != cls.mtime) changed.push_back(name);
        }

        for (const std::string& name : changed)
        {
            const std::filesystem::path path = m_registry[name].path;
            log("Script '" + name + "' cambió en disco — recargando");
            // Actualiza mtime siempre (aunque compile mal, pa no reintentar
            // en bucle el mismo contenido roto).
            m_registry[name].mtime = std::filesystem::last_write_time(path, ec);
            if (!loadScript(path))
                continue;   // error logueado; instancias viejas siguen corriendo

            if (!m_playing || !m_scene) continue;
            // Reinstancia los comps vivos de esta clase preservando el valor
            // actual de las props serializables (spec: estado no
            // serializable se pierde).
            m_scene->traverse([&](GameObject* go) {
                for (auto& s : go->getScripts())
                {
                    if (s->scriptName != name || !s->instance.valid()) continue;

                    std::map<std::string, ScriptValue> current = s->overrides;
                    for (const ScriptProp& p : m_registry[name].props)
                    {
                        sol::object v = s->instance[p.name];
                        if (v.get_type() == sol::type::number)       current[p.name] = v.as<double>();
                        else if (v.get_type() == sol::type::boolean) current[p.name] = v.as<bool>();
                        else if (v.get_type() == sol::type::string)  current[p.name] = v.as<std::string>();
                    }

                    instantiateComponentWith(*s, current);   // ver refactor abajo
                    if (s->hasAwake) callCallback(*s, "Awake", nullptr);
                    if (s->hasStart) callCallback(*s, "Start", nullptr);
                    s->started = true;
                }
            });
        }

        // 2) Scripts nuevos en la carpeta
        if (std::filesystem::is_directory(m_scriptsDir, ec))
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(m_scriptsDir, ec))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".lua") continue;
                const std::string name = entry.path().stem().string();
                if (!m_registry.count(name) && !m_compileErrors.count(name))
                    loadScript(entry.path());
            }
        }
    }
```

**Refactor incluido en este step** (el reload necesita instanciar con valores distintos de `s->overrides` — los actuales de la instancia vieja): dividir `instantiateComponent` en dos, manteniendo la firma pública intacta pa el resto de callers:

```cpp
    void ScriptManager::instantiateComponent(ScriptComponent& comp)
    {
        instantiateComponentWith(comp, comp.overrides);
    }

    void ScriptManager::instantiateComponentWith(
        ScriptComponent& comp, const std::map<std::string, ScriptValue>& values)
    {
        comp.instance = createInstance(comp.scriptName, values);
        comp.started  = false;
        comp.hasError = false;
        if (!comp.instance.valid()) return;
        comp.instance["entity"] = LuaEntity{ comp.owner, this };
        auto isFn = [&](const char* n) { return comp.instance[n].get_type() == sol::type::function; };
        comp.hasAwake = isFn("Awake"); comp.hasStart = isFn("Start");
        comp.hasUpdate = isFn("Update"); comp.hasFixedUpdate = isFn("FixedUpdate");
        comp.hasLateUpdate = isFn("LateUpdate"); comp.hasOnDestroy = isFn("OnDestroy");
    }
```

El bloque de reload usa `instantiateComponentWith(*s, current);` y después Awake+Start.

- [ ] **Step 2: main.cpp** — en el loop, fuera del gate de Play (el reload también funciona en Edit Mode pa refrescar registry/props de la UI), justo después de `DonTopo::Input::update();`:

```cpp
            scriptManager.pollChanges();
```

- [ ] **Step 3: Build** — `./build.bat`, sin errores.

- [ ] **Step 4: Commit**

```bash
git add engine/include/DonTopo/ScriptManager.h engine/src/ScriptManager.cpp sandbox/src/main.cpp
git commit -m "feat(script): hot reload por polling de mtime con preservación de props"
```

---

### Task 10: EditorUI — sección Scripts (Add, props auto, Reset, quitar, errores)

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h` (declarar `drawScriptsSection`)
- Modify: `engine/src/EditorUI.cpp` (drawAddComponentButton + nueva drawScriptsSection + llamada desde drawProperties + log de scripts al Log Console)

**Interfaces:**
- Consumes: `ScriptManager::getRegistry/hasClass/getCompileError/instantiateComponent/callOnDestroy`, `ScriptComponent` (overrides, scriptName), `pushLog`.
- Produces: UI completa de scripts en Properties. Ninguna task posterior consume nada de aquí.

- [ ] **Step 1: Entrada "Script" en `drawAddComponentButton`** — tras el bloque de Audio Clip, antes de `ImGui::EndPopup()`:

```cpp
        if (m_scriptManager && !m_scriptManager->getRegistry().empty())
        {
            if (ImGui::BeginMenu("Script"))
            {
                for (const auto& [name, cls] : m_scriptManager->getRegistry())
                {
                    if (ImGui::MenuItem(name.c_str()))
                    {
                        auto comp = std::make_unique<ScriptComponent>(name, m_selected);
                        ScriptComponent* raw = comp.get();
                        m_selected->addScript(std::move(comp));
                        if (m_isPlaying)
                        {
                            // En Play el comp entra en caliente: instancia ya;
                            // Awake/Start los dispara el lifecycle (started
                            // == false) en el siguiente update.
                            m_scriptManager->instantiateComponent(*raw);
                        }
                        pushLog("Componente Script '" + name + "' añadido a '" + m_selected->name + "'");
                    }
                }
                ImGui::EndMenu();
            }
        }
```

Includes en EditorUI.cpp: `"DonTopo/ScriptComponent.h"` (ScriptManager.h ya viene de Task 8).

- [ ] **Step 2: `drawScriptsSection`** — declarar en EditorUI.h junto a `drawAudioClipSection();` y llamarla en `drawProperties()` justo antes de `drawAddComponentButton()` (línea ~1053):

```cpp
void EditorUI::drawScriptsSection()
{
    if (!m_selected || !m_scriptManager || !m_selected->hasScripts()) return;

    ScriptComponent* toRemove = nullptr;

    for (auto& compPtr : m_selected->getScripts())
    {
        ScriptComponent* comp = compPtr.get();
        ImGui::PushID(comp);

        bool open = ImGui::CollapsingHeader(
            (comp->scriptName + " (Script)").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 12.0f);
        if (ImGui::SmallButton("x"))
            toRemove = comp;

        if (open)
        {
            if (!m_scriptManager->hasClass(comp->scriptName))
            {
                const std::string* err = m_scriptManager->getCompileError(comp->scriptName);
                if (err)
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                        "Error de compilación:\n%s", err->c_str());
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                        "Script no encontrado: %s.lua", comp->scriptName.c_str());
                // Overrides intactos (spec: no se pierden datos)
            }
            else
            {
                const ScriptClass& cls = m_scriptManager->getRegistry().at(comp->scriptName);
                const bool live = m_isPlaying && comp->instance.valid();

                for (const ScriptProp& prop : cls.props)
                {
                    // Valor mostrado: instancia viva > override > default
                    ScriptValue value = prop.defaultValue;
                    if (auto it = comp->overrides.find(prop.name); it != comp->overrides.end())
                        value = it->second;
                    if (live)
                    {
                        sol::object lv = comp->instance[prop.name];
                        if (lv.get_type() == sol::type::number)       value = lv.as<double>();
                        else if (lv.get_type() == sol::type::boolean) value = lv.as<bool>();
                        else if (lv.get_type() == sol::type::string)  value = lv.as<std::string>();
                    }

                    const std::string label = prettyPropLabel(prop.name);
                    bool edited = false;

                    if (std::holds_alternative<double>(value))
                    {
                        double d = std::get<double>(value);
                        if (prop.isInteger)
                        {
                            int i = static_cast<int>(d);
                            if (ImGui::DragInt(label.c_str(), &i)) { value = double(i); edited = true; }
                        }
                        else
                        {
                            float f = static_cast<float>(d);
                            if (ImGui::DragFloat(label.c_str(), &f, 0.1f)) { value = double(f); edited = true; }
                        }
                    }
                    else if (std::holds_alternative<bool>(value))
                    {
                        bool b = std::get<bool>(value);
                        if (ImGui::Checkbox(label.c_str(), &b)) { value = b; edited = true; }
                    }
                    else
                    {
                        char buf[256] = {};
                        const std::string& s = std::get<std::string>(value);
                        strncpy_s(buf, s.c_str(), sizeof(buf) - 1);
                        if (ImGui::InputText(label.c_str(), buf, sizeof(buf)))
                        { value = std::string(buf); edited = true; }
                    }

                    if (edited)
                    {
                        comp->overrides[prop.name] = value;
                        if (live)
                        {
                            std::visit([&](auto&& v) {
                                using T = std::decay_t<decltype(v)>;
                                if constexpr (std::is_same_v<T, double>)
                                {
                                    if (prop.isInteger) comp->instance[prop.name] = static_cast<int64_t>(v);
                                    else                comp->instance[prop.name] = v;
                                }
                                else comp->instance[prop.name] = v;
                            }, value);
                        }
                        pushLog("Script '" + comp->scriptName + "." + prop.name +
                                "' cambiado en '" + m_selected->name + "'");
                    }
                }

                if (ImGui::Button("Reset"))
                {
                    comp->overrides.clear();
                    if (live)
                    {
                        // Reaplica defaults del .lua a la instancia viva
                        for (const ScriptProp& prop : cls.props)
                            std::visit([&](auto&& v) {
                                using T = std::decay_t<decltype(v)>;
                                if constexpr (std::is_same_v<T, double>)
                                {
                                    if (prop.isInteger) comp->instance[prop.name] = static_cast<int64_t>(v);
                                    else                comp->instance[prop.name] = v;
                                }
                                else comp->instance[prop.name] = v;
                            }, prop.defaultValue);
                    }
                    pushLog("Script '" + comp->scriptName + "' reseteado a defaults en '" +
                            m_selected->name + "'");
                }
            }
        }
        ImGui::PopID();
    }

    if (toRemove)
    {
        if (m_isPlaying) m_scriptManager->callOnDestroy(*toRemove);
        const std::string name = toRemove->scriptName;
        m_selected->removeScript(toRemove);
        pushLog("Componente Script '" + name + "' quitado de '" + m_selected->name + "'");
    }
}
```

Helper (función libre en anonymous namespace de EditorUI.cpp):

```cpp
    // "attackRange" -> "Attack Range" (labels de props de scripts)
    std::string prettyPropLabel(const std::string& raw)
    {
        std::string out;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            char c = raw[i];
            if (i == 0) { out += static_cast<char>(std::toupper(static_cast<unsigned char>(c))); continue; }
            if (std::isupper(static_cast<unsigned char>(c))) out += ' ';
            out += c;
        }
        return out;
    }
```

- [ ] **Step 3: Redirigir el log de ScriptManager al Log Console.** EditorUI ya tiene `pushLog`; el wiring de main.cpp (Task 8 Step 4) usaba stdout. Cambiar en main.cpp, DESPUÉS de `renderer.setScriptManager(&scriptManager)`: no hay acceso público a pushLog desde fuera, así que añadir en EditorUI.h:

```cpp
    // Punto de entrada externo al Log Console (usado por ScriptManager vía
    // el wiring de main.cpp: mensajes de compilación/errores de scripts).
    void pushExternalLog(const std::string& message) { pushLog(message); }
```

Y un passthrough en Renderer (`void pushEditorLog(const std::string& m)` → `m_editorUI.pushExternalLog(m)`). En main.cpp:

```cpp
        scriptManager.setLogCallback([&renderer](const std::string& msg) {
            renderer.pushEditorLog(msg);
        });
```

(OJO: el callback se setea ANTES de `scriptManager.init("Scripts")` pa capturar los logs de carga inicial; renderer ya existe en ese punto.)

- [ ] **Step 4: Build** — `./build.bat`, sin errores.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp engine/include/DonTopo/Renderer.h engine/src/Renderer.cpp sandbox/src/main.cpp
git commit -m "feat(script): sección Scripts en Properties con props auto-generadas"
```

---

### Task 11: Scripts de ejemplo + verificación final

**Files:**
- Create: `Scripts/Rotator.lua`
- Create: `Scripts/Mover.lua`

**Interfaces:**
- Consumes: toda la API Lua (Tasks 5–8).
- Produces: smoke tests vivos + documentación por ejemplo.

- [ ] **Step 1: `Scripts/Rotator.lua`**

```lua
-- Rota la entity sobre Y. 'speed' (grados/seg) aparece editable en el
-- panel Properties automáticamente.
Rotator = {
    speed = 45
}

function Rotator:Awake()
    Log.Info("Rotator despierto en " .. self.entity.name)
end

function Rotator:Update(dt)
    local t = self.entity:GetTransform()
    t:Rotate(Vec3.new(0, self.speed * dt, 0))
end
```

- [ ] **Step 2: `Scripts/Mover.lua`**

```lua
-- Mueve la entity con las flechas del teclado. Demuestra Input, props
-- múltiples y FixedUpdate.
Mover = {
    speed = 100,
    verbose = false
}

function Mover:Start()
    if self.verbose then Log.Info("Mover listo en " .. self.entity.name) end
end

function Mover:Update(dt)
    local t = self.entity:GetTransform()
    local d = Vec3.new(0, 0, 0)
    if Input.IsKeyDown(Key.Right) then d.x = d.x + self.speed * dt end
    if Input.IsKeyDown(Key.Left)  then d.x = d.x - self.speed * dt end
    if Input.IsKeyDown(Key.Up)    then d.z = d.z - self.speed * dt end
    if Input.IsKeyDown(Key.Down)  then d.z = d.z + self.speed * dt end
    t:Translate(d)
end

function Mover:OnDestroy()
    if self.verbose then Log.Info("Mover destruido") end
end
```

- [ ] **Step 3: Build final completo**

Run: `./build.bat`
Expected: sin errores ni warnings nuevos.

- [ ] **Step 4: Verificación de carga (headless-ish):** arrancar el sandbox unos segundos y comprobar en el Log Console (o stdout) las líneas `Script 'Rotator' registrado (1 props)` y `Script 'Mover' registrado (2 props)`. Si no se puede arrancar GUI en el entorno del worker, dejarlo anotado como pendiente de verificación manual.

- [ ] **Step 5: Commit**

```bash
git add Scripts/Rotator.lua Scripts/Mover.lua
git commit -m "feat(script): añade scripts de ejemplo Rotator y Mover"
```

- [ ] **Step 6: Actualizar la lista de verificación manual GUI pendiente** (memoria del proyecto): añadir "Scripting Lua (Add Script, props UI, Play lifecycle, hot reload, Instantiate/Destroy, save/load con scripts)".

---

## Checklist de verificación manual GUI (post-implementación, humano)

1. Arrancar sandbox → Log Console muestra Rotator/Mover registrados.
2. Add > Script > Rotator en un cubo → sección "Rotator (Script)" con campo "Speed".
3. Play → el cubo rota; editar Speed en caliente cambia la velocidad.
4. Editar `Rotator.lua` (speed default + lógica) con el motor corriendo → recarga en ~1s, sigue rotando con el código nuevo.
5. Script con error de sintaxis → error en Log Console, motor vivo, sección en rojo.
6. Guardar escena, recargar → el componente Rotator y su override de Speed sobreviven.
7. Stop → todo restaurado (posición y scripts).
8. Mover con flechas (script Mover) en Play.
