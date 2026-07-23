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
#include "DonTopo/Physics/Colliders/Collider.h"

#include <glm/glm.hpp>
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

int main()
{
    PhysicsManager pm;
    pm.init();
    test_free_fall(pm);
    test_kinematic_no_fall(pm);
    test_freeze_position_y(pm);
    test_add_impulse(pm);
    test_rebuild_preserves_shape(pm);
    test_trigger_needs_a_rigidbody(pm);
    test_trigger_fires_when_other_has_rigidbody(pm);
    test_trigger_fires_when_trigger_has_rigidbody(pm);
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL PHYSICS TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
