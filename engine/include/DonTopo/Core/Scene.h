#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "DonTopo/Core/GameObject.h"

namespace DonTopo
{
    class PhysicsManager;
    class AudioManager;

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
            bool fromJson(const nlohmann::json& j, PhysicsManager& physics, AudioManager& audio);

            // Serializa el árbol completo a path en formato JSON (vía
            // toJson()). false si la escritura falla.
            bool save(const std::string& path) const;
            // Lee y parsea path, delega en fromJson(...). false si el
            // fichero no existe o el JSON es inválido.
            bool load(const std::string& path, PhysicsManager& physics, AudioManager& audio);

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

            // Colapsa los avisos repetidos de m_warnings in situ, conservando el
            // orden de primera aparición y añadiendo " (xN)" a los que salieron
            // más de una vez. Se llama al final de cada operación que rellena
            // m_warnings, nunca durante: los productores empujan sin mirar.
            void collapseWarnings();

            std::vector<std::string> m_warnings;
    };
}
