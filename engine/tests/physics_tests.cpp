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
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL PHYSICS TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
