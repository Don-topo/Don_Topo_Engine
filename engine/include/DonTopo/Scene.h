#pragma once
#include <string>
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

            template <typename Fn>
            void traverse(Fn fn) { m_root.traverse(fn); }

            void update(float dt, PhysicsManager& physics);
            void shutdown(PhysicsManager& physics, AudioManager& audio);

            // Serializa el árbol completo (transforms, mesh, colliders, audio
            // clip) a path en formato JSON. false si la escritura falla.
            bool save(const std::string& path) const;
            // Reemplaza el árbol actual por el contenido de path. Limpia la
            // escena existente (shutdown + children.clear()) SOLO si el
            // fichero es válido — una carga fallida no modifica la escena en
            // memoria. Recrea colliders/audio vía physics/audio (mismas
            // factories que usa EditorUI). No toca Renderer — el caller debe
            // registrar/liberar los meshes en GPU (ver EditorUI::drawSceneDialog).
            bool load(const std::string& path, PhysicsManager& physics, AudioManager& audio);

        private:
            std::string m_name;
            GameObject  m_root;
    };
}
