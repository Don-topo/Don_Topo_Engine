#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <vector>

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#endif

namespace DonTopo { class Collider; class BoxCollider; class SphereCollider; class CapsuleCollider; class PlaneCollider; }

namespace DonTopo {

class PhysicsManager {
public:
    PhysicsManager() = default;
    ~PhysicsManager();
    PhysicsManager(const PhysicsManager&)            = delete;
    PhysicsManager& operator=(const PhysicsManager&) = delete;

    void init();
    void shutdown();

    std::shared_ptr<BoxCollider> createBoxColliderComponent(const glm::vec3& halfExtents,
                                                              const glm::vec3& center,
                                                              const glm::mat4& worldTransform,
                                                              bool useGravity,
                                                              float density = 1.0f);

    std::shared_ptr<SphereCollider> createSphereColliderComponent(float radius,
                                                                    const glm::vec3& center,
                                                                    const glm::mat4& worldTransform,
                                                                    bool useGravity,
                                                                    float density = 1.0f);

    std::shared_ptr<CapsuleCollider> createCapsuleColliderComponent(float radius,
                                                                      float halfHeight,
                                                                      const glm::vec3& center,
                                                                      const glm::mat4& worldTransform,
                                                                      bool useGravity,
                                                                      float density = 1.0f);

    std::shared_ptr<PlaneCollider> createPlaneColliderComponent(const glm::vec3& center,
                                                                  const glm::mat4& worldTransform);

    void stepSimulation(float dt);

    // Marca/desmarca un collider como trigger: flip de flags PhysX
    // (Collider::applyTriggerFlag) + alta/baja en el registro que se recorre
    // cada frame para sintetizar onTriggerStay. Entry point público — lo
    // llamará Core al integrar editor/scripting.
    void setTrigger(const std::shared_ptr<Collider>& collider, bool enabled);

    // Llamado por ~Collider: purga el collider de los sets de overlap de todos
    // los triggers vivos, evitando punteros colgantes antes del próximo Stay.
    void onColliderDestroyed(Collider* collider);

#ifdef DT_PHYSX_ENABLED
    bool raycast(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance, physx::PxRaycastBuffer& hit);
#endif

private:
    // Triggers registrados (weak: los GameObjects poseen los colliders vía
    // shared_ptr). Se recorren cada frame para emitir onTriggerStay; los
    // expirados se podan al vuelo.
    std::vector<std::weak_ptr<Collider>> m_triggerColliders;

#ifdef DT_PHYSX_ENABLED
    void* m_foundation      = nullptr; // physx::PxFoundation*
    void* m_physics         = nullptr; // physx::PxPhysics*
    void* m_scene           = nullptr; // physx::PxScene*
    void* m_dispatcher      = nullptr; // physx::PxDefaultCpuDispatcher*
    void* m_material        = nullptr; // physx::PxMaterial*
    void* m_triggerCallback = nullptr; // TriggerDispatcher* (PxSimulationEventCallback)
#endif
};

} // namespace DonTopo
