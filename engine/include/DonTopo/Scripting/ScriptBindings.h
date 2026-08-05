#pragma once

#include <string>

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

    // Buzón de una sola casilla para DonTopo.loadScene: el binding NO carga la
    // escena (destruiría el GameObject que está ejecutando el script en curso),
    // solo deja aquí la ruta. Quien es dueño de la escena —EditorUI::draw en el
    // editor, el bucle de frame en el runtime— la drena FUERA del tick de
    // scripts y hace la carga. Si un frame deja varias peticiones gana la
    // última: las anteriores se pisan al escribir.
    // Devuelve true y rellena outPath si había petición pendiente (y la
    // consume); false si no había ninguna.
    bool takePendingSceneLoad(std::string& outPath);
}

} // namespace DonTopo
