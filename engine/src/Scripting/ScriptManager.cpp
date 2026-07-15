#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/Colliders/Collider.h"
#include <algorithm>

namespace DonTopo
{
    // Adapter que engancha los callbacks de trigger del módulo de física a la
    // cola de ScriptManager. Uno por collider registrado en onPlayStart;
    // captura el GameObject dueño del collider trigger. `e.other` es el owner
    // opaco (void*) del otro collider = GameObject* (lo setea el editor/carga).
    class ScriptTriggerListener : public ITriggerListener
    {
    public:
        ScriptTriggerListener(ScriptManager* mgr, GameObject* owner)
            : m_mgr(mgr), m_owner(owner) {}

        void onTriggerEnter(const TriggerEvent& e) override
        {
            m_mgr->onTriggerEvent(m_owner, TriggerPhase::Enter, static_cast<GameObject*>(e.other));
        }
        void onTriggerStay(const TriggerEvent& e) override
        {
            m_mgr->onTriggerEvent(m_owner, TriggerPhase::Stay, static_cast<GameObject*>(e.other));
        }
        void onTriggerExit(const TriggerEvent& e) override
        {
            m_mgr->onTriggerEvent(m_owner, TriggerPhase::Exit, static_cast<GameObject*>(e.other));
        }

    private:
        ScriptManager* m_mgr;
        GameObject*    m_owner;
    };

    ScriptManager::ScriptManager()  = default;
    ScriptManager::~ScriptManager() = default;

    void ScriptManager::init(const std::string& scriptsDir)
    {
        // Solo libs sin acceso a proceso/filesystem: los scripts de gameplay
        // no necesitan io/os, y así un script no puede tocar disco.
        m_lua.open_libraries(sol::lib::base, sol::lib::math,
                             sol::lib::string, sol::lib::table);

        ScriptBindings::registerAll(*this);

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
            // Si nunca llegó a registrarse, guardamos su mtime pa poder
            // reintentar la carga cuando el archivo cambie.
            if (!m_registry.count(className))
            {
                std::error_code ec;
                m_erroredScripts[className] = { path, std::filesystem::last_write_time(path, ec) };
            }
            log("Script '" + className + "': error de compilación: " + err.what());
            return false;
        }

        sol::object classObj = m_lua[className];
        if (classObj.get_type() != sol::type::table)
        {
            m_compileErrors[className] =
                "el archivo no define una tabla global '" + className + "'";
            if (!m_registry.count(className))
            {
                std::error_code ec;
                m_erroredScripts[className] = { path, std::filesystem::last_write_time(path, ec) };
            }
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
        m_erroredScripts.erase(className);
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

    void ScriptManager::rebuildAliveSet()
    {
        m_alive.clear();
        if (!m_scene) return;
        m_scene->traverse([this](GameObject* go) { m_alive.insert(go); });
    }

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
        if (!comp.instance.valid()) return;   // clase no registrada (missing)

        comp.instance["entity"] = LuaEntity{ comp.owner, this };

        auto isFn = [&](const char* n) {
            return comp.instance[n].get_type() == sol::type::function;
        };
        comp.hasAwake          = isFn("Awake");
        comp.hasStart          = isFn("Start");
        comp.hasUpdate         = isFn("Update");
        comp.hasFixedUpdate    = isFn("FixedUpdate");
        comp.hasLateUpdate     = isFn("LateUpdate");
        comp.hasOnDestroy      = isFn("OnDestroy");
        comp.hasOnTriggerEnter = isFn("OnTriggerEnter");
        comp.hasOnTriggerStay  = isFn("OnTriggerStay");
        comp.hasOnTriggerExit  = isFn("OnTriggerExit");
    }

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

    void ScriptManager::callTriggerCallback(ScriptComponent& comp, const char* fn, GameObject* other)
    {
        if (comp.hasError || !comp.instance.valid()) return;
        sol::protected_function f = comp.instance[fn];
        auto r = f(comp.instance, LuaEntity{ other, this });
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

    void ScriptManager::onTriggerEvent(GameObject* owner, TriggerPhase phase, GameObject* other)
    {
        m_triggerQueue.push_back({ owner, phase, other });
    }

    void ScriptManager::registerTriggerListeners()
    {
        if (!m_scene) return;
        m_scene->traverse([&](GameObject* go) {
            std::shared_ptr<Collider> collider = go->anyCollider();
            if (!collider) return;
            auto listener = std::make_unique<ScriptTriggerListener>(this, go);
            collider->addListener(listener.get());
            m_triggerListeners.push_back(std::move(listener));
            m_triggerListenerColliders.push_back(collider); // shared -> weak
        });
    }

    void ScriptManager::clearTriggerListeners()
    {
        // Desregistra de los colliders todavía vivos (onPlayStop corre ANTES de
        // que el restore de la escena destruya/recree los colliders de Play);
        // los ya expirados se ignoran (su lista de listeners murió con ellos).
        for (size_t i = 0; i < m_triggerListeners.size(); ++i)
        {
            if (auto collider = m_triggerListenerColliders[i].lock())
                collider->removeListener(m_triggerListeners[i].get());
        }
        m_triggerListeners.clear();
        m_triggerListenerColliders.clear();
        m_triggerQueue.clear();
    }

    void ScriptManager::drainTriggerQueue()
    {
        if (m_triggerQueue.empty()) return;
        // Snapshot: un callback puede encolar más triggers o mutar la escena;
        // se procesa lo de este frame y lo reentrante queda pal siguiente.
        std::vector<QueuedTrigger> batch;
        batch.swap(m_triggerQueue);

        for (const QueuedTrigger& t : batch)
        {
            if (!isAlive(t.owner)) continue;
            // Snapshot de los scripts del owner: un callback puede
            // Add/RemoveComponent (el remove va diferido, así que los punteros
            // siguen válidos este frame).
            std::vector<ScriptComponent*> scripts;
            for (auto& s : t.owner->getScripts()) scripts.push_back(s.get());

            for (ScriptComponent* s : scripts)
            {
                switch (t.phase)
                {
                    case TriggerPhase::Enter:
                        if (s->hasOnTriggerEnter) callTriggerCallback(*s, "OnTriggerEnter", t.other);
                        break;
                    case TriggerPhase::Stay:
                        if (s->hasOnTriggerStay) callTriggerCallback(*s, "OnTriggerStay", t.other);
                        break;
                    case TriggerPhase::Exit:
                        if (s->hasOnTriggerExit) callTriggerCallback(*s, "OnTriggerExit", t.other);
                        break;
                }
            }
        }
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

        // Colliders ya vivos aquí (Play no los recrea al arrancar): registra
        // el listener de triggers en cada uno.
        registerTriggerListeners();
    }

    void ScriptManager::onPlayStop()
    {
        clearTriggerListeners();
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

        // Triggers encolados por el paso de física de este frame
        // (physics.stepSimulation corre antes que scriptManager.update en el
        // loop principal): OnTriggerEnter/Stay/Exit antes de Update, cercano al
        // orden de Unity (callbacks de física preceden a Update).
        drainTriggerQueue();

        for (auto* c : comps) if (c->hasUpdate) callCallback(*c, "Update", &dt);

        m_fixedAccumulator = std::min(m_fixedAccumulator + dt, kMaxAccumulator);
        while (m_fixedAccumulator >= kFixedStep)
        {
            float step = kFixedStep;
            for (auto* c : comps) if (c->hasFixedUpdate) callCallback(*c, "FixedUpdate", &step);
            m_fixedAccumulator -= kFixedStep;
        }

        for (auto* c : comps) if (c->hasLateUpdate) callCallback(*c, "LateUpdate", nullptr);

        // RemoveComponent("Script:X") diferidos.
        // Snapshot antes de llamar a Lua — un callback puede añadir
        // componentes/entities y invalidar la iteración en vivo.
        std::vector<ScriptComponent*> toRemove;
        m_scene->traverse([&](GameObject* go) {
            for (auto& s : go->getScripts())
                if (s->pendingRemove) toRemove.push_back(s.get());
        });
        for (ScriptComponent* s : toRemove) callOnDestroy(*s);
        m_scene->traverse([&](GameObject* go) {
            auto& scripts = go->getScripts();
            scripts.erase(
                std::remove_if(scripts.begin(), scripts.end(),
                    [](const std::unique_ptr<ScriptComponent>& s) { return s->pendingRemove; }),
                scripts.end());
        });

        // Cola de destroy de entities (Scene.Destroy) — tras LateUpdate
        // La cola se mueve a un local antes de iterar: un OnDestroy puede
        // llamar Scene.Destroy y hacer push_back sobre m_destroyQueue en
        // plena iteración (UB con range-for sobre el propio vector). Lo
        // encolado reentrante se procesa el frame siguiente.
        std::vector<GameObject*> queue;
        queue.swap(m_destroyQueue);
        for (GameObject* go : queue)
        {
            if (!isAlive(go)) continue;   // destruido dos veces o hijo de otro destruido
            // Snapshot antes de llamar a Lua — un callback puede añadir
            // componentes/entities y invalidar la iteración en vivo.
            std::vector<ScriptComponent*> subtreeScripts;
            go->traverse([&](GameObject* n) {
                for (auto& s : n->getScripts()) subtreeScripts.push_back(s.get());
            });
            for (ScriptComponent* s : subtreeScripts) callOnDestroy(*s);
            if (m_onDestroying) m_onDestroying(go);
            m_scene->removeGameObject(go);
            rebuildAliveSet();
        }
    }

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
            // El editor pudo borrar entities este mismo frame.
            rebuildAliveSet();

            // Reinstancia los comps vivos de esta clase preservando el valor
            // actual de las props serializables (spec: estado no
            // serializable se pierde).
            // Snapshot antes de llamar a Lua — un callback puede añadir
            // componentes/entities y invalidar la iteración en vivo.
            std::vector<ScriptComponent*> toReinstantiate;
            m_scene->traverse([&](GameObject* go) {
                for (auto& s : go->getScripts())
                    if (s->scriptName == name && s->instance.valid())
                        toReinstantiate.push_back(s.get());
            });
            for (ScriptComponent* s : toReinstantiate)
            {
                std::map<std::string, ScriptValue> current = s->overrides;
                for (const ScriptProp& p : m_registry[name].props)
                {
                    sol::object v = s->instance[p.name];
                    if (v.get_type() == sol::type::number)       current[p.name] = v.as<double>();
                    else if (v.get_type() == sol::type::boolean) current[p.name] = v.as<bool>();
                    else if (v.get_type() == sol::type::string)  current[p.name] = v.as<std::string>();
                }

                instantiateComponentWith(*s, current);
                if (s->hasAwake) callCallback(*s, "Awake", nullptr);
                if (s->hasStart) callCallback(*s, "Start", nullptr);
                s->started = true;
            }
        }

        // 1b) Reintento de scripts que fallaron su primera carga si su
        // archivo cambió — los ya registrados se cubren arriba.
        std::vector<std::string> retryErrored;
        for (auto& [name, entry] : m_erroredScripts)
        {
            auto mtime = std::filesystem::last_write_time(entry.first, ec);
            if (!ec && mtime != entry.second) retryErrored.push_back(name);
        }
        for (const std::string& name : retryErrored)
        {
            const std::filesystem::path path = m_erroredScripts[name].first;
            log("Script '" + name + "': reintentando script con error previo");
            m_erroredScripts[name].second = std::filesystem::last_write_time(path, ec);
            loadScript(path);
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
}
