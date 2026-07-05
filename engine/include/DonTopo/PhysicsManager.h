#pragma once

namespace DonTopo {

class PhysicsManager {
public:
    PhysicsManager() = default;
    ~PhysicsManager();
    PhysicsManager(const PhysicsManager&)            = delete;
    PhysicsManager& operator=(const PhysicsManager&) = delete;

    void init();
    void shutdown();

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
