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
