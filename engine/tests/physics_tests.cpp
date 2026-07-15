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

int main()
{
    PhysicsManager pm;
    pm.init();
    test_free_fall(pm);
    test_kinematic_no_fall(pm);
    test_freeze_position_y(pm);
    test_add_impulse(pm);
    test_rebuild_preserves_shape(pm);
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL PHYSICS TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
