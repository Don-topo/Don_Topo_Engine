#pragma once
#include <string>
#include <nlohmann/json_fwd.hpp>
#include "DonTopo/GameObject.h"

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
    };
}
