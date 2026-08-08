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
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/ReflectionProbeComponent.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"

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

            // JobId de la carga de mesh en vuelo, 0 = ninguna. Es un uint64_t
            // opaco a propósito: Core no conoce AsyncAssetLoader, y el
            // destructor NO cancela nada — el pump ya descarta los resultados
            // cuyo targetId no existe.
            uint64_t pendingMeshJob = 0;

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

            // Rigidbody: dinámica del cuerpo (masa/gravedad/fuerzas/constraints).
            // Requiere un collider que aporte la forma; uno por objeto.
            void setRigidbody(std::shared_ptr<Rigidbody> rb) { m_rigidbody = std::move(rb); }
            const std::shared_ptr<Rigidbody>& getRigidbody() const { return m_rigidbody; }
            bool hasRigidbody() const { return m_rigidbody != nullptr; }

            void setAudioClip(std::shared_ptr<AudioClipComponent> clip) { m_audioClip = std::move(clip); }
            const std::shared_ptr<AudioClipComponent>& getAudioClip() const { return m_audioClip; }
            bool hasAudioClip() const { return m_audioClip != nullptr; }

            // Audio Listener: desde dónde se oye el audio 3D. Como mucho uno por
            // escena, igual que la cámara — el invariante lo impone
            // Scene::findAudioListener, no esta clase. La posición y los ejes
            // salen del worldTransform, no del componente.
            void setAudioListener(std::shared_ptr<AudioListenerComponent> l) { m_audioListener = std::move(l); }
            const std::shared_ptr<AudioListenerComponent>& getAudioListener() const { return m_audioListener; }
            bool hasAudioListener() const { return m_audioListener != nullptr; }

            // Cámara de juego: al dar a Play el Renderer renderiza desde este
            // GameObject (su worldTransform da posición y orientación). Como
            // mucho una por escena — el invariante lo impone Scene::findCamera,
            // no esta clase, igual que la exclusividad de colliders la impone
            // el editor.
            void setCameraComponent(std::shared_ptr<CameraComponent> camera) { m_cameraComponent = std::move(camera); }
            const std::shared_ptr<CameraComponent>& getCameraComponent() const { return m_cameraComponent; }
            bool hasCameraComponent() const { return m_cameraComponent != nullptr; }

            // Animator: máquina de estados que decide qué clip del SkinnedMesh
            // se reproduce. A diferencia de la cámara, no hay invariante de
            // unicidad por escena: cada GameObject skinned lleva el suyo.
            void setAnimator(std::shared_ptr<AnimatorComponent> a) { m_animator = std::move(a); }
            const std::shared_ptr<AnimatorComponent>& getAnimator() const { return m_animator; }
            bool hasAnimator() const { return m_animator != nullptr; }

            // Reflection Probe: sonda de entorno. Sin invariante de unicidad
            // por escena (al contrario que la cámara): caben las que quepan en
            // memoria, y el Renderer resuelve qué sonda ilumina cada objeto por
            // radio de influencia.
            void setReflectionProbe(std::shared_ptr<ReflectionProbeComponent> p) { m_reflectionProbe = std::move(p); }
            const std::shared_ptr<ReflectionProbeComponent>& getReflectionProbe() const { return m_reflectionProbe; }
            bool hasReflectionProbe() const { return m_reflectionProbe != nullptr; }

            // Luz. Tampoco tiene invariante de unicidad: caben varias del mismo
            // tipo por escena, y es Scene quien se queda con las primeras
            // MAX_LIGHTS al recolectarlas para el Renderer. La posición y la
            // dirección salen del worldTransform, no del componente.
            void setLight(std::shared_ptr<LightComponent> l) { m_light = std::move(l); }
            const std::shared_ptr<LightComponent>& getLight() const { return m_light; }
            bool hasLight() const { return m_light != nullptr; }

            // Canvas: la raíz de la UI 2D. Sin invariante de unicidad por
            // escena (como la luz, no como la cámara), pero el canvas VIVO es
            // uno solo —el del Renderer—, así que quien dibuja aplica el
            // primero en pre-orden (Scene::findCanvas). La posición no sale del
            // worldTransform: la UI es espacio de pantalla.
            void setCanvas(std::shared_ptr<CanvasComponent> c) { m_canvas = std::move(c); }
            const std::shared_ptr<CanvasComponent>& getCanvas() const { return m_canvas; }
            bool hasCanvas() const { return m_canvas != nullptr; }

            // Button: un widget de la UI 2D. Solo tiene sentido colgando de un
            // Canvas (el gate del editor es PropertiesPanel::uiComponentsAvailable),
            // y como el Canvas, es SOLO DATOS: el nodo vivo lo monta quien dibuja
            // con syncUiButtons(). Uno por GameObject, igual que la luz.
            void setButton(std::shared_ptr<ButtonComponent> b) { m_button = std::move(b); }
            const std::shared_ptr<ButtonComponent>& getButton() const { return m_button; }
            bool hasButton() const { return m_button != nullptr; }

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

            // Visibilidad del componente Mesh. false = la malla no se manda a la
            // GPU: ni pass de escena, ni sombras, ni AO. Física, colisiones y
            // selección en el viewport siguen igual. Llega al Renderer por frame
            // vía setObjectMeshVisible/setSkinnedMeshVisible, igual que el SSR.
            bool meshVisible = true;

            // Screen Space Reflections por objeto. No es un componente: son dos
            // campos del propio GameObject, igual que el transform, porque lo que
            // configuran es cómo se dibuja SU malla. ssrIntensity es la
            // reflectividad a incidencia normal (F0 en ssr.comp): 1 = espejo,
            // valores bajos reflejan sobre todo de canto. El Renderer los recibe
            // por frame vía setObjectSsr/setSkinnedSsr, y con ssrEnabled a false
            // el objeto no aporta máscara ninguna.
            bool  ssrEnabled   = false;
            float ssrIntensity = 0.5f;

        private:
            std::shared_ptr<Mesh> m_mesh;
            std::shared_ptr<BoxCollider> m_boxCollider;
            std::shared_ptr<SphereCollider> m_sphereCollider;
            std::shared_ptr<CapsuleCollider> m_capsuleCollider;
            std::shared_ptr<PlaneCollider> m_planeCollider;
            std::shared_ptr<Rigidbody> m_rigidbody;
            std::shared_ptr<AudioClipComponent> m_audioClip;
            std::shared_ptr<AudioListenerComponent> m_audioListener;
            std::shared_ptr<CameraComponent> m_cameraComponent;
            std::shared_ptr<AnimatorComponent> m_animator;
            std::shared_ptr<ReflectionProbeComponent> m_reflectionProbe;
            std::shared_ptr<LightComponent> m_light;
            std::shared_ptr<CanvasComponent> m_canvas;
            std::shared_ptr<ButtonComponent> m_button;
            std::vector<std::unique_ptr<ScriptComponent>> m_scripts;
    };
}
