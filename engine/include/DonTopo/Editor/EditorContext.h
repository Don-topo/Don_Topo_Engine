#pragma once
#include <functional>
#include <string>
#include <filesystem>
#include <glm/glm.hpp>

namespace DonTopo {

class GameObject;
class PhysicsManager;
class AudioManager;
class Renderer;
class Scene;
class ScriptManager;
class UndoManager;

// Estado compartido entre los paneles del editor, construido de nuevo cada
// frame dentro de EditorUI::draw() y pasado por referencia a cada
// Panel::draw(). `selected` es una referencia real a EditorUI::m_selected:
// un panel que la reasigna (p.ej. ScenePanel al hacer click en un nodo)
// propaga el cambio a los paneles que se dibujan después en el mismo frame
// (Viewport, Properties), igual que hacía el m_selected único de EditorUI.
struct EditorContext {
    GameObject*& selected;
    bool&        isPlaying;

    PhysicsManager* physics       = nullptr;
    Renderer*       renderer      = nullptr;
    AudioManager*   audio         = nullptr;
    Scene*          scene         = nullptr;
    ScriptManager*  scriptManager = nullptr;
    UndoManager*    undo          = nullptr;

    std::function<void(const std::string&)>   pushLog;
    std::function<void(GameObject*)>          onDelete;
    std::function<void(const glm::vec3&)>     onAxisSelected;
    // Abre path en el Script Editor (EditorUI::m_scriptEditor, fuera del
    // Consumes original de PropertiesPanel — Task 5 añadió este callback
    // porque drawScriptsSection/drawNewScriptPopup necesitan abrir el
    // fichero .lua tras editar/crear un script, y ScriptEditorPanel sigue
    // siendo propiedad de EditorUI, no de ningún panel). Vacío/no asignado
    // por defecto — solo lo rellena EditorUI::draw().
    std::function<void(const std::filesystem::path&)> openScript;
    // Abre el panel Animator (EditorUI::m_animatorPanel, fuera del alcance de
    // PropertiesPanel — mismo caso y mismo patrón que openScript). Vacío por
    // defecto: solo lo rellena EditorUI::draw().
    std::function<void()> openAnimator;
};

} // namespace DonTopo
