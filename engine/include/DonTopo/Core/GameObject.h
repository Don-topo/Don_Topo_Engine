#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <memory>
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Audio/AudioClipComponent.h"

namespace DonTopo
{
    class ScriptComponent;

    class GameObject
    {
        public:
            // Único entre todos los GameObject de la sesión (contador atómico
            // en el constructor) — usado por los comandos de Undo/Redo pa
            // resolver el objeto en vivo vía Scene::findById tras un ciclo
            // undo/redo que reconstruye el GameObject (Undo de Delete), donde
            // un GameObject* crudo quedaría colgado.
            uint64_t id;

            explicit GameObject(std::string name = "");
            ~GameObject();
            GameObject(GameObject&&) noexcept;
            GameObject& operator=(GameObject&&) noexcept;

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

            // Devuelve el collider del GameObject como base Collider (hay como
            // mucho uno por la exclusividad mutua), o nullptr si no tiene.
            // Usado por el scripting para registrar el listener de triggers sin
            // ramificar por tipo concreto.
            std::shared_ptr<Collider> anyCollider() const
            {
                if (m_boxCollider)     return m_boxCollider;
                if (m_sphereCollider)  return m_sphereCollider;
                if (m_capsuleCollider) return m_capsuleCollider;
                if (m_planeCollider)   return m_planeCollider;
                return nullptr;
            }

            void setAudioClip(std::shared_ptr<AudioClipComponent> clip) { m_audioClip = std::move(clip); }
            const std::shared_ptr<AudioClipComponent>& getAudioClip() const { return m_audioClip; }
            bool hasAudioClip() const { return m_audioClip != nullptr; }

            // Scripts Lua — a diferencia del resto de slots, vector: se
            // permiten varios scripts por GameObject (incluso repetidos).
            void addScript(std::unique_ptr<ScriptComponent> script);
            void removeScript(ScriptComponent* script);
            std::vector<std::unique_ptr<ScriptComponent>>&       getScripts()       { return m_scripts; }
            const std::vector<std::unique_ptr<ScriptComponent>>& getScripts() const { return m_scripts; }
            bool hasScripts() const { return !m_scripts.empty(); }

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
            std::shared_ptr<AudioClipComponent> m_audioClip;
            std::vector<std::unique_ptr<ScriptComponent>> m_scripts;
    };
}
