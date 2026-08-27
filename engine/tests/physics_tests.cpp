// Test headless del núcleo de física (sin GUI). Plain main + asserts, sin
// framework — coherente con un proyecto C++/CMake/Ninja sin infra de tests.
//
// PhysX sólo admite UNA PxFoundation por proceso (crearla dos veces, aunque se
// libere entremedias, crashea). Por eso se comparte un único PhysicsManager
// entre todos los tests: cada test crea sus colliders como locales, que al
// salir de la función liberan su actor de la escena — así sólo hay un cuerpo
// vivo a la vez y los tests no se interfieren.
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Physics/Colliders/Collider.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Avanza la simulación n pasos de dt segundos.
static void step(PhysicsManager& pm, int n, float dt) { for (int i = 0; i < n; ++i) pm.stepSimulation(dt); }

// Un cuerpo dinámico con gravedad debe caer (Y decrece).
static void test_free_fall(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(y1 < y0 - 1.0f);
}

// Kinematic no cae.
static void test_kinematic_no_fall(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setIsKinematic(true);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f);
}

// Freeze-Y mantiene Y aunque haya gravedad.
static void test_freeze_position_y(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setConstraints(RB_FreezePositionY);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f);
}

// addImpulse cambia la velocidad en la dirección esperada.
static void test_add_impulse(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    rb->addImpulse(glm::vec3(100.0f, 0.0f, 0.0f));
    step(pm, 1, 1.0f / 60.0f);
    CHECK(rb->getVelocity().x > 0.0f);
}

// --- ForceMode ---------------------------------------------------------------
//
// Aplica el MISMO vector con dos modos distintos sobre dos cuerpos idénticos
// (masa 1, sin gravedad, sin drag) y devuelve la velocidad tras UN paso.
// Force integra durante el paso: v = F*dt/m. VelocityChange escribe la
// velocidad de golpe: v = F. Con dt = 1/60 son 60× de diferencia, así que un
// mapeo mal hecho no puede colarse por redondeo.
static float velocityAfterOneStep(PhysicsManager& pm, ForceMode mode)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    CHECK(std::fabs(rb->getMass() - 1.0f) < 1e-6f); // la fórmula de abajo asume masa 1
    rb->addForce(glm::vec3(100.0f, 0.0f, 0.0f), mode);
    step(pm, 1, 1.0f / 60.0f);
    return rb->getVelocity().x;
}

// Force vs VelocityChange con el mismo vector: la velocidad resultante difiere
// en el factor dt esperado.
static void test_force_mode_changes_magnitude(PhysicsManager& pm)
{
    const float dt = 1.0f / 60.0f;
    float vForce  = velocityAfterOneStep(pm, ForceMode::Force);
    float vVelCh  = velocityAfterOneStep(pm, ForceMode::VelocityChange);
    std::printf("  ForceMode: Force -> %.4f | VelocityChange -> %.4f\n", vForce, vVelCh);
    CHECK(std::fabs(vForce - 100.0f * dt) < 0.01f); // F*dt/m
    CHECK(std::fabs(vVelCh - 100.0f)      < 0.01f); // v de golpe
    CHECK(vVelCh > vForce * 10.0f);                 // inequívocamente distintos
}

// El modo por defecto es Force: addForce(v) sin modo == addForce(v, Force).
// Sin esto, un default cambiado por descuido rompería en silencio todo el
// código de tres argumentos que ya existe.
static void test_force_mode_default_is_force(PhysicsManager& pm)
{
    // Separados en Z para que no se empujen entre ellos: dos cajas en el mismo
    // origen se despenetran y contaminan la velocidad medida.
    auto rbA = std::make_shared<Rigidbody>();
    rbA->setUseGravity(false);
    auto colA = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(colA, rbA);
    rbA->addForce(glm::vec3(100.0f, 0.0f, 0.0f)); // sin modo

    auto rbB = std::make_shared<Rigidbody>();
    rbB->setUseGravity(false);
    auto colB = pm.createBoxColliderComponent(
        glm::vec3(1.0f), glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 100.0f)), /*dynamic=*/true);
    pm.attachRigidbody(colB, rbB);
    rbB->addForce(glm::vec3(100.0f, 0.0f, 0.0f), ForceMode::Force);

    step(pm, 1, 1.0f / 60.0f);
    CHECK(rbA->getVelocity().x > 0.0f); // que no sean iguales por ser ambas cero
    CHECK(std::fabs(rbA->getVelocity().x - rbB->getVelocity().x) < 1e-5f);
}

// Rebuild static <-> dynamic conserva el shape. Tras detach el collider sigue
// vivo y estático (no cae) y su geometría queda intacta.
static void test_rebuild_preserves_shape(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    pm.detachRigidbody(col); // vuelve a static
    float y0 = col->getWorldTransform()[3].y;
    step(pm, 30, 1.0f / 60.0f);
    float y1 = col->getWorldTransform()[3].y;
    CHECK(std::fabs(y1 - y0) < 0.001f);       // static no cae
    CHECK(col->getHalfExtents() == glm::vec3(1.0f)); // geometría intacta
}

// --- Triggers: la regla de "al menos un Rigidbody" ---------------------------
//
// PhysX NO genera pares para dos actores estáticos: no se pueden mover el uno
// respecto al otro, así que ni siquiera llama al filter shader. Un collider sin
// Rigidbody es PxRigidStatic desde que la dinámica se separó del Collider, de
// modo que un trigger sin Rigidbody NO detecta objetos que tampoco lo tengan.
//
// Es la misma regla que Unity ("al menos uno de los dos necesita Rigidbody"),
// pero aquí no había nada que la dijera: se marcaba Is Trigger y no pasaba
// nada, sin diagnóstico. Estos tests la fijan en código ejecutable, y de paso
// miden qué combinaciones funcionan de verdad en vez de suponerlo.

// Listener de prueba: cuenta los Enter/Exit que recibe.
struct CountingListener : ITriggerListener {
    int enters = 0;
    int exits  = 0;
    void onTriggerEnter(const TriggerEvent&) override { ++enters; }
    void onTriggerExit (const TriggerEvent&) override { ++exits;  }
};

// Monta trigger y objeto SOLAPADOS (mismo origen) y simula. Devuelve los Enter
// que recibió el trigger. withRbTrigger/withRbOther deciden si cada lado lleva
// Rigidbody, que es lo único que cambia entre los tres tests de abajo.
static int entersWith(PhysicsManager& pm, bool withRbTrigger, bool withRbOther)
{
    auto trigger = pm.createBoxColliderComponent(glm::vec3(10.0f), glm::vec3(0.0f),
                                                  glm::mat4(1.0f), withRbTrigger);
    auto rbT = std::make_shared<Rigidbody>();
    if (withRbTrigger)
    {
        pm.attachRigidbody(trigger, rbT);
        rbT->setIsKinematic(true);   // que no se caiga durante la prueba
    }
    pm.setTrigger(trigger, true);

    auto other = pm.createBoxColliderComponent(glm::vec3(5.0f), glm::vec3(0.0f),
                                                glm::mat4(1.0f), withRbOther);
    auto rbO = std::make_shared<Rigidbody>();
    if (withRbOther)
    {
        pm.attachRigidbody(other, rbO);
        rbO->setIsKinematic(true);
    }

    CountingListener listener;
    trigger->addListener(&listener);
    step(pm, 5, 1.0f / 60.0f);
    trigger->removeListener(&listener);
    pm.setTrigger(trigger, false);
    return listener.enters;
}

// EL CASO QUE ROMPÍA: los dos sin Rigidbody, o sea los dos PxRigidStatic.
// PhysX no forma el par y el trigger no se entera de nada.
static void test_trigger_needs_a_rigidbody(PhysicsManager& pm)
{
    CHECK(entersWith(pm, /*trigger*/false, /*other*/false) == 0);
}

// Basta con que lo tenga EL OBJETO que entra: static vs dynamic sí forma par.
static void test_trigger_fires_when_other_has_rigidbody(PhysicsManager& pm)
{
    CHECK(entersWith(pm, /*trigger*/false, /*other*/true) > 0);
}

// O con que lo tenga el propio trigger, que es el caso simétrico.
static void test_trigger_fires_when_trigger_has_rigidbody(PhysicsManager& pm)
{
    CHECK(entersWith(pm, /*trigger*/true, /*other*/false) > 0);
}

// --- Material de física por collider ----------------------------------------
//
// Cada collider tiene su propio PxMaterial (antes lo compartían todos), así que
// dos esferas con restitution distinta deben rebotar distinto. El modo de
// combinación de PhysX es eAVERAGE: la restitution efectiva del contacto es la
// media entre la de la esfera y la del plano (0.1 por defecto).

// Sigue el punto más bajo alcanzado y la altura máxima POSTERIOR a ese mínimo,
// que es justo el ápice del primer rebote.
struct BounceTrack {
    float minY = 1e9f;
    float apex = -1e9f;
    void feed(float y) { if (y < minY) { minY = y; apex = y; } else if (y > apex) apex = y; }
    float rise() const { return apex - minY; }
};

// Deja caer dos esferas (restitution 0.0 y 0.9) sobre un PlaneCollider y
// devuelve cuánto rebotó cada una. Van separadas en X para que choquen sólo
// contra el plano y no entre ellas.
//
// createDynamic=false crea las esferas como estáticas y las promociona con
// attachRigidbody, lo que pasa por rebuildActor: ese camino re-adjunta la MISMA
// shape al actor nuevo, y con ella su PxMaterial. Es la comprobación de que el
// material sobrevive al rebuild MIRANDO LA SIMULACIÓN, no la copia en C++ que
// devuelven los getters.
static void dropTwoSpheres(PhysicsManager& pm, bool createDynamic,
                            float& riseDead, float& riseBouncy)
{
    auto plane = pm.createPlaneColliderComponent(glm::vec3(0.0f), glm::mat4(1.0f));

    auto rbDead   = std::make_shared<Rigidbody>();
    auto rbBouncy = std::make_shared<Rigidbody>();

    auto dead = pm.createSphereColliderComponent(
        25.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(-200.0f, 300.0f, 0.0f)), createDynamic);
    dead->setBounciness(0.0f);
    pm.attachRigidbody(dead, rbDead);

    auto bouncy = pm.createSphereColliderComponent(
        25.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(200.0f, 300.0f, 0.0f)), createDynamic);
    bouncy->setBounciness(0.9f);
    pm.attachRigidbody(bouncy, rbBouncy);

    BounceTrack tDead, tBouncy;
    for (int i = 0; i < 240; ++i)
    {
        pm.stepSimulation(1.0f / 60.0f);
        tDead.feed(dead->getWorldTransform()[3].y);
        tBouncy.feed(bouncy->getWorldTransform()[3].y);
    }
    riseDead   = tDead.rise();
    riseBouncy = tBouncy.rise();
}

// Dos esferas dinámicas con restitution distinta rebotan a alturas distintas.
static void test_restitution_changes_bounce_height(PhysicsManager& pm)
{
    float riseDead = 0.0f, riseBouncy = 0.0f;
    dropTwoSpheres(pm, /*createDynamic=*/true, riseDead, riseBouncy);
    std::printf("  rebote: restitution 0.0 -> %.2f | restitution 0.9 -> %.2f\n", riseDead, riseBouncy);
    // La "muerta" no rebota a cero: el modo de combinación es eAVERAGE, así que
    // contra el plano (0.1 por defecto) le queda una restitution efectiva de
    // 0.05. Lo que se fija aquí es que la elástica rebota un orden de magnitud
    // más, no un valor absoluto exacto.
    CHECK(riseBouncy > 20.0f);             // la elástica rebota de verdad
    CHECK(riseDead < 15.0f);               // la muerta apenas despega
    CHECK(riseBouncy > riseDead * 3.0f);   // y la diferencia es inequívoca
}

// setFriction/setBounciness devuelven lo escrito y sobreviven al attach/detach
// de Rigidbody (que reconstruye el actor por dentro).
static void test_material_survives_rebuild(PhysicsManager& pm)
{
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                              glm::mat4(1.0f), /*dynamic=*/false);
    // Defaults recién creado: los mismos que tenía el PxMaterial global de
    // antes, o sea lo que ve una escena guardada sin estos campos.
    CHECK(std::fabs(col->getStaticFriction()  - 0.5f) < 1e-6f);
    CHECK(std::fabs(col->getDynamicFriction() - 0.5f) < 1e-6f);
    CHECK(std::fabs(col->getBounciness()      - 0.1f) < 1e-6f);

    col->setFriction(0.31f, 0.22f);
    col->setBounciness(0.77f);
    CHECK(std::fabs(col->getStaticFriction()  - 0.31f) < 1e-6f);
    CHECK(std::fabs(col->getDynamicFriction() - 0.22f) < 1e-6f);
    CHECK(std::fabs(col->getBounciness()      - 0.77f) < 1e-6f);

    auto rb = std::make_shared<Rigidbody>();
    pm.attachRigidbody(col, rb);   // static -> dynamic (rebuildActor)
    CHECK(std::fabs(col->getStaticFriction()  - 0.31f) < 1e-6f);
    CHECK(std::fabs(col->getDynamicFriction() - 0.22f) < 1e-6f);
    CHECK(std::fabs(col->getBounciness()      - 0.77f) < 1e-6f);

    pm.detachRigidbody(col);       // dynamic -> static (rebuildActor otra vez)
    CHECK(std::fabs(col->getStaticFriction()  - 0.31f) < 1e-6f);
    CHECK(std::fabs(col->getDynamicFriction() - 0.22f) < 1e-6f);
    CHECK(std::fabs(col->getBounciness()      - 0.77f) < 1e-6f);

    // Los getters sólo leen la copia en C++: si rebuildActor perdiera el
    // PxMaterial, seguirían diciendo lo correcto. Esto lo mide en la
    // simulación — el bounciness se escribe ANTES del rebuild.
    float riseDead = 0.0f, riseBouncy = 0.0f;
    dropTwoSpheres(pm, /*createDynamic=*/false, riseDead, riseBouncy);
    std::printf("  rebote tras rebuildActor: restitution 0.0 -> %.2f | restitution 0.9 -> %.2f\n",
                riseDead, riseBouncy);
    CHECK(riseBouncy > 20.0f);
    CHECK(riseDead < 15.0f);
    CHECK(riseBouncy > riseDead * 3.0f);
}

// --- Paso fijo con acumulador -----------------------------------------------

// Deja el acumulador del PhysicsManager compartido a 0: un dt enorme agota los
// sub-steps y el sobrante se descarta, así cada test arranca igual aunque el
// anterior dejara un resto.
static void flushAccumulator(PhysicsManager& pm) { pm.stepSimulation(1000.0f); }

// Caída libre de un cuerpo nuevo tras `calls` llamadas de `dt` segundos.
// El collider es local: al salir se lleva su actor, no interfiere con el resto.
static float fallDistance(PhysicsManager& pm, int calls, float dt)
{
    auto rb  = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    for (int i = 0; i < calls; ++i) pm.stepSimulation(dt);
    return y0 - col->getWorldTransform()[3].y;
}

// Un frame de 3 pasos fijos avanza lo mismo que 3 frames de un paso.
static void test_fixed_step_is_framerate_independent(PhysicsManager& pm)
{
    // El caller real pasa el dt medido del frame, no un múltiplo exacto en
    // float del paso fijo: 3.0f/60.0f queda un pelo POR DEBAJO de sumar
    // 1.0f/60.0f tres veces, y sin margen de redondeo el bucle se comería el
    // tercer sub-step. Por eso el dt de abajo se escribe así y no como 3*fixed.
    CHECK(std::fabs(pm.getFixedDeltaTime() - 1.0f / 60.0f) < 1e-9f);
    const float fixed = 1.0f / 60.0f;
    flushAccumulator(pm);
    float three = fallDistance(pm, 3, fixed);
    flushAccumulator(pm);
    float once  = fallDistance(pm, 1, 3.0f / 60.0f);
    std::printf("  determinismo: 3x1 paso -> %.6f | 1x3 pasos -> %.6f\n", three, once);
    CHECK(std::fabs(three - once) < 1e-4f);
    CHECK(three > 0.0f);
}

// Un dt gigante no simula más de maxSubSteps sub-steps (ni cuelga).
static void test_giant_dt_clamped_to_max_substeps(PhysicsManager& pm)
{
    const float fixed = pm.getFixedDeltaTime();
    const int   maxSs = pm.getMaxSubSteps();
    flushAccumulator(pm);
    float reference = fallDistance(pm, maxSs, fixed);  // exactamente maxSubSteps
    flushAccumulator(pm);
    float giant     = fallDistance(pm, 1, 5.0f);       // 300 sub-steps si no hay clamp
    std::printf("  dt gigante: %d pasos -> %.6f | dt=5s -> %.6f\n", maxSs, reference, giant);
    CHECK(std::fabs(giant - reference) < 1e-4f);
}

// Tras agotar los sub-steps el sobrante se tira: el frame siguiente vuelve a
// costar un solo sub-step, no otra tanda entera de deuda pendiente.
static void test_giant_dt_leaves_no_debt(PhysicsManager& pm)
{
    const float fixed = pm.getFixedDeltaTime();
    const int   maxSs = pm.getMaxSubSteps();
    flushAccumulator(pm);
    float reference = fallDistance(pm, maxSs + 1, fixed);  // maxSubSteps + 1 sub-steps

    flushAccumulator(pm);
    auto rb  = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    float y0 = col->getWorldTransform()[3].y;
    pm.stepSimulation(5.0f);    // maxSubSteps sub-steps, resto descartado
    pm.stepSimulation(fixed);   // con deuda acarreada serían maxSubSteps más
    float measured = y0 - col->getWorldTransform()[3].y;
    std::printf("  sin deuda: %d pasos -> %.6f | 5s + 1 paso -> %.6f\n", maxSs + 1, reference, measured);
    CHECK(std::fabs(measured - reference) < 1e-4f);
}

// Los setters rechazan lo que colgaría el bucle de sub-steps.
static void test_fixed_step_setters_reject_bad_values(PhysicsManager& pm)
{
    const float originalDt    = pm.getFixedDeltaTime();
    const int   originalSteps = pm.getMaxSubSteps();

    pm.setFixedDeltaTime(0.0f);
    CHECK(pm.getFixedDeltaTime() == originalDt);
    pm.setFixedDeltaTime(-1.0f);
    CHECK(pm.getFixedDeltaTime() == originalDt);
    pm.setFixedDeltaTime(1.0f / 120.0f);            // válido: sí cambia
    CHECK(std::fabs(pm.getFixedDeltaTime() - 1.0f / 120.0f) < 1e-9f);
    pm.setFixedDeltaTime(originalDt);

    pm.setMaxSubSteps(0);
    CHECK(pm.getMaxSubSteps() == 1);
    pm.setMaxSubSteps(-5);
    CHECK(pm.getMaxSubSteps() == 1);
    pm.setMaxSubSteps(originalSteps);
    CHECK(pm.getMaxSubSteps() == originalSteps);
}

// ---------------------------------------------------------------------------
// Sweeps y overlaps (Physics.SphereCast / OverlapSphere / OverlapBox por
// debajo). Consultas de sólo lectura: no hace falta simular ni un paso, basta
// con que los actores estén en la escena.
// ---------------------------------------------------------------------------

// Prefiltro que acepta TODO devolviendo eBLOCK: exactamente lo que hace el
// RaycastFilter del binding de Lua cuando la shape pasa los filtros. Sin él la
// consulta no ejercería el eNO_BLOCK que mete overlapSphere/overlapBox (sin
// prefiltro, PhysX ya reporta todos los solapes como touch y el test pasaría
// aunque esa línea no existiera).
class AcceptAllBlocking : public physx::PxQueryFilterCallback
{
public:
    physx::PxQueryHitType::Enum preFilter(const physx::PxFilterData&,
                                          const physx::PxShape*,
                                          const physx::PxRigidActor*,
                                          physx::PxHitFlags&) override
    {
        return physx::PxQueryHitType::eBLOCK;
    }
    physx::PxQueryHitType::Enum postFilter(const physx::PxFilterData&,
                                           const physx::PxQueryHit&,
                                           const physx::PxShape*,
                                           const physx::PxRigidActor*) override
    {
        return physx::PxQueryHitType::eBLOCK;
    }
};

static AcceptAllBlocking g_queryFilter;

// Mismos flags que arma el binding: prefiltro + static + dynamic.
static physx::PxQueryFilterData defaultQueryFilter()
{
    physx::PxQueryFilterData fd;
    fd.flags = physx::PxQueryFlag::ePREFILTER | physx::PxQueryFlag::eSTATIC |
               physx::PxQueryFlag::eDYNAMIC;
    return fd;
}

// Un rayo de grosor cero pasa a 120 de una esfera de radio 100 (falla por 20),
// pero barriendo una esfera de radio 50 sí la toca. Fija que el radio llega
// hasta PhysX: con radius 0 el sweep se comportaría como el raycast y no
// tocaría nada.
static void test_sphere_cast_uses_the_radius(PhysicsManager& pm)
{
    auto diana = pm.createSphereColliderComponent(
        100.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 500.0f)), /*dynamic=*/false);

    physx::PxSweepBuffer gordo, fino;
    const bool tocaGordo = pm.sphereCast(physx::PxVec3(0.0f, 120.0f, 0.0f),
                                          physx::PxVec3(0.0f, 0.0f, 1.0f),
                                          50.0f, 1000.0f, gordo, defaultQueryFilter(), &g_queryFilter);
    const bool tocaFino  = pm.sphereCast(physx::PxVec3(0.0f, 120.0f, 0.0f),
                                          physx::PxVec3(0.0f, 0.0f, 1.0f),
                                          1.0f, 1000.0f, fino, defaultQueryFilter(), &g_queryFilter);
    CHECK(tocaGordo == true);
    CHECK(tocaFino == false);
    // Contacto por delante de la esfera: menos de los 500 del centro.
    if (tocaGordo) CHECK(gordo.block.distance > 0.0f && gordo.block.distance < 500.0f);
}

// Barrido que pasa de largo: misma esfera, dirección opuesta, y otro que se
// queda corto por maxDistance.
static void test_sphere_cast_miss(PhysicsManager& pm)
{
    auto diana = pm.createSphereColliderComponent(
        100.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 500.0f)), /*dynamic=*/false);

    physx::PxSweepBuffer alReves, corto, directo;
    CHECK(pm.sphereCast(physx::PxVec3(0.0f), physx::PxVec3(0.0f, 0.0f, -1.0f),
                        50.0f, 1000.0f, alReves, defaultQueryFilter(), &g_queryFilter) == false);
    CHECK(pm.sphereCast(physx::PxVec3(0.0f), physx::PxVec3(0.0f, 0.0f, 1.0f),
                        50.0f, 100.0f, corto, defaultQueryFilter(), &g_queryFilter) == false);
    // Y el caso positivo de control: de frente sí toca, a 500-100-50 = 350.
    CHECK(pm.sphereCast(physx::PxVec3(0.0f), physx::PxVec3(0.0f, 0.0f, 1.0f),
                        50.0f, 1000.0f, directo, defaultQueryFilter(), &g_queryFilter) == true);
    CHECK(std::fabs(directo.block.distance - 350.0f) < 1.0f);
}

// 0, 1 y N solapes con la misma escena, sólo cambiando el radio. El caso N es
// el que se rompe si falta el eNO_BLOCK: sin él la consulta cierra en el primer
// solape y devuelve 1.
static void test_overlap_sphere_counts(PhysicsManager& pm)
{
    auto a = pm.createSphereColliderComponent(
        50.0f, glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/false);
    auto b = pm.createSphereColliderComponent(
        50.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 300.0f)), /*dynamic=*/false);
    auto c = pm.createSphereColliderComponent(
        50.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 600.0f)), /*dynamic=*/false);

    physx::PxOverlapBufferN<16> vacio, uno, tres;
    pm.overlapSphere(physx::PxVec3(0.0f, 5000.0f, 0.0f), 10.0f, vacio, defaultQueryFilter(), &g_queryFilter);
    pm.overlapSphere(physx::PxVec3(0.0f), 10.0f, uno, defaultQueryFilter(), &g_queryFilter);
    pm.overlapSphere(physx::PxVec3(0.0f, 0.0f, 300.0f), 400.0f, tres, defaultQueryFilter(), &g_queryFilter);

    CHECK(vacio.getNbTouches() == 0);
    CHECK(uno.getNbTouches() == 1);
    CHECK(tres.getNbTouches() == 3);
}

// La caja de overlap está ORIENTADA: una caja larga en Z ve la diana a z=300,
// y la misma caja girada 90° sobre Y (larga en X) ya no. Sin pasar la rotación
// a PhysX, las dos consultas darían 1.
static void test_overlap_box_honours_rotation(PhysicsManager& pm)
{
    auto diana = pm.createSphereColliderComponent(
        10.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 300.0f)), /*dynamic=*/false);

    const physx::PxVec3 half(50.0f, 50.0f, 1000.0f);
    physx::PxOverlapBufferN<16> alineada, girada;
    pm.overlapBox(physx::PxVec3(0.0f), half, physx::PxQuat(physx::PxIdentity),
                  alineada, defaultQueryFilter(), &g_queryFilter);
    pm.overlapBox(physx::PxVec3(0.0f), half,
                  physx::PxQuat(physx::PxHalfPi, physx::PxVec3(0.0f, 1.0f, 0.0f)),
                  girada, defaultQueryFilter(), &g_queryFilter);

    CHECK(alineada.getNbTouches() == 1);
    CHECK(girada.getNbTouches() == 0);
}

// --- Capas de colisión -------------------------------------------------------
//
// La matriz arranca ENTERA a true y el PhysicsManager es compartido: cada test
// toca sólo su celda y la restaura al salir, si no los tests de después
// simularían con otra matriz. Se usan capas != 0 a propósito, para que los
// colliders del resto de tests (todos en la capa 0) no se enteren.

// Monta un suelo estático en la capa 'capaSuelo' y una caja dinámica en
// 'capaCaja' a 1000 de altura. Devuelve las dos, vivas, pa poder tocar la
// matriz con la escena YA construida.
struct EscenaDeCapas {
    std::shared_ptr<BoxCollider> suelo;
    std::shared_ptr<BoxCollider> caja;
    std::shared_ptr<Rigidbody>   rb;
};

static EscenaDeCapas montarEscenaDeCapas(PhysicsManager& pm, int capaSuelo, int capaCaja)
{
    flushAccumulator(pm);
    EscenaDeCapas e;
    e.suelo = pm.createBoxColliderComponent(glm::vec3(500.0f, 10.0f, 500.0f), glm::vec3(0.0f),
                                            glm::mat4(1.0f), /*dynamic=*/false);
    e.suelo->setLayer(capaSuelo);

    e.caja = pm.createBoxColliderComponent(
        glm::vec3(10.0f), glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1000.0f, 0.0f)), /*dynamic=*/true);
    e.caja->setLayer(capaCaja);
    e.rb = std::make_shared<Rigidbody>();
    pm.attachRigidbody(e.caja, e.rb);
    return e;
}

// Con la celda (1,2) apagada el par ni se forma: la caja atraviesa el suelo.
// La misma escena con la matriz por defecto la deja reposando encima (control:
// test_layer_reenabled_at_runtime_makes_contact).
static void test_layer_off_suppresses_contact(PhysicsManager& pm)
{
    pm.setLayerCollision(1, 2, false);
    {
        EscenaDeCapas e = montarEscenaDeCapas(pm, /*suelo=*/1, /*caja=*/2);
        step(pm, 240, 1.0f / 60.0f);
        const float y = e.caja->getWorldTransform()[3].y;
        std::printf("  capas (1,2) off: y final = %.2f\n", y);
        CHECK(y < -100.0f); // ha pasado de largo el suelo (que reposaría en 20)
    }
    pm.setLayerCollision(1, 2, true);
}

// Reactivar la celda EN RUNTIME, con las shapes ya creadas y cayendo, tiene que
// devolver el contacto: es lo que fija que setLayerCollision reescriba el
// PxFilterData de los colliders vivos y no sólo la matriz.
static void test_layer_reenabled_at_runtime_makes_contact(PhysicsManager& pm)
{
    pm.setLayerCollision(1, 2, false);
    {
        EscenaDeCapas e = montarEscenaDeCapas(pm, /*suelo=*/1, /*caja=*/2);

        // 0,2 s con la capa filtrada: cae ~20, sigue MUY por encima del suelo.
        step(pm, 12, 1.0f / 60.0f);
        const float yAntes = e.caja->getWorldTransform()[3].y;
        CHECK(yAntes > 500.0f);

        pm.setLayerCollision(1, 2, true);
        step(pm, 240, 1.0f / 60.0f);
        const float yDespues = e.caja->getWorldTransform()[3].y;
        std::printf("  capas (1,2) on en runtime: y = %.2f -> %.2f\n", yAntes, yDespues);
        CHECK(yDespues > 15.0f);  // reposa sobre el suelo (10 de suelo + 10 de media caja)
        CHECK(yDespues < 25.0f);
    }
    pm.setLayerCollision(1, 2, true);
}

// Un trigger tampoco ve lo que la matriz filtra: la comprobación de capas va
// ANTES de la rama de trigger del filter shader.
static int entersWithLayers(PhysicsManager& pm, int capaTrigger, int capaOtro)
{
    auto trigger = pm.createBoxColliderComponent(glm::vec3(10.0f), glm::vec3(0.0f),
                                                 glm::mat4(1.0f), /*dynamic=*/false);
    trigger->setLayer(capaTrigger);
    pm.setTrigger(trigger, true);

    // El que entra lleva Rigidbody kinematic: es la combinación que SÍ dispara
    // con la matriz por defecto (ver test_trigger_fires_when_other_has_rigidbody).
    auto other = pm.createBoxColliderComponent(glm::vec3(5.0f), glm::vec3(0.0f),
                                               glm::mat4(1.0f), /*dynamic=*/true);
    other->setLayer(capaOtro);
    auto rbO = std::make_shared<Rigidbody>();
    pm.attachRigidbody(other, rbO);
    rbO->setIsKinematic(true);

    CountingListener listener;
    trigger->addListener(&listener);
    step(pm, 5, 1.0f / 60.0f);
    trigger->removeListener(&listener);
    pm.setTrigger(trigger, false);
    return listener.enters;
}

static void test_trigger_respects_layer_matrix(PhysicsManager& pm)
{
    pm.setLayerCollision(3, 4, false);
    const int filtrado = entersWithLayers(pm, /*trigger=*/3, /*otro=*/4);
    pm.setLayerCollision(3, 4, true);
    const int abierto = entersWithLayers(pm, /*trigger=*/3, /*otro=*/4);

    std::printf("  trigger con capas: filtrado -> %d enters | abierto -> %d enters\n",
                filtrado, abierto);
    CHECK(filtrado == 0);
    CHECK(abierto > 0);
}

// La matriz por defecto no filtra nada y los índices inválidos no revientan.
static void test_layer_matrix_defaults_and_bounds(PhysicsManager& pm)
{
    for (int i = 0; i < PhysicsManager::kLayerCount; ++i)
        CHECK(pm.layerMask(i) == 0xFFFFFFFFu);
    CHECK(pm.getLayerCollision(0, 31));
    CHECK(pm.getLayerName(0) == "Default");
    CHECK(pm.getLayerName(1).empty());

    // Fuera de rango: no-op, sin desbordar la matriz ni el shift.
    pm.setLayerCollision(-1, 0, false);
    pm.setLayerCollision(0, 32, false);
    CHECK(pm.layerMask(0) == 0xFFFFFFFFu);
    CHECK(!pm.getLayerCollision(-1, 0));
    CHECK(pm.layerMask(32) == 0u);
    CHECK(pm.getLayerName(32).empty());

    // Y una capa inválida en el collider conserva la que tenía.
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/false);
    col->setLayer(7);
    col->setLayer(99);
    col->setLayer(-3);
    CHECK(col->getLayer() == 7);
}

// Alta y baja de capas. Borrar COMPACTA: reasigna los colliders y desplaza la
// matriz, que es lo que impide que la lista quede con huecos.
static void test_add_and_remove_layers(PhysicsManager& pm)
{
    CHECK(pm.layerCount() == 1);          // solo "Default" al arrancar
    CHECK(pm.addLayer("Suelo")   == 1);
    CHECK(pm.addLayer("Enemigos") == 2);
    CHECK(pm.addLayer("Balas")   == 3);
    CHECK(pm.layerCount() == 4);

    // La 0 no se borra; un índice que no existe tampoco.
    CHECK(!pm.removeLayer(0));
    CHECK(!pm.removeLayer(4));

    // Matriz: se apaga (1,3) — que tras borrar la 2 pasará a ser (1,2).
    pm.setLayerCollision(1, 3, false);

    auto enLaQueMuere = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                                      glm::mat4(1.0f), /*dynamic=*/false);
    enLaQueMuere->setLayer(2);
    auto porEncima = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                                   glm::mat4(1.0f), /*dynamic=*/false);
    porEncima->setLayer(3);

    CHECK(pm.removeLayer(2));
    CHECK(pm.layerCount() == 3);

    // Los colliders: el de la capa muerta cae a la 0, el de encima baja uno.
    CHECK(enLaQueMuere->getLayer() == 0);
    CHECK(porEncima->getLayer() == 2);

    // Nombres desplazados y el hueco liberado, vacío.
    CHECK(pm.getLayerName(1) == "Suelo");
    CHECK(pm.getLayerName(2) == "Balas");
    CHECK(pm.getLayerName(3).empty());

    // La celda apagada viaja con la capa: era (1,3), ahora es (1,2).
    CHECK(!pm.getLayerCollision(1, 2));
    CHECK(pm.getLayerCollision(1, 1));
    // Y la capa liberada vuelve a "colisiona con todo".
    CHECK(pm.layerMask(3) == 0xFFFFFFFFu);

    // Restaurar pa los tests siguientes (el PhysicsManager es compartido).
    pm.setLayerCollision(1, 2, true);
    while (pm.layerCount() > 1) pm.removeLayer(pm.layerCount() - 1);
    CHECK(pm.layerCount() == 1);
}

// --- Callbacks de colisión (pares no-trigger) --------------------------------
//
// Gemelos de los de trigger, pero por el camino de onContact. Diferencia clave:
// el Stay es NATIVO (eNOTIFY_TOUCH_PERSISTS), no sintetizado por frame, así que
// aquí se cuenta lo que emite PhysX tal cual.

// Análogo a CountingListener, con las 3 fases.
struct CountingCollisionListener : ICollisionListener {
    int enters = 0;
    int stays  = 0;
    int exits  = 0;
    void onCollisionEnter(const CollisionEvent&) override { ++enters; }
    void onCollisionStay (const CollisionEvent&) override { ++stays;  }
    void onCollisionExit (const CollisionEvent&) override { ++exits;  }
};

// Suelo estático a ras y caja dinámica 200 por encima, en las capas dadas. La
// caja cae ~200 en 0,64 s, o sea que 60 sub-steps bastan para el impacto.
static EscenaDeCapas montarImpacto(PhysicsManager& pm, int capaSuelo, int capaCaja)
{
    flushAccumulator(pm);
    EscenaDeCapas e;
    e.suelo = pm.createBoxColliderComponent(glm::vec3(500.0f, 10.0f, 500.0f), glm::vec3(0.0f),
                                            glm::mat4(1.0f), /*dynamic=*/false);
    e.suelo->setLayer(capaSuelo);

    e.caja = pm.createBoxColliderComponent(
        glm::vec3(10.0f), glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 200.0f, 0.0f)), /*dynamic=*/true);
    e.caja->setLayer(capaCaja);
    e.rb = std::make_shared<Rigidbody>();
    pm.attachRigidbody(e.caja, e.rb);
    return e;
}

// El ciclo completo sobre el MISMO par: Enter al tocar el suelo, Stay mientras
// sigue apoyada, Exit al teletransportarla lejos.
static void test_collision_enter_stay_exit(PhysicsManager& pm)
{
    EscenaDeCapas e = montarImpacto(pm, /*suelo=*/0, /*caja=*/0);
    CountingCollisionListener l;
    e.caja->addCollisionListener(&l);

    step(pm, 90, 1.0f / 60.0f);   // cae, impacta y se queda apoyada
    std::printf("  colision: enters=%d stays=%d exits=%d (tras el impacto)\n",
                l.enters, l.stays, l.exits);
    CHECK(l.enters > 0);   // TOUCH_FOUND
    CHECK(l.stays  > 0);   // TOUCH_PERSISTS, mientras siguen en contacto
    CHECK(l.exits == 0);   // todavía no se ha separado de nada

    // Separar de golpe: setGlobalPose + autowake, PhysX rompe el par.
    const int staysAntes = l.stays;
    e.caja->teleport(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3000.0f, 0.0f)));
    step(pm, 10, 1.0f / 60.0f);
    std::printf("  colision: exits=%d tras separar (stays %d -> %d)\n",
                l.exits, staysAntes, l.stays);
    CHECK(l.exits > 0);              // TOUCH_LOST
    CHECK(l.stays == staysAntes);    // y deja de haber contacto que persista

    e.caja->removeCollisionListener(&l);
}

// La matriz de capas manda también en las colisiones: el par filtrado no genera
// NI UN evento, y el mismo montaje con la celda abierta sí (control).
static void test_collision_respects_layer_matrix(PhysicsManager& pm)
{
    pm.setLayerCollision(5, 6, false);
    CountingCollisionListener filtrado;
    {
        EscenaDeCapas e = montarImpacto(pm, /*suelo=*/5, /*caja=*/6);
        e.caja->addCollisionListener(&filtrado);
        step(pm, 90, 1.0f / 60.0f);
        e.caja->removeCollisionListener(&filtrado);
        // Y de paso: sin par, la caja atraviesa el suelo.
        CHECK(e.caja->getWorldTransform()[3].y < 0.0f);
    }

    pm.setLayerCollision(5, 6, true);
    CountingCollisionListener abierto;
    {
        EscenaDeCapas e = montarImpacto(pm, /*suelo=*/5, /*caja=*/6);
        e.caja->addCollisionListener(&abierto);
        step(pm, 90, 1.0f / 60.0f);
        e.caja->removeCollisionListener(&abierto);
    }

    std::printf("  colision con capas: filtrado -> %d/%d/%d | abierto -> %d/%d/%d\n",
                filtrado.enters, filtrado.stays, filtrado.exits,
                abierto.enters, abierto.stays, abierto.exits);
    CHECK(filtrado.enters == 0);
    CHECK(filtrado.stays  == 0);
    CHECK(filtrado.exits  == 0);
    CHECK(abierto.enters > 0);
    CHECK(abierto.stays  > 0);
}

// Un trigger NO emite eventos de colisión: PhysX no genera contactos para una
// shape eTRIGGER_SHAPE y el filter shader ni siquiera le pide eNOTIFY_TOUCH_*.
// Lo que reciba, lo recibe por el camino de trigger (que sigue intacto).
static void test_trigger_emits_no_collision_events(PhysicsManager& pm)
{
    EscenaDeCapas e = montarImpacto(pm, /*suelo=*/0, /*caja=*/0);
    pm.setTrigger(e.suelo, true);

    CountingCollisionListener colision;
    CountingListener          trigger;
    e.caja->addCollisionListener(&colision);
    e.suelo->addListener(&trigger);

    step(pm, 90, 1.0f / 60.0f);

    e.caja->removeCollisionListener(&colision);
    e.suelo->removeListener(&trigger);
    pm.setTrigger(e.suelo, false);

    std::printf("  suelo como trigger: colision=%d/%d/%d, trigger enters=%d\n",
                colision.enters, colision.stays, colision.exits, trigger.enters);
    CHECK(colision.enters == 0);
    CHECK(colision.stays  == 0);
    CHECK(trigger.enters > 0);   // control: el camino de trigger sigue vivo
}

// --- CCD e interpolación por Rigidbody ---------------------------------------

// El flag eENABLE_CCD tal y como lo ve PhysX en el actor (no la copia en C++
// del Rigidbody: eso lo devolvería getCcd() sin probar nada).
static bool actorHasCcdFlag(const std::shared_ptr<Collider>& col)
{
    auto* actor = static_cast<physx::PxRigidActor*>(col->actorHandle());
    auto* dyn   = actor ? actor->is<physx::PxRigidDynamic>() : nullptr;
    return dyn && (dyn->getRigidBodyFlags() & physx::PxRigidBodyFlag::eENABLE_CCD);
}

// X de la pose CRUDA del actor, saltándose getWorldTransform: es la referencia
// contra la que se compara la pose interpolada.
static float rawActorX(const std::shared_ptr<Collider>& col)
{
    auto* actor = static_cast<physx::PxRigidActor*>(col->actorHandle());
    return actor ? actor->getGlobalPose().p.x : 0.0f;
}

// setCcd escribe el flag en el actor de PhysX, y sólo cuando se pide.
static void test_ccd_flag_reaches_the_actor(PhysicsManager& pm)
{
    auto rb  = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);

    // Default: apagado en el componente y en el actor (ninguna escena existente
    // cambia de comportamiento).
    CHECK(!rb->getCcd());
    CHECK(!actorHasCcdFlag(col));

    rb->setCcd(true);
    CHECK(rb->getCcd());
    CHECK(actorHasCcdFlag(col));

    // PhysX no admite CCD en kinematic: el flag efectivo se cae solo...
    rb->setIsKinematic(true);
    CHECK(rb->getCcd());              // la intención del usuario se conserva
    CHECK(!actorHasCcdFlag(col));     // pero el actor no lo lleva
    // ...y vuelve al salir de kinematic.
    rb->setIsKinematic(false);
    CHECK(actorHasCcdFlag(col));

    rb->setCcd(false);
    CHECK(!actorHasCcdFlag(col));
}

// Cuánto atraviesa una esfera muy rápida un suelo fino, con y sin CCD. Devuelve
// la Y final: por encima del suelo = parada, muy por debajo = túnel.
static float dropFastSphereOnThinFloor(PhysicsManager& pm, bool ccd)
{
    flushAccumulator(pm);

    // Suelo fino: 4 unidades de grosor (halfExtent Y = 2) y ancho de sobra.
    auto floorCol = pm.createBoxColliderComponent(glm::vec3(500.0f, 2.0f, 500.0f), glm::vec3(0.0f),
                                                   glm::mat4(1.0f), /*dynamic=*/false);

    auto rb  = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);       // la velocidad la ponemos nosotros, sin acumular gravedad
    rb->setCcd(ccd);
    auto ball = pm.createSphereColliderComponent(
        10.0f, glm::vec3(0.0f),
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 500.0f, 0.0f)), /*dynamic=*/true);
    pm.attachRigidbody(ball, rb);
    // 30000 u/s a 1/60 = 500 unidades por sub-step: el test discreto compara
    // dos poses que caen a los dos lados del suelo y no ve nada entre ellas.
    rb->setVelocity(glm::vec3(0.0f, -30000.0f, 0.0f));

    for (int i = 0; i < 10; ++i) pm.stepSimulation(1.0f / 60.0f);
    return ball->getWorldTransform()[3].y;
}

// Con CCD la esfera no atraviesa el suelo fino; sin CCD sí (control).
static void test_ccd_prevents_tunneling(PhysicsManager& pm)
{
    float without = dropFastSphereOnThinFloor(pm, /*ccd=*/false);
    float with    = dropFastSphereOnThinFloor(pm, /*ccd=*/true);
    std::printf("  túnel: sin CCD y=%.1f | con CCD y=%.1f\n", without, with);
    CHECK(without < -100.0f);   // control: sin CCD se cuela y sigue cayendo
    CHECK(with    >   0.0f);    // con CCD se queda encima del suelo
}

// Con interpolate, getWorldTransform devuelve la mezcla pose_previa/pose_actual
// según el alpha del acumulador, no la pose cruda del actor.
static void test_interpolate_returns_intermediate_pose(PhysicsManager& pm)
{
    const float fixed = pm.getFixedDeltaTime();
    flushAccumulator(pm);

    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);       // movimiento rectilíneo uniforme: la pose
    rb->setInterpolate(true);       // intermedia esperada es exactamente la media
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    const float speed = 600.0f;     // unidades/s
    rb->setVelocity(glm::vec3(speed, 0.0f, 0.0f));

    // Un frame de exactamente un paso fijo: el acumulador queda a 0 -> alpha 0
    // -> la pose visible es la PREVIA al sub-step (la interpolación va un paso
    // por detrás, es su precio).
    pm.stepSimulation(fixed);
    const float rawAfterStep = rawActorX(col);
    const float shownAtAlpha0 = col->getWorldTransform()[3].x;
    CHECK(rawAfterStep > 5.0f);                             // el actor SÍ se movió
    CHECK(std::fabs(shownAtAlpha0 - 0.0f) < 0.5f);          // lo visible sigue en el origen
    CHECK(std::fabs(shownAtAlpha0 - rawAfterStep) > 5.0f);  // y NO es la pose cruda

    // Medio paso más de tiempo real: no cabe otro sub-step, así que el actor no
    // se mueve y sólo sube el alpha a 0.5 -> media exacta entre las dos poses.
    pm.stepSimulation(fixed * 0.5f);
    const float rawAfterHalf   = rawActorX(col);
    const float shownAtAlphaHalf = col->getWorldTransform()[3].x;
    const float expected = 0.5f * (0.0f + rawAfterHalf);
    std::printf("  interpolación: crudo=%.3f | alpha=0 -> %.3f | alpha=0.5 -> %.3f (esperado %.3f)\n",
                rawAfterHalf, shownAtAlpha0, shownAtAlphaHalf, expected);
    CHECK(std::fabs(rawAfterHalf - rawAfterStep) < 1e-3f);  // el actor no avanzó
    CHECK(std::fabs(shownAtAlphaHalf - expected) < 0.1f);
}

// Sin interpolate (el default), getWorldTransform devuelve la pose cruda: el
// comportamiento de toda escena existente no cambia.
static void test_interpolate_off_returns_raw_pose(PhysicsManager& pm)
{
    const float fixed = pm.getFixedDeltaTime();
    flushAccumulator(pm);

    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    CHECK(!rb->getInterpolate());
    rb->setVelocity(glm::vec3(600.0f, 0.0f, 0.0f));

    pm.stepSimulation(fixed);
    pm.stepSimulation(fixed * 0.5f);   // alpha 0.5: si interpolara, se notaría
    CHECK(std::fabs(col->getWorldTransform()[3].x - rawActorX(col)) < 1e-4f);
    CHECK(rawActorX(col) > 5.0f);      // y el cuerpo se movió de verdad
}

// --- Escala del Transform ----------------------------------------------------
// PxTransform no admite escala, así que la del GameObject se hornea en la
// geometría de la shape. Estos helpers leen lo que acabó DE VERDAD en PhysX (no
// el valor configurado del collider, que no debe cambiar nunca).
static physx::PxVec3 shapeHalfExtents(const std::shared_ptr<Collider>& col)
{
    auto* shape = static_cast<physx::PxShape*>(col->geometryShape());
    return static_cast<const physx::PxBoxGeometry&>(shape->getGeometry()).halfExtents;
}

static float shapeSphereRadius(const std::shared_ptr<Collider>& col)
{
    auto* shape = static_cast<physx::PxShape*>(col->geometryShape());
    return static_cast<const physx::PxSphereGeometry&>(shape->getGeometry()).radius;
}

static physx::PxCapsuleGeometry shapeCapsule(const std::shared_ptr<Collider>& col)
{
    auto* shape = static_cast<physx::PxShape*>(col->geometryShape());
    return static_cast<const physx::PxCapsuleGeometry&>(shape->getGeometry());
}

// Escala uniforme 2x en una caja: la geometría dobla en los tres ejes y el
// tamaño configurado (lo que ve el inspector y se serializa) NO cambia.
static void test_scale_uniform_box(PhysicsManager& pm)
{
    const glm::mat4 xform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f),
                                             xform, /*dynamic=*/false);
    const physx::PxVec3 he = shapeHalfExtents(col);
    CHECK(std::fabs(he.x - 2.0f) < 1e-4f);
    CHECK(std::fabs(he.y - 4.0f) < 1e-4f);
    CHECK(std::fabs(he.z - 6.0f) < 1e-4f);
    CHECK(col->getHalfExtents() == glm::vec3(1.0f, 2.0f, 3.0f));
}

// Escala uniforme 2x en una esfera: el radio dobla, m_radius no.
static void test_scale_uniform_sphere(PhysicsManager& pm)
{
    const glm::mat4 xform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), xform, /*dynamic=*/false);
    CHECK(std::fabs(shapeSphereRadius(col) - 20.0f) < 1e-4f);
    CHECK(col->getRadius() == 10.0f);
}

// Escala NO uniforme en una caja: cada eje va por su cuenta.
static void test_scale_non_uniform_box(PhysicsManager& pm)
{
    const glm::mat4 xform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 4.0f));
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             xform, /*dynamic=*/false);
    const physx::PxVec3 he = shapeHalfExtents(col);
    CHECK(std::fabs(he.x - 2.0f) < 1e-4f);
    CHECK(std::fabs(he.y - 3.0f) < 1e-4f);
    CHECK(std::fabs(he.z - 4.0f) < 1e-4f);
}

// Espejo y escala cero. Se prueban por setWorldScale y no por una matriz: una
// matriz con un eje a 0 es singular y glm::decompose saca de ella una rotación
// con NaN que PhysX rechaza al crear el actor (problema aparte, anterior a
// esto). Con escala negativa manda el valor absoluto —un espejo no adelgaza la
// caja— y con 0 se acota a un mínimo positivo, porque PhysX rechaza extents <= 0.
static void test_scale_mirror_and_zero_are_clamped(PhysicsManager& pm)
{
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/false);
    col->setWorldScale(glm::vec3(-2.0f, 3.0f, 0.0f));
    const physx::PxVec3 he = shapeHalfExtents(col);
    CHECK(std::fabs(he.x - 2.0f) < 1e-4f);
    CHECK(std::fabs(he.y - 3.0f) < 1e-4f);
    CHECK(he.z > 0.0f && he.z < 1e-3f);
}

// Una esfera con escala no uniforme sigue siendo esfera: manda el eje mayor.
// La cápsula reparte: radio con max(|x|,|z|), medio-alto con |y|.
static void test_scale_non_uniform_sphere_and_capsule(PhysicsManager& pm)
{
    const glm::mat4 xform = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 5.0f, 3.0f));
    auto sph = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), xform, /*dynamic=*/false);
    CHECK(std::fabs(shapeSphereRadius(sph) - 50.0f) < 1e-4f);

    auto cap = pm.createCapsuleColliderComponent(10.0f, 20.0f, glm::vec3(0.0f),
                                                 xform, /*dynamic=*/false);
    const physx::PxCapsuleGeometry g = shapeCapsule(cap);
    CHECK(std::fabs(g.radius - 30.0f) < 1e-4f);      // max(|2|, |3|) = 3
    CHECK(std::fabs(g.halfHeight - 100.0f) < 1e-4f); // |5| en Y
    CHECK(cap->getRadius() == 10.0f && cap->getHalfHeight() == 20.0f);
}

// Escala 1: la geometría es EXACTAMENTE la configurada (igualdad, no
// tolerancia). Es el guardián de "ninguna escena existente cambia".
static void test_scale_one_is_identical(PhysicsManager& pm)
{
    auto box = pm.createBoxColliderComponent(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/false);
    const physx::PxVec3 he = shapeHalfExtents(box);
    CHECK(he.x == 1.0f && he.y == 2.0f && he.z == 3.0f);

    auto sph = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), glm::mat4(1.0f),
                                                /*dynamic=*/false);
    CHECK(shapeSphereRadius(sph) == 10.0f);

    // Y una rotación pura tampoco la toca, aunque glm::decompose devuelva
    // 1±1e-7 en sus ejes (la comparación de escala va con tolerancia).
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.3f, 0.9f, 0.2f));
    auto rotated = pm.createBoxColliderComponent(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f),
                                                 rot, /*dynamic=*/false);
    const physx::PxVec3 rhe = shapeHalfExtents(rotated);
    CHECK(rhe.x == 1.0f && rhe.y == 2.0f && rhe.z == 3.0f);

    // Ni un ruido de escala por debajo de la tolerancia: comparar con == en vez
    // de con tolerancia dejaría 1.0000005 de factor y la geometría cambiaría.
    rotated->setWorldScale(glm::vec3(1.0f + 5e-7f));
    const physx::PxVec3 nhe = shapeHalfExtents(rotated);
    CHECK(nhe.x == 1.0f && nhe.y == 2.0f && nhe.z == 3.0f);
}

// La escala se re-aplica cuando cambia, no sólo al crear: el mismo camino que
// ya empuja la pose (syncTransform) la lleva.
static void test_scale_reapplied_on_change(PhysicsManager& pm)
{
    auto rb = std::make_shared<Rigidbody>();
    rb->setIsKinematic(true);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    CHECK(shapeHalfExtents(col).x == 1.0f);

    col->syncTransform(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 2.0f)));
    CHECK(std::fabs(shapeHalfExtents(col).x - 2.0f) < 1e-4f);

    // Y de vuelta a 1: no se queda pegada al último valor.
    col->syncTransform(glm::mat4(1.0f));
    CHECK(shapeHalfExtents(col).x == 1.0f);
}

// El collider escalado colisiona con lo que su tamaño configurado no alcanzaría:
// la escala llega a la simulación, no sólo al dato de la shape.
static void test_scaled_box_actually_collides(PhysicsManager& pm)
{
    // Suelo estático fino en y=0, escalado 10x en X/Z (100 de medio-ancho).
    auto floor = pm.createBoxColliderComponent(glm::vec3(10.0f, 1.0f, 10.0f), glm::vec3(0.0f),
                                               glm::scale(glm::mat4(1.0f), glm::vec3(10.0f, 1.0f, 10.0f)),
                                               /*dynamic=*/false);
    // Cuerpo que cae a 50 en X: fuera del suelo sin escalar (10), dentro con ella.
    auto rb  = std::make_shared<Rigidbody>();
    auto body = pm.createSphereColliderComponent(5.0f, glm::vec3(0.0f),
                                                 glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, 40.0f, 0.0f)),
                                                 /*dynamic=*/true);
    pm.attachRigidbody(body, rb);
    step(pm, 120, 1.0f / 60.0f);
    CHECK(body->getWorldTransform()[3].y > 0.0f); // se quedó encima del suelo
    (void)floor;
}

// Cambiar la escala DURANTE Play, sobre un collider sin Rigidbody, llega a
// PhysX. Es el camino static de Scene::update, que decide si empujar la pose
// comparando la del actor con la del GameObject NORMALIZADA (sin escala): sin
// una comparación aparte de la escala, un cambio de sólo-escala no mueve un bit
// de esa matriz y la geometría se queda con el tamaño del frame anterior.
static void test_scale_change_during_play_reaches_physx(PhysicsManager& pm)
{
    Scene scene("escala");
    GameObject* go = scene.addGameObject("caja");
    go->localTransform = glm::mat4(1.0f);
    go->updateWorldTransforms(glm::mat4(1.0f));

    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             go->worldTransform, /*dynamic=*/false);
    go->setBoxCollider(col);

    scene.update(1.0f / 60.0f, pm);
    CHECK(shapeHalfExtents(col).x == 1.0f); // sin tocar: escala 1

    // Dos updates: el traverse lee worldTransform y la propagación
    // local->world corre AL FINAL de update, así que el cambio entra en el
    // frame siguiente (misma latencia que vería un script Lua).
    go->localTransform = glm::scale(glm::mat4(1.0f), glm::vec3(3.0f));
    scene.update(1.0f / 60.0f, pm);
    scene.update(1.0f / 60.0f, pm);
    CHECK(std::fabs(shapeHalfExtents(col).x - 3.0f) < 1e-4f);

    // Y con la escala ya estable la geometría se queda quieta: no se re-escala
    // sobre sí misma frame a frame. (Que ADEMÁS no se llame a teleport no se
    // puede ver desde aquí: el collider no lleva cuenta de teletransportes.)
    scene.update(1.0f / 60.0f, pm);
    CHECK(std::fabs(shapeHalfExtents(col).x - 3.0f) < 1e-4f);

    // El GameObject muere con la Scene; el collider lo sigue teniendo el
    // shared_ptr local, que libera su actor al salir de la función.
    go->setBoxCollider(nullptr);
}

int main()
{
    PhysicsManager pm;
    pm.init();
    test_free_fall(pm);
    test_kinematic_no_fall(pm);
    test_freeze_position_y(pm);
    test_add_impulse(pm);
    test_force_mode_changes_magnitude(pm);
    test_force_mode_default_is_force(pm);
    test_rebuild_preserves_shape(pm);
    test_trigger_needs_a_rigidbody(pm);
    test_trigger_fires_when_other_has_rigidbody(pm);
    test_trigger_fires_when_trigger_has_rigidbody(pm);
    test_restitution_changes_bounce_height(pm);
    test_material_survives_rebuild(pm);
    test_fixed_step_is_framerate_independent(pm);
    test_giant_dt_clamped_to_max_substeps(pm);
    test_giant_dt_leaves_no_debt(pm);
    test_fixed_step_setters_reject_bad_values(pm);
    test_sphere_cast_uses_the_radius(pm);
    test_sphere_cast_miss(pm);
    test_overlap_sphere_counts(pm);
    test_overlap_box_honours_rotation(pm);
    test_layer_matrix_defaults_and_bounds(pm);
    test_layer_off_suppresses_contact(pm);
    test_layer_reenabled_at_runtime_makes_contact(pm);
    test_trigger_respects_layer_matrix(pm);
    test_add_and_remove_layers(pm);
    test_collision_enter_stay_exit(pm);
    test_collision_respects_layer_matrix(pm);
    test_trigger_emits_no_collision_events(pm);
    test_ccd_flag_reaches_the_actor(pm);
    test_ccd_prevents_tunneling(pm);
    test_interpolate_returns_intermediate_pose(pm);
    test_interpolate_off_returns_raw_pose(pm);
    test_scale_uniform_box(pm);
    test_scale_uniform_sphere(pm);
    test_scale_non_uniform_box(pm);
    test_scale_mirror_and_zero_are_clamped(pm);
    test_scale_non_uniform_sphere_and_capsule(pm);
    test_scale_one_is_identical(pm);
    test_scale_reapplied_on_change(pm);
    test_scaled_box_actually_collides(pm);
    test_scale_change_during_play_reaches_physx(pm);
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL PHYSICS TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
