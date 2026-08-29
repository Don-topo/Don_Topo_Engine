#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Editor/LogPanel.h"
#include "DonTopo/Editor/ScenePanel.h"
#include "DonTopo/Editor/ViewportPanel.h"
#include "DonTopo/Editor/PropertiesPanel.h"
#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/AnimatorPanel.h"
#include "DonTopo/Editor/SpriteEditorPanel.h"
#include "DonTopo/Editor/InputActionsPanel.h"
#include "DonTopo/Editor/PerformancePanel.h"
#include "DonTopo/Editor/LoadingModal.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/RenderBackend.h"
#include "DonTopo/Renderer/UiLayer.h"

namespace IGFD { class FileDialog; }

namespace DonTopo {

class GameObject;
class PhysicsManager;
class AudioManager;
class Renderer;
class EditorRenderer;
class Camera;
class Scene;
class ScriptManager;
class ScriptEditorPanel;

// El editor es el dueño del Renderer, y no al revés: así el motor no tiene
// que conocer al editor. La relación se invierte también en el dibujo — el
// Renderer llama de vuelta por la interfaz UiLayer, que esta clase implementa.
class EditorUI : public UiLayer {
public:
    EditorUI();
    ~EditorUI() override;
    EditorUI(const EditorUI&)            = delete;
    EditorUI& operator=(const EditorUI&) = delete;

    // El backend que posee este editor. Quien lo construye sabe cuál es
    // —Vulkan o DirectX 12— y le pasa la propiedad aquí; el editor solo lo usa
    // por la interfaz, así que a partir de este punto le da igual.
    //
    // Hay que ponerlo ANTES de draw(): sin backend no hay paneles que dibujar.
    void setRenderer(std::unique_ptr<EditorRenderer> renderer);

    // El que se le puso. Referencia y no puntero: llamar aquí sin haberlo
    // puesto es un error de montaje, no una situación que manejar.
    EditorRenderer& renderer();

    // viewportTexture es opaco: el backend que lo creó sabe qué es (un
    // VkDescriptorSet o un descriptor GPU de D3D12) y aquí solo viaja hasta
    // ImGui::Image, que lo trata igual en los dos casos.
    void draw(uint64_t viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView);

    bool isViewportHovered() const { return m_viewportPanel.isHovered(); }

    // Para alimentar el input de la UI de juego desde el bucle: esquina de la
    // imagen del viewport y si el ratón está justo encima de ella.
    glm::vec2 viewportImagePos()     const { return m_viewportPanel.imagePos(); }
    bool      isViewportImageHovered() const { return m_viewportPanel.imageHovered(); }

    // ── UiLayer ──────────────────────────────────────────────────────────────
    // El backend de ImGui (contexto, pool de descriptores y los dos _Impl_)
    // vive aquí porque es un detalle del editor, no del motor.
    void     initUi(const InitInfo& info) override;
    void     shutdownUi() override;
    uint64_t registerUiTexture(uint64_t sampler, uint64_t view) override;
    void     unregisterUiTexture(uint64_t handle) override;
    void     buildUiFrame(uint64_t viewportTexture, GameObject* sceneRoot,
                          const glm::mat4& cameraView) override;
    void     recordUi(void* commandList) override;

    // true mientras Play Mode está activo (física + audio corriendo).
    bool isPlaying() const override { return m_isPlaying; }

    // Notificado justo antes de desenganchar node de su padre (node sigue
    // siendo válido y su subárbol completo también), para que el dueño
    // pueda liberar recursos externos (meshes/texturas en GPU) asociados.
    void setOnDelete(std::function<void(GameObject*)> cb) { m_onDelete = std::move(cb); }
    // Llamado con el eje mundo (1,0,0 / 0,1,0 / 0,0,1) al clicar la bola del axis gizmo.
    void setOnAxisSelected(std::function<void(const glm::vec3&)> cb) { m_onAxisSelected = std::move(cb); }
    // Puntero no-propietario: PhysicsManager vive en main.cpp, fuera del
    // ciclo de vida del EditorUI. Necesario para crear el actor PhysX al
    // pulsar "Add > Box Collider" desde el panel Properties.
    void setPhysicsManager(PhysicsManager* physics) { m_physics = physics; }
    // Puntero no-propietario: AudioManager vive fuera del EditorUI (ver
    // wiring en sandbox/main.cpp), mismo patrón que m_physics. Necesario
    // para cargar/reproducir clips desde la sección Audio Clip del panel
    // Properties.
    void setAudioManager(AudioManager* audio) { m_audio = audio; }
    // Puntero no-propietario: Scene vive en main.cpp, fuera del ciclo de
    // vida del EditorUI (mismo patrón que m_physics/m_audio). Necesario
    // para delegar el borrado diferido (ScenePanel::m_pendingDelete) en
    // Scene::removeGameObject en vez de mutar children a mano.
    void setScene(Scene* scene) { m_scene = scene; }
    // Centra la cámara en m_selected (no-op si no hay selección). Usado por
    // el atajo de teclado "F" en main.cpp.
    void focusSelected(Camera& camera);
    // Puntero no-propietario, mismo patrón que m_physics. Necesario para
    // disparar el ciclo de vida al pulsar Play/Stop y para la sección
    // Scripts del panel Properties (Task 10).
    void setScriptManager(ScriptManager* sm) { m_scriptManager = sm; }
    // Punto de entrada externo al Log Console (usado por ScriptManager vía
    // el wiring de main.cpp: mensajes de compilación/errores de scripts).
    void pushExternalLog(const std::string& message) { m_logPanel.push(message); }

    // Suelta m_selected si cae dentro del subárbol de node (incluido node).
    // Lo llama el destroy de Play (ScriptManager, vía Renderer) JUSTO antes de
    // liberar el GameObject: de lo contrario el editor conservaría un puntero
    // colgante y crashearía al dibujar Properties/gizmo el frame siguiente.
    // Es el mismo saneo que hace ScenePanel al borrar desde la jerarquía.
    void onGameObjectDestroyed(GameObject* node);

    // Aplica a la escena los resultados que devuelve AsyncAssetLoader::pumpCompleted
    // y cierra el batch con UN solo flushPendingUploads. Lo llama el pump por
    // frame de main.cpp.
    void onAssetsLoaded(std::vector<LoadedMesh> results, Scene& scene, EditorRenderer& renderer);

    // Lo rellena main() antes del bucle; sin él, los drops no encolan nada y
    // Load Scene se queda en la ruta síncrona.
    void setAssetLoader(AsyncAssetLoader* loader) { m_assetLoader = loader; }

    // Selector de proyecto: primer estado del bucle de ImGui. Mientras haya un
    // selector puesto, draw() le cede el frame ENTERO y no dibuja ni menú, ni
    // toolbar, ni dockspace, ni panel alguno — el editor no aparece hasta que
    // el callback devuelve true (proyecto elegido), y entonces se suelta y el
    // frame siguiente ya es el editor de siempre. La UI del selector vive en
    // main(): aquí solo se cede el turno.
    void setProjectSelector(std::function<bool()> fn) { m_projectSelector = std::move(fn); }
    bool isProjectSelectorActive() const { return static_cast<bool>(m_projectSelector); }

    // Proyecto elegido, no-propietario (vive en main()). Viaja a los paneles por
    // EditorContext::project: es el que decide qué rutas se pueden leer/escribir.
    void setProject(const ProjectContext* project) { m_project = project; }

    // Backend con el que ARRANCÓ este proceso, que main() ya resolvió antes de
    // crear el Renderer. El combo del menú View lo compara con el elegido para
    // saber si hace falta reiniciar; el editor nunca lo cambia en caliente.
    void setActiveRenderBackend(RenderBackend backend) { m_activeBackend = backend; }

    // Abre la escena de arranque del proyecto (ProjectContext::kStartupScene), la
    // que create() deja hecha: vacía, así que al entrar solo se ve el skybox. Va
    // por la MISMA ruta que el Load Scene del menú File, así que reemplaza lo que
    // hubiera cargado (la escena de demo del arranque) y limpia el undo. Si el
    // proyecto no tiene esa escena —uno creado antes de que existiera— no toca
    // nada y deja una línea en el Log.
    bool openProjectScene();

private:
    static constexpr float kToolbarHeight = 30.0f;
    // Ctrl+Z/Ctrl+Y — no-op si !m_scene, si m_isPlaying, o si algún widget de
    // texto tiene el foco (WantTextInput, evita chocar con el undo nativo de
    // un ImGuiInputTextMultiline como el del Script Editor).
    void handleUndoRedoShortcut();
    void drawMenuBar();
    void drawToolbar();
    void drawDockSpace();
    void drawSceneDialog();
    // Guarda del sandbox de rutas: true si `path` se puede leer/escribir con el
    // proyecto abierto. Sin proyecto (tests headless, arranque previo al
    // selector) deja pasar todo, que es como se comportaba el editor antes. Si
    // lo rechaza, deja la línea en el Log y el caller devuelve sin tocar disco.
    bool projectAllows(const std::filesystem::path& path, const char* what);
    // --- Ajustes del menú View persistidos por proyecto -------------------
    // Foto del estado actual: los efectos se leen del Renderer y la visibilidad
    // de los 9 paneles de sus GetOpenPtr(). El Renderer es la única fuente de
    // verdad, así que no hay copia que mantener sincronizada control a control.
    ProjectContext::ViewSettings currentSettings();
    // Vuelca al Renderer y a los paneles los ajustes del proyecto recién
    // abierto. Se llama cada frame desde draw() y sólo hace algo cuando
    // m_project cambia. Sin proyecto (tests headless) no toca nada.
    void applyProjectSettings();
    // Escribe currentSettings() en el project.json. La llaman los controles del
    // menú View al TERMINAR el cambio (click de checkbox/combo, o el
    // IsItemDeactivatedAfterEdit de un slider), nunca por frame de arrastre.
    void saveProjectSettings();
    // Limpia GPU de la escena actual, reemplaza su contenido con j (vía
    // Scene::fromJson) y re-registra GPU (estático + skinned) de lo que
    // quede — tanto si fromJson tuvo éxito (árbol nuevo) como si falló
    // (árbol viejo intacto, con índices reseteados antes de liberar GPU).
    // Limpia m_selected si fromJson tuvo éxito. Devuelve lo que devuelva
    // fromJson. Usado por drawSceneDialog (Load Scene) y por el handler de
    // Stop en drawToolbar. false sin efecto si falta algún puntero
    // (m_scene/m_renderer/m_physics/m_audio).
    // async == true (Load Scene): los meshes se encolan en m_assetLoader y se
    // abre el modal de progreso. async == false (restore de Play->Stop y
    // cualquier ruta sin loader): carga síncrona, determinista, sin modal.
    bool reloadSceneFromJson(const nlohmann::json& j, bool async);
    // Carga la escena del fichero path: valida la estructura del JSON, recarga
    // la escena (async) y reporta el resultado por m_sceneIOError/Log. Ruta
    // única de carga por fichero — la usan el Load Scene del menú File y el
    // clic en un .json del Content Browser (ctx.requestLoadScene).
    bool loadSceneFile(const std::string& path);
    // Export Game — drena el diálogo de carpeta destino y pinta los dos
    // popups modales (nombre y confirmación de sobrescritura). Se llama cada
    // frame desde draw(), igual que drawSceneDialog.
    void drawExportDialog();
    // Ejecuta el export completo con m_exportDestDir y m_exportNameBuffer ya
    // fijados. Vuelca al Log Console tanto los errores como el resumen.
    void runExport();

    // Log Console — extraído a LogPanel (Task 2).
    LogPanel m_logPanel;
    // Scene — árbol jerárquico de GameObjects, extraído a ScenePanel (Task 3).
    ScenePanel m_scenePanel;
    // Viewport — render 3D embebido + gizmo de selección, extraído a
    // ViewportPanel (Task 4).
    ViewportPanel m_viewportPanel;
    // Properties — transform/colliders/mesh/audio/scripts/add-component,
    // extraído a PropertiesPanel (Task 5).
    PropertiesPanel m_propertiesPanel;
    // Content Browser — explorador de assets con rename/delete, extraído a
    // ContentBrowserPanel (Task 6).
    ContentBrowserPanel m_contentBrowserPanel;

    // Scene save/load — instancia propia de diálogo, mismo motivo que los
    // diálogos de PropertiesPanel (Instance() singleton no soporta
    // diálogos concurrentes). Se reusa la misma instancia para Save y Load
    // porque nunca están abiertos a la vez (ambos disparados desde botones
    // secuenciales del toolbar).
    std::unique_ptr<IGFD::FileDialog> m_sceneFileDialog;
    bool        m_sceneDlgOpen = false;
    bool        m_sceneDlgIsSave = false;
    // Último error de guardado/carga de escena (vacío si ninguno pendiente).
    std::string m_sceneIOError;
    // Fichero de la escena en memoria (último guardado o cargado con éxito).
    // Vacío = escena que nunca se guardó: ctx.requestSaveScene tiene que pasar
    // por el diálogo Save Scene en vez de escribir directamente.
    std::string m_currentScenePath;
    // Escena que hay que cargar en cuanto el diálogo Save Scene en vuelo
    // confirme y guarde (el "Guardar" del modal del Content Browser sobre una
    // escena sin fichero). Vacío = el diálogo no encadena ninguna carga.
    std::string m_pendingSceneLoadAfterSave;

    // Export Game — instancia propia de diálogo por el mismo motivo que
    // m_sceneFileDialog: IGFD::FileDialog::Instance() no soporta diálogos
    // concurrentes.
    std::unique_ptr<IGFD::FileDialog> m_exportDialog;
    bool        m_exportDlgOpen          = false;
    std::string m_exportDestDir;
    char        m_exportNameBuffer[64]   = "Game";
    bool        m_openExportNamePopup    = false;
    bool        m_openExportConfirmPopup = false;

    // Play Mode — snapshot en RAM del árbol justo antes de pulsar Play,
    // restaurado íntegro al pulsar Stop (tipo Unity Play-In-Editor). No se
    // bloquea la edición mientras está activo — cualquier cambio se
    // descarta igual al restaurar.
    // Slider del factor de SSAA. Cambiarlo recrea TODOS los targets internos,
    // asi que se aplica al SOLTAR y hay que conservar el valor en vuelo. Es
    // miembro y no un static de la funcion: aquel sobrevivia al cambio de
    // proyecto, y su refresco dependia de IsAnyItemActive(), que es global.
    // Carpeta del cielo del proyecto. Se edita en el menu View y se aplica al
    // pulsar, no a cada tecla: recargarlo suelta el cubemap y reconvoluciona
    // el IBL global.
    char           m_skyboxFolder[260] = "assets/skybox";

    float          m_ssaaPendingFactor = 2.0f;
    bool           m_ssaaSliderActive  = false;

    bool           m_isPlaying = false;
    nlohmann::json m_playSnapshot;

    // Undo/Redo — historial de las últimas 50 acciones de edición (Transform,
    // propiedades de collider, Create/Delete/Reparent GameObject). Se resetea
    // en Load Scene y al entrar/salir de Play Mode (ver reloadSceneFromJson y
    // el handler de Play en drawToolbar).
    UndoManager m_undoHistory;

    // Scene selection
    GameObject* m_selected = nullptr;
    std::function<void(GameObject*)> m_onDelete;
    std::function<void(const glm::vec3&)> m_onAxisSelected;

    // Con qué API se arrancó la interfaz. Decide qué backend de ImGui se usa
    // en cada punto: iniciar, empezar el frame, grabar y cerrar.
    GraphicsApi m_api = GraphicsApi::Vulkan;

#ifdef DT_D3D12_ENABLED
    // Reparto del rango de descriptores que el backend cede a la interfaz.
    // ImGui pide y suelta por su cuenta, así que hay que llevar la cuenta.
    struct D3D12SrvPool {
        uint64_t              cpuStart = 0;
        uint64_t              gpuStart = 0;
        uint32_t              stride   = 0;
        uint32_t              capacity = 0;
        uint32_t              next     = 0;
        std::vector<unsigned> released;
    };
    D3D12SrvPool m_d3dSrvPool;

    void initUiD3D12(const InitInfo& info);
#endif

    PhysicsManager* m_physics = nullptr;
    // Propiedad: el editor mantiene vivo el backend que le dieron. unique_ptr a
    // tipo incompleto — se destruye en el .cpp.
    std::unique_ptr<EditorRenderer> m_renderer;
    AudioManager*   m_audio = nullptr;
    // Buffer del pump de fallos de carga de audio (EditorUI::draw). Miembro y
    // no local para no reasignar un vector en cada frame; se limpia antes de
    // cada uso.
    std::vector<std::string> m_audioFailures;
    Scene*          m_scene = nullptr;
    ScriptManager*  m_scriptManager = nullptr;

    // Panel de edición de código .lua (Task: Script Editor Panel). unique_ptr
    // + forward declaration para no arrastrar <TextEditor.h>/<imgui.h> a todo
    // el que incluya EditorUI.h — mismo patrón que m_sceneFileDialog.
    std::unique_ptr<ScriptEditorPanel> m_scriptEditor;
    AnimatorPanel m_animatorPanel;
    // Editor de sub-rects de un atlas. Se abre desde el campo Atlas de un
    // Button, y la petición llega DIFERIDA: quien la pide (PropertiesPanel) lo
    // hace mientras se está construyendo el EditorContext, que es justo lo que
    // el panel necesita para abrir la imagen.
    SpriteEditorPanel m_spriteEditor;
    std::string       m_pendingSpriteAtlas;
    PerformancePanel m_performancePanel;
    // Mapa de acciones de input. Su constructor carga el JSON de persistencia,
    // así que el panel ya viene con lo de la sesión anterior al abrirlo.
    InputActionsPanel m_inputActionsPanel;

    // Overlay de progreso de Load Scene. Solo lo activa reloadSceneFromJson en
    // la ruta asíncrona; el pump de main.cpp lo va actualizando cada frame.
    LoadingModal      m_loadingModal;
    // Loader asíncrono no-propietario (vive en main.cpp). Lo rellena
    // setAssetLoader antes del bucle. nullptr => Load Scene cae a síncrono.
    AsyncAssetLoader* m_assetLoader = nullptr;

    // Ver setProjectSelector/setProject. Vacío/nullptr en los tests headless: sin
    // selector el editor dibuja como siempre, y sin proyecto las guardas de ruta
    // se comportan igual que antes de que existiera el concepto.
    std::function<bool()>  m_projectSelector;
    const ProjectContext*  m_project = nullptr;
    // Último proyecto cuyos ajustes del menú View ya se aplicaron. Distinto de
    // m_project => toca aplicar (una vez por apertura de proyecto).
    const ProjectContext*  m_appliedProject = nullptr;

    // Backend de render. Dos valores a propósito, porque no tienen por qué
    // coincidir: m_activeBackend es con el que se creó el device de ESTE
    // proceso, y m_selectedBackend el que el proyecto pide para el PRÓXIMO
    // arranque. Mientras difieran, el menú View enseña el aviso de reinicio.
    RenderBackend m_activeBackend   = RenderBackend::Vulkan;
    RenderBackend m_selectedBackend = RenderBackend::Vulkan;

    // Backend del diálogo de Export Game, como índice del combo. Es INDEPENDIENTE
    // de los dos de arriba: el juego se exporta para la máquina del jugador, que
    // no tiene por qué usar el mismo backend que este editor. Ajuste de sesión,
    // no se persiste en el project.json — el destino es el game.cfg del paquete.
    int m_exportBackend = 0;

    // Backend de ImGui. El device se guarda en initUi porque shutdownUi lo
    // necesita para liberar el pool y ahí ya no hay InitInfo.
    VkDescriptorPool m_uiDescPool = VK_NULL_HANDLE;
    VkDevice         m_uiDevice   = VK_NULL_HANDLE;
};

} // namespace DonTopo
