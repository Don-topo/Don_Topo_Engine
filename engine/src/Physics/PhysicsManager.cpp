#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <stdexcept>
#include <string>

using namespace physx;

namespace {
    PxDefaultAllocator      g_allocator;
    PxDefaultErrorCallback  g_errorCallback;

    // Mismo truco usado en CapsuleCollider.cpp/PlaneCollider.cpp: PhysX
    // orienta PxCapsuleGeometry a lo largo de X y define la normal de
    // PxPlaneGeometry como el eje X local del shape. Esta rotación fija
    // (90° sobre Z) mapea ese eje X a Y en ambos casos.
    PxQuat axisCorrection() { return PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f)); }
}

static void physxCheck(void* ptr, const char* ctx) {
    if (!ptr)
        throw std::runtime_error(std::string(ctx) + ": creation failed");
}
#endif

namespace DonTopo {

PhysicsManager::~PhysicsManager() { shutdown(); }

void PhysicsManager::init()
{
#ifdef DT_PHYSX_ENABLED
    auto* foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_allocator, g_errorCallback);
    physxCheck(foundation, "PxCreateFoundation");
    m_foundation = foundation;

    // El mundo usa centímetros (gravedad -981 = -9.81 m/s² * 100), no metros.
    // PxTolerancesScale default asume 1 unidad = 1 metro; con ese default,
    // sleepThreshold/contactOffset/bounceThresholdVelocity quedan ~100x
    // demasiado pequeños para velocidades en cm/s, así que un actor en reposo
    // nunca alcanza el umbral de sueño y vibra indefinidamente.
    PxTolerancesScale scale;
    scale.length = 100.0f; // 100 unidades = 1 metro
    scale.speed  = 981.0f; // velocidad típica de caída tras 1s bajo esta gravedad
    auto* physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, scale);
    physxCheck(physics, "PxCreatePhysics");
    m_physics = physics;

    auto* dispatcher = PxDefaultCpuDispatcherCreate(2);
    physxCheck(dispatcher, "PxDefaultCpuDispatcherCreate");
    m_dispatcher = dispatcher;

    PxSceneDesc sceneDesc(physics->getTolerancesScale());
    sceneDesc.gravity       = PxVec3(0.0f, -981.0f, 0.0f);
    sceneDesc.cpuDispatcher = dispatcher;
    sceneDesc.filterShader  = PxDefaultSimulationFilterShader;
    auto* scene = physics->createScene(sceneDesc);
    physxCheck(scene, "PxPhysics::createScene");
    m_scene = scene;

    auto* material = physics->createMaterial(0.5f, 0.5f, 0.1f);
    physxCheck(material, "PxPhysics::createMaterial");
    m_material = material;
#endif
}

void PhysicsManager::shutdown()
{
#ifdef DT_PHYSX_ENABLED
    if (m_scene)      { static_cast<PxScene*>(m_scene)->release();      m_scene = nullptr; }
    if (m_dispatcher) { static_cast<PxDefaultCpuDispatcher*>(m_dispatcher)->release(); m_dispatcher = nullptr; }
    if (m_physics)    { static_cast<PxPhysics*>(m_physics)->release();  m_physics = nullptr; }
    if (m_foundation) { static_cast<PxFoundation*>(m_foundation)->release(); m_foundation = nullptr; }
    m_material = nullptr; // liberado implícitamente por PxPhysics::release()
#endif
}

std::shared_ptr<BoxCollider> PhysicsManager::createBoxColliderComponent(
    const glm::vec3& halfExtents,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool useGravity,
    float density)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* material = static_cast<PxMaterial*>(m_material);
    auto* scene = static_cast<PxScene*>(m_scene);

    PxRigidDynamic* actor = physics->createRigidDynamic(pose);
    physxCheck(actor, "PxPhysics::createRigidDynamic");

    PxBoxGeometry geometry(halfExtents.x, halfExtents.y, halfExtents.z);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    PxRigidBodyExt::updateMassAndInertia(*actor, density);

    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !useGravity);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !useGravity);

    scene->addActor(*actor);

    return std::make_shared<BoxCollider>(actor, shape, halfExtents, center, useGravity);
#else
    (void)worldTransform;
    (void)density;
    return std::make_shared<BoxCollider>(nullptr, nullptr, halfExtents, center, useGravity);
#endif
}

std::shared_ptr<SphereCollider> PhysicsManager::createSphereColliderComponent(
    float radius,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool useGravity,
    float density)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* material = static_cast<PxMaterial*>(m_material);
    auto* scene = static_cast<PxScene*>(m_scene);

    PxRigidDynamic* actor = physics->createRigidDynamic(pose);
    physxCheck(actor, "PxPhysics::createRigidDynamic");

    PxSphereGeometry geometry(radius);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    PxRigidBodyExt::updateMassAndInertia(*actor, density);

    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !useGravity);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !useGravity);

    scene->addActor(*actor);

    return std::make_shared<SphereCollider>(actor, shape, radius, center, useGravity);
#else
    (void)worldTransform;
    (void)density;
    return std::make_shared<SphereCollider>(nullptr, nullptr, radius, center, useGravity);
#endif
}

std::shared_ptr<CapsuleCollider> PhysicsManager::createCapsuleColliderComponent(
    float radius,
    float halfHeight,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool useGravity,
    float density)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* material = static_cast<PxMaterial*>(m_material);
    auto* scene = static_cast<PxScene*>(m_scene);

    PxRigidDynamic* actor = physics->createRigidDynamic(pose);
    physxCheck(actor, "PxPhysics::createRigidDynamic");

    PxCapsuleGeometry geometry(radius, halfHeight);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z), axisCorrection()));

    PxRigidBodyExt::updateMassAndInertia(*actor, density);

    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !useGravity);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, !useGravity);

    scene->addActor(*actor);

    return std::make_shared<CapsuleCollider>(actor, shape, radius, halfHeight, center, useGravity);
#else
    (void)worldTransform;
    (void)density;
    return std::make_shared<CapsuleCollider>(nullptr, nullptr, radius, halfHeight, center, useGravity);
#endif
}

std::shared_ptr<PlaneCollider> PhysicsManager::createPlaneColliderComponent(
    const glm::vec3& center,
    const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );

    auto* physics = static_cast<PxPhysics*>(m_physics);
    auto* material = static_cast<PxMaterial*>(m_material);
    auto* scene = static_cast<PxScene*>(m_scene);

    PxRigidDynamic* actor = physics->createRigidDynamic(pose);
    physxCheck(actor, "PxPhysics::createRigidDynamic");

    // El actor debe quedar kinematic ANTES de attachear el shape de plano:
    // PhysX rechaza (createExclusiveShape devuelve null) un shape de
    // geometría plane/mesh como simulation shape sobre un PxRigidDynamic que
    // todavía no es kinematic en el momento del attach.
    actor->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
    actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);

    PxPlaneGeometry geometry;
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z), axisCorrection()));

    // Sin updateMassAndInertia: un plano no tiene volumen, PhysX no puede
    // calcular masa/inercia sobre esa geometría. No hace falta — el actor
    // siempre queda kinematic (nunca se simula como cuerpo dinámico).

    scene->addActor(*actor);

    return std::make_shared<PlaneCollider>(actor, shape, center);
#else
    (void)worldTransform;
    return std::make_shared<PlaneCollider>(nullptr, nullptr, center);
#endif
}

#ifdef DT_PHYSX_ENABLED
bool PhysicsManager::raycast(const PxVec3& origin, const PxVec3& dir, float maxDistance, PxRaycastBuffer& hit)
{
    return static_cast<PxScene*>(m_scene)->raycast(origin, dir, maxDistance, hit);
}
#endif

void PhysicsManager::stepSimulation(float dt)
{
#ifdef DT_PHYSX_ENABLED
    if (dt <= 0.0f) return; // primer frame: dt=0 (last se inicializa == now), PxScene::simulate exige > 0

    static_cast<PxScene*>(m_scene)->simulate(dt);
    static_cast<PxScene*>(m_scene)->fetchResults(true);
#else
    (void)dt;
#endif
}

} // namespace DonTopo
