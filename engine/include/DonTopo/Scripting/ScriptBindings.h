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

    // Tabla Time: la escriben estas dos funciones, no el binding, porque sus
    // valores cambian cada frame y una tabla Lua no puede tener propiedades
    // calculadas sin un metatable por campo (más caro que reescribir cuatro
    // números). Las llama ScriptManager: tick() una vez por Update, reset() al
    // entrar en Play. Fuera de Play los valores se quedan congelados en los del
    // último frame jugado, que es lo que un script vería igualmente.
    void tickTime(ScriptManager& mgr, float dt);
    void resetTime(ScriptManager& mgr);

    // Vacía la tabla del lua_State donde viven las funciones Lua enganchadas a
    // los botones. La llama ScriptManager::invalidateScriptCallbacks junto con
    // el relevo de la época: la época deja mudos a los callbacks viejos y esto
    // suelta las funciones para que el GC de Lua se las lleve.
    void clearUiCallbacks(ScriptManager& mgr);
}

} // namespace DonTopo
