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
