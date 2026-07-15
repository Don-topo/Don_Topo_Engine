#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Colliders/Collider.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"

#include <algorithm>

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

    // Recibe los pares de trigger de PhysX y los reenvía a los callbacks del
    // collider. cada PxShape lleva en su actor un userData = Collider* (lo pone
    // PhysicsManager al crear el collider), así se recupera quién solapó a
    // quién. PhysX solo emite Enter/Exit (TOUCH_FOUND/LOST); el Stay lo
    // sintetiza PhysicsManager::stepSimulation recorriendo los overlaps.
    class TriggerDispatcher : public PxSimulationEventCallback {
    public:
        void onTrigger(PxTriggerPair* pairs, PxU32 count) override {
            for (PxU32 i = 0; i < count; ++i) {
                const PxTriggerPair& p = pairs[i];
                // shape ya liberado (actor destruido este frame): userData
                // colgaría, se ignora.
                if (p.flags & (PxTriggerPairFlag::eREMOVED_SHAPE_TRIGGER |
                               PxTriggerPairFlag::eREMOVED_SHAPE_OTHER))
                    continue;

                PxRigidActor* tActor = p.triggerShape->getActor();
                PxRigidActor* oActor = p.otherShape->getActor();
                if (!tActor || !oActor) continue;

                auto* triggerCol = static_cast<DonTopo::Collider*>(tActor->userData);
                auto* otherCol   = static_cast<DonTopo::Collider*>(oActor->userData);
                if (!triggerCol || !otherCol) continue;

                if (p.status & PxPairFlag::eNOTIFY_TOUCH_FOUND)
                    triggerCol->beginOverlap(otherCol);
                else if (p.status & PxPairFlag::eNOTIFY_TOUCH_LOST)
                    triggerCol->endOverlap(otherCol);
            }
        }

        // Resto de eventos de simulación: no usados.
        void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
        void onWake(PxActor**, PxU32) override {}
        void onSleep(PxActor**, PxU32) override {}
        void onContact(const PxContactPairHeader&, const PxContactPair*, PxU32) override {}
        void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}
    };

    // Filter shader: para pares que involucran un trigger, pide notificación
    // Enter/Exit (eTRIGGER_DEFAULT) SIN suprimir por kinematic — el
    // PxDefaultSimulationFilterShader descarta los pares kinematic-kinematic y
    // kinematic-static, y como aquí casi todos los colliders son kinematic
    // (useGravity=false), sin esto los triggers no dispararían nada. Los pares
    // NO-trigger se delegan al shader por defecto para preservar exactamente el
    // comportamiento de colisión previo.
    PxFilterFlags dtTriggerFilterShader(
        PxFilterObjectAttributes attr0, PxFilterData fd0,
        PxFilterObjectAttributes attr1, PxFilterData fd1,
        PxPairFlags& pairFlags, const void* constantBlock, PxU32 constantBlockSize)
    {
        if (PxFilterObjectIsTrigger(attr0) || PxFilterObjectIsTrigger(attr1)) {
            pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
            return PxFilterFlag::eDEFAULT;
        }
        return PxDefaultSimulationFilterShader(attr0, fd0, attr1, fd1,
                                               pairFlags, constantBlock, constantBlockSize);
    }
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
    sceneDesc.filterShader  = dtTriggerFilterShader;
    auto* scene = physics->createScene(sceneDesc);
    physxCheck(scene, "PxPhysics::createScene");
    m_scene = scene;

    auto* material = physics->createMaterial(0.5f, 0.5f, 0.1f);
    physxCheck(material, "PxPhysics::createMaterial");
    m_material = material;

    // Callback que recibe los pares de trigger y los reenvía a los colliders.
    auto* triggerCallback = new TriggerDispatcher();
    scene->setSimulationEventCallback(triggerCallback);
    m_triggerCallback = triggerCallback;
#endif
}

void PhysicsManager::shutdown()
{
#ifdef DT_PHYSX_ENABLED
    if (m_scene)      { static_cast<PxScene*>(m_scene)->release();      m_scene = nullptr; }
    // Tras liberar la escena nadie más referencia el callback: se borra aquí.
    if (m_triggerCallback) { delete static_cast<TriggerDispatcher*>(m_triggerCallback); m_triggerCallback = nullptr; }
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

    auto collider = std::make_shared<BoxCollider>(actor, shape, halfExtents, center, useGravity);
    collider->setManager(this);
    // userData del actor = Collider* base (upcast explícito para respetar
    // cualquier offset de la base); lo lee el TriggerDispatcher para saber
    // quién solapó a quién.
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)density;
    auto collider = std::make_shared<BoxCollider>(nullptr, nullptr, halfExtents, center, useGravity);
    collider->setManager(this);
    return collider;
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

    auto collider = std::make_shared<SphereCollider>(actor, shape, radius, center, useGravity);
    collider->setManager(this);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)density;
    auto collider = std::make_shared<SphereCollider>(nullptr, nullptr, radius, center, useGravity);
    collider->setManager(this);
    return collider;
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

    auto collider = std::make_shared<CapsuleCollider>(actor, shape, radius, halfHeight, center, useGravity);
    collider->setManager(this);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)density;
    auto collider = std::make_shared<CapsuleCollider>(nullptr, nullptr, radius, halfHeight, center, useGravity);
    collider->setManager(this);
    return collider;
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

    auto collider = std::make_shared<PlaneCollider>(actor, shape, center);
    collider->setManager(this);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    auto collider = std::make_shared<PlaneCollider>(nullptr, nullptr, center);
    collider->setManager(this);
    return collider;
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
    // primer frame: dt=0 (last se inicializa == now), PxScene::simulate exige
    // > 0. En fetchResults se despachan los Enter/Exit vía TriggerDispatcher.
    if (dt > 0.0f)
    {
        static_cast<PxScene*>(m_scene)->simulate(dt);
        static_cast<PxScene*>(m_scene)->fetchResults(true);
    }
#else
    (void)dt;
#endif

    // Sintetiza onTriggerStay: PhysX solo da Enter/Exit, así que se recorren los
    // triggers vivos y se emite Stay por cada overlap actual. Los triggers
    // expirados (GameObject destruido) se podan al vuelo. Sin PhysX el registro
    // está siempre vacío.
    for (auto it = m_triggerColliders.begin(); it != m_triggerColliders.end(); )
    {
        auto collider = it->lock();
        if (!collider) { it = m_triggerColliders.erase(it); continue; }
        collider->dispatchStay();
        ++it;
    }
}

void PhysicsManager::setTrigger(const std::shared_ptr<Collider>& collider, bool enabled)
{
    if (!collider) return;
    collider->applyTriggerFlag(enabled);

    // Poda expirados y detecta si ya estaba registrado (una sola pasada).
    bool present = false;
    for (auto it = m_triggerColliders.begin(); it != m_triggerColliders.end(); )
    {
        auto existing = it->lock();
        if (!existing) { it = m_triggerColliders.erase(it); continue; }
        if (existing == collider) present = true;
        ++it;
    }

    if (enabled && !present)
    {
        m_triggerColliders.push_back(collider);
    }
    else if (!enabled && present)
    {
        m_triggerColliders.erase(
            std::remove_if(m_triggerColliders.begin(), m_triggerColliders.end(),
                [&](const std::weak_ptr<Collider>& w) {
                    auto s = w.lock();
                    return !s || s == collider;
                }),
            m_triggerColliders.end());
    }
}

void PhysicsManager::onColliderDestroyed(Collider* collider)
{
    // El collider que muere puede estar en los overlaps de otros triggers:
    // se purga de todos para no dejar punteros colgantes en el próximo Stay.
    // Si el que muere era él mismo un trigger, su weak_ptr ya no bloquea
    // (refcount 0 durante ~Collider) y se poda aquí.
    for (auto it = m_triggerColliders.begin(); it != m_triggerColliders.end(); )
    {
        auto trigger = it->lock();
        if (!trigger) { it = m_triggerColliders.erase(it); continue; }
        trigger->removeOverlapSilent(collider);
        ++it;
    }
}

} // namespace DonTopo
