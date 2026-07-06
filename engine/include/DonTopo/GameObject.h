#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include "DonTopo/Mesh.h"
#include "DonTopo/SkinnedMesh.h"
#include "DonTopo/BoxCollider.h"
#include "DonTopo/SphereCollider.h"
#include "DonTopo/CapsuleCollider.h"
#include "DonTopo/PlaneCollider.h"

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

            void setBoxCollider(std::shared_ptr<BoxCollider> bc) { m_boxCollider = std::move(bc); }
            const std::shared_ptr<BoxCollider>& getBoxCollider() const { return m_boxCollider; }
            bool hasBoxCollider() const { return m_boxCollider != nullptr; }

            void setSphereCollider(std::shared_ptr<SphereCollider> sc) { m_sphereCollider = std::move(sc); }
            const std::shared_ptr<SphereCollider>& getSphereCollider() const { return m_sphereCollider; }
            bool hasSphereCollider() const { return m_sphereCollider != nullptr; }

            void setCapsuleCollider(std::shared_ptr<CapsuleCollider> cc) { m_capsuleCollider = std::move(cc); }
            const std::shared_ptr<CapsuleCollider>& getCapsuleCollider() const { return m_capsuleCollider; }
            bool hasCapsuleCollider() const { return m_capsuleCollider != nullptr; }

            void setPlaneCollider(std::shared_ptr<PlaneCollider> pc) { m_planeCollider = std::move(pc); }
            const std::shared_ptr<PlaneCollider>& getPlaneCollider() const { return m_planeCollider; }
            bool hasPlaneCollider() const { return m_planeCollider != nullptr; }

            // true si tiene cualquiera de los 4 tipos de collider — los 4 son
            // mutuamente excluyentes (impuesto por EditorUI, no por esta clase),
            // usado como guard único en el popup "Add".
            bool hasAnyCollider() const
            {
                return m_boxCollider || m_sphereCollider || m_capsuleCollider || m_planeCollider;
            }

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
            std::shared_ptr<BoxCollider> m_boxCollider;
            std::shared_ptr<SphereCollider> m_sphereCollider;
            std::shared_ptr<CapsuleCollider> m_capsuleCollider;
            std::shared_ptr<PlaneCollider> m_planeCollider;
    };
}
