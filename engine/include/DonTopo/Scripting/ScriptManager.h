#pragma once
#include <sol/sol.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "DonTopo/Scripting/ScriptComponent.h"

namespace DonTopo {

class Scene;
class GameObject;
class PhysicsManager;
class AudioManager;
class Collider;
class ScriptTriggerListener;   // adapter ITriggerListener->queue (defined in .cpp)
class ScriptCollisionListener; // adapter ICollisionListener->the SAME queue

// Phase of a trigger event queued from the physics module.
enum class TriggerPhase { Enter, Stay, Exit };

// Which physics path produced the queued event. Trigger and collision share a
// queue (same arrival order, same draining, same reentrancy model); this
// is the only thing that decides whether OnTrigger* or OnCollision* is called.
enum class ContactKind { Trigger, Collision };

// A serializable prop detected in a script's class table.
struct ScriptProp {
    std::string name;
    ScriptValue defaultValue;
    // Lua 5.4 distinguishes 5 (integer) from 5.0 (float); the UI uses DragInt vs
    // DragFloat depending on this.
    bool isInteger = false;
};

struct ScriptClass {
    sol::table classTable;
    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
    std::vector<ScriptProp> props;   // alphabetical order, stable for the UI
};

// Owner of the engine's single Lua VM. Loads/registers/instantiates scripts.
// The engine never calls Lua outside this class.
class ScriptManager {
public:
    ScriptManager();
    ~ScriptManager();
    ScriptManager(const ScriptManager&)            = delete;
    ScriptManager& operator=(const ScriptManager&) = delete;

    // Opens safe libs (base/math/string/table), registers bindings and loads
    // all the .lua files in scriptsDir (recursive). The pointers are
    // non-owning, same pattern as EditorUI::setPhysicsManager.
    void init(const std::string& scriptsDir);
    void setScene(Scene* scene)                    { m_scene = scene; }
    void setPhysicsManager(PhysicsManager* p)      { m_physics = p; }
    void setAudioManager(AudioManager* a)          { m_audio = a; }
    void setLogCallback(std::function<void(const std::string&)> cb) { m_log = std::move(cb); }

    bool loadScript(const std::filesystem::path& path);
    bool hasClass(const std::string& name) const { return m_registry.count(name) > 0; }
    const std::map<std::string, ScriptClass>& getRegistry() const { return m_registry; }
    const std::string* getCompileError(const std::string& name) const;
    // Scripts folder resolved in init(), used by the editor to create
    // new .lua files in the right place.
    const std::filesystem::path& scriptsDirPath() const { return m_scriptsDir; }

    // New instance table: copy of props (defaults + overrides) +
    // metatable __index -> class table. Invalid table if name does not exist.
    sol::table createInstance(const std::string& name,
                              const std::map<std::string, ScriptValue>& overrides);

    sol::state& lua() { return m_lua; }
    Scene* scene() const              { return m_scene; }
    PhysicsManager* physics() const   { return m_physics; }
    AudioManager* audioManager() const { return m_audio; }

    void log(const std::string& msg) { if (m_log) m_log(msg); }

    // Epoch of the callbacks that scripts leave hooked OUTSIDE the
    // lua_State (today those of the UI, which live in the ButtonComponent and can
    // fire on any frame). Whoever stores one keeps this weak_ptr and
    // calls NOTHING if it has expired: it expires when the ScriptManager dies (and
    // with it the lua_State the call would point to) and when a script is hot
    // reloaded, because the code and instance that registered the
    // callback are no longer the ones that run. Hooking it again is up to the script.
    std::weak_ptr<char> callbackEpoch() const { return m_callbackEpoch; }
    void invalidateScriptCallbacks();

    // true if go is still in the scene tree. The bindings query it
    // before dereferencing: a destroyed entity produces a Lua error, never a crash.
    bool isAlive(GameObject* go) const { return m_alive.count(go) > 0; }
    // Rebuilds the set from the scene. Called at the start of each lifecycle
    // update and after any structural add/remove.
    void rebuildAliveSet();

    void setOnInstantiated(std::function<void(GameObject*)> cb) { m_onInstantiated = std::move(cb); }
    // Deferred deletion, processed at the end of the lifecycle frame.
    void queueDestroy(GameObject* go) { m_destroyQueue.push_back(go); }
    // Creates comp's instance table (defaults+overrides), injects
    // self.entity and caches which callbacks it defines. Does NOT call Awake/Start.
    void instantiateComponent(ScriptComponent& comp);
    const std::function<void(GameObject*)>& onInstantiated() const { return m_onInstantiated; }

    // Called by the ScriptTriggerListener adapter (from the physics step)
    // to queue a trigger event; it is drained in update() of the same frame.
    void onTriggerEvent(GameObject* owner, TriggerPhase phase, GameObject* other);
    // Twin for non-trigger pairs (ScriptCollisionListener adapter). Same
    // queue, same frame.
    void onCollisionEvent(GameObject* owner, TriggerPhase phase, GameObject* other);

    void onPlayStart();
    void onPlayStop();
    void update(float dt);
    // Protected OnDestroy of a single component (used by EditorUI when
    // removing the component in Play and by the queue processing).
    void callOnDestroy(ScriptComponent& comp);

    // Hot reload: detects mtime changes and new .lua files in Scripts/.
    // Callable every frame; it only scans 1 out of every 60 calls (~1s at 60fps).
    void pollChanges();

    // FixedUpdate fixed step. Public because the bindings publish it as-is
    // as Time.fixedDeltaTime: duplicating the constant there would leave two
    // sources of truth that would drift apart as soon as one of them changed.
    static constexpr float kFixedStep = 1.0f / 60.0f;
private:
    // Core of instantiateComponent parameterized by values (hot
    // reload instantiates with the current values, not with comp.overrides).
    void instantiateComponentWith(ScriptComponent& comp,
                                  const std::map<std::string, ScriptValue>& values);
    int m_pollCounter = 0;

    // Calls comp.instance[fn](instance[, dt]) protected; error -> log +
    // hasError (the comp stops receiving callbacks).
    void callCallback(ScriptComponent& comp, const char* fn, const float* dt);
    // Variant for triggers: passes the Entity `other` as an argument.
    void callTriggerCallback(ScriptComponent& comp, const char* fn, GameObject* other);
    // Same, but it checks on the instance whether the script defines `fn` instead of
    // looking at a cached flag. The OnCollision* use it: the probe costs one
    // table lookup, the same one callTriggerCallback already does to get the
    // function, so there is no extra lookup per dispatched event.
    void callOptionalCallback(ScriptComponent& comp, const char* fn, GameObject* other);
    // Like callOptionalCallback, with a string argument (OnAnimationEvent).
    void callOptionalStringCallback(ScriptComponent& comp, const char* fn, const std::string& arg);

    // Registers a ScriptTriggerListener AND a ScriptCollisionListener on the
    // collider of every GameObject that has one (onPlayStart); unregisters them
    // from the live colliders and cleans up the adapters (onPlayStop).
    // drainTriggerQueue dispatches the queue to the OnTrigger*/OnCollision*
    // callbacks of the owner's scripts.
    void registerTriggerListeners();
    void clearTriggerListeners();
    void drainTriggerQueue();
    // Delivers each Animator's firedEvents() to OnAnimationEvent of the
    // scripts on its GameObject. Runs once per update, before Update.
    void deliverAnimationEvents();

    // A pending physics event to dispatch to Lua.
    struct QueuedTrigger {
        GameObject* owner;
        TriggerPhase phase;
        GameObject* other;
        ContactKind kind = ContactKind::Trigger;
    };
    std::vector<QueuedTrigger> m_triggerQueue;
    std::vector<std::unique_ptr<ScriptTriggerListener>> m_triggerListeners;
    std::vector<std::unique_ptr<ScriptCollisionListener>> m_collisionListeners;
    // Colliders where a listener was registered, to unregister in onPlayStop
    // (weak: a GameObject can be destroyed in Play and take its collider with it).
    std::vector<std::weak_ptr<Collider>> m_triggerListenerColliders;
    std::vector<std::weak_ptr<Collider>> m_collisionListenerColliders;
    // All the live ScriptComponents in the scene, in traverse order.
    std::vector<ScriptComponent*> collectComponents();
    static constexpr float kMaxAccumulator = 0.25f;   // anti spiral-of-death
    float m_fixedAccumulator = 0.0f;
    bool  m_playing = false;

    // Extracts the serializable props (number/boolean/string) from classTable.
    std::vector<ScriptProp> detectProps(const sol::table& classTable);

    sol::state m_lua;

    // Liveness witness of the callbacks registered from Lua (see
    // callbackEpoch). It is declared AFTER m_lua on purpose: when the manager is destroyed
    // it is released before the state, so no callback ever
    // sees a half-closed lua_State.
    std::shared_ptr<char> m_callbackEpoch = std::make_shared<char>(0);
    std::filesystem::path m_scriptsDir;
    std::map<std::string, ScriptClass> m_registry;
    // Last compile error per script name (persists even if the
    // script stays registered with its previous valid version).
    std::map<std::string, std::string> m_compileErrors;
    // Scripts that failed their first load, retried when their
    // mtime changes; those already registered are retried via m_registry.
    std::map<std::string, std::pair<std::filesystem::path, std::filesystem::file_time_type>> m_erroredScripts;
    std::function<void(const std::string&)> m_log;

    Scene*          m_scene   = nullptr;
    PhysicsManager* m_physics = nullptr;
    AudioManager*   m_audio   = nullptr;
    std::set<GameObject*> m_alive;

    std::vector<GameObject*> m_destroyQueue;
    std::function<void(GameObject*)> m_onInstantiated;
};

} // namespace DonTopo
