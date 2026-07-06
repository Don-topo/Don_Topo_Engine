#pragma once
#include <glm/glm.hpp>
#include <memory>

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#endif

namespace DonTopo { class BoxCollider; }

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

    void stepSimulation(float dt);

#ifdef DT_PHYSX_ENABLED
    bool raycast(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance, physx::PxRaycastBuffer& hit);
#endif

private:
#ifdef DT_PHYSX_ENABLED
    void* m_foundation = nullptr; // physx::PxFoundation*
    void* m_physics    = nullptr; // physx::PxPhysics*
    void* m_scene      = nullptr; // physx::PxScene*
    void* m_dispatcher = nullptr; // physx::PxDefaultCpuDispatcher*
    void* m_material   = nullptr; // physx::PxMaterial*
#endif
};

} // namespace DonTopo
