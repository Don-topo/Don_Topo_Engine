#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
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

    // Valores del PxMaterial que se crea por collider. Coinciden con los
    // defaults de Collider (m_staticFriction/m_dynamicFriction/m_restitution) y
    // con los del material global que compartían todos los colliders antes, así
    // que una escena existente simula exactamente igual.
    constexpr float kDefaultStaticFriction  = 0.5f;
    constexpr float kDefaultDynamicFriction = 0.5f;
    constexpr float kDefaultRestitution     = 0.1f;

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

        // Pares NO-trigger que se tocan de verdad. Gemelo de onTrigger, con dos
        // diferencias que vienen de PhysX, no de aquí:
        //  - El Stay SÍ es nativo (eNOTIFY_TOUCH_PERSISTS), así que no hay que
        //    sintetizarlo por frame como en los triggers.
        //  - Una colisión no tiene lado "dueño": se notifica a LOS DOS
        //    colliders, cada uno con el otro como `other` (igual que Unity).
        // Los flags de notificación solo se piden para pares no-trigger (ver
        // dtTriggerFilterShader), así que aquí nunca llega un trigger.
        void onContact(const PxContactPairHeader& header,
                       const PxContactPair* pairs, PxU32 count) override
        {
            // Actor borrado este frame: su userData ya cuelga.
            if (header.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 |
                                PxContactPairHeaderFlag::eREMOVED_ACTOR_1))
                return;

            auto* colA = static_cast<DonTopo::Collider*>(header.actors[0]->userData);
            auto* colB = static_cast<DonTopo::Collider*>(header.actors[1]->userData);
            if (!colA || !colB) return;

            for (PxU32 i = 0; i < count; ++i)
            {
                const PxContactPair& cp = pairs[i];
                // Misma guarda que en onTrigger, a nivel de shape.
                if (cp.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 |
                                PxContactPairFlag::eREMOVED_SHAPE_1))
                    continue;

                if (cp.events & PxPairFlag::eNOTIFY_TOUCH_FOUND)
                {
                    colA->dispatchCollisionEnter(colB);
                    colB->dispatchCollisionEnter(colA);
                }
                else if (cp.events & PxPairFlag::eNOTIFY_TOUCH_PERSISTS)
                {
                    colA->dispatchCollisionStay(colB);
                    colB->dispatchCollisionStay(colA);
                }
                else if (cp.events & PxPairFlag::eNOTIFY_TOUCH_LOST)
                {
                    colA->dispatchCollisionExit(colB);
                    colB->dispatchCollisionExit(colA);
                }
            }
        }

        // Resto de eventos de simulación: no usados.
        void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
        void onWake(PxActor**, PxU32) override {}
        void onSleep(PxActor**, PxU32) override {}
        void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}
    };

    // Filter shader: para pares que involucran un trigger, pide notificación
    // Enter/Exit (eTRIGGER_DEFAULT) SIN suprimir por kinematic — el
    // PxDefaultSimulationFilterShader descarta los pares kinematic-kinematic y
    // kinematic-static, y sin esto un trigger no vería a los objetos con
    // Rigidbody kinematic, que son la mayoría de los que se mueven por script.
    // Los pares NO-trigger se delegan al shader por defecto para preservar
    // exactamente el comportamiento de colisión previo.
    //
    // OJO con lo que este shader NO puede arreglar: los pares static-static no
    // llegan hasta aquí. PhysX no los forma siquiera —dos actores estáticos no
    // pueden moverse el uno respecto al otro—, así que un trigger sin
    // Rigidbody no detecta objetos que tampoco lo tengan. Es la regla de Unity
    // ("al menos uno de los dos necesita Rigidbody"); la avisa el editor en la
    // sección del collider y la fijan los tests de trigger de physics_tests.cpp.
    // (Este comentario decía antes que "casi todos los colliders son
    // kinematic": dejó de ser cierto cuando la dinámica se separó del Collider
    // y un collider sin Rigidbody pasó a ser PxRigidStatic.)
    PxFilterFlags dtTriggerFilterShader(
        PxFilterObjectAttributes attr0, PxFilterData fd0,
        PxFilterObjectAttributes attr1, PxFilterData fd1,
        PxPairFlags& pairFlags, const void* constantBlock, PxU32 constantBlockSize)
    {
        // CAPAS, antes que nada: word0 = bit de la capa propia, word1 = máscara
        // de capas con las que colisiona (lo escribe
        // PhysicsManager::refreshColliderFilter). El par sólo sobrevive si CADA
        // lado acepta al otro. Va delante de la rama de trigger a propósito: una
        // capa filtrada tampoco debe disparar onTriggerEnter.
        //
        // eSUPPRESS, no eKILL: la matriz se puede reactivar en runtime y PhysX
        // tiene que poder volver a formar el par (eKILL lo descartaría pa
        // siempre). Con la matriz por defecto word1 es 0xFFFFFFFF y ningún par
        // se suprime.
        if ((fd0.word1 & fd1.word0) == 0 || (fd1.word1 & fd0.word0) == 0)
            return PxFilterFlag::eSUPPRESS;

        if (PxFilterObjectIsTrigger(attr0) || PxFilterObjectIsTrigger(attr1)) {
            pairFlags = PxPairFlag::eTRIGGER_DEFAULT;
            return PxFilterFlag::eDEFAULT;
        }
        // OJO: al shader por defecto se le pasa PxFilterData VACÍO a propósito,
        // no el nuestro. PxDefaultSimulationFilterShader indexa su tabla interna
        // de grupos con el word0 crudo (gCollisionTable[word0][word0], 32x32) y
        // convierte word2/word3 en un PxGroupsMask; con nuestro word0 = 1<<capa
        // (hasta 2^31) leería fuera de la tabla. Vaciándolo ve exactamente lo
        // mismo que veía antes de existir las capas —todo a cero—, que es lo que
        // preserva el comportamiento previo bit a bit.
        PxFilterFlags flags =
            PxDefaultSimulationFilterShader(attr0, PxFilterData(), attr1, PxFilterData(),
                                            pairFlags, constantBlock, constantBlockSize);

        // Y sobre lo que decidiera el shader por defecto, se piden los avisos de
        // contacto para OnCollisionEnter/Stay/Exit. Se hace DESPUÉS y solo si el
        // par sobrevive: si el shader lo mató o lo suprimió, pairFlags no
        // significa nada y añadirle bits no cambiaría el resultado, pero sí
        // enmascararía el motivo al depurar.
        //
        // Solo llega aquí lo no-trigger: la rama de trigger de arriba retorna
        // antes con eTRIGGER_DEFAULT intacto. Es a propósito — PhysX ni siquiera
        // genera contactos para una shape marcada eTRIGGER_SHAPE, así que pedir
        // eNOTIFY_TOUCH_* ahí sería ruido que nunca se dispara.
        //
        // Coste: PERSISTS hace que PhysX llame a onContact cada sub-step por cada
        // par en contacto, incluso si nadie escucha. A cambio, el Stay es nativo
        // y no hay que recorrer registros por frame como en los triggers.
        //
        // eDETECT_CCD_CONTACT va en el mismo lote: sin este bit en el par, el
        // pase continuo de la escena NO se ejecuta para él y marcar el cuerpo
        // con eENABLE_CCD no haría nada. Pedirlo aquí no activa CCD por sí solo
        // —PhysX salta el barrido si ningún actor del par lleva el flag de
        // cuerpo—, así que los pares de siempre siguen resolviéndose igual.
        if (!(flags & (PxFilterFlag::eKILL | PxFilterFlag::eSUPPRESS)))
            pairFlags |= PxPairFlag::eNOTIFY_TOUCH_FOUND
                       | PxPairFlag::eNOTIFY_TOUCH_LOST
                       | PxPairFlag::eNOTIFY_TOUCH_PERSISTS
                       | PxPairFlag::eDETECT_CCD_CONTACT;

        return flags;
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
    // CCD a nivel de ESCENA: requisito previo, no un interruptor global. PhysX
    // exige el flag para reservar el pase de barrido continuo, pero ese pase
    // sólo mira a los cuerpos que lleven además PxRigidBodyFlag::eENABLE_CCD
    // (lo pone Rigidbody::setCcd, default OFF). Sin ningún cuerpo marcado el
    // coste es nulo y la simulación es exactamente la de antes.
    sceneDesc.flags |= PxSceneFlag::eENABLE_CCD;
    auto* scene = physics->createScene(sceneDesc);
    physxCheck(scene, "PxPhysics::createScene");
    m_scene = scene;

    // Ya no hay material global: cada collider crea el suyo en su factoría
    // (material de física por collider, ver kDefault* arriba).

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
#endif
}

std::shared_ptr<BoxCollider> PhysicsManager::createBoxColliderComponent(
    const glm::vec3& halfExtents,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool dynamic)
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
    auto* scene = static_cast<PxScene*>(m_scene);

    // Material EXCLUSIVO de este collider (mismos valores que el global de
    // antes, así ninguna escena cambia de comportamiento). Es refcounted: la
    // shape se queda una referencia en createExclusiveShape, así que soltamos
    // la nuestra justo después y el material muere con la shape.
    PxMaterial* material = physics->createMaterial(kDefaultStaticFriction,
                                                   kDefaultDynamicFriction,
                                                   kDefaultRestitution);
    physxCheck(material, "PxPhysics::createMaterial(box)");

    PxRigidActor* actor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(actor, "PxPhysics::createRigidActor(box)");

    PxBoxGeometry geometry(halfExtents.x, halfExtents.y, halfExtents.z);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    material->release();
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    if (dynamic)
    {
        // Masa por defecto: Rigidbody la recalcula en bindActor. Sin gravedad ni
        // kinematic aquí; los pone attachRigidbody -> Rigidbody::bindActor.
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(actor), 1.0f);
    }

    scene->addActor(*actor);

    auto collider = std::make_shared<BoxCollider>(actor, shape, halfExtents, center);
    // La escala del Transform no cabe en la PxTransform del actor: se hornea en
    // la geometría. La shape se creó con el tamaño configurado, así que con
    // escala 1 esto no toca nada (setWorldScale sale antes de setGeometry).
    collider->setWorldScale(scale);
    collider->setManager(this);
    // Alta en el registro + PxFilterData inicial de su capa (la 0 por defecto).
    registerCollider(collider);
    // userData del actor = Collider* base (upcast explícito para respetar
    // cualquier offset de la base); lo lee el TriggerDispatcher para saber
    // quién solapó a quién.
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)dynamic;
    auto collider = std::make_shared<BoxCollider>(nullptr, nullptr, halfExtents, center);
    collider->setManager(this);
    registerCollider(collider);
    return collider;
#endif
}

std::shared_ptr<SphereCollider> PhysicsManager::createSphereColliderComponent(
    float radius,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool dynamic)
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
    auto* scene = static_cast<PxScene*>(m_scene);

    // Material exclusivo del collider; ver nota en createBoxColliderComponent.
    PxMaterial* material = physics->createMaterial(kDefaultStaticFriction,
                                                   kDefaultDynamicFriction,
                                                   kDefaultRestitution);
    physxCheck(material, "PxPhysics::createMaterial(sphere)");

    PxRigidActor* actor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(actor, "PxPhysics::createRigidActor(sphere)");

    PxSphereGeometry geometry(radius);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    material->release();
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z)));

    if (dynamic)
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(actor), 1.0f);

    scene->addActor(*actor);

    auto collider = std::make_shared<SphereCollider>(actor, shape, radius, center);
    collider->setWorldScale(scale); // ver nota en createBoxColliderComponent
    collider->setManager(this);
    registerCollider(collider);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)dynamic;
    auto collider = std::make_shared<SphereCollider>(nullptr, nullptr, radius, center);
    collider->setManager(this);
    registerCollider(collider);
    return collider;
#endif
}

std::shared_ptr<CapsuleCollider> PhysicsManager::createCapsuleColliderComponent(
    float radius,
    float halfHeight,
    const glm::vec3& center,
    const glm::mat4& worldTransform,
    bool dynamic)
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
    auto* scene = static_cast<PxScene*>(m_scene);

    // Material exclusivo del collider; ver nota en createBoxColliderComponent.
    PxMaterial* material = physics->createMaterial(kDefaultStaticFriction,
                                                   kDefaultDynamicFriction,
                                                   kDefaultRestitution);
    physxCheck(material, "PxPhysics::createMaterial(capsule)");

    PxRigidActor* actor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(actor, "PxPhysics::createRigidActor(capsule)");

    PxCapsuleGeometry geometry(radius, halfHeight);
    PxShape* shape = PxRigidActorExt::createExclusiveShape(*actor, geometry, *material);
    physxCheck(shape, "PxRigidActorExt::createExclusiveShape");
    material->release();
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z), axisCorrection()));

    if (dynamic)
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(actor), 1.0f);

    scene->addActor(*actor);

    auto collider = std::make_shared<CapsuleCollider>(actor, shape, radius, halfHeight, center);
    collider->setWorldScale(scale); // ver nota en createBoxColliderComponent
    collider->setManager(this);
    registerCollider(collider);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    (void)dynamic;
    auto collider = std::make_shared<CapsuleCollider>(nullptr, nullptr, radius, halfHeight, center);
    collider->setManager(this);
    registerCollider(collider);
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
    auto* scene = static_cast<PxScene*>(m_scene);

    // Material exclusivo del collider; ver nota en createBoxColliderComponent.
    PxMaterial* material = physics->createMaterial(kDefaultStaticFriction,
                                                   kDefaultDynamicFriction,
                                                   kDefaultRestitution);
    physxCheck(material, "PxPhysics::createMaterial(plane)");

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
    material->release();
    shape->setLocalPose(PxTransform(PxVec3(center.x, center.y, center.z), axisCorrection()));

    // Sin updateMassAndInertia: un plano no tiene volumen, PhysX no puede
    // calcular masa/inercia sobre esa geometría. No hace falta — el actor
    // siempre queda kinematic (nunca se simula como cuerpo dinámico).

    scene->addActor(*actor);

    auto collider = std::make_shared<PlaneCollider>(actor, shape, center);
    collider->setManager(this);
    registerCollider(collider);
    Collider* base = collider.get();
    actor->userData = base;
    return collider;
#else
    (void)worldTransform;
    auto collider = std::make_shared<PlaneCollider>(nullptr, nullptr, center);
    collider->setManager(this);
    registerCollider(collider);
    return collider;
#endif
}

void PhysicsManager::attachRigidbody(const std::shared_ptr<Collider>& collider,
                                     const std::shared_ptr<Rigidbody>& rb)
{
    if (!collider || !rb) return;
#ifdef DT_PHYSX_ENABLED
    void* actor = collider->actorHandle();
    // Si el actor todavía es static, reconstruirlo como dynamic antes de enlazar.
    if (actor && !static_cast<PxRigidActor*>(actor)->is<PxRigidDynamic>())
        rebuildActor(collider, /*dynamic=*/true);
    rb->bindActor(collider->actorHandle());
#else
    (void)collider;
    rb->bindActor(nullptr);
#endif
}

void PhysicsManager::detachRigidbody(const std::shared_ptr<Collider>& collider)
{
    if (!collider) return;
#ifdef DT_PHYSX_ENABLED
    void* actor = collider->actorHandle();
    if (actor && static_cast<PxRigidActor*>(actor)->is<PxRigidDynamic>())
        rebuildActor(collider, /*dynamic=*/false);
    // Sin Rigidbody no hay nada que interpolar (la propiedad vive allí), y un
    // static al que el editor mueva por el Transform no debe arrastrar la pose
    // previa de cuando era dinámico.
    collider->setInterpolate(false);
    // Nota: el Rigidbody que apuntaba a este collider conserva un m_actor que
    // ahora cuelga (el dynamic viejo fue liberado por rebuildActor). Contrato:
    // los callers (editor "Remove Rigidbody", Lua RemoveComponent) sueltan el
    // shared_ptr<Rigidbody> inmediatamente después de detach, así que nadie lo
    // desreferencia. Si aparece un caller que reutilice el Rigidbody, debe
    // re-bindear (rb->bindActor(nullptr) o attach a otro collider) antes de usarlo.
#else
    (void)collider;
#endif
}

#ifdef DT_PHYSX_ENABLED
void* PhysicsManager::rebuildActor(const std::shared_ptr<Collider>& collider, bool dynamic)
{
    auto* physics  = static_cast<PxPhysics*>(m_physics);
    auto* scene    = static_cast<PxScene*>(m_scene);
    auto* oldActor = static_cast<PxRigidActor*>(collider->actorHandle());
    if (!oldActor) return nullptr;

    PxTransform pose = oldActor->getGlobalPose();
    auto* shape = static_cast<PxShape*>(collider->geometryShape());
    bool wasTrigger = collider->isTrigger();

    // PxShape es refcounted: se coge una ref extra pa que sobreviva al detach
    // del actor viejo, y se suelta tras re-adjuntarla al nuevo.
    shape->acquireReference();
    oldActor->detachShape(*shape);
    scene->removeActor(*oldActor);
    oldActor->release();

    PxRigidActor* newActor = dynamic
        ? static_cast<PxRigidActor*>(physics->createRigidDynamic(pose))
        : static_cast<PxRigidActor*>(physics->createRigidStatic(pose));
    physxCheck(newActor, "rebuildActor: createRigidActor");
    newActor->attachShape(*shape);
    shape->release();
    if (dynamic)
        PxRigidBodyExt::updateMassAndInertia(*static_cast<PxRigidDynamic*>(newActor), 1.0f);
    scene->addActor(*newActor);
    newActor->userData = collider.get();

    collider->setActorHandle(newActor);
    // Los flags de trigger viven en la shape (que sobrevivió), pero se re-asegura.
    if (wasTrigger) collider->applyTriggerFlag(true);
    // El PxFilterData también vive en la shape, así que el swap de actor lo
    // conserva; se reescribe igualmente pa que la capa siga siendo la fuente de
    // la verdad aunque alguien toque la shape por otro camino.
    refreshColliderFilter(collider.get());
    return newActor;
}
#endif

#ifdef DT_PHYSX_ENABLED
bool PhysicsManager::raycast(const PxVec3& origin, const PxVec3& dir, float maxDistance, PxRaycastBuffer& hit)
{
    return static_cast<PxScene*>(m_scene)->raycast(origin, dir, maxDistance, hit);
}

bool PhysicsManager::raycast(const PxVec3& origin, const PxVec3& dir, float maxDistance,
                             PxRaycastBuffer& hit, const PxQueryFilterData& filterData,
                             PxQueryFilterCallback* filterCall)
{
    // Sin escena (el editor fuera de Play no llama a init) no hay nada que
    // consultar: false en vez de deref de nulo.
    if (!m_scene) return false;
    return static_cast<PxScene*>(m_scene)->raycast(
        origin, dir, maxDistance, hit,
        PxHitFlags(PxHitFlag::ePOSITION | PxHitFlag::eNORMAL),
        filterData, filterCall);
}

bool PhysicsManager::raycastAll(const PxVec3& origin, const PxVec3& dir, float maxDistance,
                                PxRaycastBuffer& hits, const PxQueryFilterData& filterData,
                                PxQueryFilterCallback* filterCall)
{
    if (!m_scene) return false;

    // eNO_BLOCK degrada a eTOUCH todo lo que el prefiltro marque como eBLOCK,
    // así el rayo atraviesa el primer impacto y sigue recogiendo los demás.
    PxQueryFilterData fd = filterData;
    fd.flags |= PxQueryFlag::eNO_BLOCK;

    const bool any = static_cast<PxScene*>(m_scene)->raycast(
        origin, dir, maxDistance, hits,
        PxHitFlags(PxHitFlag::ePOSITION | PxHitFlag::eNORMAL),
        fd, filterCall);

    // PhysX entrega los touches en el orden en que los encuentra el barrido
    // espacial, no por distancia. Se ordena aquí (sobre el almacenamiento del
    // propio buffer) para que el contrato valga para todos los callers.
    if (hits.nbTouches > 1 && hits.touches)
        std::sort(hits.touches, hits.touches + hits.nbTouches,
                  [](const PxRaycastHit& a, const PxRaycastHit& b) { return a.distance < b.distance; });

    return any;
}

bool PhysicsManager::sphereCast(const PxVec3& origin, const PxVec3& dir, float radius,
                                float maxDistance, PxSweepBuffer& hit,
                                const PxQueryFilterData& filterData,
                                PxQueryFilterCallback* filterCall)
{
    if (!m_scene) return false;

    // La geometría del barrido va en su propia pose; el origen del sweep es la
    // posición inicial del centro de la esfera, no un punto sobre su
    // superficie.
    return static_cast<PxScene*>(m_scene)->sweep(
        PxSphereGeometry(radius), PxTransform(origin), dir, maxDistance, hit,
        PxHitFlags(PxHitFlag::ePOSITION | PxHitFlag::eNORMAL),
        filterData, filterCall);
}

bool PhysicsManager::overlapSphere(const PxVec3& center, float radius, PxOverlapBuffer& hits,
                                   const PxQueryFilterData& filterData,
                                   PxQueryFilterCallback* filterCall)
{
    if (!m_scene) return false;

    // Sin eNO_BLOCK el prefiltro (que devuelve eBLOCK) cerraría la consulta en
    // el primer solape y sólo se reportaría uno.
    PxQueryFilterData fd = filterData;
    fd.flags |= PxQueryFlag::eNO_BLOCK;

    return static_cast<PxScene*>(m_scene)->overlap(
        PxSphereGeometry(radius), PxTransform(center), hits, fd, filterCall);
}

bool PhysicsManager::overlapBox(const PxVec3& center, const PxVec3& halfExtents,
                                const PxQuat& rotation, PxOverlapBuffer& hits,
                                const PxQueryFilterData& filterData,
                                PxQueryFilterCallback* filterCall)
{
    if (!m_scene) return false;

    PxQueryFilterData fd = filterData;
    fd.flags |= PxQueryFlag::eNO_BLOCK;

    return static_cast<PxScene*>(m_scene)->overlap(
        PxBoxGeometry(halfExtents), PxTransform(center, rotation), hits, fd, filterCall);
}
#endif

void PhysicsManager::setFixedDeltaTime(float dt)
{
    // Un paso <= 0 dejaría el bucle de sub-steps restando 0 al acumulador, o
    // sumándole: cuelgue seguro. Se ignora y se conserva el valor anterior.
    if (dt <= 0.0f) return;
    m_fixedDeltaTime = dt;
}

void PhysicsManager::setMaxSubSteps(int steps)
{
    // Con 0 sub-steps la física no avanzaría nunca y el acumulador crecería
    // hasta descartarse cada frame: mínimo uno.
    m_maxSubSteps = (steps < 1) ? 1 : steps;
}

void PhysicsManager::stepSimulation(float dt)
{
#ifdef DT_PHYSX_ENABLED
    // El dt real del frame sólo alimenta el acumulador; a PxScene::simulate
    // siempre se le pasa m_fixedDeltaTime (ver el porqué en el header). dt <= 0
    // se ignora: el primer frame llega con 0 (last se inicializa == now) y
    // PxScene::simulate exige > 0. En fetchResults se despachan los Enter/Exit
    // vía TriggerDispatcher.
    if (dt > 0.0f) m_accumulator += dt;

    // Margen para el error de coma flotante: 3 * (1.0f/60.0f) acumulado en
    // float queda un pelo por debajo de sumar 1.0f/60.0f tres veces, y sin
    // margen esa comparación se comería un sub-step (justo lo que rompe el
    // determinismo que buscamos).
    constexpr float kEpsilon = 1e-6f;

    int steps = 0;
    while (m_accumulator + kEpsilon >= m_fixedDeltaTime && steps < m_maxSubSteps)
    {
        // Pose de partida del sub-step, para los colliders que interpolan. Va
        // ANTES de simulate: es la mitad "vieja" de la mezcla que hará
        // getWorldTransform. No-op en los que no interpolan (el default).
        for (auto& weak : m_colliders)
            if (auto c = weak.lock()) c->capturePreviousPose();

        static_cast<PxScene*>(m_scene)->simulate(m_fixedDeltaTime);
        static_cast<PxScene*>(m_scene)->fetchResults(true);
        m_accumulator -= m_fixedDeltaTime;
        ++steps;

        // Stay UNA VEZ POR SUB-STEP, dentro del bucle, para que Enter/Stay/Exit
        // lleven la misma cadencia que la simulación (los Enter/Exit los emite
        // fetchResults de este mismo sub-step). Consecuencia asumida: un frame
        // lento emite varios Stay seguidos.
        dispatchTriggerStay();
    }

    // Si tras agotar los sub-steps aún sobra tiempo, se TIRA en vez de quedar a
    // deber: acarrear la deuda tras un stall hace que los frames siguientes
    // vayan siempre al máximo de sub-steps, tarden más, y acumulen más deuda
    // todavía (espiral de la muerte).
    if (m_accumulator + kEpsilon >= m_fixedDeltaTime) m_accumulator = 0.0f;

    // Cuánto del siguiente paso fijo lleva ya consumido el tiempo real: es el
    // alpha con el que los colliders que interpolan mezclan la pose previa con
    // la actual. Se empuja también cuando no ha habido sub-step —ahí es
    // justamente donde el alpha crece y el cuerpo sigue avanzando en pantalla
    // en vez de quedarse clavado hasta el siguiente paso.
    {
        const float alpha = (m_fixedDeltaTime > 0.0f)
            ? glm::clamp(m_accumulator / m_fixedDeltaTime, 0.0f, 1.0f)
            : 1.0f;
        for (auto& weak : m_colliders)
            if (auto c = weak.lock()) c->setInterpolationAlpha(alpha);
    }
#else
    (void)dt;
    dispatchTriggerStay();
#endif
}

void PhysicsManager::dispatchTriggerStay()
{
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

void PhysicsManager::registerCollider(const std::shared_ptr<Collider>& collider)
{
    if (!collider) return;
    m_colliders.push_back(collider);
    // Primer volcado del filtro: sin él la shape se quedaría con el
    // PxFilterData a cero y el shader de capas suprimiría todos sus pares.
    refreshColliderFilter(collider.get());
}

void PhysicsManager::refreshAllColliderFilters()
{
    for (auto it = m_colliders.begin(); it != m_colliders.end(); )
    {
        auto collider = it->lock();
        if (!collider) { it = m_colliders.erase(it); continue; }
        refreshColliderFilter(collider.get());
        ++it;
    }
}

void PhysicsManager::refreshColliderFilter(Collider* collider)
{
#ifdef DT_PHYSX_ENABLED
    if (!collider) return;
    auto* shape = static_cast<PxShape*>(collider->geometryShape());
    if (!shape) return;
    const int layer = collider->getLayer();
    if (!isValidLayer(layer)) return;

    // word2/word3 quedan a cero: no los usa ni este shader ni el de queries.
    PxFilterData fd = shape->getSimulationFilterData();
    fd.word0 = 1u << static_cast<uint32_t>(layer);
    fd.word1 = m_layerMasks[static_cast<size_t>(layer)];
    shape->setSimulationFilterData(fd);
#else
    (void)collider;
#endif
}

void PhysicsManager::setLayerCollision(int a, int b, bool enabled)
{
    if (!isValidLayer(a) || !isValidLayer(b)) return;

    const uint32_t bitA = 1u << static_cast<uint32_t>(a);
    const uint32_t bitB = 1u << static_cast<uint32_t>(b);
    if (enabled)
    {
        m_layerMasks[static_cast<size_t>(a)] |= bitB;
        m_layerMasks[static_cast<size_t>(b)] |= bitA;
    }
    else
    {
        m_layerMasks[static_cast<size_t>(a)] &= ~bitB;
        m_layerMasks[static_cast<size_t>(b)] &= ~bitA;
    }

    // La matriz es global pero el filtro está COPIADO en cada shape: sin este
    // repaso el cambio no lo vería ningún collider ya creado.
    refreshAllColliderFilters();
}

bool PhysicsManager::getLayerCollision(int a, int b) const
{
    if (!isValidLayer(a) || !isValidLayer(b)) return false;
    return (m_layerMasks[static_cast<size_t>(a)] & (1u << static_cast<uint32_t>(b))) != 0;
}

int PhysicsManager::addLayer(const std::string& name)
{
    if (m_layerCount >= kLayerCount) return -1;
    const int nueva = m_layerCount++;
    // La fila estaba liberada: se deja "colisiona con todo", que es el default de
    // una capa recién creada. Si la ocupó una capa borrada antes, removeLayer ya
    // la dejó así.
    m_layerNames[static_cast<size_t>(nueva)] = name;
    return nueva;
}

bool PhysicsManager::removeLayer(int layer)
{
    // La 0 es la de respaldo: si se pudiera borrar, los colliders reasignados no
    // tendrían adónde ir.
    if (layer <= 0 || layer >= m_layerCount) return false;

    // Colliders primero, con la numeración VIEJA todavía en pie: los de la capa
    // que muere caen a la 0, los de encima bajan un puesto para que la lista no
    // deje huecos.
    for (auto it = m_colliders.begin(); it != m_colliders.end(); )
    {
        auto collider = it->lock();
        if (!collider) { it = m_colliders.erase(it); continue; }
        const int suya = collider->getLayer();
        if (suya == layer)     collider->setLayer(0);
        else if (suya > layer) collider->setLayer(suya - 1);
        ++it;
    }

    // Matriz: quitar fila y columna 'layer'. Se hace sobre una copia booleana
    // porque desplazar bits en sitio se pisa a sí mismo.
    const int viejo = m_layerCount;
    bool tabla[kLayerCount][kLayerCount];
    for (int a = 0; a < viejo; ++a)
        for (int b = 0; b < viejo; ++b)
            tabla[a][b] = getLayerCollision(a, b);

    for (int a = 0; a < viejo - 1; ++a)
    {
        const int origenA = (a >= layer) ? a + 1 : a;
        uint32_t  fila    = 0xFFFFFFFFu; // los bits >= layerCount nuevo: sin filtrar
        for (int b = 0; b < viejo - 1; ++b)
        {
            const int origenB = (b >= layer) ? b + 1 : b;
            if (!tabla[origenA][origenB]) fila &= ~(1u << static_cast<uint32_t>(b));
        }
        m_layerMasks[static_cast<size_t>(a)] = fila;
    }

    // La última queda liberada: nombre vacío y sin filtros, lista pa que la
    // reutilice el siguiente addLayer.
    m_layerMasks[static_cast<size_t>(viejo - 1)] = 0xFFFFFFFFu;

    for (int i = layer; i < viejo - 1; ++i)
        m_layerNames[static_cast<size_t>(i)] = m_layerNames[static_cast<size_t>(i + 1)];
    m_layerNames[static_cast<size_t>(viejo - 1)].clear();

    m_layerCount = viejo - 1;

    // Los colliders ya llevan su índice nuevo, pero su word1 salió de la matriz
    // VIEJA: hay que reescribirlo con la compactada.
    refreshAllColliderFilters();
    return true;
}

void PhysicsManager::setLayerName(int layer, const std::string& name)
{
    if (!isValidLayer(layer)) return;
    m_layerNames[static_cast<size_t>(layer)] = name;
}

std::string PhysicsManager::getLayerName(int layer) const
{
    if (!isValidLayer(layer)) return std::string();
    return m_layerNames[static_cast<size_t>(layer)];
}

uint32_t PhysicsManager::layerMask(int layer) const
{
    if (!isValidLayer(layer)) return 0u;
    return m_layerMasks[static_cast<size_t>(layer)];
}

void PhysicsManager::onColliderDestroyed(Collider* collider)
{
    // Poda del registro de capas: el que muere ya no bloquea su weak_ptr
    // (refcount 0 durante ~Collider), así que sale aquí junto con cualquier otro
    // expirado. Sin esto el vector crecería sin fin.
    m_colliders.erase(
        std::remove_if(m_colliders.begin(), m_colliders.end(),
                       [](const std::weak_ptr<Collider>& w) { return w.expired(); }),
        m_colliders.end());

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
