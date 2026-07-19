#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace ax::NodeEditor { struct EditorContext; }
namespace IGFD { class FileDialog; }

namespace DonTopo {

struct EditorContext;
class GameObject;

// Ventana "Animator" — canvas de nodos del grafo de estados del GameObject
// seleccionado (nodo = estado con su clip y su loop, link = transición).
//
// Panel propio y no un bloque de Properties porque el canvas de
// imgui-node-editor necesita zoom y pan propios: en la columna de Properties
// sería inservible.
class AnimatorPanel {
public:
    AnimatorPanel();
    ~AnimatorPanel();
    AnimatorPanel(const AnimatorPanel&)            = delete;
    AnimatorPanel& operator=(const AnimatorPanel&) = delete;

    void draw(EditorContext& ctx);
    bool* GetOpenPtr() { return &m_open; }
    void open() { m_open = true; }

private:
    // Vuelca AnimatorComponent::State::editorPos al canvas. Solo al cambiar de
    // objeto: hacerlo cada frame pelearía con el ratón del usuario y los nodos
    // no se podrían arrastrar.
    void syncPositionsFromComponent(GameObject* go);
    // Camino inverso, cada frame: el canvas es la fuente de verdad de las
    // posiciones mientras el panel está abierto, y editorPos es lo que se
    // serializa.
    void syncPositionsToComponent(GameObject* go);
    void drawParameterList(EditorContext& ctx, GameObject* go);
    void drawGraph(EditorContext& ctx, GameObject* go);
    void drawConditionsPopup(EditorContext& ctx, GameObject* go);
    // Lista de ficheros FBX que aportan clips, con Add/Remove y rename inline.
    void drawAnimationSources(EditorContext& ctx, GameObject* go);
    // Drena el diálogo de fichero cada frame, incondicionalmente — incluso si
    // el panel está cerrado o colapsado (por eso draw() la llama fuera del
    // if(m_open) y del ImGui::Begin/End, no dentro): si solo se drenara con
    // la ventana visible, cerrar o colapsar el panel con el diálogo abierto
    // dejaría m_animSrcDlgOpen (y el estado interno de IGFD) atascado en true
    // para siempre. Mismo patrón que PropertiesPanel::draw + drawMeshDialog.
    void drawAnimationSourceDialog(EditorContext& ctx);
    // Importa path como fuente del GameObject seleccionado, vía comando (undo).
    void importAnimationSource(EditorContext& ctx, GameObject* go, const std::string& path);

    ax::NodeEditor::EditorContext* m_ctx = nullptr;
    bool m_open = false;   // arranca cerrado: es un panel especializado

    // Último GameObject cuyas posiciones se volcaron al canvas. Al cambiar la
    // selección hay que re-volcarlas.
    GameObject* m_boundTo = nullptr;

    // Índice de la transición cuyo popup de condiciones está abierto, -1 si
    // ninguno. Diferido al final del frame: abrir un popup en mitad del canvas
    // rompe el layout del node editor.
    int m_conditionsFor = -1;

    // Índice del estado sobre el que se abrió el menú contextual de nodo,
    // -1 si ninguno. Se usa un miembro plano en vez de ImGui::GetStateStorage()
    // (el plan B que contemplaba el spec): con un solo popup de nodo activo a
    // la vez no hace falta la indirección de la state storage de ImGui, y un
    // miembro es más fácil de razonar y de testear a ojo.
    int m_nodeCtxTarget = -1;

    char m_newParamName[64] = {};
    int  m_newParamType     = 0;   // índice en ParamType: 0 bool, 1 trigger, 2 int, 3 float

    // Instancia propia y no compartida con los diálogos de PropertiesPanel:
    // IGFD guarda estado por instancia, y compartirla haría que redimensionar
    // un popup tocara el otro.
    std::unique_ptr<IGFD::FileDialog> m_animSrcDialog;
    bool m_animSrcDlgOpen = false;
    // Id del GameObject objetivo, capturado al pulsar "Add Animation FBX..."
    // (OpenDialog), NO leído de ctx.selected al drenar: el diálogo no es
    // modal, así que el usuario puede cambiar de selección mientras elige el
    // fichero, y el FBX tiene que ir a quien estaba seleccionado al abrir el
    // diálogo. Se resuelve vía Scene::findById en vez de guardar un
    // GameObject* crudo por el mismo motivo que los comandos de Undo: el
    // objeto puede haberse borrado (o reconstruido) mientras el diálogo
    // estaba abierto.
    uint64_t m_animSrcDlgTarget = 0;
    std::string m_animSrcError;      // último error, en rojo bajo la lista
    // Clip cuyo nombre se está editando, "" si ninguno.
    std::string m_renamingClip;
    char m_renameBuf[64] = {};
};

} // namespace DonTopo
