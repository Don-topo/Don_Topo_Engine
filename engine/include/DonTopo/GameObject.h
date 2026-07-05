#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include "DonTopo/Mesh.h"
#include "DonTopo/SkinnedMesh.h"
#include "DonTopo/BoxCollider.h"

namespace DonTopo
{
    class GameObject
    {
        public:
            explicit GameObject(std::string name = "");

            GameObject* addChild(std::string childName);

            void setMesh(std::shared_ptr<Mesh> mesh) { m_mesh = std::move(mesh); }
            const std::shared_ptr<Mesh>& getMesh() const { return m_mesh; }
            bool hasMesh()   const { return m_mesh != nullptr; }
            bool isSkinned() const { return m_mesh && dynamic_cast<SkinnedMesh*>(m_mesh.get()) != nullptr; }
            SkinnedMesh* getSkinnedMesh() const { return m_mesh ? dynamic_cast<SkinnedMesh*>(m_mesh.get()) : nullptr; }

            void setCollider(std::shared_ptr<BoxCollider> collider) { m_collider = std::move(collider); }
            const std::shared_ptr<BoxCollider>& getCollider() const { return m_collider; }
            bool hasCollider() const { return m_collider != nullptr; }

            void updateWorldTransforms(const glm::mat4& parentWorld = glm::mat4(1.0f));

            template <typename Fn>
            void traverse(Fn fn)
            {
                fn(this);
                for (auto& c : children) c->traverse(fn);
            }

            std::string name;
            glm::mat4   localTransform {1.0f};
            glm::mat4   worldTransform {1.0f};
            GameObject* parent = nullptr;
            std::vector<std::unique_ptr<GameObject>> children;

            // El Renderer mantiene dos colecciones/pipelines separados (estático vs skinned),
            // por eso hacen falta dos índices en vez de un único meshIndex plano.
            int staticRenderIndex  = -1;
            int skinnedRenderIndex = -1;

        private:
            std::shared_ptr<Mesh> m_mesh;
            std::shared_ptr<BoxCollider> m_collider;
    };
}
