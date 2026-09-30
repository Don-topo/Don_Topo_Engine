#pragma once
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#endif

namespace DonTopo { class Collider; class BoxCollider; class SphereCollider; class CapsuleCollider; class PlaneCollider; class Rigidbody; }

namespace DonTopo {

class PhysicsManager {
public:
    PhysicsManager() = default;
    ~PhysicsManager();
    PhysicsManager(const PhysicsManager&)            = delete;
    PhysicsManager& operator=(const PhysicsManager&) = delete;

    void init();
    void shutdown();

    // dynamic=false -> PxRigidStatic (collider without Rigidbody); dynamic=true ->
    // PxRigidDynamic (collider with Rigidbody, the config is applied afterwards by
    // attachRigidbody -> Rigidbody::bindActor).
    std::shared_ptr<BoxCollider> createBoxColliderComponent(const glm::vec3& halfExtents,
                                                              const glm::vec3& center,
                                                              const glm::mat4& worldTransform,
                                                              bool dynamic);

    std::shared_ptr<SphereCollider> createSphereColliderComponent(float radius,
                                                                    const glm::vec3& center,
                                                                    const glm::mat4& worldTransform,
                                                                    bool dynamic);

    std::shared_ptr<CapsuleCollider> createCapsuleColliderComponent(float radius,
                                                                      float halfHeight,
                                                                      const glm::vec3& center,
                                                                      const glm::mat4& worldTransform,
                                                                      bool dynamic);

    // Plane: always static/kinematic, never carries a Rigidbody (signature without dynamic).
    std::shared_ptr<PlaneCollider> createPlaneColliderComponent(const glm::vec3& center,
                                                                  const glm::mat4& worldTransform);

    // Ensures the collider actor is a PxRigidDynamic (rebuilds it if it was
    // static), then links the Rigidbody (rb->bindActor). Collider WITHOUT
    // Rigidbody = static; WITH = dynamic.
    void attachRigidbody(const std::shared_ptr<Collider>& collider, const std::shared_ptr<Rigidbody>& rb);
    // Rebuilds the collider actor as a PxRigidStatic (undoes attach).
    void detachRigidbody(const std::shared_ptr<Collider>& collider);

    void stepSimulation(float dt);

    // Fixed step: the real frame dt is accumulated and consumed in chunks of
    // m_fixedDeltaTime, so the simulation does not depend on the framerate.
    // <= 0 is ignored (keeps the previous value): a 0 would hang the loop
    // that subtracts the step from the accumulator.
    void  setFixedDeltaTime(float dt);
    float getFixedDeltaTime() const { return m_fixedDeltaTime; }

    // Ceiling of sub-steps per call; clamped to >= 1 (with 0 physics would
    // never advance). Whatever remains in the accumulator after exhausting them is thrown away.
    void setMaxSubSteps(int steps);
    int  getMaxSubSteps() const { return m_maxSubSteps; }

    // Marks/unmarks a collider as trigger: flip of PhysX flags
    // (Collider::applyTriggerFlag) + add/remove in the registry that is traversed
    // every frame to synthesize onTriggerStay. Public entry point — it will be
    // called by Core when integrating editor/scripting.
    void setTrigger(const std::shared_ptr<Collider>& collider, bool enabled);

    // Called by ~Collider: purges the collider from the overlap sets of all
    // live triggers, avoiding dangling pointers before the next Stay.
    void onColliderDestroyed(Collider* collider);

    // --- Collision layers ----------------------------------------------------
    //
    // 32 fixed layers (index 0-31, the width of PxFilterData::word0). Layer 0 is
    // called "Default"; the rest are born unnamed. The collision matrix is
    // SYMMETRIC and starts entirely true: with the defaults, each shape carries
    // word0 = 1<<layer and word1 = all ones, no pair is suppressed, and the
    // simulation is exactly the one from before layers existed (triggers
    // included).
    // kLayerCount is the CEILING (the width of word0), not how many there are: layers are
    // created and deleted from the editor and the live ones are always the prefix
    // [0, layerCount()). Layer 0 always exists and cannot be deleted.
    static constexpr int kLayerCount = 32;
    static bool isValidLayer(int layer) { return layer >= 0 && layer < kLayerCount; }

    int  layerCount() const { return m_layerCount; }

    // Creates a layer at the end. Returns its index, or -1 if there are already kLayerCount.
    int  addLayer(const std::string& name);

    // Deletes the layer and COMPACTS: colliders that used it move to 0, those of
    // higher layers go down one index, and the matrix loses its row and its column
    // (the last one is freed and goes back to "collides with everything"). Returns false
    // for layer 0 and for an index that does not exist.
    //
    // NOTE: it renumbers. A script that stores raw layer indices points to another
    // layer after a deletion — it is the price of the list having no gaps.
    bool removeLayer(int layer);

    // Enables/disables collision between two layers. Writes BOTH halves of
    // the matrix (a-b and b-a) and recomputes the word1 of all live colliders,
    // so the change also applies in the middle of a game. Invalid index:
    // no-op.
    void setLayerCollision(int a, int b, bool enabled);
    // false for an invalid index (there is no row to query).
    bool getLayerCollision(int a, int b) const;

    // Editable name of the layer; purely informational (UI and project.json), the
    // filtering always goes by index. Invalid index: no-op / empty string.
    void        setLayerName(int layer, const std::string& name);
    std::string getLayerName(int layer) const;

    // Mask of the layer: bit b set to 1 = 'layer' collides with layer b. It is
    // literally the word1 that gets written into the PxFilterData of its shapes.
    // Invalid index: 0.
    uint32_t layerMask(int layer) const;

    // Rewrites the PxFilterData of the collider shape from its current layer.
    // Called by Collider::setLayer (storing the number is not enough: the filter that
    // PhysX looks at lives in the shape) and by the 4 factories on creation. No-op without PhysX,
    // without a shape or with a layer out of range.
    void refreshColliderFilter(Collider* collider);

#ifdef DT_PHYSX_ENABLED
    bool raycast(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance, physx::PxRaycastBuffer& hit);

    // Same query with filters: filterData chooses which actors are traversed
    // (eSTATIC / eDYNAMIC) and filterCall discards shapes one by one (triggers,
    // actor to ignore). It requests ePOSITION|eNORMAL, which is what the Lua
    // binding consumes. Returns false without touching PhysX if there is no PxScene yet
    // (outside Play).
    bool raycast(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance,
                 physx::PxRaycastBuffer& hit, const physx::PxQueryFilterData& filterData,
                 physx::PxQueryFilterCallback* filterCall);

    // Multi-hit: adds PxQueryFlag::eNO_BLOCK to filterData, so every hit is
    // reported as a touch and the query does not stop at the first. hits has to
    // be a PxRaycastBufferN<N> (a plain PxRaycastBuffer has no
    // touch storage and would only collect the blocking one); touches are
    // returned SORTED by ascending distance — PhysX delivers them
    // unordered. If the buffer fills up, PhysX truncates silently: the caller
    // detects it with getNbTouches() == getMaxNbTouches(). Same hit flags and same
    // absent-scene guard as raycast().
    bool raycastAll(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance,
                    physx::PxRaycastBuffer& hits, const physx::PxQueryFilterData& filterData,
                    physx::PxQueryFilterCallback* filterCall);

    // Sweep of a sphere of radius 'radius' from origin along dir:
    // the "thick" ray needed to move a character without it
    // slipping through corners. Single-hit (the first that blocks), same hit
    // flags as raycast() so the binding returns the same table. If the
    // sphere already overlaps something at the origin, PhysX reports distance 0 and the point/
    // normal are not reliable (eMTD is not requested). Same absent-scene guard.
    bool sphereCast(const physx::PxVec3& origin, const physx::PxVec3& dir, float radius,
                    float maxDistance, physx::PxSweepBuffer& hit,
                    const physx::PxQueryFilterData& filterData,
                    physx::PxQueryFilterCallback* filterCall);

    // Which shapes overlap a static sphere at 'center'. Multi-hit: adds
    // PxQueryFlag::eNO_BLOCK just like raycastAll, otherwise the first eBLOCK of the
    // prefilter would close the query. hits has to be a PxOverlapBufferN<N>;
    // when it fills up, PhysX truncates silently (getNbTouches() ==
    // getMaxNbTouches()). Unordered: an overlap has no distance.
    bool overlapSphere(const physx::PxVec3& center, float radius,
                       physx::PxOverlapBuffer& hits,
                       const physx::PxQueryFilterData& filterData,
                       physx::PxQueryFilterCallback* filterCall);

    // Same but with an oriented box (halfExtents + world rotation).
    bool overlapBox(const physx::PxVec3& center, const physx::PxVec3& halfExtents,
                    const physx::PxQuat& rotation, physx::PxOverlapBuffer& hits,
                    const physx::PxQueryFilterData& filterData,
                    physx::PxQueryFilterCallback* filterCall);
#endif

private:
#ifdef DT_PHYSX_ENABLED
    // Changes the actor type of the collider (static<->dynamic) preserving shape,
    // pose and trigger state. Returns the new PxRigidActor* as void*.
    void* rebuildActor(const std::shared_ptr<Collider>& collider, bool dynamic);
#endif

    // Traverses m_triggerColliders emitting onTriggerStay and pruning expired ones.
    // Called once per sub-step (see stepSimulation).
    void dispatchTriggerStay();

    // Registration in m_colliders + first dump of the PxFilterData. Called by the 4
    // factories right after setManager().
    void registerCollider(const std::shared_ptr<Collider>& collider);

    // Recomputes the PxFilterData of ALL live colliders. The matrix is
    // global but the filter is copied into each shape, so a matrix
    // change forces rewriting all of them.
    void refreshAllColliderFilters();

    // All colliders created by this manager (weak: the owners are the
    // GameObjects). Only useful to recompute filters; it is pruned in
    // onColliderDestroyed, which runs on every collider death.
    std::vector<std::weak_ptr<Collider>> m_colliders;

    // Collision matrix, compressed to one mask per layer: bit b of
    // m_layerMasks[a] = "a collides with b". Starts entirely at ones.
    std::array<uint32_t, kLayerCount> m_layerMasks = [] {
        std::array<uint32_t, kLayerCount> m{};
        m.fill(0xFFFFFFFFu);
        return m;
    }();

    // Live layers: always >= 1 (0 is not deleted).
    int m_layerCount = 1;

    // Layer names. Only 0 is born named ("Default"), as in Unity.
    std::array<std::string, kLayerCount> m_layerNames = [] {
        std::array<std::string, kLayerCount> n;
        n[0] = "Default";
        return n;
    }();

    // Registered triggers (weak: the GameObjects own the colliders via
    // shared_ptr). They are traversed every frame to emit onTriggerStay; expired
    // ones are pruned on the fly.
    std::vector<std::weak_ptr<Collider>> m_triggerColliders;

    // Time accumulator of the fixed step. PxScene::simulate with the real frame dt
    // makes physics non-deterministic (the same scenario falls differently at
    // 60 and at 144 fps) and with a long frame —asset loading, breakpoint— the
    // integrator makes a huge jump and bodies pass through each other. By storing the
    // remainder and always simulating chunks of m_fixedDeltaTime, the result
    // only depends on the total elapsed time.
    float m_fixedDeltaTime = 1.0f / 60.0f;
    int   m_maxSubSteps    = 8;
    float m_accumulator    = 0.0f;

#ifdef DT_PHYSX_ENABLED
    void* m_foundation      = nullptr; // physx::PxFoundation*
    void* m_physics         = nullptr; // physx::PxPhysics*
    void* m_scene           = nullptr; // physx::PxScene*
    void* m_dispatcher      = nullptr; // physx::PxDefaultCpuDispatcher*
    void* m_triggerCallback = nullptr; // TriggerDispatcher* (PxSimulationEventCallback)
#endif
};

} // namespace DonTopo
