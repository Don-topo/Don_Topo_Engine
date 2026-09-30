#pragma once

#include <string>

namespace DonTopo {

class ScriptManager;
class GameObject;

// Lightweight handle the bindings pass to Lua instead of a raw GameObject*.
// All methods validate mgr->isAlive(go) before touching the pointer
// (the validation arrives with the lifecycle in Task 6/8).
struct LuaEntity {
    GameObject*    go  = nullptr;
    ScriptManager* mgr = nullptr;
};

namespace ScriptBindings {
    // Registers the whole API (Vec3, Log, Input/Key, Entity, Transform,
    // components, Scene) in mgr's VM. Called once from
    // ScriptManager::init.
    void registerAll(ScriptManager& mgr);

    // Single-slot mailbox for DonTopo.loadScene: the binding does NOT load the
    // scene (it would destroy the GameObject running the current script),
    // it only leaves the path here. Whoever owns the scene (EditorUI::draw in the
    // editor, the frame loop in the runtime) drains it OUTSIDE the script tick
    // and does the load. If a frame leaves several requests the
    // last one wins: the earlier ones are overwritten on write.
    // Returns true and fills outPath if there was a pending request (and
    // consumes it); false if there was none.
    bool takePendingSceneLoad(std::string& outPath);

    // Time table: these two functions write it, not the binding, because its
    // values change every frame and a Lua table cannot have
    // computed properties without a metatable per field (more expensive than rewriting four
    // numbers). ScriptManager calls them: tick() once per Update, reset() when
    // entering Play. Outside Play the values stay frozen at those of the
    // last frame played, which is what a script would see anyway.
    void tickTime(ScriptManager& mgr, float dt);
    void resetTime(ScriptManager& mgr);

    // Empties the table in the lua_State where the Lua functions hooked to
    // the buttons live. ScriptManager::invalidateScriptCallbacks calls it together with
    // the epoch handover: the epoch mutes the old callbacks and this
    // releases the functions so Lua's GC takes them away.
    void clearUiCallbacks(ScriptManager& mgr);
}

} // namespace DonTopo
