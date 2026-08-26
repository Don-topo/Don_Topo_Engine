// Test headless del núcleo de física (sin GUI). Plain main + asserts, sin
// framework — coherente con un proyecto C++/CMake/Ninja sin infra de tests.
//
// PhysX sólo admite UNA PxFoundation por proceso (crearla dos veces, aunque se
// libere entremedias, crashea). Por eso se comparte un único PhysicsManager
// entre todos los tests: cada test crea sus colliders como locales, que al
// salir de la función liberan su actor de la escena — así sólo hay un cuerpo
// vivo a la vez y los tests no se interfieren.
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
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
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL PHYSICS TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
