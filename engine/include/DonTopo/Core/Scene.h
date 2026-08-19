#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "DonTopo/Core/GameObject.h"

namespace DonTopo
{
    class PhysicsManager;
    class AudioManager;
    class AsyncAssetLoader;
    struct Mesh;

    // Cache opcional de mallas ya cargadas en RAM, indexada por sourcePath. La
    // consulta la carga de escena (nodeFromJson) para saltarse el ReadFile de
    // disco de un sourcePath que ya se precargó — el runtime la rellena en
    // paralelo con el JobSystem y muestra progreso en el splash mientras tanto.
    // Los valores pueden ser SkinnedMesh (un FBX con rig): la carga hace un
    // dynamic_cast para reconstruir el tipo correcto. El caller conserva la
    // propiedad; la carga hace copia profunda de la malla que use.
    using PreloadedMeshCache = std::unordered_map<std::string, std::shared_ptr<Mesh>>;

    class Scene
    {
        public:
            explicit Scene(std::string name = "Scene");

            GameObject& getRoot() { return m_root; }
            const GameObject& getRoot() const { return m_root; }

            GameObject* addGameObject(const std::string& name, GameObject* parent = nullptr);
            void removeGameObject(GameObject* node);

            // Busca por GameObject::id en todo el árbol (incluida la raíz).
            // nullptr si ningún nodo tiene ese id. O(n) sobre el árbol — usado
            // por los comandos de Undo/Redo (Command.cpp) pa resolver su
            // objetivo en vivo en cada execute()/undo(), nunca un puntero crudo.
            GameObject* findById(uint64_t id);

            // Única fuente de verdad del invariante "como mucho una cámara por
            // escena": la buscan el gate de "Add" de Properties, el menú
            // contextual del panel Scene, el switch de cámara del Renderer y el
            // aviso al dar a Play — ninguno guarda estado propio. Pre-orden
            // desde la raíz (gana la primera), nullptr si no hay ninguna. O(n)
            // sobre el árbol, igual que findById.
            GameObject* findCamera();
            const GameObject* findCamera() const;

            // Misma idea pa el invariante "como mucho un Audio Listener por
            // escena": lo consultan el gate de "Add" de Properties, el gate de
            // reproducción al entrar en Play y la resolución del listener que se
            // le pasa a AudioManager::update cada frame. Pre-orden desde la raíz
            // (gana el primero), nullptr si no hay ninguno.
            GameObject* findAudioListener();
            const GameObject* findAudioListener() const;

            // Canvas de UI que se aplica al canvas vivo del Renderer. Aquí no
            // hay invariante que imponer (caben varios en la escena), pero el
            // UiCanvas del Renderer es uno solo: gana el primero en pre-orden.
            // Lo consultan el bucle del editor y el del runtime exportado, cada
            // frame, y el gizmo del área útil. nullptr si no hay ninguno.
            GameObject* findCanvas();
            const GameObject* findCanvas() const;

            // Los widgets de UI de la escena, listos para syncUiWidgets: las
            // cuatro listas por tipo y la JERARQUÍA aplanada a (id, id del padre)
            // en pre-orden, con 0 para los que cuelgan de la raíz del canvas.
            //
            // El "padre" es el ancestro más cercano que TENGA algún componente
            // de UI, no el padre inmediato: un GameObject intermedio sin UI no
            // aporta rect contra el que anclarse, así que no puede sostener a
            // nadie y sus hijos suben al primero que sí.
            //
            // Vive aquí y no en cada bucle porque lo necesitan los tres (editor
            // con los dos backends y runtime exportado), y tres copias de este
            // recorrido es como se desincronizan.
            void collectUiWidgets(
                std::vector<std::pair<uint64_t, const ButtonComponent*>>& buttons,
                std::vector<std::pair<uint64_t, const TextComponent*>>& texts,
                std::vector<std::pair<uint64_t, const ProgressBarComponent*>>& bars,
                std::vector<std::pair<uint64_t, const LayoutComponent*>>& layouts,
                std::vector<std::pair<uint64_t, uint64_t>>& parents) const;

            // Avisos de la última operación que tuvo que corregir la escena
            // cargada (campos corruptos, varias cámaras, clips que ya no casan).
            // Core no conoce el Log Console: EditorUI los vuelca tras cargar. Se
            // limpian al principio de cada operación que los pueda rellenar, así
            // que nunca crecen sin control.
            //
            // Los repetidos vienen colapsados a una sola entrada con " (xN)" al
            // final: un mesh corrupto genera un aviso IDÉNTICO por vértice (el
            // contexto es el nombre del objeto, no el índice), y sin colapsar una
            // sola malla rota escribe miles de líneas en el Log y sepulta los
            // demás avisos de esa misma carga.
            //
            // OJO: hoy solo los drena el editor tras cargar escena. El aviso del
            // clone (Instantiate de Lua, en Play) no tiene consumidor de Log —
            // queda registrado pa los tests y pa un futuro consumidor.
            const std::vector<std::string>& lastWarnings() const { return m_warnings; }

            // Serializa solo el subárbol de node (mismo formato de nodo que
            // usa toJson() internamente, incluido su id) — usado por
            // CreateGameObjectCommand/DeleteGameObjectCommand (Command.cpp)
            // pa capturar el snapshot de un GameObject sin serializar la
            // escena entera.
            nlohmann::json subtreeToJson(const GameObject* node) const;

            // Reconstruye un subárbol desde j como hijo de parent (o de la
            // raíz si parent es nullptr, mismo criterio que cloneGameObject),
            // insertado en la posición index de parent->children (si index
            // queda fuera de rango, al final). Los render indices del
            // subtree quedan a -1: el caller debe registrar los meshes en
            // GPU (ver Renderer::registerGameObject). nullptr si la
            // reconstrucción falla (subárbol malformado).
            GameObject* insertFromJson(const nlohmann::json& j, GameObject* parent, size_t index,
                                        PhysicsManager& physics, AudioManager& audio);

            // Deep clone de src (transform, mesh, colliders, audio, scripts
            // con overrides) como hijo nuevo de parent (o del padre de src si
            // parent es nullptr). Los render indices del subtree quedan a -1:
            // el caller debe registrar los meshes en GPU. nullptr si src es
            // la raíz o la reconstrucción falla.
            GameObject* cloneGameObject(GameObject* src, GameObject* parent,
                                        PhysicsManager& physics, AudioManager& audio);

            // Recolecta las luces de la escena en el formato que come el
            // Renderer (setLights/setLightRadii). Pre-orden desde la raíz, y se
            // queda con las primeras MAX_LIGHTS: el resto se descarta EN
            // SILENCIO — es un tope del bloque UBO, no un error de la escena.
            //
            // La posición y la dirección salen del worldTransform de cada
            // GameObject (columna 3 y -Z local), así que hay que llamarlo
            // DESPUÉS de propagar los transforms del frame. Devuelve cuántos
            // GameObject con luz había en total, que es lo que permite al caller
            // distinguir "escena sin luces" de "escena con más de las que caben".
            //
            // Core no conoce el Renderer: los dos setters los llama el caller
            // (una escena sin luces no tiene por qué dejar el viewport a
            // oscuras, y esa decisión es de quien monta el frame).
            size_t collectLights(std::vector<Light>& outLights,
                                 std::vector<float>& outRadii) const;

            template <typename Fn>
            void traverse(Fn fn) { m_root.traverse(fn); }

            void update(float dt, PhysicsManager& physics);
            void shutdown(PhysicsManager& physics, AudioManager& audio);

            // Serializa el árbol completo (transforms, mesh, colliders, audio
            // clip) a un nlohmann::json en memoria.
            nlohmann::json toJson() const;
            // Reemplaza el árbol actual por el contenido de j. Limpia la
            // escena existente (shutdown + move-assignment) SOLO si j es
            // válido — una carga fallida no modifica la escena en memoria.
            // Recrea colliders/audio vía physics/audio (mismas factories que
            // usa EditorUI). No toca Renderer — el caller debe registrar/
            // liberar los meshes en GPU (ver EditorUI::reloadSceneFromJson).
            //
            // loader == nullptr → carga síncrona, comportamiento idéntico al de
            // siempre. Es lo que usan el restore de Play→Stop y los tests.
            //
            // loader != nullptr → los GameObject se crean completos pero sin
            // mesh, y cada sourcePath encola una petición. El caller es
            // responsable de bombear y de mostrar el progreso.
            //
            // preloaded == nullptr → sin cache, cada sourcePath se lee de disco
            // como siempre. preloaded != nullptr → antes de leer el disco se
            // consulta la cache por sourcePath y, si está, se usa una copia
            // profunda de la malla precargada (skinned incluido). Un miss cae al
            // camino de disco normal, así que el resultado es idéntico salvo por
            // no repetir el ReadFile. Va DESPUÉS de loader a propósito: los
            // callers existentes (editor, tests) no lo pasan y quedan byte a
            // byte iguales.
            bool fromJson(const nlohmann::json& j, PhysicsManager& physics, AudioManager& audio,
                          AsyncAssetLoader* loader = nullptr,
                          const PreloadedMeshCache* preloaded = nullptr);

            // Serializa el árbol completo a path en formato JSON (vía
            // toJson()). false si la escritura falla.
            bool save(const std::string& path) const;
            // Lee y parsea path, delega en fromJson(...). false si el
            // fichero no existe o el JSON es inválido. Ver fromJson para el
            // contrato de loader.
            bool load(const std::string& path, PhysicsManager& physics, AudioManager& audio,
                      AsyncAssetLoader* loader = nullptr,
                      const PreloadedMeshCache* preloaded = nullptr);

        private:
            std::string m_name;
            GameObject  m_root;

            // Impone el invariante de una cámara por escena tras reconstruir el
            // árbol: se queda con la primera en pre-orden y le quita el
            // CameraComponent al resto (el GameObject se conserva — solo se cae
            // el componente). Así un .scene editado a mano con dos cámaras se
            // abre igual, con aviso, en vez de fallar la carga o quedar en un
            // estado donde findCamera() decide sobre una escena incoherente.
            void pruneExtraCameras();

            // Lo mismo pal Audio Listener: se queda con el primero en pre-orden
            // y le quita el componente al resto, dejando un aviso por objeto
            // descartado. El GameObject se conserva.
            void pruneExtraAudioListeners();

            // Colapsa los avisos repetidos de m_warnings in situ, conservando el
            // orden de primera aparición y añadiendo " (xN)" a los que salieron
            // más de una vez. Se llama al final de cada operación que rellena
            // m_warnings, nunca durante: los productores empujan sin mirar.
            void collapseWarnings();

            std::vector<std::string> m_warnings;
    };
}
