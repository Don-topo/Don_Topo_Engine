#pragma once
#include <string>
#include <map>
#include <variant>
#include <sol/sol.hpp>

namespace DonTopo {

class GameObject;

// Value of a serializable script prop (the 3 types ScriptManager detects
// in the class table).
using ScriptValue = std::variant<double, bool, std::string>;

// A Lua script attached to a GameObject. The instance (Lua table) only
// exists in Play Mode; in Edit Mode the component is just name + overrides.
class ScriptComponent {
public:
    ScriptComponent(std::string name, GameObject* ownerGo)
        : scriptName(std::move(name)), owner(ownerGo) {}

    ScriptComponent(const ScriptComponent&)            = delete;
    ScriptComponent& operator=(const ScriptComponent&) = delete;

    std::string scriptName;
    GameObject* owner = nullptr;

    // Lua instance table: invalid (default) outside Play Mode.
    sol::table instance;
    bool started = false;
    // Runtime error in a callback: it stops receiving callbacks until hot
    // reload or Stop (avoids error spam and a crash loop).
    bool hasError = false;
    // RemoveComponent from Lua in the middle of Update is deferred to the end of
    // the frame (same reason as the entity destroy queue).
    bool pendingRemove = false;

    // Cache of which callbacks the script defines. It is computed once when
    // instantiating, not every frame (spec).
    bool hasAwake = false, hasStart = false, hasUpdate = false,
         hasFixedUpdate = false, hasLateUpdate = false, hasOnDestroy = false;
    // Trigger callbacks (Unity style): they receive the Entity that caused the
    // overlap. Only the trigger side of the pair receives them.
    bool hasOnTriggerEnter = false, hasOnTriggerStay = false, hasOnTriggerExit = false;

    // Props edited in the editor that differ from the .lua default.
    // Only this is serialized; the defaults live in the script.
    std::map<std::string, ScriptValue> overrides;
};

} // namespace DonTopo
