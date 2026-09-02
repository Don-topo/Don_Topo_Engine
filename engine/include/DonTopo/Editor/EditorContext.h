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
class EditorRenderer;
class Scene;
class ScriptManager;
class UndoManager;
class AsyncAssetLoader;
class ProjectContext;

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
    // La interfaz, no el Renderer de Vulkan: los paneles solo usan lo que
    // cualquier backend puede dar.
    EditorRenderer* renderer      = nullptr;
    AudioManager*   audio         = nullptr;
    Scene*          scene         = nullptr;
    ScriptManager*  scriptManager = nullptr;
    UndoManager*    undo          = nullptr;

    std::function<void(const std::string&)>   pushLog;
    // Igual que pushLog pero etiquetando la línea con un módulo ("Renderer",
    // "Physics", ...), que el Log Console pinta como chip de color. Viaja por
    // el mismo callback de un argumento —el panel decodifica el prefijo
    // "[Modulo] "—, así que los callers de pushLog no cambian.
    void logModule(const std::string& module, const std::string& message) const
    {
        if (pushLog)
            pushLog("[" + module + "] " + message);
    }
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
    // Abre el editor de sprites sobre esa imagen (EditorUI::m_spriteEditor,
    // fuera del alcance de PropertiesPanel — mismo patrón que openAnimator).
    std::function<void(const std::string&)> openSpriteEditor;
    // Alguien ha cambiado los sub-rects de un atlas. Lo usa PropertiesPanel para
    // tirar su caché de nombres: sin esto los combos siguen enseñando la lista
    // anterior hasta cambiar de ruta. Vacío por defecto.
    std::function<void()> onSpritesChanged;
    // Guarda los ajustes del proyecto (project.json). Lo usa RenderingPanel:
    // sus 41 controles se aplican Y persisten, y el comando de deshacer de cada
    // uno vuelve a llamarlo para que deshacer deje el fichero como estaba. El
    // dueño del project.json sigue siendo EditorUI. Vacío por defecto.
    std::function<void()> saveSettings;
    // Abre la ventana de ambiente/skybox (EditorUI::m_environmentWindowOpen).
    // Mismo patrón que openAnimator: la ventana vive en EditorUI —está atada a
    // su diálogo de carpetas y a applySkyboxFolder— y RenderingPanel solo tiene
    // el botón que la abre. Vacío por defecto.
    std::function<void()> openEnvironment;

    // Loader de assets asíncrono (vive en main.cpp, no-propietario). Sin él,
    // el drop de FBX no encola nada (loadMeshForSelected es no-op). Lo rellena
    // EditorUI::draw() a partir de EditorUI::m_assetLoader.
    AsyncAssetLoader* assetLoader = nullptr;

    // Proyecto abierto (vive en main(), no-propietario). Decide qué rutas puede
    // leer o escribir un panel: todo lo del usuario —escenas, scripts, assets—
    // pasa por project->contains() antes de tocar disco. nullptr en los tests
    // headless y en cualquier ruta anterior al selector: sin proyecto no hay
    // sandbox y el comportamiento es el de siempre.
    const ProjectContext* project = nullptr;

    // true mientras el modal de carga está activo (Load Scene en vuelo). Veta la
    // edición —gizmo, reparent de jerarquía, drops de asset— pero NO el render:
    // la ventana sigue pintando frames. Los sitios de edición lo consultan con
    // `if (!ctx.editingLocked)`. Default false: fuera de Load Scene todo se edita
    // como siempre (los tests headless lo dejan en su default).
    bool editingLocked = false;

    // Carga la escena de disco en path por la misma ruta que el Load Scene del
    // menú File (validación de JSON + reloadSceneFromJson + clear del undo).
    // Mismo patrón que openScript/openAnimator: sólo lo rellena
    // EditorUI::draw(), vacío por defecto en los tests headless.
    std::function<void(const std::filesystem::path&)> requestLoadScene;
    // Guarda la escena actual y, si el guardado sale bien, carga thenLoad (vacío
    // = no cargar nada después). Si la escena nunca se guardó, abre el mismo
    // diálogo Save Scene del menú File y encadena la carga a su confirmación;
    // si el usuario lo cancela, no se carga nada.
    std::function<void(const std::filesystem::path& thenLoad)> requestSaveScene;
};

} // namespace DonTopo
