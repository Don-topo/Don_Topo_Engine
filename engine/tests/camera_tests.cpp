// Test headless del CameraComponent (sin GUI). Plain main + asserts, sin
// framework — coherente con physics_tests.cpp.
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// Defaults a la escala de este repo, no a los de Unity.
static void test_defaults()
{
    CameraComponent c;
    CHECK(c.getMode() == CameraComponent::ProjectionMode::Perspective);
    CHECK(nearlyEqual(c.getFov(), 45.0f));
    CHECK(nearlyEqual(c.getOrthographicSize(), 100.0f));
    CHECK(nearlyEqual(c.getNear(), 1.0f));
    CHECK(nearlyEqual(c.getFar(), 2000.0f));
}

// Los clamps viven en el componente: un JSON editado a mano no puede instalar
// una proyección degenerada.
static void test_clamps()
{
    CameraComponent c;
    c.setFov(0.0f);      CHECK(c.getFov() >= 1.0f);
    c.setFov(500.0f);    CHECK(c.getFov() <= 179.0f);
    c.setOrthographicSize(-5.0f); CHECK(c.getOrthographicSize() > 0.0f);
    c.setNear(-5.0f);    CHECK(c.getNear() > 0.0f);
    // far nunca queda por debajo de near.
    c.setNear(10.0f);
    c.setFar(5.0f);
    CHECK(c.getFar() > c.getNear());
    // near nunca sobrepasa far, y hacerlo no debe mover far.
    CameraComponent d;
    d.setFar(100.0f);
    d.setNear(500.0f);
    CHECK(d.getNear() < d.getFar());
    CHECK(nearlyEqual(d.getFar(), 100.0f));
}

// El Y-flip de Vulkan va DENTRO de projectionMatrix: sus dos consumidores (UBO
// del Renderer y Gizmos::drawFrustum) lo necesitan.
static void test_projection_has_vulkan_y_flip()
{
    CameraComponent c;
    glm::mat4 p = c.projectionMatrix(16.0f / 9.0f);
    CHECK(p[1][1] < 0.0f);
}

// Perspectiva y ortográfica no pueden dar la misma matriz.
static void test_projection_modes_differ()
{
    CameraComponent c;
    glm::mat4 persp = c.projectionMatrix(1.0f);
    c.setMode(CameraComponent::ProjectionMode::Orthographic);
    glm::mat4 ortho = c.projectionMatrix(1.0f);
    CHECK(persp != ortho);
    // En ortográfica, w del punto proyectado es 1 (sin división perspectiva).
    glm::vec4 clip = ortho * glm::vec4(0.0f, 0.0f, -50.0f, 1.0f);
    CHECK(nearlyEqual(clip.w, 1.0f));
}

// Un aspect degenerado (viewport de ancho 0 al minimizar) no debe producir NaN.
static void test_projection_degenerate_aspect()
{
    CameraComponent c;
    glm::mat4 p = c.projectionMatrix(0.0f);
    CHECK(!std::isnan(p[0][0]));
}

// La cámara mira a -Z local (convención de glm/lookAt y de DonTopo::Camera,
// cuyo yaw por defecto de -90° da front = (0,0,-1)).
static void test_view_from_world_translation()
{
    glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
    glm::mat4 view  = CameraComponent::viewFromWorld(world);
    // El origen del mundo queda 10 unidades delante de la cámara, o sea en -Z.
    glm::vec4 p = view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(nearlyEqual(p.x, 0.0f));
    CHECK(nearlyEqual(p.y, 0.0f));
    CHECK(nearlyEqual(p.z, -10.0f));
}

// La escala del GameObject NO debe entrar en la view (deformaría la imagen).
static void test_view_from_world_ignores_scale()
{
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 10.0f));
    glm::mat4 unscaled = CameraComponent::viewFromWorld(t);
    glm::mat4 scaled   = CameraComponent::viewFromWorld(t * glm::scale(glm::mat4(1.0f), glm::vec3(5.0f)));
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK(nearlyEqual(unscaled[col][row], scaled[col][row]));
}

// findCamera() es la ÚNICA fuente de verdad del invariante "una cámara por
// escena": tiene que encontrarla esté donde esté, no solo colgando de la raíz.
static void test_find_camera_at_any_depth()
{
    Scene scene("Test");
    CHECK(scene.findCamera() == nullptr);

    GameObject* parent = scene.addGameObject("Parent");
    GameObject* child  = scene.addGameObject("Child", parent);
    GameObject* nieto  = scene.addGameObject("Nieto", child);
    nieto->setCameraComponent(std::make_shared<CameraComponent>());

    CHECK(scene.findCamera() == nieto);
}

// La cámara puede vivir en CUALQUIER GameObject, no solo en uno llamado
// "Camera".
static void test_find_camera_ignores_name()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("CualquierNombre");
    go->setCameraComponent(std::make_shared<CameraComponent>());
    CHECK(scene.findCamera() == go);
    CHECK(scene.findCamera()->hasCameraComponent());
}

// Pre-orden: gana la primera en el recorrido, no una cualquiera.
static void test_find_camera_returns_first_in_preorder()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    a->setCameraComponent(std::make_shared<CameraComponent>());
    b->setCameraComponent(std::make_shared<CameraComponent>());
    CHECK(scene.findCamera() == a);
}

int main()
{
    test_defaults();
    test_clamps();
    test_projection_has_vulkan_y_flip();
    test_projection_modes_differ();
    test_projection_degenerate_aspect();
    test_view_from_world_translation();
    test_view_from_world_ignores_scale();
    test_find_camera_at_any_depth();
    test_find_camera_ignores_name();
    test_find_camera_returns_first_in_preorder();
    if (g_failures == 0) std::printf("ALL CAMERA TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
