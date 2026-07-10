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

        private:
            std::string m_name;
            GameObject  m_root;
    };
}
