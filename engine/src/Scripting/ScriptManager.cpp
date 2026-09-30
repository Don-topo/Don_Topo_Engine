#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Physics/Colliders/Collider.h"
#include <algorithm>

namespace DonTopo
{
    // Adapter that hooks the physics module's trigger callbacks to the
    // ScriptManager queue. One per collider registered in onPlayStart;
    // it captures the GameObject that owns the trigger collider. `e.other` is the opaque
    // owner (void*) of the other collider = GameObject* (set by the editor/loading).
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

    // Twin of the previous one for NON-trigger pairs. Same owner, same queue: the
    // only thing that changes is that the event is marked as ContactKind::Collision,
    // so it inherits the reentrancy model as is (snapshot in
    // drainTriggerQueue) and the order relative to Update.
    class ScriptCollisionListener : public ICollisionListener
    {
    public:
        ScriptCollisionListener(ScriptManager* mgr, GameObject* owner)
            : m_mgr(mgr), m_owner(owner) {}

        void onCollisionEnter(const CollisionEvent& e) override
        {
            m_mgr->onCollisionEvent(m_owner, TriggerPhase::Enter, static_cast<GameObject*>(e.other));
        }
        void onCollisionStay(const CollisionEvent& e) override
        {
            m_mgr->onCollisionEvent(m_owner, TriggerPhase::Stay, static_cast<GameObject*>(e.other));
        }
        void onCollisionExit(const CollisionEvent& e) override
        {
            m_mgr->onCollisionEvent(m_owner, TriggerPhase::Exit, static_cast<GameObject*>(e.other));
        }

    private:
        ScriptManager* m_mgr;
        GameObject*    m_owner;
    };

    ScriptManager::ScriptManager()  = default;
    ScriptManager::~ScriptManager() = default;

    void ScriptManager::init(const std::string& scriptsDir)
    {
        // Only libs without process/filesystem access: gameplay scripts
        // do not need io/os, and this way a script cannot touch disk.
        m_lua.open_libraries(sol::lib::base, sol::lib::math,
                             sol::lib::string, sol::lib::table);

        ScriptBindings::registerAll(*this);

        m_scriptsDir = scriptsDir;
        std::error_code ec;
        if (!std::filesystem::is_directory(m_scriptsDir, ec))
        {
            log("Scripts: folder '" + scriptsDir + "' not found, no scripts");
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
            // If it never got registered, we store its mtime so we can
            // retry the load when the file changes.
            if (!m_registry.count(className))
            {
                std::error_code ec;
                m_erroredScripts[className] = { path, std::filesystem::last_write_time(path, ec) };
            }
            log("Script '" + className + "': compilation error: " + err.what());
            return false;
        }

        sol::object classObj = m_lua[className];
        if (classObj.get_type() != sol::type::table)
        {
            m_compileErrors[className] =
                "the file does not define a global table '" + className + "'";
            if (!m_registry.count(className))
            {
                std::error_code ec;
                m_erroredScripts[className] = { path, std::filesystem::last_write_time(path, ec) };
            }
            log("Script '" + className + "': does not define the global table '" + className + "'");
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
        log("Script '" + className + "' registered (" +
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
                    // lua_isinteger distinguishes 5 (integer) from 5.0 (float)
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
                    continue; // nested functions/tables: they are not props
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

        // Copy of props to the instance: each instance has its own,
        // editing one does not touch the others. Functions are inherited via metatable.
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
        if (!comp.instance.valid()) return;   // class not registered (missing)

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

    void ScriptManager::callOptionalCallback(ScriptComponent& comp, const char* fn, GameObject* other)
    {
        if (comp.hasError || !comp.instance.valid()) return;
        sol::object entry = comp.instance[fn];
        if (entry.get_type() != sol::type::function) return; // el script no lo define
        sol::protected_function f = entry;
        auto r = f(comp.instance, LuaEntity{ other, this });
        if (!r.valid())
        {
            sol::error err = r;
            log("Script '" + comp.scriptName + "' " + fn + ": " + std::string(err.what()));
            comp.hasError = true;
        }
    }

    void ScriptManager::callOptionalStringCallback(ScriptComponent& comp, const char* fn, const std::string& arg)
    {
        if (comp.hasError || !comp.instance.valid()) return;
        sol::object entry = comp.instance[fn];
        if (entry.get_type() != sol::type::function) return; // el script no lo define
        sol::protected_function f = entry;
        auto r = f(comp.instance, arg);
        if (!r.valid())
        {
            sol::error err = r;
            log("Script '" + comp.scriptName + "' " + fn + ": " + std::string(err.what()));
            comp.hasError = true;
        }
    }

    void ScriptManager::deliverAnimationEvents()
    {
        // First they are collected and then called: a callback can instantiate or
        // destroy objects, and that must not happen with the traverse open.
        std::vector<GameObject*> conEventos;
        m_scene->traverse([&](GameObject* go) {
            const auto& anim = go->getAnimator();
            if (anim && !anim->firedEvents().empty() && !go->getScripts().empty())
                conEventos.push_back(go);
        });
        for (GameObject* go : conEventos)
        {
            if (!isAlive(go) || !go->getAnimator()) continue;
            // Copy: a callback can touch the Animator (Play, CrossFade).
            const std::vector<std::string> nombres = go->getAnimator()->firedEvents();
            std::vector<ScriptComponent*> scripts;
            for (auto& s : go->getScripts()) scripts.push_back(s.get());
            for (const std::string& n : nombres)
                for (ScriptComponent* s : scripts)
                    callOptionalStringCallback(*s, "OnAnimationEvent", n);
        }
    }

    void ScriptManager::callOnDestroy(ScriptComponent& comp)
    {
        if (comp.hasOnDestroy) callCallback(comp, "OnDestroy", nullptr);
    }

    void ScriptManager::onTriggerEvent(GameObject* owner, TriggerPhase phase, GameObject* other)
    {
        m_triggerQueue.push_back({ owner, phase, other, ContactKind::Trigger });
    }

    void ScriptManager::onCollisionEvent(GameObject* owner, TriggerPhase phase, GameObject* other)
    {
        m_triggerQueue.push_back({ owner, phase, other, ContactKind::Collision });
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

            // The same collider carries both adapters: which one fires is decided by
            // PhysX depending on whether it is a trigger or not, and that can change in the middle of
            // Play (col.isTrigger from Lua).
            auto collisionListener = std::make_unique<ScriptCollisionListener>(this, go);
            collider->addCollisionListener(collisionListener.get());
            m_collisionListeners.push_back(std::move(collisionListener));
            m_collisionListenerColliders.push_back(collider);
        });
    }

    void ScriptManager::clearTriggerListeners()
    {
        // Unregisters from the colliders that are still alive (onPlayStop runs BEFORE
        // the scene restore destroys/recreates the Play colliders);
        // the already-expired ones are ignored (their listener list died with them).
        for (size_t i = 0; i < m_triggerListeners.size(); ++i)
        {
            if (auto collider = m_triggerListenerColliders[i].lock())
                collider->removeListener(m_triggerListeners[i].get());
        }
        for (size_t i = 0; i < m_collisionListeners.size(); ++i)
        {
            if (auto collider = m_collisionListenerColliders[i].lock())
                collider->removeCollisionListener(m_collisionListeners[i].get());
        }
        m_triggerListeners.clear();
        m_triggerListenerColliders.clear();
        m_collisionListeners.clear();
        m_collisionListenerColliders.clear();
        m_triggerQueue.clear();
    }

    void ScriptManager::drainTriggerQueue()
    {
        if (m_triggerQueue.empty()) return;
        // Snapshot: a callback can queue more triggers or mutate the scene;
        // this frame's are processed and the reentrant ones are left for the next.
        std::vector<QueuedTrigger> batch;
        batch.swap(m_triggerQueue);

        for (const QueuedTrigger& t : batch)
        {
            if (!isAlive(t.owner)) continue;
            // Snapshot of the owner's scripts: a callback can
            // Add/RemoveComponent (the remove is deferred, so the pointers
            // stay valid this frame).
            std::vector<ScriptComponent*> scripts;
            for (auto& s : t.owner->getScripts()) scripts.push_back(s.get());

            for (ScriptComponent* s : scripts)
            {
                if (t.kind == ContactKind::Collision)
                {
                    // No cached flag in ScriptComponent (see
                    // callOptionalCallback): the call itself
                    // does the probing.
                    switch (t.phase)
                    {
                        case TriggerPhase::Enter:
                            callOptionalCallback(*s, "OnCollisionEnter", t.other); break;
                        case TriggerPhase::Stay:
                            callOptionalCallback(*s, "OnCollisionStay", t.other); break;
                        case TriggerPhase::Exit:
                            callOptionalCallback(*s, "OnCollisionExit", t.other); break;
                    }
                    continue;
                }
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
        // Time.time counts from this instant: a second Play after a Stop
        // starts from zero, it does not continue where the previous session left off.
        ScriptBindings::resetTime(*this);
        m_destroyQueue.clear();
        rebuildAliveSet();

        auto comps = collectComponents();
        for (auto* c : comps) instantiateComponent(*c);
        // Two-pass like Unity: all the Awake before the first Start.
        for (auto* c : comps) if (c->hasAwake) callCallback(*c, "Awake", nullptr);
        for (auto* c : comps)
        {
            if (c->hasStart) callCallback(*c, "Start", nullptr);
            c->started = true;
        }

        // Colliders already alive here (Play does not recreate them on start): registers
        // the trigger listener on each one.
        registerTriggerListeners();
    }

    void ScriptManager::invalidateScriptCallbacks()
    {
        // Renewing the witness takes all the already registered callbacks out of play
        // (they hold it as a weak_ptr) WITHOUT touching the component that
        // holds them: the std::function is still there, but it calls nothing.
        m_callbackEpoch = std::make_shared<char>(0);
        ScriptBindings::clearUiCallbacks(*this);
    }

    void ScriptManager::onPlayStop()
    {
        clearTriggerListeners();
        // The instances are destroyed next: a callback that survives
        // the Stop would call a method of an object that already did OnDestroy.
        invalidateScriptCallbacks();
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
        // Before any callback: the Awake and Start of new components
        // must already see THIS frame's Time, not the previous one's.
        ScriptBindings::tickTime(*this, dt);
        rebuildAliveSet();

        // Snapshot of pointers: scripts can add components in the
        // middle of the frame (they are picked up the next frame); deletions ALWAYS go
        // through deferred queues, so no pointer in the snapshot
        // dies during the iteration.
        auto comps = collectComponents();

        // Comps added after Play (Instantiate, AddComponent, editor):
        // Awake (if it did not already come from Instantiate: invalid instance) + Start.
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

        // Triggers and collisions queued by this frame's physics step
        // (physics.stepSimulation runs before scriptManager.update in the
        // main loop): OnTrigger*/OnCollision* before Update, close to Unity's
        // order (physics callbacks precede Update).
        drainTriggerQueue();

        // Animator events from the last advance (applySkinnedFrame): one per
        // frame on each side, so each batch is delivered once.
        deliverAnimationEvents();

        for (auto* c : comps) if (c->hasUpdate) callCallback(*c, "Update", &dt);

        m_fixedAccumulator = std::min(m_fixedAccumulator + dt, kMaxAccumulator);
        while (m_fixedAccumulator >= kFixedStep)
        {
            float step = kFixedStep;
            for (auto* c : comps) if (c->hasFixedUpdate) callCallback(*c, "FixedUpdate", &step);
            m_fixedAccumulator -= kFixedStep;
        }

        for (auto* c : comps) if (c->hasLateUpdate) callCallback(*c, "LateUpdate", nullptr);

        // Deferred RemoveComponent("Script:X").
        // Snapshot before calling Lua: a callback can add
        // components/entities and invalidate the live iteration.
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

        // Entity destroy queue (Scene.Destroy), after LateUpdate.
        // The queue is moved to a local before iterating: an OnDestroy can
        // call Scene.Destroy and push_back onto m_destroyQueue in the
        // middle of the iteration (UB with a range-for over the vector itself). What is
        // queued reentrantly is processed the next frame.
        std::vector<GameObject*> queue;
        queue.swap(m_destroyQueue);
        for (GameObject* go : queue)
        {
            if (!isAlive(go)) continue;   // destroyed twice or child of another destroyed one
            // Snapshot before calling Lua: a callback can add
            // components/entities and invalidate the live iteration.
            std::vector<ScriptComponent*> subtreeScripts;
            go->traverse([&](GameObject* n) {
                for (auto& s : n->getScripts()) subtreeScripts.push_back(s.get());
            });
            for (ScriptComponent* s : subtreeScripts) callOnDestroy(*s);
            // The GPU and the selection are now released by Scene::removeGameObject via
            // its listener: a single place for the three callers.
            m_scene->removeGameObject(go);
            rebuildAliveSet();
        }
    }

    void ScriptManager::pollChanges()
    {
        if (++m_pollCounter < 60) return;
        m_pollCounter = 0;

        std::error_code ec;

        // 1) Changes in registered scripts
        std::vector<std::string> changed;
        for (auto& [name, cls] : m_registry)
        {
            auto mtime = std::filesystem::last_write_time(cls.path, ec);
            if (!ec && mtime != cls.mtime) changed.push_back(name);
        }

        for (const std::string& name : changed)
        {
            const std::filesystem::path path = m_registry[name].path;
            log("Script '" + name + "' changed on disk, reloading");
            // Always update mtime (even if it compiles badly, so as not to retry
            // the same broken content in a loop).
            m_registry[name].mtime = std::filesystem::last_write_time(path, ec);
            if (!loadScript(path))
                continue;   // error logged; old instances keep running

            // The code that registered the UI callbacks no longer exists: any that
            // remained hooked point to the old class. They are invalidated here
            // and the reloaded script hooks them again in its Start.
            invalidateScriptCallbacks();

            if (!m_playing || !m_scene) continue;
            // The editor may have deleted entities this same frame.
            rebuildAliveSet();

            // Reinstantiates the live comps of this class preserving the current
            // value of the serializable props (spec: non-serializable
            // state is lost).
            // Snapshot before calling Lua: a callback can add
            // components/entities and invalidate the live iteration.
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

        // 1b) Retry of scripts that failed their first load if their
        // file changed; those already registered are covered above.
        std::vector<std::string> retryErrored;
        for (auto& [name, entry] : m_erroredScripts)
        {
            auto mtime = std::filesystem::last_write_time(entry.first, ec);
            if (!ec && mtime != entry.second) retryErrored.push_back(name);
        }
        for (const std::string& name : retryErrored)
        {
            const std::filesystem::path path = m_erroredScripts[name].first;
            log("Script '" + name + "': retrying a script that failed before");
            m_erroredScripts[name].second = std::filesystem::last_write_time(path, ec);
            loadScript(path);
        }

        // 2) New scripts in the folder
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
